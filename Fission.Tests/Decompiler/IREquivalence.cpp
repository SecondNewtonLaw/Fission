// Require recompilable Luau, plus an IR fixpoint for constructs represented losslessly.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "BytecodeLifter.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "Luau/Common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
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

    // Compile + lift `source`, flattening every function's opcode stream depth-first
    // (main, then each nested closure in declaration order). nullopt if it does not compile.
    std::optional<std::vector<LiftedOperation>> LiftOpcodes(const std::string &source) {
        EnableLuauFFlagsOnce();
        std::string bc;
        if (!LuauCompiles(source, &bc))
            return std::nullopt;

        Deserializer deserializer{};
        auto des = deserializer.Deserialize(bc);
        if (!des || des->functions.empty())
            return std::nullopt;

        Fission::InstructionDecoder decoder{};
        BytecodeLifter lifter{&decoder};
        const LiftedFunction lifted = lifter.LiftDeserializedBytecode(*des);

        std::vector<LiftedOperation> ops;
        std::function<void(const LiftedFunction &)> walk = [&](const LiftedFunction &f) {
            for (const auto &inst : f.instructions)
                ops.push_back(inst.operation);
            for (const auto &sub : f.subfunctions)
                walk(sub);
        };
        walk(lifted);
        return ops;
    }

    std::string OpcodesToString(const std::vector<LiftedOperation> &ops) {
        std::ostringstream out;
        for (size_t i = 0; i < ops.size(); ++i) {
            if (i)
                out << ' ';
            out << OperationToString(ops[i]);
        }
        return out.str();
    }

    std::string Decompile(const std::string &source, DecompileResult &code) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        // suppress the generator's stdout echo so a snippet does not spam the test log.
        std::ostringstream sink;
        std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOptLevel, kDebugLevel});
        std::cout.rdbuf(coutBuf);
        code = result.resultCode;
        return result.decompilationOutput;
    }

    // Floor: decompiles, and the produced source is valid Luau.
    std::string RequireValidRoundtrip(const std::string &source) {
        DecompileResult code{};
        const std::string out = Decompile(source, code);
        INFO("decompiled source:\n" << out);
        REQUIRE(code == DecompileResult::Success);
        REQUIRE(LuauCompiles(out));
        return out;
    }

    // Strong: valid Luau AND re-lifting the decompiled source reproduces the exact opcode stream.
    void RequireSameIR(const std::string &source) {
        const auto before = LiftOpcodes(source);
        REQUIRE(before.has_value());

        const std::string out = RequireValidRoundtrip(source);

        const auto after = LiftOpcodes(out);
        REQUIRE(after.has_value());

        INFO("original IR : " << OpcodesToString(*before));
        INFO("relifted IR : " << OpcodesToString(*after));
        INFO("decompiled source:\n" << out);
        CHECK(*before == *after);
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
    DecompilationResult DecompileWithCaptures(const std::string &source) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        std::ostringstream sink;
        std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());
        auto result =
            decompiler.DecompileTestCode(source, DecompilerFlags::CaptureAST | DecompilerFlags::CaptureCFGGraph, Luau::CompileOptions{kOptLevel, kDebugLevel});
        std::cout.rdbuf(coutBuf);
        return result;
    }

    // String-aware structural check: every {/[ closes with the matching }/] and every "string" terminates.
    // Not a full JSON validator; enough to catch a serializer emitting unbalanced or truncated output.
    bool IsWellFormedJson(const std::string &json) {
        std::vector<char> stack;
        bool inString = false, escaped = false;
        for (char c : json) {
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (c == '\\')
                    escaped = true;
                else if (c == '"')
                    inString = false;
                continue;
            }
            if (c == '"')
                inString = true;
            else if (c == '{' || c == '[')
                stack.push_back(c);
            else if (c == '}') {
                if (stack.empty() || stack.back() != '{')
                    return false;
                stack.pop_back();
            } else if (c == ']') {
                if (stack.empty() || stack.back() != '[')
                    return false;
                stack.pop_back();
            }
        }
        return stack.empty() && !inString;
    }

    // Decompile already-compiled, non-Roblox (identity-decoder) Luau bytecode.
    std::string DecompileVanillaOrFail(const std::string &bytecode) {
        Decompiler decompiler{};
        auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
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

TEST_CASE("IR: integer arithmetic round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a + b * 2 - 1 end");
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
        CHECK(compare("return {[1e100]='x'}", "return {[1e100]='y'}") == Kind::Diverge);
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
        const auto preludes = fuzz::CompilePreludes([&](const std::string &, std::string *out) {
            *out = "bytecode";
            return ++compiled != 2;
        });
        CHECK(preludes.empty());
    }
}

TEST_CASE("IR: constant folding is stable across round-trip", "[Decompiler][IREquivalence]") { RequireSameIR("return 1 + 2 * 3"); }

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

TEST_CASE("IR: string concatenation round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a .. \"-\" .. b end");
}

TEST_CASE("IR: array table literal round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return { 1, 2, 3, 4, 5 }"); }

TEST_CASE("IR: length and unary minus round-trip to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(t, n) return #t + (-n) end");
}

TEST_CASE("IR: builtin fastcall round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b, c) return math.max(a, math.min(b, c)) end");
}

TEST_CASE("IR: field get/set round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(t) t.x = t.y + t.z return t.x end");
}

TEST_CASE("IR: method call round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return function(s) return s:upper() end"); }

TEST_CASE("IR: nested closure round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("local function add(a, b) return a + b end return add(1, 2)");
}

TEST_CASE("IR: vararg forwarding round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(...) return select(\"#\", ...) end");
}

// Lossless IR probes

TEST_CASE("IR: multiple return values round-trip to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a, b, a + b end");
}

TEST_CASE("IR: equality comparison as a value round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a == b end");
}

TEST_CASE("IR: relational comparison as a value round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a < b end");
}

TEST_CASE("IR: logical not round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return function(a) return not a end"); }

TEST_CASE("IR: numeric index get round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(t) return t[1] + t[2] end");
}

TEST_CASE("IR: global call round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return function(x) print(x) return x end"); }

TEST_CASE("IR: mixed array/hash table literal round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return { 1, 2, x = 3, y = 4 }");
}

TEST_CASE("IR: string.format fastcall round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(n) return string.format(\"%d\", n) end");
}

TEST_CASE("IR: multiple locals feeding an expression round-trip to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b, c) local x = a + b local y = b + c return x * y end");
}

TEST_CASE("IR: floor division (reg/reg) round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a // b end");
}

TEST_CASE("IR: floor division by constant round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a) return a // 2 end");
}

TEST_CASE("IR: constant-minus-register round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a) return 10 - a end");
}

TEST_CASE("IR: constant-over-register round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a) return 10 / a end");
}

TEST_CASE("IR: modulo round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return function(a, b) return a % b end"); }

TEST_CASE("IR: power round-trips to identical opcodes", "[Decompiler][IREquivalence]") { RequireSameIR("return function(a, b) return a ^ b end"); }

TEST_CASE("IR: not-equal comparison round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(a, b) return a ~= b end");
}

TEST_CASE("IR: non-constant upvalue capture round-trips to identical opcodes", "[Decompiler][IREquivalence]") {
    RequireSameIR("return function(n) local x = n + 1 return function() return x end end");
}

TEST_CASE("IR: table literal with self-referential element does not forward-reference", "[Decompiler][IREquivalence]") {
    // `t[3] = t[1] + t[2]` reads the table being built; coalescing it into the `{...}` literal
    // would emit `t` before its own declaration (a nil global), changing behaviour.
    RequireSameIR("return function(a, b) local t = { a, b } t[3] = t[1] + t[2] return t end");
}

TEST_CASE("IR: self-referential method-call table element stays a post-declaration statement", "[Decompiler][IREquivalence]") {
    // RotatedRegion3 getAxis shape: `t[2] = t[1]:Cross(a[2])` reads the table mid-build.
    RequireSameIR("return function(a) local t = { a[1], a[2] } t[3] = t[1]:Cross(t[2]) return t end");
}

TEST_CASE("IR: nested array-table value in a keyed literal does not forward-reference", "[Decompiler][IREquivalence]") {
    // mirrors RotatedRegion3's `{ [v[1]] = { v[3], v[2], v[5] } }`: the inner array's elements are
    // table-index reads consumed by SETLIST; they must inline, not leak as later `local` decls.
    RequireSameIR("return function(v) return { [v[1]] = { v[3], v[2], v[5] } } end");
}

TEST_CASE("IR: single-use method-call result inlines into a field read (no shadowed temp)", "[Decompiler][IREquivalence]") {
    // RotatedRegion3 getAxis: `t[4] = t[1]:Cross(t[2]).unit` must inline the call into the `.unit`
    // read, not emit a `local vN = t[1]:Cross(...)` reused (shadowed) across each sibling.
    RequireSameIR(
        "return function(a) local t = { a[1], a[2], a[3] } "
        "t[4] = t[1]:Cross(t[2]).unit t[5] = t[1]:Cross(t[3]).unit t[6] = t[2]:Cross(t[3]).unit return t end"
    );
    DecompileResult code{};
    const std::string out = Decompile(
        "return function(a) local t = { a[1], a[2], a[3] } "
        "t[4] = t[1]:Cross(t[2]).unit t[5] = t[1]:Cross(t[3]).unit t[6] = t[2]:Cross(t[3]).unit return t end",
        code
    );
    INFO(out);
    CHECK(out.find(":Cross(") != std::string::npos); // the method call survived
    CHECK(out.find(").unit") != std::string::npos);  // inlined as `...).unit`, not a shadowed `local vN = ...:Cross`
}

// RotatedRegion3 regressions

TEST_CASE("Regress: self-referential table element is never forward-referenced", "[Decompiler][Regression][TableForwardRef]") {
    DecompileResult code{};
    const std::string out = Decompile("return function(a, b) local t = { a, b } t[3] = t[1] + t[2] return t end", code);
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
        code
    );
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK_FALSE(UsesGeneratedLocalBeforeDeclared(out)); // no `[k] = vN` before `local vN`
    CHECK(MaxGeneratedLocalRedeclarations(out) <= 1);   // the reused cross temp must not redeclare
    CHECK(LuauCompiles(out));
}

TEST_CASE("Regress: nested array element in a keyed literal is never forward-referenced", "[Decompiler][Regression][TableForwardRef]") {
    DecompileResult code{};
    const std::string out = Decompile("return function(v) return { [v[1]] = { v[3], v[2], v[5] } } end", code);
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
    const std::string out = Decompile("local v0 = next[true] local v0 = v0:set(\"key\", false) return v0.field", code);
    INFO(out);
    REQUIRE(code == DecompileResult::Success);
    CHECK(LuauCompiles(out));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\bdo\s*\n\s*local\s+v0\b)")));
}

// Fuzz regressions

TEST_CASE("Regress: double unary minus does not collapse into a comment", "[Decompiler][Regression][Fuzz]") {
    // `-(-x)` must emit `- -x`, never `--x`; the latter lexes as a line comment, silently
    // eating the rest of the line (the negate is dropped -> behaviour changes, often unparseable).
    RequireSameIR("return function(x) return - -x end");
}

TEST_CASE("Regress: double unary minus inside a table element stays separated", "[Decompiler][Regression][Fuzz]") {
    RequireSameIR("return function(x) return { 1, - -x } end");
}

TEST_CASE("Regress: a parenthesised first statement does not get a leading semicolon", "[Decompiler][Regression][Fuzz]") {
    // luau has no empty statement, so a leading `;` is a syntax error. When the chunk's first
    // statement is a call whose callee needs parentheses (`(true)(x)`), the disambiguating `;`
    // must be suppressed (the header comment is not a statement it can terminate).
    // (found by Fission.Fuzzing; the constant `true` inlines into the call -> `(true)(...)`.)
    RequireValidRoundtrip("local a = true a(nil) return 1");
    RequireValidRoundtrip("local n = -(-5) n() return 0");
    // same bug inside a nested block: an info-comment precedes the first real statement, so the
    // block's first paren-call must still suppress the `;`. (the expanded fuzzer surfaced this in
    // function and loop bodies; Visit(BlockStatementNode) keeps firstStmt true across comments.)
    RequireValidRoundtrip("local a = true local function f() a(nil) return 1 end return f");
    RequireValidRoundtrip("local a = true for i = 1, 3 do a(i) end return 0");
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
    RequireValidRoundtrip(deep);
}

TEST_CASE("Regress: string-constant left operand in a table element is not a computed key", "[Decompiler][Regression][Fuzz]") {
    // a table array element `"hi" * x` (binop with a string-literal left) must render as
    // `"hi" * x`, not the keyed-entry shape `["hi"] * x`; the latter is an unparseable
    // computed key with no `=`. Recompiling the output catches the malformation.
    RequireSameIR("return function(x) return { 1, \"hi\" * x } end");
    RequireValidRoundtrip("return function(x) return { 1, \"\" // x } end");
    RequireValidRoundtrip("return function(x) return { 1, \"hi\" / x } end");
}

TEST_CASE("Regress: reserved-word string table key stays bracketed", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. A string key that is a Luau keyword (`end`, `function`, ...) must render
    // as `[\"end\"] = v`, never the bare `end = v` (a syntax error). MakeTableKey now treats reserved
    // words as non-identifiers so the table-constructor path brackets them.
    RequireValidRoundtrip("return { [\"end\"] = 1, [\"function\"] = 2, [\"while\"] = 3, x = 4 }");
    RequireValidRoundtrip("return function(t) t[\"end\"] = 1 t[\"for\"] = 2 return t end");
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
    const std::string out = Decompile(src, code);
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
    RequireValidRoundtrip("return function(x) repeat until x end");
    RequireValidRoundtrip("return function(x, y) repeat until x and y end");
}

TEST_CASE("Regress: a break in a loop nested in repeat-until does not leak outside", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. `repeat for ... do break end continue until c` mis-structured: the
    // control-flow analyzer recorded the nested for's break block as the outer repeat's exit, so the
    // break was emitted at top level (`break` outside a loop). The output must stay valid Luau.
    RequireValidRoundtrip("local x = false repeat for i = 1, 2 do break end continue until x return 0");
}

TEST_CASE("Regress: generic-for loop variables are identifiers, never inlined expressions", "[Decompiler][Regression][Fuzz]") {
    // Found by Fission.Fuzzing. Generic-for loop variables were emitted via LiftExpression, which
    // inlines a reused register's value -> `for <expr> in ...` / `for v, <expr> in ...` (syntax errors).
    // They must always render as their own identifier name (mirrors the numeric-for handling).
    RequireValidRoundtrip("return function(t) for k, v in pairs(t) do print(k, v) end end");
    RequireValidRoundtrip("return function(t) for a, b, c in next, t, nil do for x, y in pairs(a) do print(x) end end end");
}

// Saved fuzz corpus

TEST_CASE("Fuzz corpus: every saved sample decompiles without crashing and recompiles", "[Decompiler][Fuzz][Corpus]") {
    // Fission.Fuzzing saves every compiling, crash-free sample here. Re-running them guards against
    // regressions on the exact inputs the fuzzer has already vetted. Absent corpus => nothing to do.
    const std::filesystem::path dir = std::filesystem::path(FISSION_SOURCE_DIR) / "Fission.Fuzzing" / "corpus";
    if (!std::filesystem::exists(dir)) {
        SUCCEED("no fuzz corpus checked in");
        return;
    }
    int ran = 0;
    for (const auto &entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() != ".lua")
            continue;
        std::ifstream f(entry.path(), std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        if (!LuauCompiles(src))
            continue;
        ++ran;
        INFO("corpus sample: " << entry.path().filename().string());
        DecompileResult code{};
        const std::string out = Decompile(src, code);
        // a graceful refusal is fine; a hard crash takes the process down, an invalid output recompiles fail.
        CHECK((code == DecompileResult::Success || code == DecompileResult::FailedToDecompile));
        if (code == DecompileResult::Success)
            CHECK(LuauCompiles(out));
    }
    INFO("corpus samples exercised: " << ran);
}

// Recompilable Luau checks

TEST_CASE("Valid: numeric for recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("local s = 0 for i = 1, 10 do s = s + i end return s");
}

TEST_CASE("Valid: while loop recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("local i = 0 while i < 10 do i = i + 1 end return i");
}

TEST_CASE("Valid: repeat-until recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("local i = 0 repeat i = i + 1 until i >= 10 return i");
}

TEST_CASE("Valid: if-elseif-else recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(x) if x > 2 then return 1 elseif x > 1 then return 2 else return 3 end end");
}

TEST_CASE("Valid: generic for over pairs recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(t) local n = 0 for k, v in pairs(t) do n = n + v end return n end");
}

TEST_CASE("Valid: and-or short-circuit recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(a, b) return a and b or 0 end");
}

// The decompiler folds the dead intermediate locals (`local x,y=a,b; return y,x` -> `return b,a`):
// behaviour-identical, but fewer opcodes, so it is a floor case rather than an IR fixpoint.
TEST_CASE("Valid: local swap recompiles cleanly (dead locals folded)", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(a, b) local x, y = a, b return y, x end");
}

// One sibling inner table is hoisted into a local; the rebuilt table holds identical values but
// the opcode order shifts, so assert the valid-Luau floor rather than an IR fixpoint.
TEST_CASE("Valid: nested table literal recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return { { 1, 2 }, { 3, 4 } }");
}

// `a and b and c` is structured back into an if-not-return chain: equivalent behaviour, different
// recompiled jump layout, hence a floor case.
TEST_CASE("Valid: and-chain as a value recompiles cleanly", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(a, b, c) return a and b and c end");
}

// Guards the GETTABLEN-as-a-statement path: the numeric index must be a declared local, never a
// dangling `vN` (which would silently read a nil global instead of the table element).
TEST_CASE("Valid: reused numeric index declares a real local", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("return function(t) local a = t[1] return a + a + t[2] end");
}

// A never-mutated constant upvalue is folded through the capture (`x=5; ()->x+1` -> `()->6`):
// behaviour-identical, but the capture opcodes vanish, so this is a floor case.
TEST_CASE("Valid: constant upvalue folds through capture", "[Decompiler][IREquivalence][ValidLuau]") {
    RequireValidRoundtrip("local x = 5 return function() return x + 1 end");
}

TEST_CASE("Deser hardening: malformed/truncated/random bytecode never hard-crashes", "[Decompiler][Deser][Hardening]") {
    // The deserializer + lifter ingest attacker-controlled bytes, so any malformed/truncated/garbage
    // input must fail gracefully (a DecompileResult code), never hard-fault. A hard crash here aborts
    // the test process; so reaching the end IS the assertion. Mirrors the Fission.Fuzzing --deser
    // campaign (which runs millions of such inputs); this locks the contract into CI.
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    std::ostringstream sink;
    std::streambuf *coutBuf = std::cout.rdbuf(sink.rdbuf());

    std::string base;
    REQUIRE(LuauCompiles("local function add(a, b) local t = { 1, 2, { 3 } } for k, v in t do add(v) end return a + b end return add(1, 2)", &base));

    const auto vanilla = [&](const std::string &bc) { return decompiler.DecompileVanillaBytecode(bc, static_cast<DecompilerFlags>(0)).resultCode; };

    // every truncation of a valid chunk (exercises short-read paths at every offset)
    for (size_t n = 0; n <= base.size(); ++n)
        (void)vanilla(base.substr(0, n));

    // deterministic byte mutations of valid bytecode + pure-random byte strings
    uint32_t s = 0x12345u;
    auto next = [&]() {
        s = s * 1103515245u + 12345u;
        return s;
    };
    for (int i = 0; i < 3000; ++i) {
        std::string m = base;
        const int flips = 1 + static_cast<int>(next() % 8);
        for (int f = 0; f < flips && !m.empty(); ++f)
            m[next() % m.size()] = static_cast<char>((next() >> 8) & 0xFF);
        (void)vanilla(m);

        std::string r;
        const int len = static_cast<int>(next() % 96);
        for (int k = 0; k < len; ++k)
            r.push_back(static_cast<char>((next() >> 8) & 0xFF));
        (void)vanilla(r);
    }

    std::cout.rdbuf(coutBuf);
    SUCCEED("deserializer + pipeline survived truncations, mutations and random inputs without crashing");
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

    // AST captured as a well-formed JSON tree rooted at a Root node, carrying the expected kinds.
    INFO("astJson:\n" << result.astJson);
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
    const auto result = DecompileWithCaptures(source);
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
}
