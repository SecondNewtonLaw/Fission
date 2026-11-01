// Linux load test for the server's freed-page return path (malloc_trim). Repeated large-script
// bursts must not leave resident memory permanently above the server's steady-state baseline.
//
// Guarded to Linux because it reads /proc/<pid>/status and because malloc_trim is glibc-only.

#include <catch2/catch_test_macros.hpp>

// ASan's quarantine retains freed decompiler memory regardless of malloc_trim, so RSS cannot
// return to the uninstrumented baseline under that allocator configuration.
#ifndef FISSION_TEST_ASAN
#if defined(__SANITIZE_ADDRESS__)
#define FISSION_TEST_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FISSION_TEST_ASAN 1
#endif
#endif
#endif

#if defined(__linux__) && !defined(FISSION_TEST_ASAN)

#include "Luau/Common.h"
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Luau/Compiler.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif

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
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
    constexpr unsigned int kFunctionCount = 1500;
    constexpr unsigned int kWarmupFunctionCount = 60;
    constexpr unsigned int kBurstCount = 4;
    // A single burst peaks well above this, so a broken trim path leaves a clearly detectable
    // excess. The allowance covers normal allocator fragmentation and test-process growth.
    constexpr std::uint64_t kResidentAllowanceKiB = 96ull * 1024;

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::string Base64Encode(const std::string &in) {
        static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        unsigned int val = 0;
        int bits = -6;
        for (unsigned char c : in) {
            val = (val << 8) + c;
            bits += 8;
            while (bits >= 0) {
                out.push_back(t[(val >> bits) & 0x3F]);
                bits -= 6;
            }
        }
        if (bits > -6)
            out.push_back(t[((val << 8) >> (bits + 8)) & 0x3F]);
        while (out.size() % 4)
            out.push_back('=');
        return out;
    }

    // A deterministic script of many independent scopes. Each scope declares one function and
    // calls it, so the script never exceeds Luau's per-function local-variable limit.
    std::string MakeScript(unsigned int functionCount) {
        std::ostringstream source;
        source << "local acc = 0\n";
        for (unsigned int i = 0; i < functionCount; ++i) {
            source << "do\n"
                   << "    local function f" << i << "(a, b, c)\n"
                   << "        local t = {a, b, c, a + b, b + c, c + a, a * 2, b * 3, c * 4}\n"
                   << "        for j = 1, #t do\n"
                   << "            if t[j] % " << (i % 3 + 2) << " == 0 then\n"
                   << "                acc = acc + t[j]\n"
                   << "            else\n"
                   << "                acc = acc - t[j]\n"
                   << "            end\n"
                   << "        end\n"
                   << "        return acc\n"
                   << "    end\n"
                   << "    acc = f" << i << "(acc, " << i << ", acc + " << i << ")\n"
                   << "end\n";
        }
        source << "return acc\n";
        return source.str();
    }

    std::string CompileScript(unsigned int functionCount) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions options{};
        options.optimizationLevel = 0;
        options.debugLevel = 2;
        return Luau::compile(MakeScript(functionCount), options);
    }

    std::string MakeRequestBody(const std::string &bytecode) {
        return "{\"mode\":\"vanilla\",\"bytecode\":\"" + Base64Encode(bytecode) + "\",\"timeout\":110}";
    }

    std::uint16_t ReserveUnusedPort() {
        net::io_context ioc;
        tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), 0}};
        return acceptor.local_endpoint().port();
    }

    std::uint64_t ReadResidentKiB(pid_t pid) {
        std::ifstream status("/proc/" + std::to_string(pid) + "/status");
        std::string line;
        while (std::getline(status, line)) {
            if (line.rfind("VmRSS:", 0) == 0) {
                std::istringstream value(line.substr(6));
                std::uint64_t kib = 0;
                value >> kib;
                return kib;
            }
        }
        return 0;
    }

    class ChildProcess {
        pid_t m_Pid = -1;

      public:
        explicit ChildProcess(pid_t pid) : m_Pid(pid) {}
        ChildProcess(const ChildProcess &) = delete;
        ChildProcess &operator=(const ChildProcess &) = delete;
        ~ChildProcess() {
            if (m_Pid > 0) {
                kill(m_Pid, SIGTERM);
                waitpid(m_Pid, nullptr, 0);
            }
        }

        pid_t Pid() const { return m_Pid; }
    };

    struct HttpResponse {
        unsigned int status = 0;
        std::string body;
    };

    HttpResponse Request(std::uint16_t port, http::verb verb, const std::string &target, const std::string &body) {
        net::io_context ioc;
        tcp::socket socket{ioc};
        socket.connect({net::ip::make_address("127.0.0.1"), port});

        http::request<http::string_body> request{verb, target, 11};
        request.set(http::field::host, "127.0.0.1");
        if (!body.empty()) {
            request.set(http::field::content_type, "application/json");
            request.body() = body;
            request.prepare_payload();
        }
        http::write(socket, request);

        beast::flat_buffer buffer;
        http::response<http::string_body> response;
        http::read(socket, buffer, response);

        beast::error_code ec;
        socket.shutdown(tcp::socket::shutdown_both, ec);
        return {response.result_int(), std::move(response.body())};
    }

    // Run one request while sampling the server's RSS, so the high-water mark is observed even
    // though the server trims immediately after writing the response.
    HttpResponse RequestAndTrackPeak(std::uint16_t port, const std::string &body, pid_t serverPid, std::uint64_t &peakKiB) {
        HttpResponse response;
        std::exception_ptr error;
        std::atomic<bool> done{false};

        std::thread request([&] {
            try {
                response = Request(port, http::verb::post, "/decompile", body);
            } catch (...) {
                error = std::current_exception();
            }
            done.store(true, std::memory_order_release);
        });

        while (!done.load(std::memory_order_acquire)) {
            peakKiB = std::max(peakKiB, ReadResidentKiB(serverPid));
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        request.join();
        if (error)
            std::rethrow_exception(error);
        peakKiB = std::max(peakKiB, ReadResidentKiB(serverPid));
        return response;
    }

    bool WaitForHealth(std::uint16_t port) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            try {
                const auto response = Request(port, http::verb::get, "/health", "");
                if (response.status == 200 && response.body.find("\"ok\":true") != std::string::npos)
                    return true;
            } catch (const std::exception &) {
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    void CheckSuccess(const char *label, const HttpResponse &response) {
        INFO(label << ": HTTP " << response.status << " body " << response.body.substr(0, 256));
        REQUIRE(response.status == 200);
        REQUIRE(response.body.find("\"resultCode\":\"Success\"") != std::string::npos);
    }
} // namespace

TEST_CASE("Server: sustained large-script load returns resident memory near baseline", "[Server][Memory][Linux]") {
    const std::uint16_t port = ReserveUnusedPort();

    // Spawn the real release server binary so the test exercises the production trim path.
    const pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        const std::string portText = std::to_string(port);
        execl(
            FISSION_SERVER_EXE, "Fission.Server", "--port", portText.c_str(), "--max-concurrency", "1", "--max-queued", "0", "--max-timeout", "120",
            static_cast<char *>(nullptr)
        );
        _exit(127);
    }

    ChildProcess child(pid);
    REQUIRE(WaitForHealth(port));

    const std::string warmupBytecode = CompileScript(kWarmupFunctionCount);
    const std::string bytecode = CompileScript(kFunctionCount);
    REQUIRE(bytecode.size() > 100ull * 1024);
    const std::string warmupBody = MakeRequestBody(warmupBytecode);
    const std::string requestBody = MakeRequestBody(bytecode);

    // Warm the decompiler thread-local storage with a smaller request so the baseline is not
    // already sitting at the large workload's high-water mark.
    CheckSuccess("warmup", Request(port, http::verb::post, "/decompile", warmupBody));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Request(port, http::verb::get, "/health", ""); // also triggers the rate-limited trim path
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const std::uint64_t baseline = ReadResidentKiB(child.Pid());
    REQUIRE(baseline > 0);

    std::uint64_t peak = baseline;
    for (unsigned int burst = 0; burst < kBurstCount; ++burst) {
        HttpResponse response = RequestAndTrackPeak(port, requestBody, child.Pid(), peak);
        // The previous request may still be inside ReleaseUnusedMemory(), which keeps its
        // outstanding slot until cleanup finishes. Retry a transient busy response.
        for (int retry = 0; retry < 50 && response.status == 503; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            response = RequestAndTrackPeak(port, requestBody, child.Pid(), peak);
        }
        CheckSuccess("burst", response);
    }

    // Let the last request age past the trim interval, then use /health to trigger the trim. The
    // response is written before ReleaseUnusedMemory finishes, so poll for the resulting RSS drop
    // instead of assuming a fixed completion delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    Request(port, http::verb::get, "/health", "");

    std::uint64_t after = ReadResidentKiB(child.Pid());
    const auto trimDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (after > baseline + kResidentAllowanceKiB && std::chrono::steady_clock::now() < trimDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        after = ReadResidentKiB(child.Pid());
    }
    INFO(
        "baseline KiB: " << baseline << ", peak KiB: " << peak << ", after " << kBurstCount << " bursts: " << after
                         << ", allowance KiB: " << kResidentAllowanceKiB
    );
    REQUIRE(after > 0);
    // The workload must actually move RSS beyond the allowance; otherwise the final check cannot
    // distinguish a working trim from allocations that never happened.
    CHECK(peak >= baseline + kResidentAllowanceKiB);
    CHECK(after <= baseline + kResidentAllowanceKiB);
}

#elif defined(FISSION_TEST_ASAN)

TEST_CASE("Server: sustained large-script load returns resident memory near baseline", "[Server][Memory][Linux]") {
    SKIP("Linux memory test is skipped under AddressSanitizer (quarantine prevents RSS baseline)");
}

#else

TEST_CASE("Server: sustained large-script load returns resident memory near baseline", "[Server][Memory][Linux]") {
    SKIP("Linux-only load test (requires /proc and malloc_trim)");
}

#endif
