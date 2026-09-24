//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cctype>
#include <cstring>
#include <string>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

// Luau::compile encodes a compile error as a buffer whose first byte is 0 followed by the message.
static bool Recompiles(const std::string &source, std::string *errorOut) {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const std::string bc = Luau::compile(source, opts);
    const bool ok = !bc.empty() && bc[0] != '\0';
    if (!ok && errorOut)
        *errorOut = bc.size() > 1 ? bc.substr(1) : "(empty)";
    return ok;
}

// True if the output contains an `anon_<n>_<n> = ...` bare assignment -- the closure-leak shape where a
// phi-consumed branch closure is written to an undeclared global instead of the merge-target local.
static size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// Count `local <name>` declarations of an EXACT register name (so `v1` does not match `v10`). A single
// register declared twice (once at an outer scope, once nested) is the phi shadowing bug: the nested
// `local` shadows the outer, so branch writes never reach the outer read.
static bool AssignsToTableLiteralField(const std::string &out) {
    for (size_t p = out.find("})."); p != std::string::npos; p = out.find("}).", p + 3)) {
        const size_t eol = out.find('\n', p);
        const std::string rest = out.substr(p, (eol == std::string::npos ? out.size() : eol) - p);
        const size_t eq = rest.find(" = ");
        if (eq != std::string::npos) // an assignment (` = `), not a comparison, whose LHS is `}).field`
            return true;
    }
    return false;
}

// Regression: a value produced on BOTH arms of an if-expression and then returned, where the return has
// a later multret-call value (`return (if c then a else b), f()`), must keep its merge phi so the
// returned value is the merged local -- not an undefined register that recompiles to nil, and not a
// store against a discarded table literal. Root cause + fix: SSABuilder ComputeLiveness variadic-RETURN
// branch now spans the tail via VariadicTailCount instead of marking zero reads. Found by the AST fuzzer
// (semantic oracle: original returned the table, decompiled returned nil).
TEST_CASE("Regress fuzz: if-expr table arm returned before a multret tail keeps its merge binding", "[Decompiler][Roundtrip][FuzzRegress][SSA]") {
    // else-arm table needs a post-construction store (`field = obj`, obj is a non-constant register).
    {
        const std::string source = "return (if ... then nil else { field = obj }), tonumber(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the corruption: `({  }).field = obj` on a throwaway table.
        CHECK_FALSE(AssignsToTableLiteralField(out));
        // the table must be built as a bound value and the field assigned to that binding.
        CHECK(out.find("field = obj") != std::string::npos);
    }
    // both arms tables -> both need the merge; same invariant.
    {
        const std::string source = "return (if ... then { a = obj } else { field = obj }), tonumber(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        CHECK_FALSE(AssignsToTableLiteralField(out));
    }
}

// Count call applications: an identifier char immediately followed by `(`. `t(1)` -> 1; the buggy
// double-lift produced the call twice (a body `local vN = vM(vK)` plus the re-lifted `until (vM(vK))`).
static size_t CountCallApplications(const std::string &s) {
    size_t n = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] == '(' && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_'))
            ++n;
    return n;
}

// Regression: a `repeat ... until f()` whose condition is an effectful call must run that call ONCE per
// iteration. The lifter force-emits the header (so mutating defs used by the trailing cond survive), but
// a def whose SOLE consumer is the until-condition was force-emitted as a body statement AND re-lifted
// into the condition -> the call ran twice (semantic oracle: original 1 call, decompiled 2). Fix: defer
// such a sole-condition-use, non-loop-carried def so only the condition materializes it. The loop-carried
// counter case (`x = x - 1 until x <= 0`) must NOT be deferred -- its store has to stay in the body.
TEST_CASE("Regress fuzz: repeat-until condition call is not duplicated into the body", "[Decompiler][Roundtrip][FuzzRegress][Loop]") {
    {
        const std::string source = "repeat until t(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the sole call `t(1)` (rendered `vN(vM)`) must appear exactly once, not once in the body and
        // once in the `until`.
        CHECK(CountCallApplications(out) == 1);
    }
    // loop-carried decrement must survive in the body (guard against over-eager deferral).
    {
        const std::string source = R"LUA(
local function f(acc, x)
    repeat
        acc = acc + x
        x = x - 1
    until x <= 0
    return acc
end
return f
)LUA";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the decrement store `arg -= 1` must be a body statement, not folded away into the condition.
        CHECK(out.find("-= 1") != std::string::npos);
    }
}

TEST_CASE("Regress fuzz: materialized calls are not evaluated again", "[Decompiler][FuzzRegress][Semantic]") {
    EnableLuauFFlagsOnce();
    const auto check = [](const std::string &source, size_t fixture) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto prelude = Luau::compile(fuzz::kSemPreludes[fixture]);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
        INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };

    check("repeat obj(129.25) until math.field[t(true)]\n", 1);
    check(
        R"LUA(local v0 = ipairs({ not nil, print(116i, 166i), "\n", 18i .. self })
v0["value"] -= (not function()
    return 145.125, select
end)
local v1 = function(p0)
    return p0(true, "a-b")
end
return v1(90) or string, nil
)LUA",
        0
    );
}

TEST_CASE("Regress fuzz: expression evaluation preserves first error", "[Decompiler][FuzzRegress][EvaluationOrder]") {
    EnableLuauFFlagsOnce();
    const auto check = [](const std::string &source, const std::string &expected) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto prelude = Luau::compile("");
        const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
        INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
        CHECK(original.trace == expected);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };

    check("return tostring(next.x, -(147i))\n", "error: attempt to index function with 'x'\n");
    check("return ({ true, - -false })[if string then (nil <= math).y else 0]\n", "error: attempt to perform arithmetic (unm) on boolean\n");
    check(
        R"LUA(
local v0 = if function()
    return (false)["end"][string.value]
end then tostring(next.x, -(147i)) else true
v0[(if false then "" else true)] = math
local v1 = {
    ["end"] = (if nil then 57i else "") and nil * nil,
    ["field"] = (-"hello").x,
    ["\n"] = v0,
    -"x",
}
v0(v0() <= "x")
local v2 = {}
local v3 = pairs(243)
local v4 = { (nil <= "x\n]")[v2(false, "")], ["x"] = v0(), ["y"] = 33.57142857142857 }
return (if obj then nil else nil) > - -105
)LUA",
        "error: attempt to index function with 'x'\n"
    );
}

TEST_CASE("Regress fuzz: table constructor preserves array-before-field error", "[Decompiler][FuzzRegress][EvaluationOrder]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local v0 = {
    ["x"] = [[a
b]],
    self[-64i],
    (pairs > "key").y,
}
v0()
v0()
local v1 = [[a
b]]
local v2 = nil
local v3 = v0.x
return ("x\n]")[(nil).x]
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), Luau::compile(""));
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), Luau::compile(""));
    INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
    CHECK(original.trace == "error: attempt to index nil with integer\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Regress fuzz: repeat body conditional is not treated as loop exit", "[Decompiler][FuzzRegress][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
repeat
    local v0, v1 = print, function(p0)
        p0(true, p0)
        p0(nil, 130.75)
        return math
    end
    local v2, v3 = (if function(p2)
        v0(nil, "x")
        obj(false, "x")
    end then math else v1.field), v1(obj, tostring)
    v1 ..= tostring.field.field
until (function(p0, ...)
    p0(p0, false)
    return select, true
end)((-nil), (if tonumber:set() then t(nil, tonumber) else function()
    return true
end))
string *= 182.5
while ... do
end
local v0, v1 = function(p0, p1, p2, ...)
    p1(false, "a-b")
    local v3 = p2(57.5, obj)
    return true
end, (-((not ipairs) <= table[tostring]))
v0 = function(p2)
    v1(pairs)
    v0("x")
    return select, nil
end
tonumber()
v1, v1 = v1.field, ((if v0(71.25) then ... else (if math then pairs else t)) or (select)[false])
return "x"
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    const auto prelude = Luau::compile(fuzz::kSemPreludes[0]);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
    INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
    CHECK(original.trace == "error: attempt to call a table value\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Regress fuzz: repeat function condition leaves following return outside", "[Decompiler][FuzzRegress][Loop]") {
    const std::string source = R"LUA(
next /= "a-b";
if next:method(..., (if next.field then function(p0, p1, p2, ...)
    p2(p2, p0);
    return nil, p2;
end else (if tonumber then tostring else obj))) then
    string[tostring.field]((-...));
    local v0 = tostring.field;
    ((-"key"))(...);
else
    for g0_0, g0_1 in select.field() do
        while g0_0 do
            g0_0, g0_0 = g0_0, 571;
            g0_0();
            t = true;
            if (if nil then next else obj) then
                continue
            end
        end
        g0_0, g0_0 = (-(-"value")), { ["hello"] = table };
        repeat
        until (g0_1.field == (-(-tostring)));
    end
    local function f0(...)
        return obj;
    end
    local v1, v2 = (f0.field), (true)();
end
repeat
    for i0 = (if true then select else true), ..., (if nil then "key" else next) do
        string(nil, false);
    end
    if tostring.field[...] then
        break
    end
until function(p0, p1, p2)
    local v3, v4, v5 = obj, p1, p2();
    p0, p2 = print, 493;
    for g6_0, g6_1 in next() do
        g6_0();
        tonumber(99.5);
    end
    return "a-b", p1;
end;
return function(p0, p1, ...)
    p0("a-b", true);
    next();
    t();
    return string, print;
end, true;
)LUA";
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK(result.decompilationOutput.find("while true do") == std::string::npos);
    const auto untilFunction = result.decompilationOutput.find("until (function");
    REQUIRE(untilFunction != std::string::npos);
    CHECK(result.decompilationOutput.find("return function(", untilFunction) != std::string::npos);
    const bool hasStrandedConditionClosure =
        result.decompilationOutput.find("local function anon_") != std::string::npos && result.decompilationOutput.find("_2(") != std::string::npos;
    CHECK_FALSE(hasStrandedConditionClosure);
}

TEST_CASE("Repeat conditional latch preserves closure condition semantics", "[Decompiler][FuzzRegress][Loop][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local iterations = 0
repeat
    iterations += 1
until function()
    return iterations == 1
end
return iterations
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    const auto originalBc = Luau::compile(source, Luau::CompileOptions{1, 2});
    const auto decompiledBc = Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2});
    const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
    REQUIRE_FALSE(originalBc.empty());
    REQUIRE_FALSE(decompiledBc.empty());
    REQUIRE_FALSE(preludeBc.empty());

    const auto verdict = fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 1\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress fuzz: loop update between continue and break conditions survives", "[Decompiler][FuzzRegress][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local total = 0
for i = 1, 11 do
    if i % 2 == 0 then continue end
    total += i
    if total > 13 then break end
end
return total
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    const auto originalBc = Luau::compile(source, Luau::CompileOptions{1, 2});
    const auto decompiledBc = Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2});
    const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
    REQUIRE_FALSE(originalBc.empty());
    REQUIRE_FALSE(decompiledBc.empty());
    REQUIRE_FALSE(preludeBc.empty());
    const auto verdict = fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 16\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress fuzz: call callee is evaluated before effectful arguments", "[Decompiler][FuzzRegress][CallOrder]") {
    EnableLuauFFlagsOnce();
    const auto decompileAndRun = [](const std::string &source) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
        return std::pair{
            fuzz::RunLuauTrace(Luau::compile(source, Luau::CompileOptions{1, 2}), preludeBc),
            fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2}), preludeBc)
        };
    };

    for (const std::string source :
         {"print.field.field(print(\"key\"), { field = print, math })\n", "if select then print.field.field(print(\"key\"), { field = print, math }) end\n"}) {
        const auto [original, decompiled] = decompileAndRun(source);
        CHECK(original.status == fuzz::SemTrace::Status::Error);
        CHECK(decompiled.status == original.status);
        CHECK(original.prints.empty());
        CHECK(decompiled.trace == original.trace);
    }
    {
        const std::string source = R"LUA(
local log = {}
local callable = setmetatable({}, { __call = function() log[#log + 1] = "call" end })
local root = setmetatable({}, { __index = function()
    log[#log + 1] = "callee"
    return { field = callable }
end })
local function argument()
    log[#log + 1] = "argument"
    return 1
end
root.field.field(argument(), { field = print, math })
return table.concat(log, ",")
)LUA";
        const auto [original, decompiled] = decompileAndRun(source);
        CHECK(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: \"callee,argument,call\"\n");
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    }
}

TEST_CASE("Regress fuzz: repeat body does not shadow values used after loop", "[Decompiler][FuzzRegress][ForwardRef]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
repeat
    if ... then
        repeat
            ipairs();
            if (-(-tonumber)) then
                break
            end
        until (-(-{ [pairs] = "x", data = next, string, field = pairs }));
    end
until ({ k = ..., [tostring] = table(), y = string(obj, string) } and (if {  } then ("").field else nil));
string = (not true);
local function f0()
    f0();
    f0(nil);
    return f0;
end
f0 += (if false then nil else 27);
return (f0:set(100.75, tostring)), f0[""][f0];
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    CHECK(CountOccurrences(result.decompilationOutput, "local function f0") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "repeat") == 1);
}
