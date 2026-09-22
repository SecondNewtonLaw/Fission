// Exercise routing, validation, decompilation, captures, and status mapping without socket I/O.

#include "Server.hpp"

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
        int val = 0, bits = -6;
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
                           "return add(1, 2)\n";
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

TEST_CASE("Server: valid vanilla bytecode decompiles successfully", "[Server][Decompile]") {
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(CompileVanilla(kSnippet))));
    INFO(res.body);
    CHECK(res.status == 200);
    CHECK(Contains(res.body, "\"ok\":true"));
    CHECK(Contains(res.body, "\"resultCode\":\"Success\""));
    CHECK(Contains(res.body, "\"decompilationOutput\":"));
    // source/ir/timing are always present; optional artifacts are absent unless requested.
    CHECK_FALSE(Contains(res.body, "\"cfg\":"));
    CHECK_FALSE(Contains(res.body, "\"ast\":"));
    CHECK_FALSE(Contains(res.body, "\"debugNotes\":"));
}

// A remote client must not be able to make the server touch its filesystem: the flags that write
// ir_out.txt / cfg.dot (WriteIRToFile / GenerateIRGraph / GenerateSSAIRGraph) and the stdout-noise
// flags are stripped server-side. Requesting writeIRToFile must still decompile but leave no file.
TEST_CASE("Server: filesystem-writing flags are stripped from a request", "[Server][Decompile][Security]") {
    namespace fs = std::filesystem;
    const fs::path irFile = fs::current_path() / "ir_out.txt";
    std::error_code rmec;
    fs::remove(irFile, rmec); // clear any stale artifact first

    const auto res = Handle("POST", "/decompile",
                            DecompileBody(Base64Encode(CompileVanilla(kSnippet)), ",\"flags\":{\"writeIRToFile\":true,\"generateIRGraph\":true}"));
    INFO(res.body);
    CHECK(res.status == 200);
    CHECK(Contains(res.body, "\"resultCode\":\"Success\""));
    // the write flag was masked, so no ir_out.txt should have appeared in the server's CWD.
    CHECK_FALSE(fs::exists(irFile));
    fs::remove(irFile, rmec);
}

// A missing or non-positive timeout must reset the (thread_local, reused) decompiler budget to the
// server default rather than inherit a prior request's larger budget. Both are accepted and succeed.
TEST_CASE("Server: an invalid or absent timeout still decompiles under the default budget", "[Server][Decompile]") {
    const std::string b64 = Base64Encode(CompileVanilla(kSnippet));
    const auto neg = Handle("POST", "/decompile", DecompileBody(b64, ",\"timeout\":-5"));
    CHECK(neg.status == 200);
    CHECK(Contains(neg.body, "\"resultCode\":\"Success\""));
    const auto absent = Handle("POST", "/decompile", DecompileBody(b64));
    CHECK(absent.status == 200);
    CHECK(Contains(absent.body, "\"resultCode\":\"Success\""));
}

TEST_CASE("Server: outputs=[cfg,ast] add the CFG graph and AST tree", "[Server][Decompile][Outputs]") {
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(CompileVanilla(kSnippet)), ",\"outputs\":[\"cfg\",\"ast\"]"));
    INFO(res.body);
    CHECK(res.status == 200);
    CHECK(Contains(res.body, "\"ok\":true"));
    // cfg is a Graphviz DOT string; ast is embedded as a real JSON tree rooted at a Root node.
    CHECK(Contains(res.body, "\"cfg\":\"digraph"));
    CHECK(Contains(res.body, "\"ast\":{\"kind\":\"Root\""));
    CHECK(Contains(res.body, "\"nodeKind\":\"IfStatement\""));
}

TEST_CASE("Server: outputs=[debug] adds bounded decompiler notes", "[Server][Decompile][Outputs]") {
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(CompileVanilla(kSnippet)), ",\"outputs\":[\"debug\"]"));
    INFO(res.body);
    CHECK(res.status == 200);
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
    const auto res =
        Handle("POST", "/decompile", DecompileBody(Base64Encode(std::string(1, '\0') + diagnostic), ",\"outputs\":[\"debug\"]"));
    INFO(res.body);
    CHECK(res.status == 422);
    CHECK(Contains(res.body, "Nothing to decompile, bytecode is a script error: " + diagnostic));
    CHECK(Contains(res.body, "\"debugNotes\":\"[Pipeline]"));
    CHECK(Contains(res.body, "compiler input contains an error diagnostic instead of bytecode"));
}

TEST_CASE("Server: a per-request timeout is accepted and clamped to the server max", "[Server][Decompile]") {
    // Verify that timeout parsing and clamping preserve a successful request.
    const auto res = Handle("POST", "/decompile", DecompileBody(Base64Encode(CompileVanilla(kSnippet)), ",\"timeout\":5"));
    CHECK(res.status == 200);
    CHECK(Contains(res.body, "\"resultCode\":\"Success\""));
}
