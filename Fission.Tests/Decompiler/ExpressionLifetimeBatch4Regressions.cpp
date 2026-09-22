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
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            std::string prelude;
            REQUIRE(fuzz::LuauCompiles(fuzz::kSemPreludes[i], &prelude));
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
}

TEST_CASE("Batch 4 table folding preserves source evaluation order", "[Decompiler][ExpressionLifetime][Batch4]") {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"t5_1", R"LUA(local v0 = obj
local v1 = ({ 974, nil, -33i })[{ ["x\n]"] = "\nhello", ["x\n]"] = true, ["x"] = false, ["a\nb"] = 37i }]
local v2 = ({ v0 + nil, ["data"] = (true).value }).value
local v3 = nil
select(v0)
return (136).value
)LUA"},
        {"t3_2", R"LUA(local v0 = function()
    return if (if 43.142857142857146 then nil else 30.857142857142858).value then 79i else (if math then true else 342).field
end
local v1 = if ({ ["field"] = v0 }).y then (function()
    return "end"
end)[function()
    return v0
end][((if nil then true else "x\n]") ~= "")] else {
    "a-b" ^ "end",
    ["end"] = [[a
b]] ^ false,
    function()
        return v0
    end,
    "key",
}
tonumber[v0] = - -v0(-57i)
local v2 = -function()
    return v1(v1)
end
v1[-56i] = #(false)[670] == #v0.value
return "end", not (-18i)["a-b"]
)LUA"},
        {"t4_2", R"LUA(local v0 = t((if function(p0, p1, p2)
p0();
return t;
end then math[next] else { field = ipairs, false }));
v0 = { v0.field[obj.field], ((-math)) };
local v1, v2 = v0[false], select(nil, 246.5);
local v3, v4, v5 = nil, ..., v1(true);
v2((pairs < next));
if tostring[math](tostring(true, v5), (if v5 then ipairs else nil)) then
    pairs.field(tonumber[pairs], { field = nil, x = select, k = 970 });
    (nil)((if nil then "" else 361), "hello");
    v4 %= v4:method();
else
    local v6, v7, v8 = v3.field, t, ...;
    local v9 = print("value", 207);
    pairs += v3[true];
end
(854)((pairs):run(v5[nil], v1[nil]), (v4:get(""))[(if (print ^ v4) then (-false) else (#400))]);
local v6 = ((true):get(tonumber.field, 118.5) // ...);
return v3(), (#v6[v5]);
)LUA"},
        {"t7_2", R"LUA(local v0 = 287
local v1 = "end"
local v2 = {
    ["data"] = { v1 },
    ["a\nb"] = if game then -61i else true,
    ["y"] = print(),
    -141,
} - { - -(117i), (nil).y, ["data"] = 126i }
local v3 = 16i
local v4 = 106
local v5 = "key"
local v6 = 29i
return if (false)[47] then function()
    return false
end else nil, game(print)
)LUA"},
    };

    for (const auto &[name, source] : cases) {
        DYNAMIC_SECTION(name) {
            ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
        }
    }
}

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
    SECTION("batch-4/t7_1") {
        const std::string source = R"LUA(repeat
    local v0, v1 = "hello", t();
    if ... then
        break
    end
until ((-{ x = true, [next] = next, x = tonumber, obj }) % function()
ipairs(nil);
return ;
end);
math ^= (select.field[...]);
for g0_0, g0_1 in (nil).field(t()) do
    repeat
        for i2 = (if true then nil else pairs), function(p2)
g0_1(nil, true);
return obj, g0_0;
end do
        end
        (true)((if nil then nil else "x"), (true));
    until math;
    local v2, v3, v4 = g0_0(168, g0_1), g0_1.field.field, tonumber(g0_0.field);
    while (not pairs.field[nil]) do
        v2, v3 = {  }, g0_1:set();
    end
end
local v0, v1 = {  }, ipairs;
while print do
    for i2 = nil, ipairs, true do
        i2(t);
    end
    local v2 = (if obj then "x" else 182);
    if ("x") then
        break
    end
end
local v2 = false;
return (if v1(true) then (-nil) else { x = 16, [nil] = 258, [true] = print }), (if v0:get("", t) then (obj) else (#false));
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
    }

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
    SECTION("batch-4/t4_1") {
        const std::string source = R"LUA(local v0 = (62.285714285714285)[(if nil then true else true)]
local v1 = false
local v2 = v1(nil)[v0]
v1(function()
    return (- -false)[(if -54i then "x\n]" else 55.57142857142857)]
end, math("key", v2)["a-b"] < nil)
next[({ false, ["a-b"] = true } <= - -("x")[nil])] = math
obj(nil)
v2()
return ("key").x, (if "value" then v1 else nil) ^ (true)[122i]
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
    }

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
    SECTION("batch-4/t2_4") {
        const std::string source = R"LUA(if { k = ..., [(string == 77)] = pairs:run("x", nil), y = { y = nil, nil, true, ipairs }, next[880] } then
    while ((-(-string)) ~= ...) do
        pairs()
    end
    ipairs(pairs)
    for g0_0 in tonumber(tonumber, 244.75) do
        g0_0(877, "x")
    end
else
end
next(...)
local function f0(p1, p2)
    f0(table)
    p1(false)
    select(false, string)
    return
end
local function f1(p2, p3)
    f0 -= nil
    return
end
return { data = nil, field = true, select, [print] = "" }, ((if 3 then true else pairs) or (if f1 then string else false))
)LUA";
        ExpressionLifetimeBatch4Regressions::CheckSemanticParity(source);
    }

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
