#include "Server.hpp"
#include "Decompiler.hpp"
#include "FissionVersion.hpp"
#include "ServerTransport.hpp"

#ifdef __clang__
#pragma clang diagnostic push
#if __has_warning("-Wc2y-extensions")
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#pragma clang diagnostic ignored "-Wmicrosoft-cpp-macro"
#pragma clang diagnostic ignored "-Wunused-value"
#pragma clang diagnostic ignored "-Wnan-infinity-disabled"
#endif

#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

#if defined(_WIN32)
#include <malloc.h>
#else
#include <poll.h>
#include <sys/socket.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#endif

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace {

    // SourceGenerator owns mutable state, so each worker needs its own decompiler.
    thread_local Decompiler g_Decompiler;

    std::chrono::steady_clock::time_point g_ServerStartTime;
    std::atomic<unsigned> g_ActiveDecompiles{0};
    std::atomic<unsigned> g_MaxConcurrentDecompiles{0};
    std::atomic<std::uint64_t> g_OutstandingDecompiles{0};

    const std::string kBase64Chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::optional<std::string> Base64Decode(const std::string &input) {
        if (input.empty())
            return std::string{};

        static const auto kDecodeTable = []() {
            std::array<int8_t, 256> table;
            table.fill(-1);
            for (size_t i = 0; i < 64; ++i)
                table[static_cast<uint8_t>(kBase64Chars[i])] = static_cast<int8_t>(i);
            return table;
        }();

        std::string output;
        output.reserve((input.size() * 3) / 4 + 1);

        uint32_t buffer = 0;
        int bitsInBuffer = 0;

        for (char c : input) {
            if (c == '=')
                break;

            int8_t value = kDecodeTable[static_cast<uint8_t>(c)];
            if (value < 0)
                return std::nullopt; // invalid character

            buffer = (buffer << 6) | static_cast<uint32_t>(value);
            bitsInBuffer += 6;

            if (bitsInBuffer >= 8) {
                bitsInBuffer -= 8;
                output.push_back(static_cast<char>((buffer >> bitsInBuffer) & 0xFF));
            }
        }

        return output;
    }

    std::string ErrorJson(const std::string &message, const std::string &debugNotes = {}) {
        json::object obj;
        obj["ok"] = false;
        obj["error"] = message;
        if (!debugNotes.empty())
            obj["debugNotes"] = debugNotes;
        return json::serialize(obj);
    }

    std::string ResultJson(DecompilationResult &&result) {
        json::object obj;
        obj["ok"] = true;
        json::object res;
        res["decompilationOutput"] = result.decompilationOutput;
        res["irOutput"] = result.irOutput;
        res["timingStatistics"] = result.timingStatistics;
        static const char *kResultCodeNames[] = {"Success", "FailedToReadFile", "FailedToDeserialize", "FailedToDecompile"};
        res["resultCode"] = kResultCodeNames[static_cast<int>(result.resultCode)];
        // Optional AST output stays structured JSON instead of a JSON-encoded string.
        if (!result.cfgGraph.empty())
            res["cfg"] = result.cfgGraph;
        if (!result.debugNotes.empty())
            res["debugNotes"] = result.debugNotes;
        if (!result.astJson.empty()) {
            boost::system::error_code ec;
            json::value astValue = json::parse(result.astJson, ec);
            if (ec)
                res["ast"] = result.astJson; // fallback: hand back the raw string if it somehow won't parse
            else
                res["ast"] = std::move(astValue);
        }
        obj["result"] = std::move(res);
        return json::serialize(obj);
    }

    std::string RootJson() {
        json::object obj;
        obj["name"] = "Fission.Server";
        obj["version"] = FISSION_VERSION;
        obj["description"] = "Luau bytecode decompilation HTTP API";
        json::object endpoints;
        endpoints["decompile"] =
            "POST /decompile  (body: {mode, bytecode, flags?, timeout?, outputs?}; outputs may include \"ir\", \"cfg\", \"ast\", and \"debug\")";
        endpoints["health"] = "GET /health";
        endpoints["root"] = "GET /";
        obj["endpoints"] = endpoints;
        return json::serialize(obj);
    }

    std::string HealthJson() {
        auto now = std::chrono::steady_clock::now();
        auto uptime = std::chrono::duration<double>(now - g_ServerStartTime).count();
        json::object obj;
        obj["ok"] = true;
        obj["status"] = "healthy";
        obj["uptime_seconds"] = uptime;
        obj["activeDecompiles"] = g_ActiveDecompiles.load(std::memory_order_relaxed);
        obj["maxConcurrentDecompiles"] = g_MaxConcurrentDecompiles.load(std::memory_order_relaxed);
        return json::serialize(obj);
    }

    DecompilerFlags FlagsFromJson(const json::object &flags) {
        DecompilerFlags result = static_cast<DecompilerFlags>(0);
        auto setFlag = [&](const char *key, DecompilerFlags flag) {
            auto it = flags.find(key);
            if (it != flags.end() && it->value().is_bool() && it->value().as_bool())
                result |= flag;
        };
        setFlag("printIR", DecompilerFlags::PrintIR);
        setFlag("writeIRToFile", DecompilerFlags::WriteIRToFile);
        setFlag("generateIRGraph", DecompilerFlags::GenerateIRGraph);
        setFlag("generateSSAIRGraph", DecompilerFlags::GenerateSSAIRGraph);
        setFlag("printTimingBreakdown", DecompilerFlags::PrintTimingBreakdown);
        setFlag("inferTypes", DecompilerFlags::InferTypes);
        setFlag("optimizeIR", DecompilerFlags::OptimizeIR);
        setFlag("inferRobloxTypes", DecompilerFlags::InferRobloxTypes);
        setFlag("autoNameVariables", DecompilerFlags::AutoNameVariables);
        setFlag("omitFissionComments", DecompilerFlags::OmitFissionComments);
        setFlag("fissionDebugNotes", DecompilerFlags::FissionDebugNotes);
        return result;
    }

    Fission::Server::HttpResult handle_decompile(const std::string &requestBody, const Fission::Server::ServerConfig &config) {
        json::value parsed;
        try {
            parsed = json::parse(requestBody);
        } catch (const std::exception &e) {
            return {400, "application/json", ErrorJson(std::string("Invalid JSON: ") + e.what())};
        }

        if (!parsed.is_object())
            return {400, "application/json", ErrorJson("Invalid JSON: expected object")};

        auto &obj = parsed.as_object();

        auto modeIt = obj.find("mode");
        if (modeIt == obj.end() || !modeIt->value().is_string())
            return {400, "application/json", ErrorJson("Missing required field: 'mode'")};

        std::string mode = modeIt->value().as_string().c_str();
        if (mode != "roblox" && mode != "vanilla")
            return {400, "application/json", ErrorJson("Invalid mode: '" + mode + "'. Expected 'roblox' or 'vanilla'.")};

        auto bcIt = obj.find("bytecode");
        if (bcIt == obj.end() || !bcIt->value().is_string())
            return {400, "application/json", ErrorJson("Missing required field: 'bytecode'")};

        auto decoded = Base64Decode(std::string(bcIt->value().as_string().c_str()));
        if (!decoded)
            return {400, "application/json", ErrorJson("Invalid base64 encoding")};

        DecompilerFlags flags = static_cast<DecompilerFlags>(0);
        auto flagsIt = obj.find("flags");
        if (flagsIt != obj.end() && flagsIt->value().is_object())
            flags = FlagsFromJson(flagsIt->value().as_object());

        // Remote requests may not write files or stdout. Concurrent requests would also race on fixed
        // artifact paths, so clients must request in-memory CFG or AST output instead.
        constexpr DecompilerFlags kServerUnsafeFlags = DecompilerFlags::PrintIR | DecompilerFlags::WriteIRToFile | DecompilerFlags::GenerateIRGraph |
                                                       DecompilerFlags::GenerateSSAIRGraph | DecompilerFlags::PrintTimingBreakdown;
        flags &= ~kServerUnsafeFlags;
        bool includeIR = false;

        // CFG and AST generation is opt-in because both add response and processing cost.
        auto outputsIt = obj.find("outputs");
        if (outputsIt != obj.end() && outputsIt->value().is_array()) {
            for (const auto &item : outputsIt->value().as_array()) {
                if (!item.is_string())
                    continue;
                std::string output = item.as_string().c_str();
                if (output == "ir")
                    includeIR = true;
                else if (output == "cfg")
                    flags |= DecompilerFlags::CaptureCFGGraph;
                else if (output == "ast")
                    flags |= DecompilerFlags::CaptureAST;
                else if (output == "debug")
                    flags |= DecompilerFlags::FissionDebugNotes;
            }
        }
        if (!includeIR)
            flags |= DecompilerFlags::OmitIR;

        // Reset reused thread-local state on every request so invalid or absent values cannot inherit
        // another request's budget.
        std::chrono::seconds effectiveTimeout = config.maxTimeout;
        auto timeoutIt = obj.find("timeout");
        if (timeoutIt != obj.end() && timeoutIt->value().is_int64()) {
            int64_t val = timeoutIt->value().as_int64();
            if (val > 0)
                effectiveTimeout =
                    std::chrono::seconds(static_cast<int64_t>(std::min(static_cast<uint64_t>(val), static_cast<uint64_t>(config.maxTimeout.count()))));
        }

        parsed = json::value{};
        g_Decompiler.SetDecompileBudget(effectiveTimeout);

        DecompilationResult result;
        if (mode == "roblox")
            result = g_Decompiler.DecompileRobloxBytecode(*decoded, flags);
        else
            result = g_Decompiler.DecompileVanillaBytecode(*decoded, flags);

        if (result.resultCode == DecompileResult::Success)
            return {200, "application/json", ResultJson(std::move(result))};

        static const char *kErrorMessages[] = {"", "Failed to read file", "Failed to deserialize bytecode", "Internal decompilation failure"};
        const unsigned int httpStatus = (result.resultCode == DecompileResult::FailedToDeserialize) ? 422u : 500u;
        const std::string errorMessage = result.errorMessage.empty() ? kErrorMessages[static_cast<int>(result.resultCode)] : result.errorMessage;
        return {httpStatus, "application/json", ErrorJson(errorMessage, result.debugNotes)};
    }

    // Bound memory consumed before request validation.
    constexpr std::uint64_t kMaxRequestBodyBytes = 32ull * 1024 * 1024;

    // Return freed decompiler pages to the OS.
    std::atomic<std::chrono::steady_clock::rep> g_lastTrimTicks{0};
    void ReleaseUnusedMemory() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ticks = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        constexpr auto kTrimInterval = std::chrono::seconds(1);
        auto last = g_lastTrimTicks.load(std::memory_order_relaxed);
        if (ticks - last < std::chrono::duration_cast<std::chrono::milliseconds>(kTrimInterval).count())
            return;
        if (!g_lastTrimTicks.compare_exchange_strong(last, ticks, std::memory_order_relaxed))
            return;
#if defined(_WIN32)
        (void)_heapmin();
#elif defined(__GLIBC__)
        (void)malloc_trim(0);
#endif
    }

    // Beast deadlines require asynchronous operations and a running io_context. Socket timeouts bound
    // these synchronous reads and writes so stalled clients cannot pin workers indefinitely.
    void SetReceiveTimeout(tcp::socket &socket, std::chrono::milliseconds timeout) {
        const auto handle = socket.native_handle();
        timeout = std::max(timeout, std::chrono::milliseconds(1));
#ifdef _WIN32
        DWORD ms = static_cast<DWORD>(timeout.count());
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    void SetBlockingSocketTimeout(tcp::socket &socket, std::chrono::milliseconds timeout) {
        const auto handle = socket.native_handle();
        timeout = std::max(timeout, std::chrono::milliseconds(1));
#ifdef _WIN32
        DWORD ms = static_cast<DWORD>(timeout.count());
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count() / 1000);
        tv.tv_usec = static_cast<decltype(tv.tv_usec)>((timeout.count() % 1000) * 1000);
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    std::size_t ReadSomeFromSocket(tcp::socket &socket, net::mutable_buffer buffer, beast::error_code &ec) {
#ifdef _WIN32
        return socket.read_some(buffer, ec);
#else
        ec = {};
        const auto bytes = ::recv(socket.native_handle(), buffer.data(), buffer.size(), 0);
        if (bytes > 0)
            return static_cast<std::size_t>(bytes);
        if (bytes == 0) {
            ec = net::error::eof;
            return 0;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            ec = net::error::try_again;
            return 0;
        }
        ec = beast::error_code(errno, boost::system::system_category());
        return 0;
#endif
    }

    void WriteResponse(tcp::socket &socket, unsigned int version, Fission::Server::HttpResult &&result) {
        http::response<http::string_body> res{static_cast<http::status>(result.status), version};
        res.set(http::field::content_type, result.contentType);
        res.set(http::field::server, "Fission.Server");
        res.body() = std::move(result.body);
        res.prepare_payload();
        beast::error_code ec;
        http::write(socket, res, ec);
    }

    void handle_session(
        tcp::socket socket, const Fission::Server::ServerConfig &config, std::chrono::steady_clock::time_point acceptedAt, Fission::Server::DecompileGate &gate,
        net::thread_pool &decompilePool, std::uint64_t maxOutstanding
    ) {
        beast::error_code ec;
        SetBlockingSocketTimeout(socket, std::chrono::duration_cast<std::chrono::milliseconds>(config.maxTimeout));
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(kMaxRequestBodyBytes);

        // Bound the whole request read, not just each individual receive. SO_RCVTIMEO alone lets a
        // client that dribbles bytes reset the timeout on every packet and hold a worker forever.
        ec = Fission::Server::ReadHttpRequest(socket, buffer, parser, acceptedAt + config.maxTimeout);
        if (ec) {
            std::cerr << "Read error: " << ec.message() << std::endl;
            return;
        }
        auto &req = parser.get();
        std::string body = std::move(req.body());

        if (req.method() != http::verb::post || req.target() != "/decompile") {
            auto result = Fission::Server::HandleRequest(http::to_string(req.method()), req.target(), body, config);
            WriteResponse(socket, req.version(), std::move(result));
            socket.shutdown(tcp::socket::shutdown_send, ec);
            ReleaseUnusedMemory();
            return;
        }

        if (std::chrono::steady_clock::now() - acceptedAt >= config.maxTimeout) {
            WriteResponse(socket, req.version(), {504, "application/json", ErrorJson("Request expired before decompilation started")});
            return;
        }
        const std::uint64_t estimate = Fission::Server::EstimateDecompileFootprint(body.size());
        if (config.maxDecompileMemoryBytes > 0 && estimate > config.maxDecompileMemoryBytes) {
            const auto budgetMiB = config.maxDecompileMemoryBytes / (1024 * 1024);
            WriteResponse(
                socket, req.version(),
                {413, "application/json", ErrorJson("Request exceeds the decompile memory budget (" + std::to_string(budgetMiB) + " MiB)")}
            );
            return;
        }
        if (g_OutstandingDecompiles.fetch_add(1, std::memory_order_relaxed) >= maxOutstanding) {
            g_OutstandingDecompiles.fetch_sub(1, std::memory_order_relaxed);
            WriteResponse(socket, req.version(), {503, "application/json", ErrorJson("Server busy: too many decompilations in flight")});
            return;
        }

        const auto method = req.method();
        const std::string target(req.target());
        const unsigned int version = req.version();
        net::post(decompilePool, [sock = std::move(socket), &config, &gate, method, target, version, body = std::move(body), estimate]() mutable {
            struct Outstanding {
                ~Outstanding() { g_OutstandingDecompiles.fetch_sub(1, std::memory_order_relaxed); }
            } outstanding;
            beast::error_code ec;
            try {
                if (gate.Acquire(estimate, config.queueWait)) {
                    struct Release {
                        Fission::Server::DecompileGate &gate;
                        std::uint64_t estimate;
                        ~Release() {
                            g_ActiveDecompiles.fetch_sub(1, std::memory_order_relaxed);
                            gate.Release(estimate);
                        }
                    } release{gate, estimate};
                    g_ActiveDecompiles.fetch_add(1, std::memory_order_relaxed);

                    auto result = Fission::Server::HandleRequest(http::to_string(method), target, body, config);
                    body.clear();
                    body.shrink_to_fit();
                    WriteResponse(sock, version, std::move(result));
                } else {
                    WriteResponse(sock, version, {503, "application/json", ErrorJson("Server busy: too many decompilations in flight")});
                }
            } catch (const std::exception &e) {
                std::cerr << "Decompile error: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Decompile error: unknown exception" << std::endl;
            }
            sock.shutdown(tcp::socket::shutdown_send, ec);
            ReleaseUnusedMemory();
        });
    }

} // anonymous namespace

namespace Fission::Server {

    boost::beast::error_code ReadHttpRequest(
        tcp::socket &socket, beast::flat_buffer &buffer, http::request_parser<http::string_body> &parser, std::chrono::steady_clock::time_point deadline
    ) {
        for (;;) {
            // Feed buffered bytes to the parser exactly once per pass. http::read_some may perform
            // several socket reads internally, which would let a partial-header client bypass the
            // deadline check between reads.
            if (buffer.size() > 0) {
                beast::error_code ec;
                const auto used = parser.put(buffer.data(), ec);
                buffer.consume(used);
                if (ec != http::error::need_more) {
                    if (ec)
                        return ec;
                    if (parser.is_done())
                        break;
                }
                // basic_parser is not eager. A header and body arriving together can leave body
                // bytes buffered after the first put, so drain them before waiting on the socket.
                if (used > 0 && buffer.size() > 0)
                    continue;
            }

            if (parser.is_done())
                break;

            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return beast::error_code(beast::error::timeout);

            // Each receive gets only the time that remains, so repeatedly arriving bytes cannot
            // extend the absolute deadline.
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            SetReceiveTimeout(socket, remaining);

#ifndef _WIN32
            // Asio's synchronous receive re-polls a blocking socket forever when recv returns
            // EAGAIN, so SO_RCVTIMEO alone is not a deadline on POSIX. Wait for readability with
            // the remaining time, then read directly so no hidden retry loop can outlive it.
            pollfd pfd{};
            pfd.fd = socket.native_handle();
            pfd.events = POLLIN;
            const auto waitMs = static_cast<int>(std::min<std::int64_t>(std::max<std::int64_t>(remaining.count(), 1), std::numeric_limits<int>::max()));
            const int pollResult = ::poll(&pfd, 1, waitMs);
            if (pollResult == 0)
                continue;
            if (pollResult < 0) {
                if (errno == EINTR)
                    continue;
                return beast::error_code(errno, boost::system::system_category());
            }
#endif

            beast::error_code ec;
            auto mutableBuffer = buffer.prepare(65536);
            const auto bytesRead = ReadSomeFromSocket(socket, mutableBuffer, ec);
            buffer.commit(bytesRead);

            if (ec == net::error::eof) {
                if (!parser.got_some())
                    return beast::error_code(http::error::end_of_stream);
                ec = {};
                parser.put_eof(ec);
                if (ec)
                    return ec;
                if (parser.is_done())
                    break;
                return beast::error_code(http::error::partial_message);
            }
            if (ec) {
                if (std::chrono::steady_clock::now() >= deadline)
                    return beast::error_code(beast::error::timeout);
                if (ec == beast::error::timeout || ec == net::error::timed_out || ec == net::error::try_again || ec == net::error::would_block)
                    continue;
                return ec;
            }
        }
        return {};
    }

    HttpResult HandleRequest(std::string_view method, std::string_view target, const std::string &body, const ServerConfig &config) {
        if (method == "GET" && target == "/")
            return {200, "application/json", RootJson()};
        if (method == "GET" && target == "/health")
            return {200, "application/json", HealthJson()};
        if (method == "POST" && target == "/decompile")
            return handle_decompile(body, config);
        return {404, "application/json", ErrorJson("Not found")};
    }

    void run_server(const ServerConfig &config) {
        g_ServerStartTime = std::chrono::steady_clock::now();

        net::io_context ioc;
        tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), config.port}};

        net::thread_pool ioPool{config.threadCount};
        const unsigned int maxActive = std::max(1u, config.maxConcurrentDecompiles);
        const unsigned int maxWaiting = config.maxQueuedRequests;
        // Decompiles run on their own pool so a request read cannot occupy a decompile slot and
        // decompile work cannot delay /health.
        net::thread_pool decompilePool{maxActive};
        DecompileGate gate{maxActive, maxWaiting, config.maxDecompileMemoryBytes};
        g_MaxConcurrentDecompiles.store(maxActive, std::memory_order_relaxed);
        const std::uint64_t maxOutstanding = static_cast<std::uint64_t>(maxActive) + maxWaiting;

        std::cout << "Fission.Server " << FISSION_VERSION << " listening on http://127.0.0.1:" << config.port << std::endl;
        std::cout << "Thread pool: " << config.threadCount << " io, " << maxActive << " decompile" << std::endl;
        std::cout << "Max concurrent decompiles: " << maxActive << " (" << maxWaiting << " queued)" << std::endl;
        std::cout << "Decompile memory budget: " << (config.maxDecompileMemoryBytes / (1024 * 1024)) << " MiB" << std::endl;
        std::cout << "Max timeout: " << config.maxTimeout.count() << "s" << std::endl;

        for (;;) {
            tcp::socket socket{ioc};
            beast::error_code ec;
            acceptor.accept(socket, ec);
            if (ec) {
                std::cerr << "Accept error: " << ec.message() << std::endl;
                continue;
            }
            const auto acceptedAt = std::chrono::steady_clock::now();
            // An escaping exception terminates a pool worker and can eventually exhaust the pool.
            net::post(ioPool, [sock = std::move(socket), &config, &gate, &decompilePool, maxOutstanding, acceptedAt]() mutable {
                try {
                    handle_session(std::move(sock), config, acceptedAt, gate, decompilePool, maxOutstanding);
                } catch (const std::exception &e) {
                    std::cerr << "Session error: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Session error: unknown exception" << std::endl;
                }
            });
        }
    }

} // namespace Fission::Server
