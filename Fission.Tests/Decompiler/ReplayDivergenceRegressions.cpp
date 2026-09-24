//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "ReplayDivergenceTestSupport.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace replay_divergence {
    // Roblox ships debug level 1: no upvalue names reach the decompiler.
    void CheckSemanticParity(const std::string &source, int optimizationLevel, int debugLevel) {
        const CompileLevels levels(optimizationLevel, debugLevel);
        const Luau::CompileOptions options{optimizationLevel, debugLevel};
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        const auto originalBytecode = Luau::compile(source, options);
        const auto reconstructedBytecode = Luau::compile(result.output, options);
        size_t successful = 0;
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], options);
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
} // namespace replay_divergence

using replay_divergence::CheckSemanticParity;

TEST_CASE("Replay: a value read twice by one instruction is not inlined twice", "[Decompiler][ReplayRegress][Semantics]") {

    CheckSemanticParity(R"LUA(local t = {}
print(t == t))LUA");
}

TEST_CASE("Replay: a boolean loaded by a jumping load keeps its value", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local a = ...
local v1 = if a then 1 else next == nil
print(v1))LUA");
}

TEST_CASE("Replay: a value is not inlined past a reassignment of its merged input", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local a, b, c = ...
print((if a then true else "x") or "y", if c then 1 else 2))LUA");
}

TEST_CASE("Replay: an infinite loop sharing a repeat header keeps its wrapper", "[Decompiler][ReplayRegress][Semantics]") {

    CheckSemanticParity(R"LUA(local n = 0
for i = 1, 3 do
    repeat
        while n < i do
            n += 1
        end
        print(n)
    until { }
    print("after", i)
end)LUA");
    CheckSemanticParity(R"LUA(local n = 0
for i = 1, 3 do
    repeat
        while n < i do
            n += 1
        end
        print(n)
    until n > i - 1
    print("after", i)
end)LUA");
}

TEST_CASE("Loop: a while whose condition exits past its header still terminates", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n = 0
while true do
    n += 1
    while n % 3 ~= 0 or n < 2 do
        n += 1
        print("spin", n)
    end
    print("go", n)
    if n > 10 then
        break
    end
end
print("done", n))LUA");
    CheckSemanticParity(R"LUA(local n = 0
local function step()
    n += 1
    return n
end
while true do
    while step() % 3 ~= 0 or n < 2 do
        print("spin", n)
    end
    print("go", n)
    if n > 8 then
        return n
    end
end)LUA");
    CheckSemanticParity(R"LUA(local k = 0
while "value" do
    k += 1
    while (k < 3 or tostring(k) == "4") do
        k += 1
        print(k)
    end
    if k > 6 then
        break
    end
end)LUA");
}

TEST_CASE("Loop: loops sharing a header keep both loops", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n, ready = 0, false
while true do
    while not ready do
        n += 1
        ready = n % 3 == 0
    end
    print("go", n)
    ready = false
    if n > 10 then
        break
    end
end
print("done", n))LUA");
    CheckSemanticParity(R"LUA(local n = 0
while true do
    repeat
        n += 1
    until n % 4 == 0
    print("tick", n)
    if n > 12 then
        break
    end
end
print("done", n))LUA");
    CheckSemanticParity(R"LUA(local n, m = 0, 0
repeat
    while n < m do
        n += 1
    end
    print("n", n)
    m += 2
until m > 6
print("done", n, m))LUA");
    CheckSemanticParity(R"LUA(local a, b = 0, 0
repeat
    repeat
        a += 1
    until a % 2 == 0
    b += 1
    print(a, b)
until b >= 3
print("done", a, b))LUA");
    CheckSemanticParity(R"LUA(local n = 0
for i = 1, 3 do
    while true do
        while n < i * 2 do
            n += 1
        end
        print(i, n)
        if n >= i * 2 then
            break
        end
    end
end
print("done", n))LUA");
}

TEST_CASE("Replay: a generic-for variable is not live on its loop's entry edge", "[Decompiler][ReplayRegress][SSA]") {
    fuzz::EnableLuauFlags();
    const std::string source = R"LUA(for g0_0 in next(((not tonumber)), ...) do
    (false).field(function(p1, p2, p3)
end);
    local v1 = tonumber[function(p1, p2)
end];
    local v2 = (if tostring.field[math["x"]] then v1[true][function(p2, p3, p4)
end] else ((tonumber / next)));
    if table.field[(not g0_0(false))] then
    end
end
if print[next:run(ipairs)].field then
    for g0_0 in (234.75)("value") do
        for g1_0, g1_1 in pairs() do
            for i3 = select, false, nil do
            end
        end
        for i1 = g0_0[g0_0], obj do
            if (if table then g0_0 else "key") then
            end
        end
    end
end
local v0 = obj;
while { (983 ^ function(p1, ...)
end), v0(ipairs), [v0] = v0 } do
    for g1_0, g1_1 in print[table](v0["a-b"]) do
        repeat
            while (if 604 then g1_0 else v0) do
            end
        until function(p3, p4, p5)
g1_1(next, 517);
end;
    end
end)LUA";
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    // the captured loop variable's name used to reach an unrelated `obj` read of the same register through loop-entry phis
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.output, &source));
}

TEST_CASE("Replay: an until-condition value merges both of its arms", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n = 0
repeat
    n += 1
until (if n > 2 then {  } else (n > 5))
for i0 = n, 5 do
    print(i0)
end)LUA");
}

TEST_CASE("Loop: a header arm that loops back through a sibling back-edge is not an exit", "[Decompiler][ReplayRegress][Semantics]") {

    CheckSemanticParity(R"LUA(local n, k = 0, 0
repeat
    while true do
        local limit = if n % 2 == 0 then 2 else 3
        for i = 1, limit do
            k += i
        end
        n += 1
        if n > 4 then
            break
        end
    end
until k > 0
print(n, k))LUA");
}

TEST_CASE("Replay: concatenation keeps its grouping", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local mt = {}
mt.__concat = function(a, b)
    print("concat", type(a), type(b))
    return setmetatable({}, mt)
end
local t = setmetatable({}, mt)
local u = (t .. "a") .. "b"
local w = t .. "a" .. "b"
local z = "a" .. (t .. "b")
local s = "p"
s ..= t .. "q"
print(type(u), type(w), type(z), type(s)))LUA");
}

TEST_CASE("Replay: code after an infinite loop is not lifted", "[Decompiler][ReplayRegress]") {
    fuzz::EnableLuauFlags();
    for (const std::string &source :
         {std::string(R"LUA(repeat
    while ("x") do
        local v0 = (if ipairs.field then {  } else string.field);
    end
    if function(p0, p1, p2)
while false do
end
end then
    end
until t:set();
local v0, v1 = t:run(print, nil), math(ipairs);)LUA"),
          std::string(R"LUA(repeat
    repeat
        if pairs[string] then
        end
        v0 = (nil >= 228);
    until false;
until { [v0] = function(p1, p2, p3, ...)
end, ..., (math + nil), x = ... };
local function f1(p2)
end
return (select .. print:get());)LUA")}) {
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.output, &source));
        CHECK(result.output.find(":run(") == std::string::npos);
        CHECK(result.output.find(":get(") == std::string::npos);
    }
}

TEST_CASE("Replay: a global named like a generated local stays global", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function set(x)
    v0 = x
    v1 = v0 and x + 1
end
set(3)
print(v0, v1)
for i = 1, 2 do
    v0 = (v0 or 0) + i
end
print(v0))LUA");
}
