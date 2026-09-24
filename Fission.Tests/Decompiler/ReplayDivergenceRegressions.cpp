//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

namespace {
    struct CompileLevels {
        int optimization, debug;
        CompileLevels(int optimizationLevel, int debugLevel)
            : optimization(std::exchange(fuzz::optimizationLevel, optimizationLevel))
            , debug(std::exchange(fuzz::debugLevel, debugLevel)) {}
        ~CompileLevels() {
            fuzz::optimizationLevel = optimization;
            fuzz::debugLevel = debug;
        }
    };

    // Roblox ships debug level 1: no upvalue names reach the decompiler.
    void CheckSemanticParity(const std::string &source, int optimizationLevel = fuzz::kOpt, int debugLevel = fuzz::kDebug) {
        const CompileLevels levels(optimizationLevel, debugLevel);
        const Luau::CompileOptions options{optimizationLevel, debugLevel};
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        const auto originalBytecode = Luau::compile(source, options);
        const auto reconstructedBytecode = Luau::compile(result.output, options);
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], options);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
} // namespace

TEST_CASE("Replay: auto-shaped upvalue names do not alias another register", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = "hello"
local v1 = v0
v1[{  }] = not t
local v2 = (0).x
return function()
    return { "end", ["end"] = 0, ["a\nb"] = v1 }
end)LUA");
    CheckSemanticParity(R"LUA(local v0 = { true }
local v1 = v0
v1[(if (if v0 then 0 else "\n") then 0 else 71i)] = ipairs
math()
next[(function()
    return v1
end).y] = v1(false))LUA");
    CheckSemanticParity(R"LUA(local v0 = function()
end
v0[{ ["end"] = false, nil, true, v0 }] = v0(true, "")[v0[true]]
local v1 = v0
local v4 = function()
    return v1
end)LUA");
    CheckSemanticParity(R"LUA(local v0 = "x\n]"
v0[{
    function()
    end,
}] = ({ 0, v0, ["x"] = nil, ["end"] = "x" })
local v2 = v0
v2[table((111i).y)] = function()
    return (v2[ipairs])
end)LUA");
    CheckSemanticParity(R"LUA(local v0 = tostring
local v1 = v0
local v3 = {
    ["\n"] = function()
    end,
    ["x\n]"] = function()
        return v1
    end,
    v0("x"),
}
pairs[(#(true).field[(true)["a-b"]])] = (nil).y[((v2))][(("a-b"))])LUA");
}

TEST_CASE("Replay: captured parameter keeps one name before and after the capture", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function f0(p1, p2)
    p1, p2 = t("x"), p1("", print);
    local v3 = ((function(p3, p4, p5)
p1(table, false);
end));
end
(f0("a-b", ""))(function(p1, p2, ...)
end);)LUA");
}

TEST_CASE("Replay: method rewrite never splices a closure past a rebinding", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function f0(p1, p2, ...)
end
f0 /= { [0] = pairs };
f0 = { ["key"] = obj, x = {  }, [string.field] = v1:method(math, 0), y = { x = f0 } };
if (((-(-function(p2, p3)
return table, f0;
end)))) then
else
end)LUA");
    CheckSemanticParity(R"LUA(for i0 = (function(p0, ...)
end), {  } do
end
local function f0(p1, p2, ...)
end
local v1, v2, v3 = (-(-(#0))), { [string.field] = ..., function(p1, p2, p3)
end, f0:run(f0) }, nil;
v1, v2 = { 0, 0, field = v2, [next] = nil }, v1:run();)LUA");
}

TEST_CASE("Replay: constructor folding stops at stores into other tables", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = { ["a\nb"] = nil, ["data"] = false, 87i, ["end"] = 0 }
local v1 = (false)
v1[nil] = 0
v0[(nil).y] = {  })LUA");
}

TEST_CASE("Replay: folded constructor elements are evaluated once in a repeat header", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    local v0 = { [t.field] = (tonumber), ["hello"] = "value" };
    v0 = (-(-v0:set(0, pairs)));
until table[{ [false] = true, 0, [false] = 0, field = select }][true];)LUA");
    CheckSemanticParity(R"LUA(repeat
until { t:set(313), field = string(48.25), [obj] = next[ipairs], { data = pairs } };
pairs();)LUA");
}

TEST_CASE("Replay: a raising read stays before stores into an earlier table", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = {  }
local v1 = false
local v2 = (if next then v0 else 913)[55]
local v3 = v1 or (-3i)["a-b"]
v0[-9i] = (true)[([[a
b]])[(- (-1i))]]
local v4 = v3()[v1(v2, 149i)]
return [[a
b]], (false)[v0] / (if false then 148i else false))LUA");
    CheckSemanticParity(R"LUA(local t = ...
local v0 = {}
local v3 = t.x
v0[1] = -t
local v4 = v3())LUA");
}

TEST_CASE("Replay: a value read twice by one instruction is not inlined twice", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = (416)(ipairs(true, 39.75));
v0 /= (v0);)LUA");
    CheckSemanticParity(R"LUA(local v0 = pairs.field(obj(print), select:method(true));
v0[v0](tostring, (-false));
local v1 = (function()
end);)LUA");
    CheckSemanticParity(R"LUA(local t = {}
print(t == t))LUA");
}

TEST_CASE("Replay: a boolean loaded by a jumping load keeps its value", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local a = ...
local v1 = if a then 1 else next == nil
print(v1))LUA");
    CheckSemanticParity(R"LUA(local v0 = pairs(if ("\nhello")[nil] then function()
end else next == nil)
local v2 = if function()
end then next() else -({
})[v0[false]])LUA");
}

TEST_CASE("Replay: a raising read stays before a later constructor's elements", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = (("x")[119] and 525 // 51).value
local v1 = (nil).field
local v2 = { v1 } + v0)LUA");
}

TEST_CASE("Replay: a value is not inlined past a reassignment of its merged input", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local a, b, c = ...
print((if a then true else "x") or "y", if c then 1 else 2))LUA");
    CheckSemanticParity(
        R"LUA(pairs((if -53i then true else "a-b") or (if "x\n]" then "\nhello" else 142i), if t then { "x", true, ipairs } else ipairs(false, 228)))LUA"
    );
}

TEST_CASE("Replay: a short-circuit over a constructor keeps its elements in place", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v2 = { {  }, 85i, { v1 }, (94.85714285714286).field } and nil
local v4 = (-(11i))[{ nil, 38.142857142857146, ["field"] = "x" }]
v2())LUA");
}

TEST_CASE("Replay: multiple assignment reads targets before overwriting them", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(for g0_0, g0_1 in pairs(table, print) do
    g0_0, g0_0 = (function(p2, p3, ...)
end), g0_0["value"].field;
end)LUA");
}

TEST_CASE("Replay: a closure condition is tested by its own value", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0, v1 = true, tonumber;
repeat
    if (function()
end).field then
        if v0[v1].field then
            break
        end
    end
until obj.field[t:method(ipairs, 0)](ipairs[function(...)
end], ((tonumber)));)LUA");
}

TEST_CASE("Replay: loop exits on the false edge keep their polarity", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    if function(p0, p1, p2, ...)
end then
        select(math, t);
        if 0 then
            break
        end
    else
    end
    table(0);
until nil;)LUA");
    CheckSemanticParity(R"LUA(while ({ x = nil, true, x = false, data = tonumber } or ((nil))) do
    repeat
        select(tostring, "key");
    until function(p0, p1, p2)
end;
end
for g0_0 in next(ipairs.field, ipairs(true)) do
end)LUA");
    CheckSemanticParity(R"LUA(repeat
    if function(p0, p1, p2, ...)
end then
        if true then
            break
        end
    end
    local v0 = obj;
    v0("", nil);
until ((not 0));
local v0, v1, v2 = (obj)((nil * "value"), ...), select:run(), ((-(-0)));)LUA");
    CheckSemanticParity(R"LUA(while (((if (if tostring then tostring else 0) then ipairs:set() else t("", nil)))) do
    local v1 = (string)[(-(-false))];
    if tonumber then
        break
    end
end)LUA");
}

TEST_CASE("Replay: a conditional jump to the latch lifts as continue", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = function(...)
end;
repeat
    if (v0) then
    else
        if (-(-function(p4, p5, p6)
end)) then
            continue
        end
    end
    (...)(next:run());
until next;
table[print](false, ...);)LUA");
}

TEST_CASE("Replay: value branches inside repeat bodies keep their merge", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
until (if { true, v0, true, field = "key" } then true else { tostring, ["key"] = next });
for g1_0 in v0((-(-nil)), ipairs.field) do
end)LUA");
    CheckSemanticParity(R"LUA(repeat
    table(((if string then false else t)));
    tonumber[false][function(...)
end]((if function(p0, p1, ...)
end then (-(-0)) else select[ipairs]));
until math:set((not { k = nil, select, k = 0 }));
for i2 = 0, v0(nil) do
end)LUA");
    CheckSemanticParity(R"LUA(local v0 = pairs;
repeat
    tonumber((if string then next else select));
    v0.field(..., {  });
    local v1 = ({ [pairs] = v0 } >= v0[v0]);
until ({ field = string, x = next, ["value"] = "", nil } // (0));
repeat
    for i1 = ipairs, nil do
    end
until v0:get();)LUA");
}

TEST_CASE("Replay: inline-order checks terminate on mutually dependent defs", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function f0()
    repeat
        math(81.75);
        if (#"hello") then
            continue
        end
    until table();
    local v4 = t(string, nil);
    return {  }, v4:set();
end
return f0)LUA");
    CheckSemanticParity(R"LUA(local v0 = (29.714285714285715)[101.42857142857143]
local v1 = ({ 822, nil })[(v0 and nil)[(6.571428571428571).x]]
v1[(651 / 863 <= (if game then nil else "\n"))] = (41i)[function()
    return 548
end]
return if (if [[a
b]] then "end" else select) then nil elseif -39i then true else select)LUA");
}

TEST_CASE("Replay: continue in repeat keeps until-condition locals declared", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    repeat
        local v0 = { ["value"] = next, k = 86.25, y = tostring, 206.75 };
        if v0 then
            for g1_0, g1_1 in v0() do
            end
            if print(next) then
                continue
            end
        end
        v0 ..= ipairs(true);
        if ((next ^ nil) <= v0[140.5]) then
            break
        end
    until ((string.field) // (if (231.5) then tostring else tostring.field));
    (select:set())(..., ({  } + string:method(pairs)));
until { field = ... };)LUA");
    CheckSemanticParity(R"LUA(if (tostring[{  }] and print(528)) then
    obj *= table[119.25];
else
    repeat
        if ... then
            ipairs(next);
        else
            pairs();
            string();
            if math then
                continue
            end
        end
        string = 129;
        if (179.75) then
            t(string);
        else
            select(string, "a-b");
            table(ipairs);
        end
        if 66 then
            break
        end
    until (obj.field >= next);
end
print("after"))LUA");
}

TEST_CASE("Replay: continue runs the statements that build the until condition", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    if ... then
    else
        if (math) then
            continue
        end
    end
    while { y = "hello", print, x = 289, data = "x" } do
    end
until { y = print["x"], [table:get(false)] = nil, {  }, field = print.field };)LUA");
}

TEST_CASE("Replay: an arm that is the enclosing join stays empty", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(string = (if ((tonumber) or (if pairs then "key" else true)) then table(print) else ...);
if (nil) then
else
    repeat
        while ("") do
        end
    until v2();
end)LUA");
    CheckSemanticParity(R"LUA(local function f0(p1)
end
repeat
    f0 = function()
end;
until (if (if 568 then "a-b" else 801) then ("a-b" > pairs) else (next));
repeat
    while { true, tonumber, tonumber } do
        if (376 >= false) then
            break
        end
    end
    if f1:set() then
    end
    if ... then
        break
    end
until ("x"):get((-(-"hello")), f0:get(15));
for g1_0 in tonumber() do
    for i3 = false, (if "value" then tonumber else ipairs) do
        if f0[f0] then
        end
    end
end)LUA");
}

TEST_CASE("Replay: an empty while over a short-circuit condition keeps its exit", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(while (t or (next or t)) do
end
local function f0(p1, p2, p3)
end
print((f0 < 161.75)))LUA");
    CheckSemanticParity(R"LUA(while (string[false] or math:set(nil, "hello")) do
end
select[table]((if pairs[function(...)
end] then ... else (select.field)));)LUA");
    CheckSemanticParity(R"LUA(local v1 = ((if next.field then (next) else ...));
while (v0[nil][tostring.field] or (v1 or 286)) do
end
return (if (false ~= nil) then pairs:get(table) else tostring:run());)LUA");
}

TEST_CASE("Replay: an infinite loop sharing a repeat header keeps its wrapper", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(while "value" do
    while (... or t(t, 603)) do
        ipairs(next);
    end
end)LUA");
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
    CheckSemanticParity(R"LUA(repeat
until (if function(p0)
end then {  } else (...));
for i0 = ((nil) + ...), ..., print do
end)LUA");
}

TEST_CASE("Loop: a header arm that loops back through a sibling back-edge is not an exit", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    while true do
        for i0 = t.field, (if pairs then false else table), function(p0, p1, p2, ...)
end do
            while tostring("x") do
            end
        end
    end
until function()
end;)LUA");
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
    CheckSemanticParity(R"LUA(local v1 = (-((145 .. obj) .. nil)))LUA");
}

TEST_CASE("Replay: code after an infinite loop is not lifted", "[Decompiler][ReplayRegress]") {
    fuzz::EnableLuauFlags();
    for (const std::string &source : {std::string(R"LUA(repeat
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

TEST_CASE("Loop audit: a break or return arm keeps the statements after its if", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n = 0
while n < 5 do
    n += 1
    if n % 2 == 0 then
    else
        if n > 3 then
            break
        end
    end
    print("tail", n)
end
print("done", n))LUA");
    CheckSemanticParity(R"LUA(local g, h = 0, 0
while g < 30 do
    g += 1
    if g % 2 == 1 then
        h += 1
    else
        if g > 24 then
            return g
        end
    end
    print(g, h)
end)LUA");
    CheckSemanticParity(R"LUA(local g, h = 0, 0
for k = 1, 2 do
    while true do
        g += 1
        if g % 3 == 0 then
        else
            if h % 2 == 1 then
                h += 1
                continue
            end
        end
        h += 2
        if g > 30 then
            break
        end
    end
    h += k
end
print("end", g, h))LUA");
}

TEST_CASE("Loop audit: a compound condition inside a loop merges after its body", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local g, h = 0, 0
while g < 30 do
    g += 1
    if (g % 2 == 0 and h > 1) or g % 5 == 0 then
        h += 2
    end
    print(g, h)
end
print("end", g, h))LUA");
}

TEST_CASE("Loop audit: a shared tail after a short-circuit runs on every path", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function f(n) print("f", n) return n end
local function probe(x)
    return (f(false) or x) and f(3)
end
print(probe(nil)) print(probe(false)) print(probe(0)))LUA");
    CheckSemanticParity(R"LUA(local function f(n) print("f", n) return n end
local t = { k = 4 }
local function probe(x)
    return (x or f(2)) and t.k
end
print(probe(nil)) print(probe(false)) print(probe(0)))LUA");
}

TEST_CASE("Loop audit: a loop exit threaded past an enclosing else still breaks", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(if { [pairs] = 1.25 } then
    while (if pairs then nil else pairs) do
        ipairs(nil, 432);
    end
else
end
ipairs -= tostring.field:get(function(p1, p2, p3)
end, "key");)LUA");
    CheckSemanticParity(R"LUA(local n, p = 0, ...
if n == 0 then
    while (if p then nil else n < 3) do
        n += 1
        print(n)
    end
else
    print("else")
end
print("done", n))LUA");
}

TEST_CASE("Loop audit: an arm holding a breaking inner loop merges before the tail", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n, log = 0, {}
local function g()
    n += 1
    return n % 2 == 0
end
local function h()
    return n > 6
end
local function f0()
    return n > 8
end
repeat
    if g() then
        repeat
        until n > 0
        if h() then
            break
        end
    end
    table.insert(log, n)
    if f0() then
        break
    end
until n > 10
print(table.concat(log, ",")))LUA");
    CheckSemanticParity(R"LUA(local function f0(p1, p2)
end
repeat
    repeat
        if { [160] = select, y = ipairs, tonumber, x = nil } then
            repeat
            until (pairs);
            if ... then
                break
            end
        end
        select = (if obj["value"] then f0.field else function(p1, p2, p3)
end);
        if f0(table, true) then
            break
        end
    until ...;
until (if f0.field then { k = f0.field, tostring[table], "hello", f0.field } else (-("value" ~= ("" and tostring))));)LUA");
}

TEST_CASE("Loop audit: a repeat followed by another loop keeps its continue target", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n, log = 0, {}
repeat
    n += 1
    if n > 3 then
    else
        if n % 2 == 1 then
            continue
        end
    end
    repeat
        table.insert(log, -n)
    until true
until n > 5
while n < 8 do
    n += 1
end
print(table.concat(log, ","), n))LUA");
    CheckSemanticParity(R"LUA(local v0 = tostring;
repeat
    if (string) then
    else
        if v0 then
            continue
        end
    end
    repeat
        v0(false, tostring);
        v0, v1 = true, v1;
    until (not select);
until v0();
while (if math:get() then function()
local function f1(p2, ...)
end
end else (not tonumber)) do
    for i1 = function(p1, p2, p3)
end, (#(math)) do
        if nil then
            while function(p2, p3)
end do
            end
            repeat
            until (math ^ false);
        end
    end
end
v0 -= (function(p1, p2, p3, ...)
end);
return function(p1, ...)
end;)LUA");
}

TEST_CASE("Loop audit: a loop shared by both arms of a short-circuit runs on each", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function probe(a, b, c)
    local n = 0
    if a or (if b then c else 5) then
        while n < 2 do
            n += 1
            print("body", n)
        end
    end
    print("after", n)
end
probe(nil, nil, nil) probe(nil, true, nil) probe(nil, true, 1) probe(1, nil, nil))LUA");
    CheckSemanticParity(R"LUA(if (obj.field or (if (not tonumber) then (not 436) else (if "key" then math else "key"))) then
    while ({ [181] = obj } >= { y = 416, true }) do
    end
end
for i0 = string[92.5].field, math.field["hello"] do
end
for i1 = (true), v0(146), (not false) do
    if (tostring // next) then
    end
end)LUA");
    CheckSemanticParity(R"LUA(while ... do
    if {  } then
        if pairs(nil, next) then
            local v0 = string:get(select);
            if (string // nil) then
                continue
            end
        end
        for i0 = function(...)
end, select[false] do
            local function f1(...)
            end
        end
        if "x" then
            continue
        end
        if (#(if true then false else tonumber)) then
        end
    end
    while tostring[ipairs(table, 100.25)][{ [nil] = "key" }] do
        local v0, v1, v2 = obj, "x", function(p0, p1)
end;
    end
end)LUA");
}

TEST_CASE("Loop audit: or-chain terms keep their comparison and single evaluation", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = function()
end
local v1 = if 640 >= v0 or v0(57i) then nil else table(607) / 720)LUA");
    CheckSemanticParity(R"LUA(repeat
    if string.field then
    else
        if ipairs then
            continue
        end
    end
    if next() then
        break
    end
until { [898] = tostring, data = tostring, [nil] = obj, field = true };)LUA");
}

TEST_CASE("Loop audit: a compound condition with a large body keeps the body on every path", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local log = {}
local function f(x)
    return x
end
local function p1(a, b, c)
    if a or f(b and c) then
        for i = 1, 2 do
            table.insert(log, "p1" .. i)
        end
        for _, v in ipairs({ 7 }) do
            table.insert(log, v)
        end
        table.insert(log, "p1")
    end
    table.insert(log, "end1")
end
local function p3(a, b, c)
    if (b and c) or a then
        for i = 1, 2 do
            table.insert(log, "p3" .. i)
        end
        for _, v in ipairs({ 7 }) do
            table.insert(log, v)
        end
        table.insert(log, "p3")
    else
        table.insert(log, "else3")
    end
end
for _, t in ipairs({ { nil, nil, nil }, { nil, true, nil }, { nil, true, 1 }, { 1, nil, nil }, { 1, true, 1 } }) do
    p1(t[1], t[2], t[3]) p3(t[1], t[2], t[3])
end
print(table.concat(log, ",")))LUA");
}

TEST_CASE("Loop audit: a condition term computed by an if-expression folds into the condition", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local log = {}
local function probe(a, b, c)
    local n = 0
    if a or (if b then c else 5) then
        for i = 1, 2 do
            local x = i * 2
            local function get()
                return x + n
            end
            table.insert(log, get())
        end
        for k, v in ipairs({ 7, 8 }) do
            local y = k + v
            table.insert(log, y)
        end
        repeat
            n += 1
            local z = n
            if z == 2 then
                continue
            end
            table.insert(log, z)
        until n > 3
    end
    table.insert(log, "after" .. n)
end
probe(nil, nil, nil) probe(nil, true, nil) probe(nil, true, 1) probe(1, nil, nil)
print(table.concat(log, ",")))LUA");
}

TEST_CASE("Loop audit: code after a break-if stays in the loop body", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local n = 0
local function t()
    n += 1
    return n > 2
end
local function run(...)
    repeat
        if ... then
            break
        end
    until (if t() then function() end else nil)
    if n then
        print(n)
    end
end
run() run(1))LUA");
    CheckSemanticParity(R"LUA(repeat
    if ... then
        break
    end
until (if t(obj, "") then function(...)
end else nil);
if ... then
end
if tonumber then
    v0 //= (not nil);
end
for g0_0 in next[table](function(p0, p1, ...)
end) do
    if 296 then
        for i2 = "key", "key" do
            if math then
            end
        end
    end
    for i1 = nil, table.field, (nil) do
        if next[i1] then
        end
    end
end)LUA");
    CheckSemanticParity(R"LUA(while next(math) do
    local function f0(p1, p2, p3, ...)
    end
    if (if f0 then pairs else f0) then
        break
    end
end
if { [tostring:get(select, "value")] = (true), [function(p0, p1)
end] = select } then
end
for i1 = ..., v0[nil] do
    if math[false] then
    end
end
return (#function(p1, p2, p3, ...)
end);)LUA");
}

TEST_CASE("Loop audit: a generated local never captures a global a nested closure reads", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    if (if ... then (-nil) else { [ipairs] = false }) then
        local function f1(p2, ...)
            v0();
        end
        f1();
        if function(p2)
end then
        end
    end
    while (-tostring) do
        if select then
        end
    end
until (if t:get() then ((true) and (632 * 241)) else pairs:set(true));
local function f0()
end
((if (not tonumber) then (103.75) else function(p1, p2)
end))();
f0(select.field, function(p1, p2, p3)
end);)LUA");
}

TEST_CASE("Replay: shared short-circuit arms run on every path", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(math = (if (t.field and ...) then function(p0, p1, p2, ...)
end else print((true <= false)));)LUA");
    CheckSemanticParity(R"LUA(if ((string and (if select then false else 0)) or tonumber(table, 0)) then
else
end
for g0_0 in string({ pairs, [t] = pairs, false, [0] = 0 }) do
end)LUA");
}

TEST_CASE("Compiler audit: a multi-value constructor tail survives a reused element register", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function multi() return "m1", "m2" end
print(#{multi()})
local t = {multi(), multi()}
print(#t, t[1], t[2], t[3]))LUA");
}

TEST_CASE("Compiler audit: a pure value is not inlined past a write that reuses its input's name", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(t(142)
local v1 = { ["data"] = 0, ["a-b"] = 0, ["y"] = "value" } or ((nil))
local v2 = ("")[function()
end]
v0(v1(not "key")))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(R"LUA(local a = {x = 1} or nil
local b = tostring(#"abc")
print(type(a), a and a.x, b))LUA", 1, 1);
}

TEST_CASE("Compiler audit: the right operand of a register and/or stays eagerly evaluated", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local v0 = -(172i)
local v1 = function()
end or v0
return (47i > nil)[function()
end])LUA";
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(R"LUA(local calls = 0
local function f() calls += 1 return calls end
local a = f()
local b = tostring or a
print(b == tostring, calls))LUA", 1, 1);
}

TEST_CASE("Compiler audit: a reassigned recursive local function stays one variable", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local calls = 0
local function fib(n)
    calls += 1
    if n < 2 then return n end
    return fib(n - 1) + fib(n - 2)
end
local function memo(f)
    local cache = {}
    return function(n)
        local v = cache[n]
        if v == nil then
            v = f(n)
            cache[n] = v
        end
        return v
    end
end
fib = memo(fib)
print(fib(20), calls))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a raising value is not moved past a for-loop prep", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    local v0 = (0 .. next);
    for i1 = "", 0 do
        v0();
    end
until function(p0, p1, ...)
end)LUA");
}

TEST_CASE("Compiler audit: a closure chosen by and/or keeps its value", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local enabled = tonumber("1")
local disabled = tonumber("x")
local h = enabled and function() return "a" end or print
local g = disabled and function() return "b" end or tostring
print(type(h), h == print, h and h(), g == tostring)
local v0 = (function()
end)
local v1 = v0 and ((function()
end)) or (false)["a-b"]
v1[v0] = - -0)LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a local function capturing itself keeps its name without debug names", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function self() return self end
print(self() == self)
local function outer()
    local function rec(n) if n > 0 then return rec(n - 1) end return n end
    return rec
end
print(outer()(3)))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
    const auto reassigned = R"LUA(local function f0(n)
    if n > 0 then return f0(n - 1) end
    return "done"
end
print(f0(3))
for i = 1, 2 do
    print(pcall(f0, i))
    f0 = tostring
end
print(f0(7)))LUA";
    CheckSemanticParity(reassigned, 1, 1);
    CheckSemanticParity(reassigned, 1, 2);
}

TEST_CASE("Compiler audit: an upvalue written through nested closures stays one variable", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function outer()
    local x = 1
    local function mid()
        local function inner() x += 1 return x end
        return inner
    end
    return mid()
end
local ii = outer()
ii()
print(ii()))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: an inlined return leaves a while or repeat loop through its return label", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local log = {}
local function retLoop(p)
    local n = 0
    repeat
        n += 1
        if n == p then return "r" .. n end
    until n > 3
    return "none"
end
local function firstBig(t)
    local i = 0
    while i < #t do
        i += 1
        if t[i] > 10 then return tostring(t[i]) end
    end
    return "none"
end
local function nested(n)
    local k = 0
    repeat
        k += 1
        if k % 2 == 0 then
            if k > n then return "even" .. k end
        end
    until k > 6
    return "end" .. k
end
local function callRet(n)
    local k = 0
    while true do
        k += 1
        if k == n then return string.rep("x", k) end
        if k > 5 then return "big" end
    end
end
for p = 1, 3 do table.insert(log, retLoop(p)) end
table.insert(log, retLoop(9))
table.insert(log, firstBig({5, 20, 7}))
table.insert(log, firstBig({1, 2}))
for n = 0, 7 do table.insert(log, nested(n)) end
for n = 1, 7 do table.insert(log, callRet(n)) end
print(table.concat(log, ",")))LUA";
    CheckSemanticParity(source, 2, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 1, 2);
}

TEST_CASE("Compiler audit: loop closures keep per-iteration locals when the register is reused", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local log = {}
local function put(x) table.insert(log, tostring(x)) end
local fns = {}
local function captured(p)
    local i = 0
    repeat
        i += 1
        local c = i * 10
        fns[#fns + 1] = function() return c end
        if i == p then break end
    until c >= 30
    local s = "cap" .. i
    put(s)
end
captured(2)
local n = 0
while n < 3 do
    n += 1
    local d = n * 2
    fns[#fns + 1] = function() return d end
end
local after = tostring(n)
put(after)
for _, fn in ipairs(fns) do put(fn()) end
print(table.concat(log, ",")))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: a snapshot of a global keeps its value after the global is written", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(counter = 0
local function bump()
    counter = counter + 1
end
local old = counter
counter = 10
print(old, counter)
local snap = counter
bump()
print(snap, counter)
local g = counter
for i = 1, 3 do
    print(g)
    counter = counter + 1
end
local none = not counter
local either = counter or 5
counter = nil
print(none, either))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a snapshot of an upvalue keeps its value after the upvalue is written", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local up = 1
local function bump()
    up = up * 3
end
local function g()
    local o = up
    up = up + 1
    local p = up
    bump()
    return o, p, up
end
print(g())
print(g()))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a copy of a captured local keeps its value after a call writes the local", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local x = 1
local function inc()
    x = x + 1
end
local a = x
inc()
print(a, x)
local flag = false
local function flip()
    flag = not flag
end
local was = not flag
flip()
print(was, flag)
local t = {}
local s = x
inc()
t[1] = s
print(t[1], x))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a multi-value call keeps every value when its register later holds a captured local", "[Decompiler][ReplayRegress][Semantics]") {
    const auto spread = R"LUA(print(obj(false));
repeat
    repeat
        ipairs("x", tostring);
    until false;
until (((12)));
local function f0(p1, p2, p3, ...)
    p2();
end
local function f1(p2, ...)
    f1(nil);
    f0();
end
f1, f1 = (((print[nil]))), math[f0]();)LUA";
    CheckSemanticParity(spread, 1, 1);
    CheckSemanticParity(spread, 1, 2);
    CheckSemanticParity(spread, 2, 2);
    const auto argument = R"LUA(t(obj(("hello")));
while tostring[print]:set() do
end
local v0, v1 = { [true] = print, y = tonumber, data = select }, (#0);
if function(p2, p3)
v1, v0 = p3, nil;
end then
end)LUA";
    CheckSemanticParity(argument, 1, 1);
    CheckSemanticParity(argument, 1, 2);
    CheckSemanticParity(argument, 2, 2);
}

TEST_CASE("Compiler audit: a reassigned local function captured by itself keeps one name through its loop merge", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f0(...)
    f0(0, 0);
end
while ((f0(false, print))) do
    f0 //= ((next));
end
return function(p2, p3, p4)
f0(p2);
end;)LUA";
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a condition's value term keeps its else edge when the then-body starts with a test", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local out = {}
local function g(sel, ip)
    if sel and (if ip then nil else 137.75) then
        if ip == 2 then
            table.insert(out, "A2")
        end
    else
        table.insert(out, "B2")
        for _ in pairs({}) do
        end
    end
end
g(true, true)
g(true, false)
g(false, true)
g(true, 2)
print(table.concat(out, ",")))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: unused leading results of a call keep later results in place", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function two()
    return 1, 2
end
local _, b = two()
print(b)
local p, q, r = string.find("xxabc", "(a)(b)")
print(r)
local first, _, third = string.byte("xyz", 1, 3)
print(first, third))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a constructor field reading a local declared after the table stays a store", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local v1 = {
    ["\n"] = -28i,
    ["x\n]"] = [[a
b]],
    nil,
    ["end"] = 93i,
}
local v2 = {  }
v1[0] = v2(-54i, 65i).field)LUA";
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a debug local reusing a table-field closure's register stays local", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local counters = {}
for i = 1, 2 do
    local mt = { __index = function(_, key)
        return key
    end }
    local total = i * 10
    counters[i] = function(step)
        total += step
        return total, mt.__index(nil, step)
    end
end
print(counters[1](1), counters[2](1), counters[1](1)))LUA";
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: closures written in a constructor or a return stay there", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local backing = { value = 8 }
local proxy = setmetatable({}, { __index = function(_, key)
    return backing[key]
end, __newindex = function(_, key, assigned)
    backing[key] = assigned
end })
local list = { function() return 1 end, function() return 2 end }
local M = {}
function M.run()
    return 3
end
local function make(n)
    return function(extra)
        return n + extra
    end
end
proxy.value = 9
print(proxy.value, list[1](), list[2](), M.run(), make(2)(3)))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 2);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    for (const char *expected : {"__index = function(", "__newindex = function(", "return function(", ".run()"})
        CHECK(output.find(expected) != std::string::npos);
    CHECK(output.find("local function __index") == std::string::npos);
    CHECK(output.find("run = function") == std::string::npos);
}

TEST_CASE("Compiler audit: an elseif chain before a return keeps its chain and branch order", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function full(step)
    if step == 0 then
        print("zero")
    elseif step > 0 then
        print("up")
    else
        print("down")
    end
    return step
end
local function partial(step)
    if step == 0 then
        print("zero")
    elseif step > 0 then
        print("up")
    end
    return step
end
print(full(1), full(-1), full(0), partial(1), partial(-1), partial(0)))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    size_t chains = 0;
    for (auto at = output.find("elseif 0 < arg0 then\n        print(\"up\")"); at != std::string::npos; at = output.find("elseif 0 < arg0 then\n        print(\"up\")", at + 1))
        ++chains;
    CHECK(chains == 2);
    CHECK(output.find("not (0 < arg0)") == std::string::npos);
}

TEST_CASE("Compiler audit: an empty while after a loop sharing the repeat header still tests its condition", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local t = setmetatable({}, { __index = function(_, key)
    print("get", key)
    return nil
end })
local rounds = 0
repeat
    while rounds > 5 do
        print("never")
    end
    while t.field do
    end
    rounds += 1
until rounds > 1
print(rounds))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: arms assigning a local before a shared return keep the assignment and the chain", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function advance(value, tag)
    print(tag)
    return value + tag
end
local function dispatch(selector)
    local result = 15
    if selector == 0 then
        result = advance(result, 0)
    elseif selector == 1 then
        result = advance(result, 1)
    elseif selector == 2 then
    else
        result = advance(result, 3)
    end
    return result
end
print(dispatch(0), dispatch(1), dispatch(2), dispatch(3)))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("elseif arg0 == 1 then\n        v1 = advance(v1, 1)") != std::string::npos);
    CHECK(output.find("return v2") == std::string::npos);
}

TEST_CASE("Compiler audit: a nested continue stays a continue instead of copying the loop tail", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(for i = 1, 4 do
    if i > 1 then
        if i == 2 then
            continue
        end
        print("a", i)
    end
    print("b", i)
end)LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("continue") != std::string::npos);
    CHECK(output.find("print(\"b\", i)") == output.rfind("print(\"b\", i)"));
    CHECK(output.find("else") == std::string::npos);
}

TEST_CASE("Compiler audit: a constructor read by nothing else builds its fields once", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(if { k = t.field } then
end
tostring[(("hello"))](tonumber(print, table), function(p0, p1, p2)
end))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a method defined after a mixed constructor stays a definition", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local obj = { value = 6, 6 }
function obj:bump(step)
    self.value += step
    return self.value
end
print(obj:bump(2), obj[1]))LUA";
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);

    const CompileLevels levels(1, 2);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find(":bump(") != std::string::npos);
    CHECK(output.find("bump = function") == std::string::npos);
}

TEST_CASE("Compiler audit: a captured local reusing a register stays local in each loop iteration", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local getters = {}
for i = 1, 2 do
    local t = {}
    local x = tostring(i)
    t.f = function()
        return x
    end
    local u = { id = i }
    u.self = function()
        return u
    end
    getters[i] = u.self
end
print(getters[1]().id, getters[2]().id))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a compound member assignment loads its target before the right-hand side runs", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local backing = { value = 2, 2 }
local proxy = setmetatable({}, { __index = function(ignored, key)
    print("get", key)
    return backing[key]
end, __newindex = function(ignored, key, assigned)
    print("set", key, assigned)
    backing[key] = assigned
end })
local function receiver()
    print("receiver")
    return proxy
end
local function operand(value)
    print("rhs", value)
    return value, 0
end
local total = 0
for outer = 1, 2 do
    for inner = 1, 4 do
        if inner % 2 == 0 then
            continue
        end
        receiver().value += operand(inner)
        total += backing.value
    end
end
local function capture(step)
    total += step
    return total, backing.value
end
print(capture(1))
return total, backing.value)LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: compound assignments to members, upvalues and globals keep their operator", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(X, K = 3, 1
local t = { v = 1, w = 2, 3, 4 }
local x, k = X, K
local function get()
    return t, x, k
end
t.v += x
t.w -= 1
t[k] *= x
t[2] //= 2
get().v += x
t.w = t.w + x
G = 1
G += 1
local total = 0
local function bump(step)
    total += step
    return total, tostring(step)
end
print(bump(2), t.v, t.w, t[1], t[2], G))LUA";
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 2);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    for (const char *expected : {"t.v += x", "t.w -= 1", "t[k] *= x", "t[2] //= 2", "get().v += x", "t.w = t.w + x", "G += 1", "total += ", "return total, tostring("})
        CHECK(output.find(expected) != std::string::npos);
}

TEST_CASE("Compiler audit: constructors passed as arguments or holding call results stay in place", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local backing = { value = 8 }
local proxy = setmetatable({}, { __index = function(_, key)
    return backing[key]
end })
local n = 0
local function tick()
    n += 1
    return n
end
local a = math.floor(tick() / 2)
local b = setmetatable({}, nil)
local c = rawlen({ 1, 2 })
local keyed = { k = tostring(1) }
local mixed = { k = tostring(2), 5 }
print({ k = { j = tick() } })
print({ k = tick() }, tick())
local s = { a = tick(), b = { c = tick() } }
print(rawlen({ tick(), tick() }), select("#", { k = tick() }))
print(proxy.value, a, b, c, keyed.k, mixed.k, mixed[1], s.a, s.b.c, n))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    for (const char *expected : {"setmetatable({  }, {\n    __index = function(", "setmetatable({  }, nil)", "rawlen({ 1, 2 })", "{ k = tostring(1) }",
                                 "{ k = tostring(2), 5 }", "print({ k = { j = tick() } })", "print({ k = tick() }, tick())", "{ a = tick(), b = { c = tick() } }",
                                 "print(rawlen({ tick(), tick() }), select(\"#\", { k = tick() }))"})
        CHECK(output.find(expected) != std::string::npos);
}

TEST_CASE("Compiler audit: a table built before a constructor is not moved after that constructor's raising fields", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local made = 0
local function make()
    made += 1
    error("make " .. made, 0)
end
local function build()
    local first = { -38, key = make(), 161 }
    local second = { data = (true).field, false, first(45) }
    return second
end
print(pcall(build))
print(made))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a constructor field reading earlier locals folds without reading a later value", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local g = tonumber("1")
local t = { x = g }
g = tonumber("5")
local h = tonumber("2")
local u = { h, k = h }
h = tonumber("7")
local w = tonumber("3")
local r = { y = -w }
w = tonumber("8")
print(t.x, g, u[1], u.k, h, r.y, w))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    for (const char *expected : {"= { x = v0 }", "= { v2, k = v2 }", "= { y = -v4 }"})
        CHECK(output.find(expected) != std::string::npos);
}

TEST_CASE("Compiler audit: code after an if whose arm returns early runs on every path", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local ticks = 0
local function wait()
    ticks += 1
    if ticks > 3 then
        error("stop", 0)
    end
end
local function run(a, b)
    if a then
        print("a")
    else
        if b then
            return
        end
    end
    print("loop")
    while true do
        wait()
    end
end
local function once(a, b)
    if a then
        print("a")
    else
        if b then
            return
        end
    end
    for i = 1, 2 do
        print(i)
    end
end
print(pcall(run, true, false))
ticks = 0
print(pcall(run, false, false))
print(pcall(run, false, true))
once(true, false)
once(false, false)
once(false, true))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: returns copied into an elseif chain's arms converge after the chain", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function advance(value, tag)
    print(tag)
    return value + tag
end
local function dispatch(selector)
    local result = 16
    if selector == 0 then
        result = advance(result, 0)
    elseif selector == 1 then
        result = advance(result, 1)
    elseif selector == 2 then
        result = advance(result, 2)
    else
        result = advance(result, 3)
    end
    return result
end
print(dispatch(0), dispatch(1), dispatch(2), dispatch(3)))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    fuzz::EnableLuauFlags();
    {
        const CompileLevels levels(1, 1);
        const auto output = fuzz::FullDecompile(source).output;
        INFO(output);
        CHECK(output.find("local v1 = 16\n    if arg0 == 0 then\n        v1 = advance(v1, 0)\n    elseif arg0 == 1 then") != std::string::npos);
        CHECK(output.find("    elseif arg0 == 2 then\n        v1 = advance(v1, 2)\n    else\n        v1 = advance(v1, 3)\n    end\n    return v1\n") != std::string::npos);
    }
    {
        const CompileLevels levels(2, 1);
        const auto output = fuzz::FullDecompile(source).output;
        INFO(output);
        CHECK(output.find("local v1 = 16\n    if arg0 == 0 then\n        print(0)\n        v1 += 0\n    elseif arg0 == 1 then") != std::string::npos);
        CHECK(output.find("    else\n        print(3)\n        v1 += 3\n    end\n    return v1\n") != std::string::npos);
    }
}

TEST_CASE("Compiler audit: a closing if keeps its source arm order without copied returns", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(x)
    if x ~= nil then
        print(1)
    else
        print(2)
    end
end
local function g(x)
    if x == 1 then
        print(1)
    elseif x == 2 then
        print(2)
    else
        print(3)
    end
end
f(nil)
f(1)
g(1)
g(2)
g(3))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("if arg0 ~= nil then\n        print(1)\n    else\n        print(2)\n    end\nend") != std::string::npos);
    CHECK(output.find("if arg0 == 1 then\n        print(1)\n    elseif arg0 == 2 then\n        print(2)\n    else\n        print(3)\n    end\nend") != std::string::npos);
    CHECK(output.find("return\n") == std::string::npos);
}

TEST_CASE("Compiler audit: a loop condition keeps a raising constructor after the index before it", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local seen = 0
local obj = setmetatable({}, { __index = function(_, key)
    seen += 1
    print("index", key)
    return {}
end })
local ok, message = pcall(function()
    repeat
    until obj[true][{ [nil] = ipairs, y = "hello" }]
end)
print(ok, seen)
ok = pcall(function()
    while obj[false][{ x = true, [nil] = true }] do
    end
end)
print(ok, seen))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: closures past the 256th function keep their own bodies", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local results = {}\n";
    for (int i = 0; i < 300; ++i)
        source += std::format("results[{}] = function(...) return {}, select('#', ...) end\n", i + 1, i);
    source += "local cases = { first = function(x) return x .. 'a' end, rest = function(...) return ... end }\n"
              "print(results[1](), results[257](1, 2), results[300](), cases.first('b'), cases.rest(4, 5))\n";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a loop variable read inside a materialized boolean term keeps its name", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local parts = { { Name = "Torso", Anchored = false }, { Name = "Other", Anchored = false }, { Name = "Head", Anchored = true } }
local function run(speaker)
    for _, part in pairs(parts) do
        if speaker and part.Anchored == false and part.Name == "Torso" == false and part.Name == "Head" == false then
            print(part.Name)
        end
    end
end
run(true))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("v.Name == \"Torso\" ~= false") != std::string::npos);
}

TEST_CASE("Compiler audit: a self-referencing local function reassigned in a branch stays local", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f0()
    return f0
end
if not tonumber("1") then
    f0 = 5
end
print(f0 == f0(), rawget(_G, "f0")))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("local function f0()") != std::string::npos);
}

TEST_CASE("Compiler audit: an and-condition holding a constructor keeps its else arm", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local t = { [true] = 1 }
local function check(flag)
    if flag and (t[true] == { [math] = pairs }) then
    else
        print("else", flag)
    end
end
check(false)
check(true))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);

    const CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto output = fuzz::FullDecompile(source).output;
    INFO(output);
    CHECK(output.find("if not arg0 or v0[true] ~= { [math] = pairs } then") != std::string::npos);
}

TEST_CASE("Compiler audit: inlined closures capture distinct arguments", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(
        R"LUA(local function f(n)
    return function() return n end
end
return f(2)(), f(-1)())LUA",
        2, 2
    );
}

TEST_CASE("Compiler audit: variadic SETLIST after a value branch keeps every result", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(c)
    local function multi() return 7, 8, 9 end
    local t = {if c then 1 else 2, multi()}
    return t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: variadic SETLIST keeps preceding keyed and chunk stores", "[Decompiler][ReplayRegress][Semantics]") {
    const auto keyed = R"LUA(local function f(c)
    local function multi() return 7, 8, 9 end
    local t = {x = 5, if c then 1 else 2, multi()}
    return t.x, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    const auto chunked = R"LUA(local function f(c)
    local function multi() return 7, 8, 9 end
    local t = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, if c then 17 else 18, multi()}
    return t[16], t[17], t[18], t[19], t[20]
end
return f(true), f(false))LUA";
    CheckSemanticParity(keyed, 1, 1);
    CheckSemanticParity(keyed, 2, 2);
    CheckSemanticParity(chunked, 1, 1);
    CheckSemanticParity(chunked, 2, 2);
}

TEST_CASE("Compiler audit: variadic SETLIST keeps a stable computed key", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(c)
    local function multi() return 7, 8, 9 end
    local k = if c then "x" else "y"
    local t = {[k] = 5, if c then 1 else 2, multi()}
    return t.x, t.y, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
    CheckSemanticParity(
        R"LUA(local function f(c)
    local function multi() return 7, 8, 9 end
    local t = {[if c then "x" else "y"] = 5, if c then 1 else 2, multi()}
    return t.x, t.y, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA",
        2, 2
    );
}
