#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
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

static bool Recompiles(const std::string &source, std::string *errorOut) {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const std::string bytecode = Luau::compile(source, opts);
    const bool ok = !bytecode.empty() && bytecode.front() != '\0';
    if (!ok && errorOut)
        *errorOut = bytecode.size() > 1 ? bytecode.substr(1) : "(empty)";
    return ok;
}

static void CheckDecompileRecompiles(const std::string &source) {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    const bool recompiles = Recompiles(result.decompilationOutput, &error);
    INFO("recompile error: " << error);
    CHECK(recompiles);
}

TEST_CASE("Regress sibling branches lift without accumulating native coroutine frames", "[Decompiler][Regression][StackDepth]") {
    std::string source = "local flag = ...\n";
    for (int i = 0; i < 1024; ++i)
        source += "if flag then ping() else pong() end\n";
    source += "return flag";
    CheckDecompileRecompiles(source);
}

TEST_CASE("Regress AST2 seed659130932 root and propagated parameter bindings", "[Fuzz][ForwardRef][Binding][Regression]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"AST2(obj(((math % t)), true);
local function f0(p1, p2, ...)
    repeat
        p1, f0 = true, next;
        f0, p1 = "value", next;
        select();
    until function(p3)
math(82, 117.5);
p1(23, true);
return nil, select;
end;
    p1(..., (print));
    return (if tostring then obj else nil), ...;
end
(f0["hello"])(table);
return (#function(p1, p2, p3, ...)
p1("a-b", tostring);
tonumber();
ipairs(next);
return ;
end), (if print then f0:method("x") else f0:set(91.5));
)AST2";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    INFO("decompiled output:\n" << result.decompilationOutput);
    REQUIRE(result.resultCode == DecompileResult::Success);
    std::string error;
    CHECK(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    CHECK(result.decompilationOutput.find("local function f0(p1, p2, ...)") != std::string::npos);
}

TEST_CASE("Regress generic-for capture survives sibling loop binding", "[Decompiler][FuzzRegress][GenericFor][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function iter()
    return next, {3}, nil
end
local saved
for a, b in iter() do
    saved = function() return a end
end
for c in iter() do
    print(c * c)
end
return saved()
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original trace: " << verdict.original.trace);
    INFO("decompiled trace: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "1\nreturn: 1\n");
}

TEST_CASE("Regress generic-for loop-carried noncapturing closure", "[Decompiler][FuzzRegress][GenericFor][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function iter()
    return next, {3}, nil
end
local saved
for a in iter() do
    saved = function() return 9 end
end
return saved()
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original trace: " << verdict.original.trace);
    INFO("decompiled trace: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "return: 9\n");
}
