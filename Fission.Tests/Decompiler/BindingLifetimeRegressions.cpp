//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "Rewriters/DeclarationHoister.hpp"
#include "SourceGenerator/Generator.hpp"
#include <catch2/catch_test_macros.hpp>

namespace BindingLifetimeRegressions {
    static void CheckResult(const std::string &source, const std::string &expected, bool declarationPlacementOnly = false) {
        fuzz::EnableLuauFlags();
        std::string originalBytecode;
        std::string reconstructedBytecode;
        std::string prelude;
        REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
        std::string output;
        if (declarationPlacementOnly) {
            Deserializer deserializer;
            const auto bytecode = deserializer.Deserialize(originalBytecode);
            REQUIRE(bytecode);
            Fission::InstructionDecoder decoder;
            BytecodeLifter lifter{&decoder};
            auto function = lifter.LiftDeserializedBytecode(*bytecode);
            ControlFlowAnalyzer analyzer;
            auto analyzed = analyzer.DetermineBasicBlocks(&function);
            analyzer.OptimizeGraph(analyzed);
            analyzer.PruneUnreachable(analyzed);
            analyzer.IdentifyStructures(analyzed);
            SSABuilder{}.Build(analyzed);
            auto ast = ASTLifter{}.Lift(analyzed);
            DeclarationHoister{}.Run(ast.statements);
            RootNode root{ast.statements};
            output = SourceGenerator{}.GenerateSource(&root);
        } else {
            const auto result = fuzz::FullDecompile(source);
            REQUIRE(result.code == DecompileResult::Success);
            output = result.output;
        }
        INFO(output);
        REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
        REQUIRE(fuzz::LuauCompiles("", &prelude));
        const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
        const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        REQUIRE(original.trace == expected);
        CHECK(reconstructed.status == original.status);
        CHECK(reconstructed.trace == original.trace);
    }
}

TEST_CASE("Captured aliases keep one binding across sibling closures", "[Decompiler][BindingLifetime]") {
    BindingLifetimeRegressions::CheckResult(R"LUA(
local v0 = { value = 7 }
local v1 = v0
local function first() return v0.value end
local function second() return v1.value end
v0.value = 19
return first(), second(), v0 == v1
)LUA", "return: 19\t19\ttrue\n");
}

TEST_CASE("Repeat condition sees value selected after an early break", "[Decompiler][BindingLifetime][RepeatConditionScope]") {
    bool declarationPlacementOnly = false;
    SECTION("declaration placement") { declarationPlacementOnly = true; }
    SECTION("complete pipeline") {}
    BindingLifetimeRegressions::CheckResult(R"LUA(
local v0 = not nil
repeat
    if string.field then
        break
    end
until if { true, v0, true, field = "key" } then true else { tostring, key = next }
return 17
)LUA", "return: 17\n", declarationPlacementOnly);
}

TEST_CASE("Captured function keeps its local binding after global publication", "[Decompiler][BindingLifetime][CapturedGlobalAlias]") {
    std::string source;
    SECTION("function without captures") {
        source = R"LUA(local v0 = function() return 23 end
published = v0
local read = function() return v0() end
published = function() return 99 end
return read(), published()
)LUA";
    }
    SECTION("function with captures") {
        source = R"LUA(local function make(value)
    local v0 = function() return value end
    published = v0
    local read = function() return v0() end
    published = function() return 99 end
    return read(), published()
end
return make(23)
)LUA";
    }
    BindingLifetimeRegressions::CheckResult(source, "return: 23\t99\n");
}

TEST_CASE("Numeric loop pinning ends before reused return registers", "[Decompiler][BindingLifetime][PinnedSSA]") {
    BindingLifetimeRegressions::CheckResult(R"LUA(radix = 16
local function run(flag)
    for i = 1, 3 do
        if flag then break end
    end
    return tostring("ok"), tonumber("10", radix)
end
local a, b = run(true)
local c, d = run(false)
return a, b, c, d
)LUA", "return: \"ok\"\t16\t\"ok\"\t16\n");
}

TEST_CASE("Duplicated numeric-loop return initializes reused bindings", "[Decompiler][BindingLifetime][PinnedSSA]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA((table.field)(nil, ipairs)
select = print()
next()
if (if { [12.25] = obj, field = pairs, field = t } then function()
    select(nil, nil)
    select()
    math()
    return nil, string
end else - -math) then
    local v0 = string:set()
    for i1 = false, v0, "" do
        i1(obj, string)
        v0()
        if v0 then break end
    end
else
    tonumber()
    tostring()
end
return ipairs:set(print), tostring(pairs, math)
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    REQUIRE(fuzz::LuauCompiles(result.output));
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.output, &source));
}

TEST_CASE("Fixed varargs preserve return and table arity", "[Decompiler][BindingLifetime][AdjustedVararg]") {
    SECTION("empty fixed return still includes nil") {
        BindingLifetimeRegressions::CheckResult("return 41.25, (...)\n", "return: 41.25\tnil\n");
    }
    SECTION("fixed and open return tails remain distinct") {
        BindingLifetimeRegressions::CheckResult(R"LUA(local function fixed(...)
    return 41.25, (...)
end
local function spread(...)
    return 41.25, ...
end
return select("#", fixed()), select("#", fixed(7, 8)), select("#", spread()), select("#", spread(7, 8))
)LUA", "return: 2\t2\t1\t3\n");
    }
    SECTION("fixed table tail does not spread extra values") {
        BindingLifetimeRegressions::CheckResult(R"LUA(local function fixed(...)
    return {41.25, (...)}
end
local values = fixed(7, 8)
return values[1], values[2], values[3]
)LUA", "return: 41.25\t7\tnil\n");
    }
}
