//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>

namespace repeat_exit_regressions {
    struct Sample {
        std::string_view name;
        std::string_view source;
    };

    constexpr Sample kSamples[] = {
        {
            "batch-1/t0_1",
            R"LUA(local v0 = table.field;
v0({ "", [6] = v0 });
while (-(-function()
tonumber();
return ;
end)) do
    local v1 = v0[nil];
    if next() then
        continue
    end
end
repeat
    while (if ((if v0 then v0 else next)) then function(p1, ...)
p1(table);
t("a-b", nil);
v0(next);
return ;
end else v0()) do
        v0 = v0.field;
        v0 += string();
        if { false, x = v0, field = string } then
            continue
        end
    end
    select = (if true then ({ string, [93.5] = math } .. (-(-nil))) else (-(t)));
    if ("value").field.field then
        break
    end
until { [print:get(string)] = true, [pairs] = (475).field, k = pairs.field[obj()] };
table(true);
return (if ... then ("hello" + next) else 172.25), ...;
)LUA"
        },
        {
            "batch-1/t5_1",
            R"LUA(local v0, v1 = tostring[next:method(select)], ...;
local v2 = pairs;
pairs((t % (#true)), { [v0:method(78.75, 477)] = { y = false, y = t } });
repeat
    while ((if v1("x", math) then v1(obj, pairs) else function(p3)
v2();
v1();
return 21.5, false;
end)) do
        local v3 = true;
        for g4_0, g4_1 in v2(tostring) do
            for i6 = v2, g4_1 do
                g4_0();
                next(nil, true);
                g4_0(g4_0);
                if obj then
                    continue
                end
            end
            local function f6(p7, p8, p9)
                return v0;
            end
        end
        if v1:method(nil) then
            continue
        end
    end
    if next.field then
        break
    end
until { [tonumber:run()] = (#(nil >= 3.75)), (if nil then (if true then math else 214) else pairs) };
obj();
v2((function(p3, ...)
return nil, 812;
end));
if (...) then
end
for i3 = function(p3, p4, p5)
p3();
p5(nil, print);
return "";
end, (("value")), (string[977] * tostring) do
    local function f4(...)
        return nil, false;
    end
    local v5 = { [993] = obj, k = false, [911] = v1, ["x"] = 93.5 };
    while (if tonumber:get("key", false) then function()
return 4.25;
end else tostring.field) do
        local v6 = (not nil);
    end
    if ipairs:set("") then
        continue
    end
end
local v3 = ((#{ ["key"] = true, [v2] = true }) ~= ...);
v2, v0 = v0:get(), (#v3.field);
return (v1).field, nil;
)LUA"
        },
        {
            "batch-3/t0_1",
            R"LUA(for g0_0 in ipairs(false, next) do
    local v1 = (g0_0 ~= 156.5);
    g0_0();
    local function f2(p3, p4, p5)
        g0_0();
        return 38, true;
    end
end
local v0, v1 = function(p0, ...)
math("a-b");
p0(453);
tonumber(p0);
return ;
end, { next, field = false, ["key"] = 194.75, string };
while ((function(p2, p3)
print();
p3();
p2();
return print, nil;
end and (#nil)))[((if table then "hello" else "hello"))[(not (if 102.5 then table else next))]] do
    for i2 = table:set(nil, 821), function(p2)
p2(false, true);
v1(665);
return ;
end do
        if (not pairs) then
            local v3 = v1;
        else
            v1 = "key";
            for g3_0 in v0(928) do
                v1(nil);
                i2();
            end
        end
    end
end
while (#(pairs)) do
end
while ((-(-(#pairs))) == (if pairs then v0:get(v1, ipairs) else { [nil] = table, [32] = true, print, [true] = 237.75 })) do
    if obj[(if { y = v1 } then v1.field else (if false then nil else nil))] then
        local v2 = (#v0(nil));
        local v3 = { x = 99, [13.75] = v2, string, field = "" };
        if (function(p4, p5, p6, ...)
v0(select);
v1();
return math, obj;
end) then
            continue
        end
    end
    local v2 = v1[70]:get();
    if "" then
        break
    end
end
for g2_0, g2_1 in v1() do
    local function f4(p5, p6, ...)
        g2_1();
        return ipairs, tostring;
    end
end
v0, v0 = true, (if select then "hello" else next);
return nil, (v0).field;
)LUA"
        },
        {
            "batch-3/t0_2",
            R"LUA(while ((not function(p0, p1, ...)
string("value", nil);
obj();
next("key");
return nil;
end)).field do
    obj = function(p0, p1, p2)
local function f3(p4)
    obj();
    math(math, 191);
    f3();
    return ;
end
for i4 = ipairs, "hello" do
end
return print, 229;
end;
    repeat
        table %= {  };
        if (if print:get(tostring, false) then (ipairs) else (if 141.75 then t else nil)) then
            math();
            if "" then
                break
            end
        end
        for g0_0 in print() do
            for g1_0, g1_1 in g0_0(tostring, pairs) do
                print();
                g1_1(false, g0_0);
                if true then
                    break
                end
            end
        end
    until (tonumber(tonumber, print));
    ((if "" then t else "x"))((table.field ~= (if nil then select else "value")));
end
for i0 = string.field, function(...)
math();
return ;
end do
    i0();
    for i1 = tonumber, "a-b" do
        if i1 then
            continue
        end
    end
    i0(math);
end
table(next());
local function f0(p1, p2, p3)
    return tonumber;
end
local v1 = math[math].field;
local function f2(p3, p4)
    p4 //= (#math);
    local v5, v6, v7 = f2("", "value"), nil, (if "" then nil else true);
    return f2[tostring], tonumber:method();
end
({  })[f0:method(30.25, 97)]();
f0(f2[504], (if "key" then nil else nil));
return f0.field[""], table;
)LUA"
        },
    };

    constexpr Sample kStructureSamples[] = {
        {
            "batch-4/t7_4",
            R"LUA(while true do
    if string then
    else
    end
    table();
    tonumber(string, table);
end
if print:method() then
end
local v0 = "key";
for g1_0 in v0() do
end
for g1_0, g1_1 in v0() do
    local v3, v4 = true, nil;
    local v5 = nil;
    local v6, v7 = true, 394;
end
v0.field(v0.field, (true ~= false));
local function f1(p2, p3)
    return string, 182;
end
return (false).field;
)LUA"
        },
        {
            "batch-4/t5_5",
            R"LUA(repeat
    if t then
    else
        obj //= string;
        if (if ipairs then "value" else "key") then
            print("value");
            table(obj, "value");
            string(true, true);
        end
        obj(tostring);
    end
    for i0 = next(string, nil), (if false then "hello" else "value") do
    end
    if ipairs(select) then
        continue
    end
until (if function(p0, p1, p2)
p1(nil);
p2("");
p2(obj, nil);
return ;
end then (-select(tonumber)) else math());
while function(...)
for g0_0 in tonumber(nil) do
    if "" then
        continue
    end
end
print(79.5, true);
return ;
end do
    if { x = nil, data = 430, tonumber, ipairs } then
        while 576 do
            obj();
            select("", nil);
        end
        for i0 = "a-b", next do
            i0(nil, true);
            tonumber(tostring);
            print();
            if false then
                continue
            end
        end
        local v0, v1 = "", "";
    else
        local v0 = {  };
    end
    print[table]();
end
local v0, v1, v2 = (if { 96, select, data = pairs, [394] = math } then (true) else (false and select)), print:get(false, print), function()
return obj;
end;
v2 -= (obj).field;
return { "a-b", [tonumber] = "hello" };
)LUA"
        },
        {
            "batch-4/t6_2",
            R"LUA(repeat
    if { false, [select] = "key", field = "x" } then
        t();
    else
        obj();
    end
    local v0, v1, v2 = 122.75, "value", next();
    local v3 = "value";
until nil;
ipairs((string).field);
local v0 = (next(t)).field;
t();
local function f1(p2, p3, p4, ...)
    return { y = nil, nil, y = false, data = true };
end
v0[tonumber](nil, (if next then true else nil));
((v0 >= print))();
local v2, v3, v4 = v0:get("x", math), { t, data = table, [nil] = 54.75, [""] = true }, (-function(p2, p3, p4, ...)
p2(nil, true);
table(true);
return ;
end);
v0(string.field, (true));
while ... do
    v3, f1 = v3:run(), (-(-3));
end
return print(), (#true);
)LUA"
        },
    };

    constexpr Sample kFreshReplaySamples[] = {
        {
            "batch-4/t5_1",
            R"LUA(local function f0(p1)
    local v2, v3 = nil, t(nil, string);
    return v2();
end
local function f1(p2, p3, p4)
    f1(p4);
    p3(true, "key");
    return false, t;
end
for i2 = "key", (if { [false] = tostring } then ... else (if next then nil else string)), {  } do
    pairs[true](232, i2[977]);
    local v3 = "a-b";
end
while (-(-(nil).field)) do
    f1 = ...;
    repeat
        while math() do
            if 27.5 then
                break
            end
        end
        local function f2(p3, ...)
            string(949, nil);
            return false, tonumber;
        end
        if tostring(64, 233.75) then
            next();
            if "hello" then
                break
            end
        else
            math("a-b", true);
            f0();
        end
    until (not obj.field);
    f0(...);
end
for g2_0 in f0(f1.field) do
    if { data = "x" } then
        local v3 = tostring[select];
        while next(f1, next) do
            f0();
        end
        local v4 = function()
string(90);
f1(true);
table(v3);
return "x", g2_0;
end;
    end
    if ipairs then
        break
    end
end
repeat
until (f1 >= function(p2, ...)
p2(ipairs);
f1(nil, nil);
ipairs(true);
return true, select;
end);
if t[true][tostring:set(201.25, tonumber)]:method((-(-{ k = false, [ipairs] = false })), ...) then
    while function()
while f1[nil] do
    if tostring then
        break
    end
end
return t, "a-b";
end do
        for g2_0 in tostring(nil) do
            for i3 = ipairs, f1 do
                if false then
                    break
                end
            end
            while t[31.25] do
                f1("value", true);
            end
            local v3, v4 = nil, 0;
        end
        tostring((print / nil));
        local v2 = ...;
    end
else
    pairs[string()](f1:method(string));
    while (if ... then (if table(t, 34.25) then (math % false) else (select ~= pairs)) else (print)) do
    end
end
for i2 = nil, ipairs, tostring() do
    while 173.5 do
        ipairs("value");
    end
end
math.field();
local function f2(p3, p4, p5)
    for g6_0, g6_1 in select(nil, nil) do
        g6_0();
        ipairs(p5, nil);
    end
    tonumber = f1.field;
    return nil, 172;
end
return ({  } ~= ...);)LUA"
        },
        {
            "batch-2/t7_1",
            R"LUA(while select do
    repeat
        local v0, v1, v2 = "key", false, true;
        if obj["hello"] then
            if "value" then
                break
            end
        end
        if (tonumber) then
            break
        end
    until next.field.field;
    (228.25)(...);
end
t[116]();
repeat
    if string:set() then
    end
    for g0_0, g0_1 in obj() do
        for g2_0 in g0_0(false) do
            string();
            table(false);
        end
        local v2, v3, v4 = next, true, g0_1();
        for i5 = nil, tostring do
        end
    end
    if ... then
    end
until (...)[t[obj][{ field = nil, [false] = math, [nil] = obj }]];
ipairs(function(p0)
return ;
end);
return ("hello").field;)LUA"
        },
    };

    constexpr Sample kBranchOwnershipSamples[] = {
        { "batch-1/t0_1-branch-ownership", R"FISSION(while {  } do
    repeat
        if tonumber(nil) then
            select(t);
            if nil then
                continue
            end
        end
        select("x");
    until { nil, 411, table };
    string(ipairs:run(33.75));
    repeat
        string = 122;
        while (if 746 then select else select) do
            tonumber();
            obj(false);
            next(obj, nil);
        end
    until (select)[t];
end
local v0, v1 = nil, t.field();
local v2, v3 = (v0).field, 882;
for g4_0 in v3(nil, obj) do
    local v5, v6, v7 = 75, nil, string();
    local v8 = v2["x"];
end
while (#next[(false // pairs)]) do
    for i4 = v3[nil], math[obj] do
        for i5 = math, false, print do
            string(nil, "value");
            v0("hello", table);
            string(next, i4);
        end
        print("a-b");
    end
    while v0 do
    end
    if ((print - false) >= v0.field) then
        break
    end
end
v0, v0 = function(p4)
p4(true, "x");
tonumber(true, "value");
v1 -= 11.25;
return select, "a-b";
end, (function()
return table, 142;
end);
repeat
    local function f4(p5, p6, p7)
        repeat
            p5(142.75);
            v1();
            f4(print, false);
        until (if tonumber then ipairs else "value");
        for i8 = false, "", "a-b" do
            p6(string, nil);
            v1(t, true);
            ipairs(v3);
        end
        return nil;
    end
    if v0[true][v3.field].field then
        continue
    end
until (if "value" then next.field:method((false), (-nil)) else tonumber[next]:set());
string = (function(p4)
tonumber();
v2(v0, tostring);
obj(true);
return "a-b";
end);
if function(p4)
local v5 = print;
for g6_0 in p4(next, false) do
    g6_0(nil);
    print();
end
return nil;
end then
else
end
local v4 = (if v3:method() then v1() else function(...)
ipairs(v2, 55.75);
v3(tonumber);
return tonumber, next;
end);
return (tonumber)[{ [nil] = t }];
)FISSION" },
        { "batch-3/t4_3-condition-branch", R"FISSION(while (if (obj or string) then nil else t(false)) do
end
for g0_0 in pairs() do
    t += table;
    repeat
        t(nil, "x");
        g0_0();
    until print(tostring);
    for g1_0, g1_1 in g0_0() do
    end
end
string.field({ y = pairs, tostring }, print);
table[912][(not table)].field((-(-ipairs[(table - table)])), function(p0, p1, p2)
repeat
    p1(p0);
until tostring;
p2, p1 = "", "x";
return false, nil;
end);
while "x" do
    local v0, v1, v2 = { k = nil, ["key"] = 450, ["a-b"] = 925, false }, (ipairs ~= next), select("key", "value");
    v2 = v1:get("hello");
end
string.field(((t >= tostring)), (if math then (false == "key") else print.field));
for i0 = tonumber[pairs], (if (if nil then string else t:set(false)) then (-(-(not nil))) else (if pairs:set(false, "a-b") then math(903, print) else ...)), function(p0, ...)
local function f1(p2, p3)
    tostring(ipairs);
    p3(767, 184.75);
    return ;
end
return ;
end do
    for g1_0 in pairs(tostring()) do
        i0({ ["key"] = false, y = select }, i0:set());
        (tostring)();
    end
    i0 = 878;
    if (...) then
        continue
    end
end
for g0_0, g0_1 in obj() do
    if (if print then 134.5 else false) then
    else
        g0_1("key", false);
        if nil then
            continue
        end
    end
    for i2 = false, g0_0, "key" do
    end
    if obj:run(g0_0, true) then
        break
    end
end
return true, { x = "key", false };
)FISSION" }
    };

    constexpr Sample kDirectArmScopeSamples[] = {
        {
            "batch-1/t2_2",
            R"LUA(next(..., select[select]);
if ipairs[select] then
    if (if { y = nil, [ipairs] = "value" } then select.field else nil) then
        select = tostring[math];
        repeat
            tonumber(false);
            math(obj);
            next(next, nil);
        until nil;
    else
    end
    local v0 = (... .. (not true));
    (true)({  }, { v0, nil });
else
    local v0 = string:set(string, nil);
    repeat
        local v1 = v0();
    until v0:run();
end
repeat
    local v0 = (...);
    local v1 = { x = (t), y = pairs("value"), (if next then select else "x"), [ipairs.field] = ... };
until math[(t).field.field];
while (if (155) then select:set(true) else (if print then 11 else select)) do
    local v0 = (table and nil);
    if v0:get(pairs, next) then
        continue
    end
end
for g0_0 in ipairs(next) do
    for g1_0, g1_1 in g0_0(true, pairs) do
        next(g1_1);
        g1_0(false, string);
    end
    t();
    for g1_0, g1_1 in g0_0(nil) do
        tostring(pairs);
    end
end
table[...](ipairs.field.field, ...);
repeat
    obj();
until string[print].field;
return (function(p0)
ipairs(p0);
return nil;
end + { [396] = math });)LUA"
        },
        {
            "batch-2/t6_12",
            R"LUA(local v0 = { field = nil, [select:method(true, nil)] = next, 651, (if select then "x" else nil) };
local function f1(p2, p3, p4)
    return { y = nil, [false] = 817, ["x"] = ipairs }, v0.field;
end
for g2_0 in pairs() do
    pairs(table:method(next), (if t() then (if nil then t else nil) else ...));
    local v3 = t:get((not g2_0));
    local v4 = function()
return true;
end;
end
for g2_0 in f1(obj, v0) do
    repeat
        g2_0();
        select("value");
    until math;
    v0 = (table % 597);
    v0 ^= false;
end
local function f2(p3, p4, p5, ...)
    t.field(("a-b"));
    return ;
end
for g3_0, g3_1 in v0[71][{ tostring, x = true, x = false, [t] = 59.5 }]() do
end
f2.field((-ipairs), f2[nil]);
repeat
    repeat
        if (ipairs[574] < f2()) then
            break
        end
    until f1.field.field.field;
    if v0 then
        continue
    end
until "a-b";
for i3 = string.field, tonumber:get(false, "value") do
end
v0["x"](..., (not false));
return string;)LUA"
        },
        {
            "batch-3/t2_2",
            R"LUA(local v0 = function()
    return (true)[163i]
end
v0[{ ["\n"] = print, if 135 then nil else nil }] = v0(nil)[string("key")] > (function()
    return true
end).y
local v1 = {
    { ["x\n]"] = 56.857142857142854 },
    (nil)[false],
    ("key").x,
    { 25i, ["x\n]"] = 233, "x" },
}
local v2 = if (nil)["a-b"] then { if game then v0 else nil, (93i)[true], ["x\n]"] = table["a-b"], false } else function()
    return true
end
local v3 = - -function()
    return "\n"
end
game(89i, {
    "x" < nil,
    ["field"] = v2,
    function()
        return 34i
    end,
})
local v4 = v2(- -obj)
local v5 = select()
v2[{ 81i - true, (0.14285714285714285)[118i], ["data"] = v1(false, 152i) }] = function()
    return - -51.285714285714285
end ~= 73.28571428571429
return (54)[(11i)["a-b"]])LUA"
        }
    };

    constexpr Sample kStructuralSemanticSamples[] = {
        {
            "batch-4/t1_4",
            R"LUA(while (if ((48)) then (if (if tostring then tostring else 137.25) then ipairs:set() else t("", nil)) else ipairs:set(nil, ipairs)) do
    while (nil) do
        for g0_0, g0_1 in t(string, 545) do
        end
        for i0 = ipairs, 311, nil do
            table(ipairs);
            i0();
            i0(194.5, true);
        end
    end
    local function f0(p1, p2, ...)
        t(table, p2);
        p2(string, false);
        return pairs;
    end
    local v1 = (string)[(-(-false))];
    if tonumber then
        break
    end
end
repeat
    print[...]((if (-nil) then { k = nil, "", [nil] = nil } else table[ipairs]));
until (not t[t[(not false)]]);
while (print(nil, "key"))[math.field] do
    for g0_0, g0_1 in math(obj, obj) do
        g0_0(true, string);
    end
    for g0_0 in table("a-b") do
        g0_0(nil);
    end
end
repeat
    while (if true then tonumber else "value") do
        next(nil);
        string("x", print);
        print(tostring);
    end
    repeat
        math();
        tonumber(true, string);
        tonumber();
    until (pairs % true);
until nil;
if { [tonumber] = pairs } then
    for g0_0 in next(tostring, string) do
        string("hello", nil);
        g0_0(674, next);
        g0_0(true, g0_0);
    end
    math(tonumber, print);
end
return table.field, ((ipairs .. t));)LUA"
        },
        {
            "batch-3/t1_3",
            R"LUA(while {  } do
    if function(p0, p1, p2)
return ;
end then
        if (if function(...)
tonumber();
tonumber(126, true);
return false;
end then ("hello" and nil) else {  }) then
            break
        end
    end
    for i0 = (if (if pairs then 74.75 else print) then select.field else (print - 166.5)), pairs(nil), (#{ x = true, [""] = table, ["value"] = next }) do
        if i0:get() then
            continue
        end
    end
end
ipairs = pairs[math][(-(-"x"))].field;
tostring(ipairs(tonumber, pairs), string);
local function f0(p1, p2)
    t(tonumber);
    ipairs(select);
    p2(nil);
    return ;
end
f0({ ipairs, x = true, 855 });
local function f1(p2, p3)
    f0("");
    f0(print);
    return 547;
end
for i2 = { select }, f1.field.field do
end
repeat
    tonumber();
    while (84.75)[tonumber:get()] do
        local function f2(p3, p4, ...)
            t(true, nil);
            return nil, false;
        end
    end
    local function f2(p3)
        f2(true, string);
        return ;
    end
until 213;
return (-pairs);)LUA"
        }
    };

    void CheckRepeatExit(const Sample &sample, bool requireMatch = false, std::string_view expectedError = {}) {
        fuzz::EnableLuauFlags();
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(
            std::string(sample.source), DecompilerFlags::FissionDebugNotes, Luau::CompileOptions{fuzz::kOpt, fuzz::kDebug}
        );
        INFO(result.decompilationOutput);
        INFO(result.debugNotes);
        REQUIRE(result.resultCode == DecompileResult::Success);

        std::string originalBytecode;
        std::string reconstructedBytecode;
        REQUIRE(fuzz::LuauCompiles(std::string(sample.source), &originalBytecode));
        REQUIRE(fuzz::LuauCompiles(result.decompilationOutput, &reconstructedBytecode));

        const auto preludes = fuzz::CompilePreludes(fuzz::LuauCompiles);
        REQUIRE(preludes.size() == fuzz::kSemPreludeCount);
        const auto verdict = fuzz::CompareSemantics(originalBytecode, reconstructedBytecode, preludes);
        INFO("fixture " << verdict.fixture << " original: " << verdict.original.trace << " reconstructed: " << verdict.decompiled.trace);
        if (requireMatch)
            CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
        else
            CHECK(verdict.kind != fuzz::SemVerdict::Kind::Diverge);
        if (!expectedError.empty()) {
            const auto originalFirst = fuzz::RunLuauTrace(originalBytecode, preludes.front());
            const auto reconstructedFirst = fuzz::RunLuauTrace(reconstructedBytecode, preludes.front());
            REQUIRE(originalFirst.status == fuzz::SemTrace::Status::Error);
            REQUIRE(reconstructedFirst.status == fuzz::SemTrace::Status::Error);
            REQUIRE(originalFirst.error.has_value());
            REQUIRE(reconstructedFirst.error.has_value());
            CHECK(*originalFirst.error == expectedError);
            CHECK(reconstructedFirst.error == originalFirst.error);
            CHECK(reconstructedFirst.trace == originalFirst.trace);
        }
    }

    void CheckNoGeneratedForward(const Sample &sample) {
        fuzz::EnableLuauFlags();
        const std::string source(sample.source);
        const auto result = fuzz::FullDecompile(source);
        INFO(sample.name);
        INFO(result.output);
        REQUIRE(result.code == DecompileResult::Success);
        std::string bytecode;
        REQUIRE(fuzz::LuauCompiles(result.output, &bytecode));
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.output, &source));
    }
} // namespace repeat_exit_regressions

TEST_CASE("Repeat exits keep post-loop statements outside repeat", "[Decompiler][FuzzRegress][RepeatExit]") {
    for (const auto &sample : repeat_exit_regressions::kSamples) {
        DYNAMIC_SECTION(sample.name) {
            repeat_exit_regressions::CheckRepeatExit(sample);
        }
    }
}

TEST_CASE("Repeat break bypasses its latch while the other branch reaches it", "[Decompiler][FuzzRegress][RepeatExitSemantic]") {
    constexpr repeat_exit_regressions::Sample sample{
        "observable-break-and-latch",
        R"LUA(local count = 0
repeat
    count += 1
    if count == 2 then
        break
    end
until count > 5
return count)LUA"
    };
    repeat_exit_regressions::CheckRepeatExit(sample, true);
}

TEST_CASE("Infinite while preserves effects in its unconditional latch", "[Decompiler][FuzzRegress][RepeatExitSemantic]") {
    constexpr repeat_exit_regressions::Sample sample{
        "observable-no-exit-while-latch",
        R"LUA(while true do
    if string then
    else
    end
    table("latch")
end)LUA"
    };
    repeat_exit_regressions::CheckRepeatExit(sample, false, "attempt to call a table value");
}

TEST_CASE("Repeat and while reconstruction preserves branch execution", "[Decompiler][FuzzRegress][RepeatStructure]") {
    for (const auto &sample : repeat_exit_regressions::kStructureSamples) {
        DYNAMIC_SECTION(sample.name) {
            repeat_exit_regressions::CheckRepeatExit(sample);
        }
    }
}

TEST_CASE("Fresh saved repeat failures remain reconstructible", "[Decompiler][FuzzRegress][RepeatFreshReplay]") {
    for (const auto &sample : repeat_exit_regressions::kFreshReplaySamples) {
        DYNAMIC_SECTION(sample.name) {
            repeat_exit_regressions::CheckRepeatExit(sample);
        }
    }
}

TEST_CASE("Loop branches retain their guarded effects", "[Decompiler][FuzzRegress][RepeatBranchOwnership]") {
    SECTION("batch-1/t0_1") {
        repeat_exit_regressions::CheckRepeatExit(
            repeat_exit_regressions::kBranchOwnershipSamples[0], false, "invalid argument #1 to 'select' (number expected, got string)"
        );
    }
    SECTION("batch-3/t4_3") {
        repeat_exit_regressions::CheckRepeatExit(
            repeat_exit_regressions::kBranchOwnershipSamples[1], false, "missing argument #1 to 'pairs' (table expected)"
        );
    }
}

TEST_CASE("Repeat branch effects stay conditional across the back-edge", "[Decompiler][FuzzRegress][RepeatBranchOwnership]") {
    constexpr repeat_exit_regressions::Sample sample{
        "controlled-repeat-branch",
        R"LUA(local count = 0
repeat
    count += 1
    if count == 1 then
        print("guarded")
        if nil then
            continue
        end
    end
until count >= 2
return count)LUA"
    };
    repeat_exit_regressions::CheckRepeatExit(sample, true);
}

TEST_CASE("Conditional while setup executes only its selected arm", "[Decompiler][FuzzRegress][RepeatBranchOwnership]") {
    constexpr repeat_exit_regressions::Sample sample{
        "controlled-conditional-while",
        R"LUA(local function mark()
    print("wrong")
    return true
end
while (if string then nil else mark()) do
    print("body")
    break
end
return 7)LUA"
    };
    repeat_exit_regressions::CheckRepeatExit(sample, true);
}

TEST_CASE("Direct branch arms keep generated locals in scope", "[Decompiler][FuzzRegress][RepeatBranchOwnership][ForwardDeclaration]") {
    for (const auto &sample : repeat_exit_regressions::kDirectArmScopeSamples) {
        DYNAMIC_SECTION(sample.name) {
            repeat_exit_regressions::CheckNoGeneratedForward(sample);
        }
    }
}

TEST_CASE("Loop branch structure preserves the first observable error", "[Decompiler][FuzzRegress][RepeatBranchOwnership][StructuralSemantics]") {
    SECTION("batch-4/t1_4") {
        repeat_exit_regressions::CheckRepeatExit(
            repeat_exit_regressions::kStructuralSemanticSamples[0], false, "attempt to index function with 'set'"
        );
    }
    SECTION("batch-3/t1_3") {
        repeat_exit_regressions::CheckRepeatExit(
            repeat_exit_regressions::kStructuralSemanticSamples[1], false, "attempt to index function with 'field'"
        );
    }
}
