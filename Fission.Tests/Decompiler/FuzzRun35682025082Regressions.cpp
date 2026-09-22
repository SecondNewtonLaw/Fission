#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    void CheckSemanticParity(const std::string &source) {
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        const auto originalBytecode = Luau::compile(source, kOptions);
        const auto reconstructedBytecode = Luau::compile(result.output, kOptions);
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], kOptions);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
} // namespace

TEST_CASE("Fuzz run 35682025082 keeps generic-for before repeat back-edge", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    while "x" do
        for g0_0, g0_1 in ipairs(true, 0) do
        end
    end
until -math:get(0))LUA");
}

TEST_CASE("Fuzz run 35682025082 keeps numeric-for before repeat back-edge", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    repeat
        for i0 = "hello", t do
        end
    until nil
until { [...] = -string })LUA");
}

TEST_CASE("Fuzz run 35682025082 keeps if-expression initializer binding", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = if { ["x"] = pairs, ipairs, "value" } then function()
end else false
local v1 = v0
v1()
local v2 = function()
    return v1(0, 0)
end
local v3 = { ["data"] = (false)[0] })LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves table key-before-value order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = {}
v0[true > false] = true * "\nhello")LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves truthy closure short-circuit", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(if ... or function(...)
end then
    pairs(obj[185.75])
end
local v0 = table
v0())LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves table assignment statement order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = {}
t[("value").x] = function()
end
v0[nil] = (not nil)["a-b"])LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves comparison before later table write", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(t()
local v1 = { "a-b", ["a\nb"] = 0, 0 }
local v2 = function()
end < nil
v1[- -0] = (false).y)LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves constructor element evaluation order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = {
    ["y"] = function()
    end,
    obj(nil, nil)[("key")[nil]],
    { ["data"] = game, 189i, ["field"] = 0 },
}
v0[0] = (nil).field)LUA");
}

TEST_CASE("Fuzz run 35682025082 preserves constructor branch evaluation order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = {
    ([[a
b]]).value.field,
    0,
    if 186i < nil then function()
    end else true,
    (nil).x,
})LUA");
}
