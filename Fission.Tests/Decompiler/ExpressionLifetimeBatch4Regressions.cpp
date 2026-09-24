//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace ExpressionLifetimeBatch4Regressions {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string Decompile(const std::string &source) {
        fuzz::EnableLuauFlags();
        REQUIRE(fuzz::LuauCompiles(source));
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        REQUIRE(fuzz::LuauCompiles(result.output));
        return result.output;
    }

    void CheckSemanticParity(const std::string &source) {
        fuzz::EnableLuauFlags();
        std::string originalBytecode;
        REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
        const auto output = Decompile(source);
        std::string reconstructedBytecode;
        REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
        size_t successful = 0;
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            std::string prelude;
            REQUIRE(fuzz::LuauCompiles(fuzz::kSemPreludes[i], &prelude));
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            REQUIRE(original.status != fuzz::SemTrace::Status::Timeout);
            REQUIRE(original.status != fuzz::SemTrace::Status::LoadFailed);
            successful += original.status == fuzz::SemTrace::Status::Ok && original.comparable;
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
        REQUIRE(successful > 0);
    }
} // namespace ExpressionLifetimeBatch4Regressions

TEST_CASE("Batch 4 table folding keeps self references after declaration", "[Decompiler][ExpressionLifetime][Batch4]") {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"t3_6", R"LUA(local v0 = {  }
v0[{ nil, 948, if 108i then v0 else nil }] = nil
ipairs[(-(109i)).field] = { "value" }
local v1 = if true then ({  })["hello"]["a-b"] else v0()
local v2 = {
    "a-b",
    function()
        return -4i
    end,
    ["a-b"] = {  },
}
return { ["\n"] = 503, ["y"] = "key", ["end"] = 22i }
)LUA"},
        {"t5_3", R"LUA(local v0 = { {  } }
v0[nil] = { v0, 188i, -27i }
local v1 = nil
local v2 = function()
    return (if 659 then v0 else -836)[{  }]
end
tostring[(if (if nil then 108i else [[a
b]]).x then 118.42857142857143 else true)] = 0
local v3 = (v1 * 65i)[function()
    return 984
end].y.y
return t(3.4285714285714284), nil
)LUA"},
    };

    for (const auto &[name, source] : cases) {
        DYNAMIC_SECTION(name) {
            const auto output = ExpressionLifetimeBatch4Regressions::Decompile(source);
            CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(output, &source));
        }
    }
}

TEST_CASE("Table folding preserves observable self reference identity", "[Decompiler][ExpressionLifetime][Batch4]") {
    const std::string source = R"LUA(local outer = { 1 }
outer[2] = { outer, 188i, -27i }
return outer[2][1] == outer, outer[2][2]
)LUA";
    ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(original.trace == "return: true\ti64(188)\n");
}

TEST_CASE("Repeat conditions preserve materialized operands across later definitions", "[Decompiler][ExpressionLifetime][RepeatCondition]") {
    SECTION("unary table operand is evaluated once per iteration") {
        const std::string source = R"LUA(local count = 0
local mt = { __unm = function()
    count += 1
    return count
end }
repeat
until -setmetatable({}, mt) == (function()
    return 2
end)()
return count
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: 2\n");
    }

    SECTION("short circuit right operand runs only when needed") {
        const std::string source = R"LUA(local left = 0
local right = 0
repeat
    left += 1
until left >= 2 or (function()
    right += 1
    return false
end)()
return left, right
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: 2\t1\n");
    }
}

TEST_CASE("Nested variadic method results are evaluated once", "[Decompiler][ExpressionLifetime][VariadicCall]") {
    const std::string source = R"LUA(local count = 0
local receiver = {  }
function receiver:run()
    count += 1
    return {  }
end
while function()
end ~= next(receiver:run()) do
    break
end
return count
)LUA";
    ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(original.trace == "return: 1\n");
}

TEST_CASE("Inlining preserves reads before intervening calls", "[Decompiler][ExpressionLifetime][EvaluationOrder]") {
    SECTION("successful metatable read precedes call") {
        const std::string source = R"LUA(local order = 0
local object = setmetatable({}, { __index = function()
    order = order * 10 + 1
    return "k"
end })
local key = object.x
local function make()
    order = order * 10 + 2
    return { k = 42 }
end
return make()[key], order
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: 42\t12\n");
    }
}

TEST_CASE("Deferred table elements preserve constructor order", "[Decompiler][ExpressionLifetime][ConstructorOrder]") {
    SECTION("successful dynamic field precedes later array read") {
        const std::string source = R"LUA(local order = 0
local receiver = { run = function()
    order = order * 10 + 1
    return "value"
end }
local indexable = setmetatable({}, { __index = function()
    order = order * 10 + 2
    return "array"
end })
local function build(flag)
    local result = { k = 0, [(flag == false)] = receiver:run(), y = { nil, true }, indexable[880] }
    return result[false], result[1]
end
local dynamic, array = build(true)
return dynamic, array, order
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, ExpressionLifetimeBatch4Regressions::kOptions), Luau::compile(""));
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: \"value\"\t\"array\"\t12\n");
    }
}
