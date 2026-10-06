#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <string>
#include <unordered_set>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

static std::string DecompileOrFail(const std::string &source, const std::string &driver = "") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);

    REQUIRE(result.resultCode == DecompileResult::Success);
    integration_test::CheckSource(source, result.decompilationOutput, Luau::CompileOptions{1, 2}, driver);
    return std::move(result.decompilationOutput);
}

static bool ContainsRegex(const std::string &source, const std::regex &pattern) { return std::regex_search(source, pattern); }

static size_t CountRegex(const std::string &source, const std::regex &pattern) {
    return static_cast<size_t>(std::distance(std::sregex_iterator(source.begin(), source.end(), pattern), std::sregex_iterator{}));
}

TEST_CASE("Roundtrip: simple return", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return 42");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*return\s+42\s*$)")));
}

TEST_CASE("Roundtrip: omitted IR preserves source and captures", "[Decompiler][Roundtrip][OmitIR]") {
    EnableLuauFFlagsOnce();
    const std::string source = "local total = 0; for i = 1, 10 do total += i end; return total";
    const auto flags = DecompilerFlags::CaptureAST | DecompilerFlags::CaptureCFGGraph | DecompilerFlags::FissionDebugNotes;
    Decompiler baseline{}, omitted{};
    const auto expected = baseline.DecompileTestCode(source, flags);
    const auto actual = omitted.DecompileTestCode(source, flags | DecompilerFlags::OmitIR);
    REQUIRE(expected.resultCode == DecompileResult::Success);
    REQUIRE(actual.resultCode == expected.resultCode);
    CHECK_FALSE(expected.irOutput.empty());
    CHECK(actual.irOutput.empty());
    CHECK(actual.decompilationOutput == expected.decompilationOutput);
    CHECK(actual.astJson == expected.astJson);
    CHECK(actual.cfgGraph == expected.cfgGraph);
    CHECK(actual.debugNotes == expected.debugNotes);
    integration_test::CheckSource(source, expected.decompilationOutput, Luau::CompileOptions{1, 2});
    integration_test::CheckSource(source, actual.decompilationOutput, Luau::CompileOptions{1, 2});
}

TEST_CASE("Roundtrip: while loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*i\s*\+=\s*1[\s\S]*return\s+i)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*\breturn\s+i[\s\S]*(?:until|end))")));
}

TEST_CASE("Roundtrip: nested calls", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(
        "return function(a, b, c) return math.max(a, math.min(b, c)) end", "local f = __integration_subject(); print(f(1, 2, 3), f(4, 2, 3), f(-4, -2, -3))"
    );
    INFO("decompile:\n" << out);
    // the inner multret call is the last argument of the outer call, so it inlines directly; the
    // faithful reconstruction keeps both nested and needs no argument spilled to a local. (An earlier
    // over-count in the B==0 arg estimator forced b/c into `local vN = argK` temporaries; the
    // producer-scan estimate no longer invents those.)
    CHECK(ContainsRegex(out, std::regex(R"(return\s+math\.max\(a,\s*math\.min\(b,\s*c\)\))")));
}

TEST_CASE("Roundtrip: table literal", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(return { 1, 2, 3, "hello" })");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+\{\s*1,\s*2,\s*3,\s*"hello"\s*\})")));
}

TEST_CASE("Roundtrip: function declaration", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local function add(a, b)
            return a + b
        end
        return add(1, 2)
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+function\s+add\([^)]*\)[\s\S]*return\s+[^\n]*\+[^\n]*[\s\S]*end)")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+add\(1,\s*2\))")));
}

TEST_CASE("Roundtrip: numeric for loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local s = 0
        for i = 1, 10 do
            s = s + i
        end
        return s
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*for\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*1,\s*10\s+do)")));
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*s\s*\+=\s*i\b)")));
}

TEST_CASE("Roundtrip: variable assignment with binary expression", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return function(a, b) return a + b end", "local f = __integration_subject(); print(f(1, 2), f(-3, 4), f(0, 0))");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+a\s*\+\s*b)")));
}

TEST_CASE("Roundtrip: if statement", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local x = math.random()
        if x > 0.5 then
            return 1
        end
        return 0
    )");

    INFO("decompile:\n" << out);
    // The branch keeps the exact `math.random() > 0.5` comparison, not the algebraically-flipped `<=`. `>` lowers
    // to `LT(0.5, x)`; flipping to `<=` would reverse the operands and, on NaN or a raising compare, change the
    // result/error. Swapping the arms instead of negating recompiles to the same LT and branch.
    CHECK(ContainsRegex(out, std::regex(R"(if\s+math\.random\(\)\s*>\s*0\.5\s+then\s+return\s+1\s+else\s+return\s+0\s+end)")));
}

TEST_CASE("Roundtrip: repeat-until loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        repeat
            i = i + 1
        until i >= 10
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(repeat\s+i\s*\+=\s*1\s+until\s*\(?i\s*>=\s*10\)?\s+return\s+i)")));
    CHECK(CountRegex(out, std::regex(R"((?:^|\n)\s*return\b)")) == 1u);
}

TEST_CASE("Integration: inline closures retain conditional results", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check(R"LUA(local function choose(flag)
    return (function()
        local value
        if flag then value = nil else value = false end
        return value
    end)()
end
print(choose(true), choose(false)))LUA");
}

TEST_CASE("Type inference: function annotations use valid Luau syntax", "[Decompiler][TypeInference]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode("local f = string.gmatch('abc', '.'); return f, f", DecompilerFlags::InferRobloxTypes);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK(result.decompilationOutput.find("(...any) -> ...any") != std::string::npos);
    CHECK(result.decompilationOutput.find(": function") == std::string::npos);
    const auto bytecode = Luau::compile(result.decompilationOutput);
    REQUIRE(!bytecode.empty());
    CHECK(bytecode.front() != '\0');
    integration_test::CheckSource(
        "local f = string.gmatch('abc', '.'); return f, f", result.decompilationOutput, Luau::CompileOptions{1, 2},
        "local first, second = __integration_subject(); print(first(), second(), first(), second())"
    );
}

TEST_CASE("Deserializer: function bytecode types emit valid annotations", "[Decompiler][Deserializer]") {
    EnableLuauFFlagsOnce();
    const std::string source = "return function(callback: ((...any) -> ...any)?) return callback and callback(7) or 0 end";
    Luau::CompileOptions options{};
    options.typeInfoLevel = 1;
    options.debugLevel = 2;
    const auto bytecode = Luau::compile(source, options);
    Decompiler decompiler;
    const auto result = decompiler.DecompileVanillaBytecode(bytecode, DecompilerFlags::InferTypes);
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("(...any) -> ...any") != std::string::npos);
    integration_test::CheckSource(
        source, result.decompilationOutput, options, "local f = __integration_subject(); print(f(nil), f(function(value) return value * 2 end))"
    );
}

TEST_CASE("Integration: adjacent initializers preserve dependency order", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check({
        R"LUA(local x, y
x = tonumber("1")
y = x + 2
print(x, y))LUA",
        R"LUA(local x = tonumber("1")
local y = tonumber("2")
x = x + y
y = x + y
print(x, y))LUA",
        R"LUA(local value = tonumber("4")
do local value = value + 1; print(value) end
print(value))LUA",
        R"LUA(local value
print(value)
value = false
print(value))LUA",
        R"LUA(local calls = 0
local function nextValue() calls += 1; return calls end
local x, y = nextValue(), nextValue()
print(x, y, calls))LUA"
    });
}

TEST_CASE("Integration: loop arm closures retain per-iteration bindings", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check({
        R"LUA(local readers = {}
for i = 1, 3 do
    if i % 2 == 1 then
        local value = i * 2
        readers[#readers + 1] = function() return value end
    end
end
print(readers[1](), readers[2]()))LUA",
        R"LUA(local readers = {}
for i = 1, 3 do
    local value
    readers[i] = function() return value end
    print(value)
    value = i
end
print(readers[1](), readers[2](), readers[3]()))LUA"
    });
}

TEST_CASE("Integration: branch joins retain suffixed binding values", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check(R"LUA(local function choose(flag)
    local v7_62, v12_62
    if flag then v7_62, v12_62 = 7, 12 else v7_62, v12_62 = 3, 4 end
    return v7_62, v12_62
end
print(choose(true))
print(choose(false)))LUA");
}

TEST_CASE("Integration: conditional initializers read enclosing binding", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check(R"LUA(local function choose(flag)
    local value = tonumber("4")
    do local value = if flag then value + 1 else value + 2; print(value) end
    print(value)
end
choose(true)
choose(false))LUA");
}

TEST_CASE("Integration: indexed assignment closures retain their branches", "[Decompiler][Rewriter][Integration]") {
    integration_test::Check(R"LUA(local target = {}
local function store(flag)
    target[(function()
        local key
        if flag then key = 1 else key = 2 end
        return key
    end)()] = if flag then 7 else 9
end
store(true)
store(false)
print(target[1], target[2]))LUA");
}
