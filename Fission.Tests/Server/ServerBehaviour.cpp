// Exercise routing, validation, decompilation, captures, and status mapping without socket I/O.

#include "../Decompiler/IntegrationTestSupport.hpp"
#include "Server.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#include <boost/json.hpp>
#pragma clang diagnostic pop

#include "Luau/Common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

namespace {
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

    // Compile a Luau snippet to vanilla bytecode (the wire format the server's "vanilla" mode expects).
    std::string CompileVanilla(const std::string &source) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        return Luau::compile(source, opts);
    }

    // Route+handle one request with the default config, silencing the decompiler's stdout echo so a
    // successful decompile does not spam the test log.
    Fission::Server::HttpResult Handle(const char *method, const char *target, const std::string &body) {
        Fission::Server::ServerConfig config{};
        std::ostringstream sink;
        std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
        auto result = Fission::Server::HandleRequest(method, target, body, config);
        std::cout.rdbuf(coutBuf);
        return result;
    }

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    // a valid /decompile body for `vanilla` mode wrapping the given base64 bytecode plus optional extra fields.
    std::string DecompileBody(const std::string &b64, const std::string &extraFields = "") {
        return "{\"mode\":\"vanilla\",\"bytecode\":\"" + b64 + "\"" + extraFields + "}";
    }

    // a representative snippet exercising a function, a branch and a return.
    const char *kSnippet = "local function add(a, b)\n"
                           "    if a > b then\n"
                           "        return a + b\n"
                           "    end\n"
                           "    return b\n"
                           "end\n"
                           "print(add(1, 2), add(4, 3), 'quoted \\\"text\\\"')\n"
                           "return add(1, 2)\n";

    void
    CheckResponse(const Fission::Server::HttpResult &response, const std::string &bytecode, const Luau::CompileOptions &options = Luau::CompileOptions{1, 2}) {
        INFO(response.body);
        REQUIRE(response.status == 200);
        REQUIRE(response.contentType == "application/json");
        const auto json = boost::json::parse(response.body);
        REQUIRE(json.at("ok").as_bool());
        const auto &result = json.at("result");
        REQUIRE(result.at("resultCode").as_string() == "Success");
        const auto &source = result.at("decompilationOutput").as_string();
        integration_test::CheckOutput(bytecode, std::string(source.data(), source.size()), options);
    }
} // namespace

// Routing

TEST_CASE("Server: GET / returns the service descriptor", "[Server][Routing]") {
    const auto res = Handle("GET", "/", "");
    CHECK(res.status == 200);
    CHECK(res.contentType == "application/json");
    CHECK(Contains(res.body, "Fission.Server"));
    CHECK(Contains(res.body, "decompile"));
}

TEST_CASE("Server: GET /health reports healthy", "[Server][Routing]") {
    const auto res = Handle("GET", "/health", "");
    CHECK(res.status == 200);
    CHECK(Contains(res.body, "\"ok\":true"));
    CHECK(Contains(res.body, "healthy"));
}

TEST_CASE("Server: an unknown route is 404", "[Server][Routing]") {
    const auto res = Handle("GET", "/does-not-exist", "");
    CHECK(res.status == 404);
    CHECK(Contains(res.body, "Not found"));
}

TEST_CASE("Server: a known path with the wrong method is 404", "[Server][Routing]") {
    // /decompile is POST-only; a GET must not be routed to the decompile handler.
    const auto res = Handle("GET", "/decompile", "");
    CHECK(res.status == 404);
}

// Request validation

TEST_CASE("Server: malformed JSON body is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "this is not json");
    CHECK(res.status == 400);
    CHECK(Contains(res.body, "Invalid JSON"));
}

TEST_CASE("Server: a non-object JSON body is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "123");
    CHECK(res.status == 400);
}

TEST_CASE("Server: missing 'mode' is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "{\"bytecode\":\"AAAA\"}");
    CHECK(res.status == 400);
    CHECK(Contains(res.body, "mode"));
}

TEST_CASE("Server: an unsupported mode is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "{\"mode\":\"banana\",\"bytecode\":\"AAAA\"}");
    CHECK(res.status == 400);
    CHECK(Contains(res.body, "Invalid mode"));
}

TEST_CASE("Server: missing 'bytecode' is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "{\"mode\":\"vanilla\"}");
    CHECK(res.status == 400);
    CHECK(Contains(res.body, "bytecode"));
}

TEST_CASE("Server: invalid base64 bytecode is rejected", "[Server][Validation]") {
    const auto res = Handle("POST", "/decompile", "{\"mode\":\"vanilla\",\"bytecode\":\"@@@@not-base64\"}");
    CHECK(res.status == 400);
    CHECK(Contains(res.body, "base64"));
}

// Decompilation

TEST_CASE("Server: valid vanilla bytecode decompiles successfully", "[Server][Decompile][Integration]") {
    EnableLuauFFlagsOnce();
    for (int optimization : {0, 1, 2}) {
        for (int debug : {0, 2}) {
            const Luau::CompileOptions options{optimization, debug};
            const auto bytecode = Luau::compile(kSnippet, options);
            const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(bytecode)));
            CheckResponse(res, bytecode, options);
            const auto result = boost::json::parse(res.body).at("result").as_object();
            CHECK(result.contains("irOutput"));
            CHECK(result.contains("timingStatistics"));
            CHECK_FALSE(result.contains("cfg"));
            CHECK_FALSE(result.contains("ast"));
            CHECK_FALSE(result.contains("debugNotes"));
        }
    }
}

// A remote client must not be able to make the server touch its filesystem: the flags that write
// ir_out.txt / cfg.dot (WriteIRToFile / GenerateIRGraph / GenerateSSAIRGraph) and the stdout-noise
// flags are stripped server-side. Requesting writeIRToFile must still decompile but leave no file.
TEST_CASE("Server: filesystem-writing flags are stripped from a request", "[Server][Decompile][Security][Integration]") {
    namespace fs = std::filesystem;
    const fs::path irFile = fs::current_path() / "ir_out.txt";
    std::error_code rmec;
    fs::remove(irFile, rmec); // clear any stale artifact first

    const auto bytecode = CompileVanilla(kSnippet);
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(bytecode), ",\"flags\":{\"writeIRToFile\":true,\"generateIRGraph\":true}"));
    CheckResponse(res, bytecode);
    // the write flag was masked, so no ir_out.txt should have appeared in the server's CWD.
    CHECK_FALSE(fs::exists(irFile));
    fs::remove(irFile, rmec);
}

// A missing or non-positive timeout must reset the (thread_local, reused) decompiler budget to the
// server default rather than inherit a prior request's larger budget. Both are accepted and succeed.
TEST_CASE("Server: an invalid or absent timeout still decompiles under the default budget", "[Server][Decompile][Integration]") {
    const auto bytecode = CompileVanilla(kSnippet);
    const std::string b64 = Base64Encode(bytecode);
    const auto neg = Handle("POST", "/decompile", DecompileBody(b64, ",\"timeout\":-5"));
    CheckResponse(neg, bytecode);
    const auto absent = Handle("POST", "/decompile", DecompileBody(b64));
    CheckResponse(absent, bytecode);
}

TEST_CASE("Server: outputs=[cfg,ast] add the CFG graph and AST tree", "[Server][Decompile][Outputs][Integration]") {
    const auto bytecode = CompileVanilla(kSnippet);
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(bytecode), ",\"outputs\":[\"cfg\",\"ast\"]"));
    CheckResponse(res, bytecode);
    // cfg is a Graphviz DOT string; ast is embedded as a real JSON tree rooted at a Root node.
    CHECK(Contains(res.body, "\"cfg\":\"digraph"));
    CHECK(Contains(res.body, "\"ast\":{\"kind\":\"Root\""));
    CHECK(Contains(res.body, "\"nodeKind\":\"IfStatement\""));
}

TEST_CASE("Server: outputs=[debug] adds bounded decompiler notes", "[Server][Decompile][Outputs][Integration]") {
    const auto bytecode = CompileVanilla(kSnippet);
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(bytecode), ",\"outputs\":[\"debug\"]"));
    CheckResponse(res, bytecode);
    CHECK(Contains(res.body, "\"debugNotes\":\"[Pipeline]"));
    CHECK(Contains(res.body, "[CFA]"));
    CHECK(Contains(res.body, "[SSA]"));
    CHECK(Contains(res.body, "[AST]"));
}

TEST_CASE("Server: undeserializable bytecode maps to a 4xx/5xx error, not a crash", "[Server][Decompile]") {
    // valid base64 but not a real Luau chunk -> the deserializer rejects it. The server must answer
    // with an error status and an ok:false body, never take the process down.
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(std::string("\x00\x00\x00\x00", 4))));
    INFO(res.body);
    CHECK((res.status == 422 || res.status == 500));
    CHECK(Contains(res.body, "\"ok\":false"));
}

TEST_CASE("Server: Luau script errors include compiler diagnostic", "[Server][Decompile]") {
    const std::string diagnostic = ":1: Incomplete statement: expected assignment or a function call";
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(std::string(1, '\0') + diagnostic), ",\"outputs\":[\"debug\"]"));
    INFO(res.body);
    CHECK(res.status == 422);
    CHECK(Contains(res.body, "Nothing to decompile, bytecode is a script error: " + diagnostic));
    CHECK(Contains(res.body, "\"debugNotes\":\"[Pipeline]"));
    CHECK(Contains(res.body, "compiler input contains an error diagnostic instead of bytecode"));
}

TEST_CASE("Server: a per-request timeout is accepted and clamped to the server max", "[Server][Decompile][Integration]") {
    // Verify that timeout parsing and clamping preserve a successful request.
    const auto bytecode = CompileVanilla(kSnippet);
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(bytecode), ",\"timeout\":5"));
    CheckResponse(res, bytecode);
}
