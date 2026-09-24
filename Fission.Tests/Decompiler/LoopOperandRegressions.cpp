//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE("Numeric for evaluates its first staged operand before a later error", "[Decompiler][LoopOperand][Semantics]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(local v0 = {}
for i1 = print("hello"), t(), v0 do
    i1 = v0[135]
end
 pairs.field(true)
return { tonumber, [t] = nil, [353] = next }, v0.field.field
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    const auto preludes = fuzz::CompilePreludes([](const std::string &prelude, std::string *bytecode) { return fuzz::LuauCompiles(prelude, bytecode); });
    REQUIRE_FALSE(preludes.empty());
    for (size_t i = 0; i < preludes.size(); ++i) {
        INFO("fixture " << i);
        const auto original = fuzz::RunLuauTrace(originalBytecode, preludes[i]);
        const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, preludes[i]);
        INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
        REQUIRE(original.status != fuzz::SemTrace::Status::Timeout);
        REQUIRE(original.status != fuzz::SemTrace::Status::LoadFailed);
        REQUIRE(original.trace.starts_with("\"hello\"\n"));
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    }
}

TEST_CASE("Generic for preserves a branch-selected state operand", "[Decompiler][LoopOperand][Semantics]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(local function choose(flag)
    local first = {11}
    local second = {22}
    local total = 0
    for _, value in next, if flag then first else second do
        total += value
    end
    return total
end
return choose(true), choose(false)
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    std::string prelude;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 11\t22\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Loop headed at function entry keeps its loop-carried update", "[Decompiler][LoopOperand][Semantics]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(local function grow(n)
    repeat
        n *= 2
    until n > 5
    return n
end
return grow(1)
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    std::string prelude;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 8\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Numeric for preserves SSA versions above 255", "[Decompiler][LoopOperand][Semantics]") {
    fuzz::EnableLuauFlags();
    std::string source = "local total = 0\n";
    for (int i = 1; i <= 300; ++i) {
        const auto bound = std::to_string(i);
        source += "for value = " + bound + ", " + bound + " do total += value end\n";
    }
    source += "return total\n";

    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    std::string prelude;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 45150\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}
