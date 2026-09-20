#include "Server.hpp"
#include "Decompiler.hpp"

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
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#pragma clang diagnostic pop
#endif

#include <array>
#include <chrono>
#include <iostream>
#include <optional>
#include <string>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
namespace json = boost::json;
using tcp = net::ip::tcp;

namespace {

    // SourceGenerator owns mutable state, so each worker needs its own decompiler.
    thread_local Decompiler g_Decompiler;

    std::chrono::steady_clock::time_point g_ServerStartTime;

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

    std::string ErrorJson(const std::string &message) {
        json::object obj;
        obj["ok"] = false;
        obj["error"] = message;
        return json::serialize(obj);
    }

    std::string ResultJson(const DecompilationResult &result) {
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
        if (!result.astJson.empty()) {
            boost::system::error_code ec;
            json::value astValue = json::parse(result.astJson, ec);
            if (ec)
                res["ast"] = result.astJson; // fallback: hand back the raw string if it somehow won't parse
            else
                res["ast"] = astValue;
        }
        obj["result"] = res;
        return json::serialize(obj);
    }

    std::string RootJson() {
        json::object obj;
        obj["name"] = "Fission.Server";
        obj["version"] = "1.0.0";
        obj["description"] = "Luau bytecode decompilation HTTP API";
        json::object endpoints;
        endpoints["decompile"] = "POST /decompile  (body: {mode, bytecode, flags?, timeout?, outputs?}; outputs may include \"cfg\" and \"ast\")";
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

        // CFG and AST generation is opt-in because both add response and processing cost.
        auto outputsIt = obj.find("outputs");
        if (outputsIt != obj.end() && outputsIt->value().is_array()) {
            for (const auto &item : outputsIt->value().as_array()) {
                if (!item.is_string())
                    continue;
                std::string output = item.as_string().c_str();
                if (output == "cfg")
                    flags |= DecompilerFlags::CaptureCFGGraph;
                else if (output == "ast")
                    flags |= DecompilerFlags::CaptureAST;
            }
        }

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
        g_Decompiler.SetDecompileBudget(effectiveTimeout);

        DecompilationResult result;
        if (mode == "roblox")
            result = g_Decompiler.DecompileRobloxBytecode(*decoded, flags);
        else
            result = g_Decompiler.DecompileVanillaBytecode(*decoded, flags);

        if (result.resultCode == DecompileResult::Success)
            return {200, "application/json", ResultJson(result)};

        static const char *kErrorMessages[] = {"", "Failed to read file", "Failed to deserialize bytecode", "Internal decompilation failure"};
        const unsigned int httpStatus = (result.resultCode == DecompileResult::FailedToDeserialize) ? 422u : 500u;
        const std::string errorMessage = result.errorMessage.empty() ? kErrorMessages[static_cast<int>(result.resultCode)] : result.errorMessage;
        return {httpStatus, "application/json", ErrorJson(errorMessage)};
    }

    // Bound memory consumed before request validation.
    constexpr std::uint64_t kMaxRequestBodyBytes = 32ull * 1024 * 1024;

    // Beast deadlines require asynchronous operations and a running io_context. Socket timeouts bound
    // these synchronous reads and writes so stalled clients cannot pin workers indefinitely.
    void SetBlockingSocketTimeout(tcp::socket &socket, std::chrono::seconds timeout) {
        const auto handle = socket.native_handle();
#ifdef _WIN32
        DWORD ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count());
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = static_cast<decltype(tv.tv_sec)>(timeout.count());
        tv.tv_usec = 0;
        setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    void handle_session(tcp::socket socket, const Fission::Server::ServerConfig &config) {
        beast::error_code ec;
        SetBlockingSocketTimeout(socket, config.maxTimeout);
        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(kMaxRequestBodyBytes);

        http::read(socket, buffer, parser, ec);
        if (ec) {
            std::cerr << "Read error: " << ec.message() << std::endl;
            return;
        }
        const auto &req = parser.get();

        const auto result = Fission::Server::HandleRequest(http::to_string(req.method()), req.target(), req.body(), config);

        http::response<http::string_body> res{static_cast<http::status>(result.status), req.version()};
        res.set(http::field::content_type, result.contentType);
        res.set(http::field::server, "Fission.Server");
        res.body() = result.body;
        res.prepare_payload();

        http::write(socket, res, ec);

        socket.shutdown(tcp::socket::shutdown_send, ec);
    }

} // anonymous namespace

namespace Fission::Server {

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

        net::thread_pool pool{config.threadCount};

        std::cout << "Fission.Server listening on http://127.0.0.1:" << config.port << std::endl;
        std::cout << "Thread pool: " << config.threadCount << " workers" << std::endl;
        std::cout << "Max timeout: " << config.maxTimeout.count() << "s" << std::endl;

        for (;;) {
            tcp::socket socket{ioc};
            beast::error_code ec;
            acceptor.accept(socket, ec);
            if (ec) {
                std::cerr << "Accept error: " << ec.message() << std::endl;
                continue;
            }
            // An escaping exception terminates a pool worker and can eventually exhaust the pool.
            net::post(pool, [sock = std::move(socket), config]() mutable {
                try {
                    handle_session(std::move(sock), config);
                } catch (const std::exception &e) {
                    std::cerr << "Session error: " << e.what() << std::endl;
                } catch (...) {
                    std::cerr << "Session error: unknown exception" << std::endl;
                }
            });
        }
    }

} // namespace Fission::Server
