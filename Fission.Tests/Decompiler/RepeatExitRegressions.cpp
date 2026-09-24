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

    constexpr Sample kBranchOwnershipSamples[] = {
        {"batch-1/t0_1-branch-ownership", R"FISSION(while {  } do
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
)FISSION"},
        {"batch-3/t4_3-condition-branch", R"FISSION(while (if (obj or string) then nil else t(false)) do
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
)FISSION"}
    };

    constexpr Sample kDirectArmScopeSamples[] = {
        {"batch-1/t2_2",
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
end + { [396] = math });)LUA"},
        {"batch-2/t6_12",
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
return string;)LUA"},
        {"batch-3/t2_2",
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
return (54)[(11i)["a-b"]])LUA"}
    };

    constexpr Sample kStructuralSemanticSamples[] = {
        {"batch-4/t1_4",
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
return table.field, ((ipairs .. t));)LUA"},
        {"batch-3/t1_3",
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
return (-pairs);)LUA"}
    };

    void CheckRepeatExit(const Sample &sample, bool requireMatch = true, std::string_view expectedError = {}) {
        fuzz::EnableLuauFlags();
        Decompiler decompiler{};
        const auto result =
            decompiler.DecompileTestCode(std::string(sample.source), DecompilerFlags::FissionDebugNotes, Luau::CompileOptions{fuzz::kOpt, fuzz::kDebug});
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

TEST_CASE("Loop branches retain their guarded effects", "[Decompiler][FuzzRegress][RepeatBranchOwnership]") {
    SECTION("batch-1/t0_1") {
        repeat_exit_regressions::CheckRepeatExit(
            repeat_exit_regressions::kBranchOwnershipSamples[0], false, "invalid argument #1 to 'select' (number expected, got string)"
        );
    }
    SECTION("batch-3/t4_3") {
        repeat_exit_regressions::CheckRepeatExit(repeat_exit_regressions::kBranchOwnershipSamples[1], false, "missing argument #1 to 'pairs' (table expected)");
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
        DYNAMIC_SECTION(sample.name) { repeat_exit_regressions::CheckNoGeneratedForward(sample); }
    }
}

TEST_CASE("Loop branch structure preserves the first observable error", "[Decompiler][FuzzRegress][RepeatBranchOwnership][StructuralSemantics]") {
    SECTION("batch-4/t1_4") {
        repeat_exit_regressions::CheckRepeatExit(repeat_exit_regressions::kStructuralSemanticSamples[0], false, "attempt to index function with 'set'");
    }
    SECTION("batch-3/t1_3") {
        repeat_exit_regressions::CheckRepeatExit(repeat_exit_regressions::kStructuralSemanticSamples[1], false, "attempt to index function with 'field'");
    }
}
