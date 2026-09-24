//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

// Regression guard for the 15 fuzzer-found INVALID_RECOMPILE samples (run-once `repeat ... until
// <truthy>` wrapping for-loops / continue / break). Each sample once decompiled to syntactically
// invalid Luau ("break outside a loop" / "Expected 'until'"). The fix set: CFA FORxPREP-header
// guards + dead-FORNLOOP-latch recovery, ASTLifter infinite-while tail-fold, and numeric-for
// body = FORNPREP fall-through (empty-body). The durable invariant these lock in: the decompiled
// output must be valid Luau (it recompiles). Sources are embedded verbatim so the test survives
// deletion of the scratch fuzz corpus. Decompile uses the same opt/debug (1, 2) the fuzzer used.
#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cctype>
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

// Luau::compile encodes a compile error as a buffer whose first byte is 0 followed by the message.
static bool Recompiles(const std::string &source, std::string *errorOut) {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const std::string bc = Luau::compile(source, opts);
    const bool ok = !bc.empty() && bc[0] != '\0';
    if (!ok && errorOut)
        *errorOut = bc.size() > 1 ? bc.substr(1) : "(empty)";
    return ok;
}

// Decompile a fuzz source, then require the result to be valid, recompilable Luau.
static void CheckDecompileRecompiles(const std::string &source) {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source); // opts default to {1, 2}

    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    std::string err;
    const bool ok = Recompiles(result.decompilationOutput, &err);
    INFO("recompile error: " << err);
    CHECK(ok);
}

TEST_CASE("Self-referential constructor crash reproducer completes", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
pairs()
local v0 = { not false }
v0[v0] = (-44i)[nil] % 944
local v1 = function() return (-"x\n]")[nil] end
local v2 = function() return { true } end
v0[not true] = ("\nhello").y
local v3 = (function() return "hello" end)[{ 416 }]
local v4 = game().y
return function() return nil end, "end"
)LUA");
}

TEST_CASE("Regress fuzz t0_1: run-once repeat wraps generic-for with conditional break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
next.field();
(false)();
tonumber[pairs[obj[false]]]("hello");
table = (-(-print[math]));
select = ipairs[next].field[(#select)][function(p0, ...)
p0 -= p0;
local function f1(p2, p3, p4, ...)
    select();
    return 134;
end
p0(select, print);
return obj;
end];
local v0, v1 = tostring[print:set()], t.field:get();
repeat
    if "key" then
        for g2_0, g2_1 in v0() do
            if true then
                break
            end
        end
        if (if v0 then "key" else "") then
            next(false);
        end
        while true do
            v1(35.75, 542);
            v1("a-b", nil);
        end
        if ... then
            break
        end
    end
until ...;
return function(...)
table(false, false);
v0(print, string);
return nil, "x";
end, ...;
)LUA");
}

TEST_CASE("Regress fuzz t1_1: empty numeric-for body must not swallow following code", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
ipairs[ipairs][...](..., tostring(select, nil));
local v0 = ...;
if { k = 81, [false] = "value", [477] = nil, ["value"] = string } then
    v0(65);
    for i1 = nil, select, pairs do
    end
else
    while true do
        v0(nil, nil);
    end
    while string.field do
        math(string);
        if tostring then
            continue
        end
    end
end
v0 ..= ...;
local v1, v2 = v0.field, (v0);
v0, v1 = (if tostring then (v2[ipairs] ^ ...) else v0[(table)]), (...);
v1[nil]({  }, function(p3)
return "hello";
end);
for g3_0, g3_1 in v2(v2.field, obj:method(t, 8.5)) do
    for i5 = (if (-(-table)) then 811 else (t)), function(p5, p6, p7)
return 183.5;
end, g3_1(nil, 417) do
        i5 //= t;
        while function(p6, p7, ...)
ipairs(nil, 637);
return 396;
end do
            v1(nil, string);
            while (#nil) do
            end
            i5();
        end
    end
    local v5 = tostring["hello"].field.field;
    v5, v1 = ((338 <= "a-b") == function(p6, p7, p8)
v5("value");
string(tostring);
return 251, 859;
end), (-{ nil, next });
    if (-(-false)) then
        break
    end
end
return function(p3, p4)
return "x", v0;
end, v2(966, 97);
)LUA");
}

TEST_CASE("Regress fuzz t3_1: run-once repeat wraps for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
while "" do
    for g0_0, g0_1 in tostring() do
        if ((-(-11))) then
            break
        end
    end
    repeat
        while nil do
        end
        for i0 = t[ipairs], obj[""] do
        end
    until false;
end
ipairs();
((nil))();
local function f0(p1)
    p1();
    f0();
    tonumber(obj);
    return "hello", nil;
end
for g1_0, g1_1 in f0() do
    local v3 = table:run(nil, next);
    for g4_0 in t(12, false) do
        string(nil);
        g4_0();
        table();
        if g4_0 then
            continue
        end
    end
    g1_0(tostring, math);
end
repeat
    local v1 = (f0[math] == f0:get(nil));
    v1 += (340);
until (-(nil)[(if tostring then f0 else "x")]);
return table, math;
)LUA");
}

TEST_CASE("Regress fuzz t3_2: run-once repeat nesting must close (until)", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
obj(function(p0, p1)
p1, p0 = 9, 55.5;
return print;
end);
for i0 = { field = obj, [math.field] = (table), x = print:set(next, nil) }, math do
    i0(i0:method(tostring), (#(obj)));
end
local function f0(p1)
    return ;
end
for i1 = ..., f0[nil], (44) do
end
for i1 = nil, "" do
    for g2_0 in f0(true) do
        f0 -= 101.25;
    end
    local v2, v3 = false, f0();
    if select[i1()] then
        continue
    end
end
select = select[{ 989, field = true, y = obj, data = "value" }];
local function f1(p2)
    if (#{ [123] = string, x = "hello" }) then
        for i3 = "a-b", 189, true do
            i3(181);
        end
        for i3 = select, "x" do
            ipairs(i3, f1);
            i3();
        end
    else
        p2(true);
        while ("hello") do
            f1(t);
            string(select);
        end
        while ipairs.field do
            f0(nil, table);
            obj(pairs);
        end
    end
    repeat
        f0(math, nil);
        tonumber();
    until next:set(math, p2);
    return next:method(), (next and "value");
end
f1 *= (false / pairs);
(f0)("x", f1);
return (f1(table, true)), select;
)LUA");
}

TEST_CASE("Regress fuzz t5_1: run-once repeat + continue wraps numeric-for whose body always breaks", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
repeat
    for i0 = 150, t, select do
        if 57 then
            break
        end
    end
    if "" then
        continue
    end
until function(p0, p1, ...)
p0(p0, t);
p1(p0, "hello");
p1(30);
return ;
end;
local function f0(p1, p2)
    local function f3(p4, p5, p6)
        f0(91);
        t(true);
        obj();
        return nil, "a-b";
    end
    for g4_0 in select(ipairs) do
        g4_0();
        tonumber(obj);
        p1(select);
    end
    local v4 = p2:run(162.5);
    return nil;
end
string = next[f0:set()];
return (tostring(0.25, tostring));
)LUA");
}

TEST_CASE("Regress fuzz t6_1: run-once repeat wraps for-loop with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local function f0(p1, p2, p3)
    for i4 = next, false do
        if p1 then
            break
        end
    end
    f0, p2 = f0, true;
    return ;
end
f0((((-(-f0)) and f0.field) < 270));
for g1_0, g1_1 in f0(pairs, math) do
    math();
    math //= f0;
end
f0(f0:set());
local v1, v2 = pairs["a-b"].field, (select)[select("value")];
f0, f0 = pairs, ((-(-(244.5))));
repeat
    for g3_0 in v2(true, tostring) do
        if "x" then
            break
        end
    end
    while "x" do
        f0();
        pairs("key", ipairs);
    end
    f0 ..= "value";
    if string[nil] then
        continue
    end
until (#...);
return (nil);
)LUA");
}

TEST_CASE("Regress fuzz t7_1: run-once repeat nesting must close (until)", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local function f0(p1, p2, ...)
    return ipairs, true;
end
if (pairs() > (if (if f0(tostring) then pairs else ("value" - tonumber)) then (if ... then "x" else function(p1)
string();
return 115.25;
end) else f0())) then
    for g1_0, g1_1 in tonumber[613]() do
        (print)((-(-table)), 734);
        g1_1.field({ [385] = g1_0, [nil] = t }, g1_1[t]);
        local v3 = g1_1:run();
    end
else
    if (if f0["x"].field then "hello" else f0[54.25][function(p1, ...)
f0();
p1();
return nil;
end]) then
        local v1, v2 = t[print], t.field;
        for i3 = (41.25 or t), (tonumber * true) do
            local v4 = f0("a-b");
            math();
        end
    else
        repeat
            if string then
                continue
            end
        until (not (if 123 then true else "x"));
        for g1_0, g1_1 in string(tostring, "value") do
            local v3 = next.field;
        end
        repeat
            if (not pairs) then
                break
            end
        until { [obj] = nil, k = nil, ["key"] = f0, nil };
    end
end
while select(..., (if f0 then false else f0)) do
    while tostring(327) do
        table();
        local v1 = next;
    end
    f0 = nil;
    (print)(...);
end
while f0[nil]() do
    local v1, v2, v3 = ..., (not math), { [tonumber] = ipairs, t, k = 742 };
    v2, f0 = (v1 ~= nil), (if "x" then "" else "key");
end
f0 = (math:set("x"));
local v1, v2, v3 = string.field:get((tonumber > "a-b"), "hello"), (select("value", pairs) < nil), (f0(nil))("value");
v2 += pairs;
return ipairs.field;
)LUA");
}

TEST_CASE("Regress fuzz t9_1: run-once repeat wraps for-loop with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
while t["a-b"].field:get() do
    repeat
        (next)(pairs:get(), select:set("key"));
    until (-(-(nil % t[pairs])));
    while ipairs.field do
    end
    if pairs.field:get(next.field) then
        break
    end
end
table(function(p0)
return ipairs, p0;
end, true);
(table)(t, tostring());
repeat
    for i0 = 136.75, obj, false do
        if "" then
            break
        end
    end
    if "x" then
        if "hello" then
            continue
        end
    end
until function(p0, ...)
p0();
return ;
end;
return function()
select();
next("value", nil);
string(nil);
return 44.5;
end, { k = "x", [print] = nil, [true] = nil, x = false };
)LUA");
}

TEST_CASE("Regress fuzz t10_1: nested run-once repeat / for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local function f0(p1, p2)
    if (tostring) then
        ipairs ^= ipairs;
        for i3 = tonumber, t, "hello" do
            ipairs(123);
            p2();
        end
    else
        ipairs();
    end
    return { nil };
end
f0["hello"].field[function(p1, p2, p3)
p1(p3);
obj();
p3();
return next, "x";
end]((obj(nil))[(function(p1)
t(nil);
p1();
f0();
return false;
end)], function(p1, p2)
return pairs, "hello";
end);
if function(p1, p2, ...)
if (ipairs >= p1) then
    tonumber(true, 623);
else
    p1(nil);
end
local function f3(p4, p5, ...)
    ipairs(215);
    f3(table);
    return ;
end
local function f4(p5, p6, p7)
    return table, "hello";
end
return 187, math;
end then
    tonumber(math);
    if (if "a-b" then { f0, math, 981, [print] = "value" } else { [246] = nil, nil, [363] = false }) then
        local function f1()
            return ;
        end
        f0 -= nil;
        f1, f1 = "x", nil;
    else
        f0 += nil;
        print();
    end
end
local v1, v2 = { field = "a-b", x = function(p1, p2, ...)
return ;
end }, next[{ ["key"] = pairs, false, string, data = t }][(-(-(string)))];
v2.field.field(v2.field, v1.field:run(math(pairs)));
repeat
    v2 //= ...;
    (nil)();
    for i3 = (if nil then f0 else "key"), function(p3, ...)
f0();
return tostring;
end do
        if (41.75) then
            break
        end
    end
    if ((180)) then
        continue
    end
until function(p3, p4, ...)
v2();
repeat
    t();
    math("hello");
    f0(t);
until p3();
return ;
end;
local v3 = function(...)
v1(table);
return ;
end;
local v4, v5 = ..., f0.field(..., ...);
next[f0].field(v5, (#{ [true] = nil, next, [obj] = t, y = nil }));
return function(p6, p7, ...)
table();
p6(math, "key");
return ;
end, print;
)LUA");
}

TEST_CASE("Regress fuzz t11_1: run-once repeat wraps for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local v0 = ...;
for i1 = v0, (169.75) do
    t("hello", i1);
    t ^= nil;
    repeat
        if i1 then
            continue
        end
    until v0[i1];
    if ... then
        continue
    end
end
t({ [ipairs] = 179.5, [false] = v0, [tostring] = true }, { true, "", 124, false });
while "value" do
    for g1_0, g1_1 in t(true, select) do
        if "value" then
            break
        end
    end
    local v1 = v0[541];
    repeat
        v0(nil, tonumber);
    until (false and ipairs);
end
for i1 = t, tonumber() do
    local v2 = (if tostring then 350 else ipairs);
end
print.field(v0.field, function(p1, p2, p3)
p1(true);
select();
p3(nil, 49.5);
return v0;
end);
return (v0[false]), (if { [true] = true, obj, [math] = nil, [nil] = 910 } then (nil) else (if 136.75 then tonumber else false));
)LUA");
}

TEST_CASE("Regress fuzz t13_1: run-once repeat wraps generic-for, latch survives", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
math((true), (nil ~= 764));
repeat
    local v0 = (string - 157);
    for g1_0, g1_1 in v0() do
        if 628 then
            break
        end
    end
    math(false);
    if ("key") then
        break
    end
until function()
print(false);
return ;
end;
t[select]();
ipairs();
local v0 = ...;
next(print);
if v0[next.field] then
    local v1, v2, v3 = pairs, tonumber, v0(math, 232.75);
    v2 = (if "hello" then math else "value");
    if v0.field then
        v1(nil, print);
        v0("x");
    end
else
    ipairs -= "a-b";
    for g1_0, g1_1 in ipairs(ipairs) do
    end
end
ipairs(nil);
if select:run() then
else
    while (if { false, next, [nil] = print, select } then ... else ipairs[""]) do
        math = (-(-129));
    end
    local v1 = function(p1)
v0(ipairs, nil);
return ;
end;
    v0, v1 = v1.field, (if tostring then "a-b" else false);
end
return v0.field;
)LUA");
}

TEST_CASE("Regress fuzz t14_1: run-once repeat wraps for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local v0 = ((t));
t(v0("value", nil));
local v1 = ...;
local v2 = (696).field;
for g3_0, g3_1 in t.field() do
    g3_0, v1 = nil, v0.field;
    local v5 = g3_1.field[false];
end
local v3, v4 = ..., v1(v0, nil);
v0["value"].field(nil, (v3("a-b")));
if (if (-(-"hello")) then table else (-(-(if v1[string] then { v2, true } else {  })))) then
    local v5 = { [35.75] = (""), y = v4:set(), data = v1:method(), (if 842 then v3 else t) };
    for i6 = (if (true) then select.field else function()
v4(obj);
ipairs();
return string;
end), 965 do
    end
    if (... > v2[38.25]) then
        v1, v1 = "", v0.field;
        repeat
            for i6 = table, false, ipairs do
                if "a-b" then
                    break
                end
            end
            repeat
                if "x" then
                    continue
                end
            until (-table);
            if false then
                continue
            end
        until select;
    end
end
return function()
v0("a-b", "");
print("x", "value");
return 72;
end;
)LUA");
}

TEST_CASE("Regress fuzz t19_1: run-once repeat wraps for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
for g0_0, g0_1 in tostring() do
    g0_0(g0_0, 221);
    local v2 = (not nil);
end
select = ...;
if (-tostring("a-b", true)) then
    repeat
        pairs();
        for i0 = 592, tonumber do
            if true then
                break
            end
        end
        if (if 971 then "key" else nil) then
            break
        end
    until (table);
    obj.field((string), (false));
    for i0 = (-print), (#math), string[pairs] do
    end
else
    tonumber(print.field);
    local v0 = tonumber[t].field;
    local function f1(p2, p3, p4, ...)
        table();
        v0("hello", "hello");
        return ;
    end
end
ipairs[tostring](next.field, ...);
local function f0(p1, p2, ...)
    for g3_0, g3_1 in obj(obj) do
        if nil then
            continue
        end
    end
    local v3, v4, v5 = "", math, p1();
    return ;
end
local v1, v2, v3 = ("a-b"), (-(-"hello")), (false);
v2 += (if "value" then true else v3);
for i4 = (not function(p4, p5, ...)
string();
print(false, pairs);
p4();
return ;
end), (if (if "a-b" then 114.5 else true) then f0.field else 628) do
    local function f5(p6, p7, p8)
        table(p7);
        obj("a-b");
        return ;
    end
    f0[158.25](f0[table], { [print] = next, field = 975, k = "hello" });
    repeat
    until ...;
end
local v4 = f0[v3:method(v1)];
v2 = 114.75;
return (function(p5, p6, p7)
obj("value", true);
return "";
end), (tostring.field);
)LUA");
}

TEST_CASE("Regress fuzz t19_2: run-once repeat / phantom-loop", "[Decompiler][Roundtrip][FuzzRegress]") {
    CheckDecompileRecompiles(R"LUA(
local function f0(p1, ...)
    string(next);
    return "value";
end
if { [math] = "", [true] = nil } then
    for i1 = 194.25, t do
        t();
        f0();
        obj(i1, 389);
    end
    for g1_0 in pairs() do
        f0(false, nil);
        f0(ipairs);
        obj("value");
    end
    f0(obj);
end
repeat
    for g1_0 in f0() do
        if (not (false)) then
            break
        end
    end
    while "hello" do
        ipairs[112.25]((if "" then 873 else f0));
        if (-(-{  })) then
            continue
        end
    end
    for g1_0, g1_1 in f0.field() do
    end
until f0.field:set("key", nil);
for g1_0, g1_1 in (tostring:run(387))() do
end
f0 -= function()
t();
return "key";
end;
return function(p1, p2, p3, ...)
pairs(true);
table();
return "x";
end;
)LUA");
}

// A comparison whose branch is a no-op (`if a < b then continue end` at a loop-body end, or an
// `if a < b then end`) compiles to a JUMPIF* with D==1 (jump to the next instruction). The Luau VM
// still evaluates `a <op> b`, which raises on incompatible types. The comparison must survive as
// `if a < b then end`. This asserts both recompilation and that the
// throwing comparison is not dropped.
