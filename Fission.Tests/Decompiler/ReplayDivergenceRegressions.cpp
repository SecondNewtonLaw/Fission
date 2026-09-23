//
// Created by Dottik on 22/9/2026.
//

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

TEST_CASE("Replay: shared short-circuit arms run on every path", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(math = (if (t.field and ...) then function(p0, p1, p2, ...)
end else print((true <= false)));)LUA");
    CheckSemanticParity(R"LUA(if ((string and (if select then false else 0)) or tonumber(table, 0)) then
else
end
for g0_0 in string({ pairs, [t] = pairs, false, [0] = 0 }) do
end)LUA");
}
