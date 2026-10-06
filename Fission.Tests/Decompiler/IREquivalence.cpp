// Exercise reconstructed programs and public capture contracts.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "IntegrationTestSupport.hpp"
#include "Luau/Common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#include <boost/json.hpp>
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
    // Same compile knobs the decompiler's test entry uses internally, so IR lifted here is
    // directly comparable to the IR the decompiler consumed (optimization level changes opcodes).
    constexpr int kOptLevel = 1;
    constexpr int kDebugLevel = 2;

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    // Luau::compile encodes a compile/parse error as bytecode whose first byte is 0.
    bool LuauCompiles(const std::string &source, std::string *bytecodeOut = nullptr) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = kOptLevel;
        opts.debugLevel = kDebugLevel;
        std::string bc = Luau::compile(source, opts);
        if (bytecodeOut)
            *bytecodeOut = bc;
        return !bc.empty() && bc[0] != '\0';
    }

    std::string Decompile(
        const std::string &source, DecompileResult &code, const std::string &driver = "", const std::string &environment = "math.randomseed(0); t = {1, 2, 3}"
    ) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        // suppress the generator's stdout echo so a snippet does not spam the test log.
        std::ostringstream sink;
        std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOptLevel, kDebugLevel});
        std::cout.rdbuf(coutBuf);
        code = result.resultCode;
        REQUIRE(code == DecompileResult::Success);
        integration_test::CheckSource(source, result.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel}, driver, environment);
        return result.decompilationOutput;
    }

    // Floor: decompiles, and the produced source is valid Luau.
    std::string
    RequireValidRoundtrip(const std::string &source, const std::string &driver = "", const std::string &environment = "math.randomseed(0); t = {1, 2, 3}") {
        DecompileResult code{};
        const std::string out = Decompile(source, code, driver, environment);
        INFO("decompiled source:\n" << out);
        REQUIRE(code == DecompileResult::Success);
        REQUIRE(LuauCompiles(out));
        return out;
    }

    // Split `source` into lines for the structural malformation checks below.
    std::vector<std::string> Lines(const std::string &source) {
        std::vector<std::string> lines;
        std::istringstream in(source);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        return lines;
    }

    // True when a decompiler-generated local `vN` is referenced on a line *before* its own
    // `local vN` declaration; the forward-reference malformation that silently reads a nil global
    // (e.g. a table literal `{ ..., [k] = vN }` where `local vN = ...` is emitted afterwards).
    bool UsesGeneratedLocalBeforeDeclared(const std::string &source) {
        const auto lines = Lines(source);
        const std::regex declRe(R"(\blocal\s+(v\d+)\b)");
        std::map<std::string, size_t> firstDecl;
        for (size_t i = 0; i < lines.size(); ++i)
            for (auto it = std::sregex_iterator(lines[i].begin(), lines[i].end(), declRe); it != std::sregex_iterator(); ++it) {
                const std::string name = (*it)[1];
                if (!firstDecl.count(name))
                    firstDecl[name] = i;
            }
        for (const auto &[name, declIdx] : firstDecl) {
            const std::regex useRe("\\b" + name + "\\b");
            for (size_t i = 0; i < declIdx; ++i)
                if (std::regex_search(lines[i], useRe))
                    return true;
        }
        return false;
    }

    // Count of `local vN` declarations sharing the same name; >1 means a reused register leaked as
    // a redeclared/shadowed local instead of being inlined at its single use site.
    int MaxGeneratedLocalRedeclarations(const std::string &source) {
        const std::regex declRe(R"(\blocal\s+(v\d+)\b)");
        std::map<std::string, int> counts;
        for (auto it = std::sregex_iterator(source.begin(), source.end(), declRe); it != std::sregex_iterator(); ++it)
            counts[(*it)[1]]++;
        int worst = 0;
        for (const auto &[name, c] : counts)
            worst = c > worst ? c : worst;
        return worst;
    }

    // Decompile with the API-consumer captures (CFG DOT + AST JSON) turned on; returns the full result.
    DecompilationResult DecompileWithCaptures(const std::string &source, const std::string &driver = "") {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        std::ostringstream sink;
        std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
        auto result = decompiler.DecompileTestCode(
            source, DecompilerFlags::CaptureAST | DecompilerFlags::CaptureCFGGraph | DecompilerFlags::FissionDebugNotes,
            Luau::CompileOptions{kOptLevel, kDebugLevel}
        );
        std::cout.rdbuf(coutBuf);
        REQUIRE(result.resultCode == DecompileResult::Success);
        integration_test::CheckSource(source, result.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel}, driver);
        return result;
    }

    bool IsWellFormedJson(const std::string &json) {
        boost::system::error_code error;
        (void)boost::json::parse(json, error);
        return !error;
    }

    // Decompile already-compiled, non-Roblox (identity-decoder) Luau bytecode.
    std::string DecompileVanillaOrFail(const std::string &bytecode) {
        Decompiler decompiler{};
        auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
        integration_test::CheckOutput(bytecode, result.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel}, fuzz::kSemPreludes[3]);
        return std::move(result.decompilationOutput);
    }

    // Hand-assemble the bytecode for
    //   local a = { ["data"] = ({ ["field"] = "a-b"[, ["n"] = 383] }).field }
    //   local b = (20.5)["end"][true]
    //   return a, b
    // The Luau 0.729 compiler constant-folds `({ ["field"] = "a-b" }).field` down to the literal "a-b"
    // (the inner table never reaches bytecode), so this shape can no longer be produced from source. We
    // emit the exact pre-fold opcodes directly to keep guarding the forward-reference/register-reuse
    // defect: the inner table (R1) is read into the outer constructor, then R1 is reused for `b`.
    std::string BuildNestedTableForwardRefBytecode(bool withExtraField) {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        // strings must outlive finalize(): BytecodeBuilder keeps the refs, not copies.
        const std::string sField = "field", sData = "data", sAB = "a-b", sN = "n", sEnd = "end";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };
        const auto hashOf = [&](const std::string &s) { return static_cast<uint8_t>(Luau::BytecodeBuilder::getStringHash(sref(s))); };

        const int32_t cField = bb.addConstantString(sref(sField));
        const int32_t cData = bb.addConstantString(sref(sData));
        const int32_t cAB = bb.addConstantString(sref(sAB));
        const int32_t cEnd = bb.addConstantString(sref(sEnd));
        const int32_t cN = withExtraField ? bb.addConstantString(sref(sN)) : -1;
        const int32_t cNum = bb.addConstantNumber(20.5);
        const int32_t c383 = withExtraField ? bb.addConstantInteger(383) : -1;

        bb.beginFunction(0, /*isvararg*/ true);
        bb.emitABC(LOP_PREPVARARGS, 0, 0, 0);

        bb.emitABC(LOP_NEWTABLE, 0, 0, 0); // R0 = {} (outer `a`)
        bb.emitAux(0);
        bb.emitABC(LOP_NEWTABLE, 1, 0, 0); // R1 = {} (inner)
        bb.emitAux(0);

        bb.emitAD(LOP_LOADK, 2, static_cast<int16_t>(cAB)); // R2 = "a-b"
        bb.emitABC(LOP_SETTABLEKS, 2, 1, hashOf(sField));   // R1.field = R2
        bb.emitAux(static_cast<uint32_t>(cField));
        if (withExtraField) {
            bb.emitAD(LOP_LOADK, 2, static_cast<int16_t>(c383)); // R2 = 383
            bb.emitABC(LOP_SETTABLEKS, 2, 1, hashOf(sN));        // R1.n = R2
            bb.emitAux(static_cast<uint32_t>(cN));
        }

        bb.emitABC(LOP_GETTABLEKS, 2, 1, hashOf(sField)); // R2 = R1.field
        bb.emitAux(static_cast<uint32_t>(cField));
        bb.emitABC(LOP_SETTABLEKS, 2, 0, hashOf(sData)); // R0.data = R2
        bb.emitAux(static_cast<uint32_t>(cData));

        // reuse R1 (the inner table's register) for `b = (20.5)["end"][true]`.
        bb.emitAD(LOP_LOADK, 1, static_cast<int16_t>(cNum)); // R1 = 20.5
        bb.emitABC(LOP_GETTABLEKS, 1, 1, hashOf(sEnd));      // R1 = R1["end"]
        bb.emitAux(static_cast<uint32_t>(cEnd));
        bb.emitABC(LOP_LOADB, 2, 1, 0);    // R2 = true
        bb.emitABC(LOP_GETTABLE, 1, 1, 2); // R1 = R1[R2]

        bb.emitABC(LOP_RETURN, 0, 3, 0); // return R0, R1

        bb.endFunction(/*maxstacksize*/ 3, /*numupvalues*/ 0);
        bb.setMainFunction(0);
        bb.finalize();
        return bb.getBytecode();
    }
} // namespace

// Lossless IR fixpoint

TEST_CASE("Integration: integer arithmetic preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a + b * 2 - 1 end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3,4),f(-2,5),f(0,-1)))DRIVER"
    );
}

TEST_CASE("Semantic oracle preserves observable values and reports incomplete execution", "[Fuzz][SemanticOracle]") {
    EnableLuauFFlagsOnce();
    const auto compare = [](const std::string &original, const std::string &decompiled) {
        std::string originalBc, decompiledBc, preludeBc;
        REQUIRE(LuauCompiles(original, &originalBc));
        REQUIRE(LuauCompiles(decompiled, &decompiledBc));
        REQUIRE(LuauCompiles("", &preludeBc));
        return fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc}).kind;
    };
    using Kind = fuzz::SemVerdict::Kind;

    SECTION("sparse tables do not depend on array storage layout") {
        CHECK(compare("return {true, nil, 'hello', x=false}", "local t = {true, x=false}; t[3] = 'hello'; return t") == Kind::Match);
    }
    SECTION("numeric string keys remain distinct") {
        CHECK(compare("return {[1]='x', ['1']='y'}", "return {[1]='x', ['1']='z'}") == Kind::Diverge);
        CHECK(compare("return {[1.5]='x'}", "return {[1.5]='y'}") == Kind::Diverge);
    }
    SECTION("string contents cannot impersonate argument separators") { CHECK(compare(R"(return 'a"\t"b')", R"(return 'a', 'b')") == Kind::Diverge); }
    SECTION("nearby doubles and buffer contents stay distinct") {
        CHECK(compare("return 1.000000000000001", "return 1") == Kind::Diverge);
        CHECK(compare("return buffer.fromstring('a')", "return buffer.fromstring('b')") == Kind::Diverge);
    }
    SECTION("matching early errors and opaque returns are unchecked") {
        CHECK(compare("error('stop')", "error('stop'); print('unreachable')") == Kind::Unrunnable);
        CHECK(compare("return function() return 1 end", "return function() return 2 end") == Kind::Unrunnable);
        CHECK(compare("return {{{{{1}}}}}", "return {{{{{2}}}}}") == Kind::Unrunnable);
        CHECK(compare("local t={}; return {a=t,b=t}", "return {a={},b={}}") == Kind::Unrunnable);
        CHECK(compare("return setmetatable({}, {__index=function() return 1 end})", "return {}") == Kind::Unrunnable);
        CHECK(compare("local b=buffer.fromstring('x'); return b,b", "return buffer.fromstring('x'),buffer.fromstring('x')") == Kind::Unrunnable);
        CHECK(compare("error(tostring({}))", "error(tostring({}))") == Kind::Unrunnable);
    }
    SECTION("unchecked verdict records why execution could not establish parity") {
        const auto prelude = Luau::compile("");
        const auto errors = fuzz::CompareSemantics(Luau::compile("error('stop')"), Luau::compile("error('stop')"), {prelude});
        CHECK(errors.kind == Kind::Unrunnable);
        CHECK(errors.equalErrors == 1);
        CHECK(errors.successful == 0);
        const auto opaque = fuzz::CompareSemantics(Luau::compile("return function() end"), Luau::compile("return function() end"), {prelude});
        CHECK(opaque.kind == Kind::Unrunnable);
        CHECK(opaque.opaque == 1);
        CHECK(opaque.successful == 0);
    }
    SECTION("error differences remain findings") {
        CHECK(compare("error('one')", "error('two')") == Kind::Diverge);
        CHECK(compare("error('FUZZ_TIMEOUT')", "return 1") == Kind::Diverge);
    }
    SECTION("opaque values do not hide separate observable effects") {
        CHECK(compare("print('a'); return function() end", "print('b'); return function() end") == Kind::Diverge);
        CHECK(compare("print(function() end); print('a')", "print(function() end); print('b')") == Kind::Diverge);
        CHECK(compare("print(function() end); error('one')", "print(function() end); error('two')") == Kind::Diverge);
    }
    SECTION("fixture compilation failure cannot silently reduce coverage") {
        size_t compiled = 0;
        const auto preludes = fuzz::CompilePreludes([&](const std::string &source, std::string *out) {
            *out = Luau::compile(++compiled == 2 ? "local =" : source);
            return !out->empty() && out->front() != '\0';
        });
        CHECK(preludes.empty());
    }
}

TEST_CASE("Integration: constant folding is stable across decompilation", "[Decompiler][Integration]") { integration_test::Check(R"LUA(return 1 + 2 * 3)LUA"); }

TEST_CASE("Nested repeat and while sharing a header preserve execution order", "[Decompiler][Regression][Fuzz][SemanticLoops]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"(
        local trace = {}
        local n = 0
        repeat
            while n < 2 do
                local function first() return 1 end
                n += first()
                trace[#trace + 1] = n
                if n == 1 then break end
            end
            local function second() return 2 end
            trace[#trace + 1] = second()
        until n >= 2
        return trace
    )";
    Decompiler decompiler{};
    const auto output = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOptLevel, kDebugLevel});
    REQUIRE(output.resultCode == DecompileResult::Success);
    INFO(output.decompilationOutput);
    std::string originalBc, outputBc, preludeBc;
    REQUIRE(LuauCompiles(source, &originalBc));
    REQUIRE(LuauCompiles(output.decompilationOutput, &outputBc));
    REQUIRE(LuauCompiles("", &preludeBc));
    const auto verdict = fuzz::CompareSemantics(originalBc, outputBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Integration: string concatenation preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a .. "-" .. b end)LUA", R"DRIVER(local f = __integration_subject(); print(f("a","b"),f("","z"),f("x","")))DRIVER"
    );
}

TEST_CASE("Integration: array table literal preserves execution", "[Decompiler][Integration]") { integration_test::Check(R"LUA(return { 1, 2, 3, 4, 5 })LUA"); }

TEST_CASE("Integration: length and unary minus preserve execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(t, n) return #t + (-n) end)LUA", R"DRIVER(local f = __integration_subject(); print(f({1,2},4),f({},-3)))DRIVER"
    );
}

TEST_CASE("Integration: builtin fastcall preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b, c) return math.max(a, math.min(b, c)) end)LUA",
        R"DRIVER(local f = __integration_subject(); print(f(1,2,3),f(4,2,3),f(-4,-2,-3)))DRIVER"
    );
}

TEST_CASE("Integration: field get/set preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(t) t.x = t.y + t.z return t.x end)LUA",
        R"DRIVER(local f = __integration_subject(); local t={x=0,y=2,z=3}; print(f(t),t.x); print(f({y=-4,z=7})))DRIVER"
    );
}

TEST_CASE("Integration: method call preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(s) return s:upper() end)LUA", R"DRIVER(local f = __integration_subject(); print(f("abC"),f("")))DRIVER");
}

TEST_CASE("Integration: nested closure preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(local function add(a, b) return a + b end return add(1, 2))LUA");
}

TEST_CASE("Integration: vararg forwarding preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(...) return select("#", ...) end)LUA", R"DRIVER(local f = __integration_subject(); print(f(),f(1),f(1,nil,3)))DRIVER"
    );
}

TEST_CASE("Integration: multiple return values preserve execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a, b, a + b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3,4)); print(f(-1,2)))DRIVER"
    );
}

TEST_CASE("Integration: equality comparison as a value preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a == b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2,2),f(2,3),f(nil,false),f(false,false)))DRIVER"
    );
}

TEST_CASE("Integration: relational comparison as a value preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a < b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2,2),f(2,3),f(3,2),f(0/0,2)))DRIVER"
    );
}

TEST_CASE("Integration: logical not preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a) return not a end)LUA", R"DRIVER(local f = __integration_subject(); print(f(nil),f(false),f(0),f(""),f(true)))DRIVER"
    );
}

TEST_CASE("Integration: numeric index get preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(t) return t[1] + t[2] end)LUA", R"DRIVER(local f = __integration_subject(); print(f({3,4}),f({-2,8})))DRIVER"
    );
}

TEST_CASE("Integration: global call preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(x) print(x) return x end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3)); print(f("x")))DRIVER"
    );
}

TEST_CASE("Integration: mixed array/hash table literal preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return { 1, 2, x = 3, y = 4 })LUA");
}

TEST_CASE("Integration: string.format fastcall preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(n) return string.format("%d", n) end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3),f(-7),f(0)))DRIVER"
    );
}

TEST_CASE("Integration: multiple locals feeding an expression preserve execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b, c) local x = a + b local y = b + c return x * y end)LUA",
        R"DRIVER(local f = __integration_subject(); print(f(1,2,3),f(-3,4,0)))DRIVER"
    );
}

TEST_CASE("Integration: floor division (reg/reg) preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a // b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(7,2),f(-7,2),f(7,-2)))DRIVER"
    );
}

TEST_CASE("Integration: floor division by constant preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(a) return a // 2 end)LUA", R"DRIVER(local f = __integration_subject(); print(f(7),f(-7),f(0)))DRIVER");
}

TEST_CASE("Integration: constant-minus-register preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(a) return 10 - a end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2),f(-3),f(10)))DRIVER");
}

TEST_CASE("Integration: constant-over-register preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(a) return 10 / a end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2),f(-4),f(0)))DRIVER");
}

TEST_CASE("Integration: modulo preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a % b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(7,3),f(-7,3),f(7,-3)))DRIVER"
    );
}

TEST_CASE("Integration: power preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a ^ b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2,3),f(-2,4),f(3,0)))DRIVER"
    );
}

TEST_CASE("Integration: negative constant base of a power keeps its parentheses", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a) return (-401) ^ a, (-1.5) ^ a end)LUA",
        R"DRIVER(local f = __integration_subject(); print(f(1)); print(f(2)); print(f(0.5)))DRIVER"
    );
}

TEST_CASE("Integration: not-equal comparison preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) return a ~= b end)LUA", R"DRIVER(local f = __integration_subject(); print(f(2,2),f(2,3),f(nil,false)))DRIVER"
    );
}

TEST_CASE("Integration: non-constant upvalue capture preserves execution", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(n) local x = n + 1 return function() return x end end)LUA",
        R"DRIVER(local f = __integration_subject(); local a,b=f(3),f(7); print(a(),b(),a()))DRIVER"
    );
}

TEST_CASE("Integration: table literal with self-referential element does not forward-reference", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a, b) local t = { a, b } t[3] = t[1] + t[2] return t end)LUA",
        R"DRIVER(local f = __integration_subject(); print(f(3,4)[3],f(-2,5)[3]))DRIVER"
    );
}

TEST_CASE("Integration: self-referential method-call table element stays a post-declaration statement", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(a) local t = { a[1], a[2] } t[3] = t[1]:Cross(t[2]) return t end)LUA",
        R"DRIVER(local f = __integration_subject(); local x={n=2,Cross=function(self,other) return self.n*10+other.n end}; local y={n=3}; print(f({x,y})[3]))DRIVER"
    );
}

TEST_CASE("Integration: nested array-table value in a keyed literal does not forward-reference", "[Decompiler][Integration]") {
    integration_test::Check(
        R"LUA(return function(v) return { [v[1]] = { v[3], v[2], v[5] } } end)LUA",
        R"DRIVER(local f = __integration_subject(); local t=f({"key",2,3,4,5}); print(t.key[1],t.key[2],t.key[3]))DRIVER"
    );
}

TEST_CASE("IR: single-use method-call result inlines into a field read (no shadowed temp)", "[Decompiler][IREquivalence]") {
    // RotatedRegion3 getAxis: `t[4] = t[1]:Cross(t[2]).unit` must inline the call into the `.unit`
    // read, not emit a `local vN = t[1]:Cross(...)` reused (shadowed) across each sibling.
    integration_test::Check(
        "return function(a) local t = { a[1], a[2], a[3] } "
        "t[4] = t[1]:Cross(t[2]).unit t[5] = t[1]:Cross(t[3]).unit t[6] = t[2]:Cross(t[3]).unit return t end",
        "local f = __integration_subject(); local function v(n) return {n=n,Cross=function(self,other) print('cross',self.n,other.n); "
        "return {unit=self.n*10+other.n} end} end; local t=f({v(1),v(2),v(3)}); print(t[4],t[5],t[6])"
    );
    DecompileResult code{};
    const std::string out = Decompile(
        "return function(a) local t = { a[1], a[2], a[3] } "
        "t[4] = t[1]:Cross(t[2]).unit t[5] = t[1]:Cross(t[3]).unit t[6] = t[2]:Cross(t[3]).unit return t end",
        code,
        "local f = __integration_subject(); local function v(n) return {n=n,Cross=function(self,other) print('cross',self.n,other.n); "
        "return {unit=self.n*10+other.n} end} end; local t=f({v(1),v(2),v(3)}); print(t[4],t[5],t[6])"
    );
    INFO(out);
    CHECK(out.find(":Cross(") != std::string::npos); // the method call survived
    CHECK(out.find(").unit") != std::string::npos);  // inlined as `...).unit`, not a shadowed `local vN = ...:Cross`
}

// RotatedRegion3 regressions

TEST_CASE("Regress: self-referential table element is never forward-referenced", "[Decompiler][Regression][TableForwardRef]") {
    DecompileResult code{};
    const std::string out = Decompile(
        "return function(a, b) local t = { a, b } t[3] = t[1] + t[2] return t end", code, "local f = __integration_subject(); print(f(2,3)[3],f(-4,7)[3])"
    );
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out));
    CHECK(LuauCompiles(out));
}

TEST_CASE("Regress: getAxis cross-product chain has no forward-ref and no shadowed temp", "[Decompiler][Regression][TableForwardRef]") {
    DecompileResult code{};
    const std::string out = Decompile(
        "return function(a) local t = { a[1], a[2], a[3] } "
        "t[4] = t[1]:Cross(t[2]).unit t[5] = t[1]:Cross(t[3]).unit t[6] = t[2]:Cross(t[3]).unit return t end",
        code,
        "local f = __integration_subject(); local function v(n) return {n=n,Cross=function(self,other) print('cross',self.n,other.n); "
        "return {unit=self.n*10+other.n} end} end; local t=f({v(1),v(2),v(3)}); print(t[4],t[5],t[6])"
    );
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out)); // no `[k] = vN` before `local vN`
    CHECK(MaxGeneratedLocalRedeclarations(out) <= 1);   // the reused cross temp must not redeclare
    CHECK(LuauCompiles(out));
}

TEST_CASE("Regress: nested array element in a keyed literal is never forward-referenced", "[Decompiler][Regression][TableForwardRef]") {
    DecompileResult code{};
    const std::string out = Decompile(
        "return function(v) return { [v[1]] = { v[3], v[2], v[5] } } end", code,
        "local f = __integration_subject(); local t=f({'key',2,3,4,5}); print(t.key[1],t.key[2],t.key[3])"
    );
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out));
    CHECK(LuauCompiles(out));
}

// A nested table whose value is read into an outer table constructor must not be folded as a forward
// reference: its register is reused later, so it is a real local that must be declared before the outer
// constructor uses it; never `{ data = v.field }` with `v` declared afterwards (or dropped entirely).
// Hand-assembled: the 0.729 compiler folds `({ ["field"] = "a-b" }).field` to a bare constant, so the
// inner table no longer reaches bytecode from source (see helper).
TEST_CASE("Regress: nested table value is not forward-referenced in a constructor", "[Decompiler][Regression][TableForwardRef]") {
    const std::string out = DecompileVanillaOrFail(BuildNestedTableForwardRefBytecode(/*withExtraField*/ false));
    INFO(out);
    // The inner table survives (it was being dropped).
    CHECK(std::regex_search(out, std::regex(R"(field\s*=\s*"a-b")")));
    // It is NOT folded into the outer constructor as `{ data = <name>.field }` (a forward reference).
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\{[^{}]*\bdata\s*=\s*\w+\.field)")));
    CHECK(LuauCompiles(out));
}

// The same defect, reduced from a fuzz finding: a keyed value reading a reused-register nested table must
// stay a declared-before-use local.
TEST_CASE("Regress: reused-register nested table keeps a sound declaration order", "[Decompiler][Regression][TableForwardRef]") {
    const std::string out = DecompileVanillaOrFail(BuildNestedTableForwardRefBytecode(/*withExtraField*/ true));
    INFO(out);
    CHECK(std::regex_search(out, std::regex(R"(field\s*=\s*"a-b")")));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\{[^{}]*\bdata\s*=\s*\w+\.field)")));
    CHECK(LuauCompiles(out));
}

TEST_CASE("Regress: escaping reused local is not hidden in a synthetic do block", "[Decompiler][Regression][Scope]") {
    DecompileResult code{};
    const std::string out = Decompile(
        "local v0 = next[true] local v0 = v0:set(\"key\", false) return v0.field", code, "print(__integration_subject())",
        "next = {[true] = {set=function(self,key,value) print(key,value); return {field=7} end}}"
    );
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK(LuauCompiles(out));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\bdo\s*\n\s*local\s+v0\b)")));
}

// Fuzz regressions

TEST_CASE("Regress: double unary minus does not collapse into a comment", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(x) return - -x end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3),f(-4),f(0)))DRIVER");
}

TEST_CASE("Regress: double unary minus inside a table element stays separated", "[Decompiler][Integration]") {
    integration_test::Check(R"LUA(return function(x) return { 1, - -x } end)LUA", R"DRIVER(local f = __integration_subject(); print(f(3)[2],f(-4)[2]))DRIVER");
}

TEST_CASE("Regress: a parenthesised first statement does not get a leading semicolon", "[Decompiler][Regression][Fuzz]") {
    // luau has no empty statement, so a leading `;` is a syntax error. When the chunk's first
    // statement is a call whose callee needs parentheses (`(true)(x)`), the disambiguating `;`
    // must be suppressed (the header comment is not a statement it can terminate).
    // (found by Fission.Fuzzing; the constant `true` inlines into the call -> `(true)(...)`.)
    RequireValidRoundtrip("local a = true a(nil) return 1", "print(pcall(__integration_subject))");
    RequireValidRoundtrip("local n = -(-5) n() return 0", "print(pcall(__integration_subject))");
    // same bug inside a nested block: an info-comment precedes the first real statement, so the
    // block's first paren-call must still suppress the `;`. (the expanded fuzzer surfaced this in
    // function and loop bodies; Visit(BlockStatementNode) keeps firstStmt true across comments.)
    RequireValidRoundtrip("local a = true local function f() a(nil) return 1 end return f", "print(pcall(__integration_subject()))");
    RequireValidRoundtrip("local a = true for i = 1, 3 do a(i) end return 0", "print(pcall(__integration_subject))");
}

TEST_CASE("Regress: deeply-nested table literal does not hang source-gen", "[Decompiler][Regression][Fuzz]") {
    // Nested single-element tables must remain bounded and produce valid Luau.
    std::string deep = "local x = ";
    for (int i = 0; i < 150; ++i)
        deep += "{";
    deep += "1";
    for (int i = 0; i < 150; ++i)
        deep += "}";
    deep += " return x";
    RequireValidRoundtrip(deep, "local t=__integration_subject(); local depth=0; while type(t)=='table' do depth+=1; t=t[1] end print(depth,t)");
}

TEST_CASE("Regress: string-constant left operand in a table element is not a computed key", "[Decompiler][Regression][Fuzz]") {
    // a table array element `"hi" * x` (binop with a string-literal left) must render as
    // `"hi" * x`, not the keyed-entry shape `["hi"] * x`; the latter is an unparseable
    // computed key with no `=`. Recompiling the output catches the malformation.
    RequireValidRoundtrip("return function(x) return { 1, \"hi\" * x } end", "print(pcall(__integration_subject(),2))");
    RequireValidRoundtrip("return function(x) return { 1, \"\" // x } end", "print(pcall(__integration_subject(),2))");
    RequireValidRoundtrip("return function(x) return { 1, \"hi\" / x } end", "print(pcall(__integration_subject(),2))");
}

TEST_CASE("Regress: reserved-word string table key stays bracketed", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. A string key that is a Luau keyword (`end`, `function`, ...) must render
    // as `[\"end\"] = v`, never the bare `end = v` (a syntax error). MakeTableKey now treats reserved
    // words as non-identifiers so the table-constructor path brackets them.
    RequireValidRoundtrip("return { [\"end\"] = 1, [\"function\"] = 2, [\"while\"] = 3, x = 4 }");
    RequireValidRoundtrip("return function(t) t[\"end\"] = 1 t[\"for\"] = 2 return t end", "local t=__integration_subject()({}); print(t['end'],t['for'])");
}

// Saved fuzz corpus

TEST_CASE("Regress: deeply-nested table does not blow up source generation", "[Decompiler][Regression][Fuzz]") {
    // Cached width measurement and depth limits bound pathological nested tables.
    constexpr int depth = 70;
    std::string src = "return ";
    for (int i = 0; i < depth; ++i)
        src += "{";
    src += "1";
    for (int i = 0; i < depth; ++i)
        src += "}";
    DecompileResult code{};
    const std::string out =
        Decompile(src, code, "local t=__integration_subject(); local depth=0; while type(t)=='table' do depth+=1; t=t[1] end print(depth,t)");
    INFO(out.substr(0, 160));
    REQUIRE(code == DecompileResult::Success);
    REQUIRE(LuauCompiles(out));
}

TEST_CASE("Regress: while-true-break as a loop's first statement keeps break inside the loop", "[Decompiler][Regression][Fuzz]") {
    // An inner loop must not make its enclosing loop spill statements to top level.
    RequireValidRoundtrip("for i = 1, 10 do while true do break end print(i) break end return 0");
    RequireValidRoundtrip("for k, v in pairs(t) do while true do break end print(v) break end return 0");
}

TEST_CASE("Regress: empty repeat-until body does not crash source generation", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing (segfault). A `repeat ... until cond` with an empty body takes the
    // "condition in the latch" path, which set the condition but never the RepeatStatementNode body :
    // a null body the source generator then dereferenced. The body is now an (empty) block.
    RequireValidRoundtrip("return function(x) repeat until x end", "local f=__integration_subject(); f(true); f(0); print('done')");
    RequireValidRoundtrip("return function(x, y) repeat until x and y end", "local f=__integration_subject(); f(true,true); f(0,''); print('done')");
}

TEST_CASE("Regress: a break in a loop nested in repeat-until does not leak outside", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. `repeat for ... do break end continue until c` mis-structured: the
    // control-flow analyzer recorded the nested for's break block as the outer repeat's exit, so the
    // break was emitted at top level (`break` outside a loop). The output must stay valid Luau.
    RequireValidRoundtrip("local x = 0 repeat for i = 1, 2 do break end x += 1 continue until x >= 2 return x");
}

TEST_CASE("Regress: generic-for loop variables are identifiers, never inlined expressions", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. Generic-for loop variables were emitted via LiftExpression, which
    // inlines a reused register's value -> `for <expr> in ...` / `for v, <expr> in ...` (syntax errors).
    // They must always render as their own identifier name (mirrors the numeric-for handling).
    RequireValidRoundtrip("return function(t) for k, v in pairs(t) do print(k, v) end end", "local f=__integration_subject(); f({3,4}); f({})");
    RequireValidRoundtrip(
        "return function(t) for a, b, c in next, t, nil do for x, y in pairs(a) do print(x) end end end",
        "local f=__integration_subject(); f({[{3,4}]=7}); f({})"
    );
}

// Guards the GETTABLEN-as-a-statement path: the numeric index must be a declared local, never a
// dangling `vN` (which would silently read a nil global instead of the table element).
TEST_CASE("Valid: reused numeric index declares a real local", "[Decompiler][IREquivalence][ValidLuau]") {
    const auto out =
        RequireValidRoundtrip("return function(t) local a = t[1] return a + a + t[2] end", "local f=__integration_subject(); print(f({3,4}),f({-4,7}))");
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out));
}

// A never-mutated constant upvalue is folded through the capture (`x=5; ()->x+1` -> `()->6`):
// behaviour-identical, but the capture opcodes vanish, so this is a floor case.
TEST_CASE("Valid: constant upvalue folds through capture", "[Decompiler][IREquivalence][ValidLuau]") {
    const auto out = RequireValidRoundtrip("local x = 5 return function() return x + 1 end", "local f=__integration_subject(); print(f(),f())");
    CHECK(out.find("return 6") != std::string::npos);
}

TEST_CASE("Deserializer rejects every truncation of valid bytecode", "[Decompiler][Deser][Hardening]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    std::ostringstream sink;
    std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());

    std::string base;
    REQUIRE(LuauCompiles("local function add(a, b) local t = { 1, 2, { 3 } } for k, v in t do add(v) end return a + b end return add(1, 2)", &base));

    const auto vanilla = [&](const std::string &bc) { return decompiler.DecompileVanillaBytecode(bc, static_cast<DecompilerFlags>(0)).resultCode; };

    for (size_t n = 0; n < base.size(); ++n)
        CHECK(vanilla(base.substr(0, n)) != DecompileResult::Success);

    std::cout.rdbuf(coutBuf);
    CHECK(vanilla(base) == DecompileResult::Success);
}

TEST_CASE("Regress: nested-table generic-for generator inlines without forward-reference", "[Decompiler][Regression][TableForwardRef]") {
    // A nested table reused as a generic-for variable must not create forward references. The
    // table coalescer must not double-count a SETLIST element's use, and the
    // SSA builder never placed header phis for FORGLOOP loop variables (so a loop variable shared its
    // version with the pre-loop table that reused its register). The output must be valid Luau with no
    // generated local used before it is declared.
    DecompileResult code{};
    const std::string out = Decompile("for _, x in ({{{{{#({})}}}}}) do if x then table.insert(x, x) end end return 0", code);
    INFO("decompiled:\n" << out);
    REQUIRE(code == DecompileResult::Success);
    REQUIRE(LuauCompiles(out));
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out));
}

// CFG and AST capture API

TEST_CASE("Capture: AST serializes to well-formed JSON and CFG to a DOT graph", "[Decompiler][Capture][AST][CFG]") {
    const std::string source = "local function add(a, b)\n"
                               "    if a > b then\n"
                               "        return a + b\n"
                               "    end\n"
                               "    return b\n"
                               "end\n"
                               "return add(1, 2)\n";
    const auto result = DecompileWithCaptures(source);
    REQUIRE(result.resultCode == DecompileResult::Success);

    // CFG captured in-memory as Graphviz DOT (no cfg.dot written to disk).
    CHECK(result.cfgGraph.find("digraph") != std::string::npos);
    CHECK(result.cfgGraph.find("<B>WHY</B>") != std::string::npos);
    CHECK(result.cfgGraph.find("partition:") != std::string::npos);
    CHECK(result.cfgGraph.find("link:") != std::string::npos);
    CHECK(result.cfgGraph.find("SSA:") != std::string::npos);
    CHECK(result.debugNotes.find("[Pipeline]") != std::string::npos);
    CHECK(result.debugNotes.find("[CFA]") != std::string::npos);
    CHECK(result.debugNotes.find("[SSA]") != std::string::npos);
    CHECK(result.debugNotes.find("[AST]") != std::string::npos);

    // AST captured as a well-formed JSON tree rooted at a Root node, carrying the expected kinds.
    INFO("AST JSON bytes: " << result.astJson.size());
    REQUIRE_FALSE(result.astJson.empty());
    CHECK(IsWellFormedJson(result.astJson));
    CHECK(result.astJson.rfind("{\"kind\":\"Root\"", 0) == 0);
    CHECK(result.astJson.find("\"FunctionDeclaration\"") != std::string::npos);
    CHECK(result.astJson.find("\"IfStatement\"") != std::string::npos);
    CHECK(result.astJson.find("\"ReturnStatement\"") != std::string::npos);
    CHECK(result.astJson.find("\"BinaryExpression\"") != std::string::npos);

    // every node carries a structural category + a coarse nodeKind, and none is left Uncategorized.
    CHECK(result.astJson.find("\"category\":\"Program\"") != std::string::npos);
    CHECK(result.astJson.find("\"category\":\"Statement\"") != std::string::npos);
    CHECK(result.astJson.find("\"category\":\"Expression\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"IfStatement\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"FunctionDeclaration\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"Unknown\"") == std::string::npos);
}

TEST_CASE("Capture: AST stays well-formed JSON across varied constructs", "[Decompiler][Capture][AST]") {
    // exercise loops, tables, method calls, varargs, strings with control bytes; the escaping and
    // every Visit override; and assert the serialized tree is still structurally balanced JSON.
    const std::string source = "return function(...)\n"
                               "    local t = { a = 1, [\"x\\ny\"] = 2, 3, 4 }\n"
                               "    for i = 1, 10 do t[i] = i end\n"
                               "    for k, v in pairs(t) do print(k, v) end\n"
                               "    local s = 0\n"
                               "    while s < 5 do s = s + 1 end\n"
                               "    repeat s = s - 1 until s == 0\n"
                               "    t:method(s, ...)\n"
                               "    return s and t or nil\n"
                               "end";
    const auto result = DecompileWithCaptures(source, "print(pcall(__integration_subject(),1,nil,3))");
    REQUIRE(result.resultCode == DecompileResult::Success);
    REQUIRE_FALSE(result.astJson.empty());
    CHECK(IsWellFormedJson(result.astJson));
    CHECK(result.astJson.rfind("{\"kind\":\"Root\"", 0) == 0);

    // method call, loops, literals and varargs each surface their appropriate nodeKind, and the
    // whole tree is fully categorized (no Unknown means every constructed node set its kind).
    CHECK(result.astJson.find("\"nodeKind\":\"MethodCallExpression\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"ForNumeric\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"LiteralValue\"") != std::string::npos);
    CHECK(result.astJson.find("\"category\":\"Literal\"") != std::string::npos);
    CHECK(result.astJson.find("\"nodeKind\":\"Unknown\"") == std::string::npos);
}

TEST_CASE("Capture: no captures unless the flags are set", "[Decompiler][Capture]") {
    // the common (flagless) decompile path must not pay for CFG/AST capture.
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    std::ostringstream sink;
    std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
    auto result = decompiler.DecompileTestCode("return 1", static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOptLevel, kDebugLevel});
    std::cout.rdbuf(coutBuf);
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.cfgGraph.empty());
    CHECK(result.astJson.empty());
    CHECK(result.debugNotes.empty());
    integration_test::CheckSource("return 1", result.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel});
}

TEST_CASE("Debug notes do not change decompiled source", "[Decompiler][DebugNotes]") {
    EnableLuauFFlagsOnce();
    const std::string source = "local x = 1\nif x > 0 then x += 2 end\nreturn x";
    Decompiler plainDecompiler{};
    Decompiler debugDecompiler{};
    const auto plain = plainDecompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOptLevel, kDebugLevel});
    const auto debug = debugDecompiler.DecompileTestCode(source, DecompilerFlags::FissionDebugNotes, Luau::CompileOptions{kOptLevel, kDebugLevel});
    REQUIRE(plain.resultCode == DecompileResult::Success);
    REQUIRE(debug.resultCode == DecompileResult::Success);
    CHECK(debug.decompilationOutput == plain.decompilationOutput);
    CHECK(plain.debugNotes.empty());
    CHECK_FALSE(debug.debugNotes.empty());
    CHECK(debug.debugNotes.size() < 65536);
    integration_test::CheckSource(source, plain.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel});
    integration_test::CheckSource(source, debug.decompilationOutput, Luau::CompileOptions{kOptLevel, kDebugLevel});
}
