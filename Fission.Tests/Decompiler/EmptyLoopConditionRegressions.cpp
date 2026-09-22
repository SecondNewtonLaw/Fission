//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Empty repeat preserves raising condition setup", "[Decompiler][EmptyLoopCondition][Semantics]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(repeat
until (false / ...);
tostring.field((not (-(-print))), select(t));
for g0_0 in tonumber((... > print[nil]), (-(-obj[string]))) do
    for g1_0, g1_1 in (g0_0)((t + nil), ...) do
    end
end
repeat
    for i0 = next("a-b", print), ((-t) * math[true]) do
    end
    table ^= function(p0, p1, p2)
p1(nil);
tonumber(pairs);
return "value";
end;
    ipairs();
until (string[obj]());
for i0 = ..., function()
tonumber("hello", false);
return "";
end, tostring:run(183.25, 51) do
    i0 ..= ipairs;
    (134.25)((if i0 then nil else i0));
end
for g0_0, g0_1 in math() do
    local v2 = { field = g0_0("value", nil), x = (obj * next), nil };
    g0_0 = (if g0_0.field.field then (if ... then v2:run() else ...) else (t)[(if 207 then 32 else nil)]);
    repeat
    until ((if (nil) then v2(g0_0, true) else g0_1:run(tonumber, print)));
end
local v0 = math[nil].field.field.field;
while ... do
end
return (true / next.field);
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    std::string preludeBytecode;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    REQUIRE(fuzz::LuauCompiles("", &preludeBytecode));

    const auto original = fuzz::RunLuauTrace(originalBytecode, preludeBytecode);
    const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, preludeBytecode);
    INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(original.trace == "error: attempt to perform arithmetic (div) on boolean and nil\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Empty repeat reevaluates a computed condition until it succeeds", "[Decompiler][EmptyLoopCondition][Semantics]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(local count = 0
local function bump()
    count += 1
    return count
end
repeat
until bump() + 0 >= 2
return count
)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);

    std::string originalBytecode;
    std::string decompiledBytecode;
    std::string preludeBytecode;
    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(result.output, &decompiledBytecode));
    REQUIRE(fuzz::LuauCompiles("", &preludeBytecode));

    const auto original = fuzz::RunLuauTrace(originalBytecode, preludeBytecode);
    const auto decompiled = fuzz::RunLuauTrace(decompiledBytecode, preludeBytecode);
    INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(original.trace == "return: 2\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}
