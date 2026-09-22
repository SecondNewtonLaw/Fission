// Regression guard for the 15 fuzzer-found INVALID_RECOMPILE samples (run-once `repeat ... until
// <truthy>` wrapping for-loops / continue / break). Each sample once decompiled to syntactically
// invalid Luau ("break outside a loop" / "Expected 'until'"). The fix set: CFA FORxPREP-header
// guards + dead-FORNLOOP-latch recovery, ASTLifter infinite-while tail-fold, and numeric-for
// body = FORNPREP fall-through (empty-body). The durable invariant these lock in: the decompiled
// output must be valid Luau (it recompiles). Sources are embedded verbatim so the test survives
// deletion of the scratch fuzz corpus. Decompile uses the same opt/debug (1, 2) the fuzzer used.
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
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

TEST_CASE("Roblox corpus RegEx fixture does not stack-overflow during lifting", "[Decompiler][Regression][RobloxCorpus]") {
    EnableLuauFFlagsOnce();
    const auto fixture = std::filesystem::path(FISSION_SOURCE_DIR) /
                         "Bytecode_12_Dump/CorePackages.Packages._Index.RegExp.RegExp.RegEx.lbc";
    if (!std::filesystem::exists(fixture)) {
        SUCCEED("optional Bytecode_12_Dump fixture is absent");
        return;
    }

    std::ifstream input(fixture, std::ios::binary);
    REQUIRE(input);
    std::stringstream bytes;
    bytes << input.rdbuf();

    Decompiler decompiler{};
    decompiler.SetDecompileBudget(std::chrono::seconds(30));
    const auto result = decompiler.DecompileRobloxBytecode(bytes.str(), static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    std::string error;
    const bool recompiles = Recompiles(result.decompilationOutput, &error);
    INFO("recompile error: " << error);
    CHECK(recompiles);
}

TEST_CASE("Regress sibling branches lift without accumulating native coroutine frames", "[Decompiler][Regression][StackDepth]") {
    std::string source = "local flag = ...\n";
    for (int i = 0; i < 1024; ++i)
        source += "if flag then ping() else pong() end\n";
    source += "return flag";
    CheckDecompileRecompiles(source);
}

static void CheckNoIntroducedGeneratedGlobals(const std::string &source) {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Forward-reference oracle recognizes Luau local bindings", "[Fuzz][ForwardRef][Regression]") {
    const std::string source = GENERATE(
        "local function v0() local v0 = {} return v0 end v0()",
        "for v2 in next, {} do print(v2) end local v2 = 1 print(v2)",
        "for v6 in next, {} do for v7 in next, {} do print(v7) end end local v6 = 1 print(v6)",
        "local v1_3, v2 = 1, 2 print(v2) local v2 = 3 print(v2)"
    );

    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(source));
}

TEST_CASE("Forward-reference oracle rejects a local hidden from its use", "[Fuzz][ForwardRef][Regression]") {
    const std::string source = "print(v0) local v0 = 1";

    CHECK(fuzz::UsesGeneratedLocalBeforeDeclared(source));
    CHECK(fuzz::ClassifyForwardRef(source) == "USE_BEFORE_DECL");
}

TEST_CASE("Forward-reference oracle ignores globals outside nested local scopes", "[Fuzz][ForwardRef][Regression]") {
    const std::string source = "do local v0 = 1 end return v0";

    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(source));
    CHECK(fuzz::ClassifyForwardRef(source) == "OTHER");
}

TEST_CASE("Forward-reference oracle catches shadowing of an original global", "[Fuzz][ForwardRef][Regression]") {
    const std::string original = "v0 = 9 return v0";
    const std::string output = "print(v0) local v0 = 1";

    CHECK(fuzz::UsesGeneratedLocalBeforeDeclared(output, &original));
    CHECK(fuzz::ClassifyForwardRef(output, &original) == "USE_BEFORE_DECL");
}

TEST_CASE("Forward-reference oracle preserves source global-before-local binding", "[Fuzz][ForwardRef][Regression]") {
    const std::string source = "print(v0) local v0 = 1";

    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(source, &source));
    CHECK(fuzz::ClassifyForwardRef(source, &source) == "OTHER");
}

TEST_CASE("Nested generated local does not capture later global read", "[Fuzz][ForwardRef][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = "do local v0 = 1 end return v0";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: nil\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Loop reconstruction declares suffixed SSA uses", "[Fuzz][ForwardRef][Regression]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
repeat
    for g in next, {} do
    end
until select
while t:run(tonumber)() do
end
)LUA");
}

TEST_CASE("If-expression reads keep their producer in scope", "[Fuzz][ForwardRef][Regression]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
local v0 = game.field
local v1 = if pairs(nil) / 187 then nil else false
v1[game] = game(181i)
return v1, (5.571428571428571)[false] >= pairs
)LUA");
}

TEST_CASE("Branch and loop temporaries never become generated globals", "[Fuzz][ForwardRef][Regression]") {
    const std::string source = GENERATE(
        R"LUA(
next.field.field[tonumber.field.field](function(...)
    return t, "a-b"
end)
for i0 = (if "a-b" then ... else string[math]), function(...)
    table()
    math()
    math()
end, math do
    local v1, v2 = ..., ipairs(false, ipairs)
end
for i0 = 303, string.field.field, ipairs do
    if (if i0 then true else print) then
        break
    end
end
tonumber -= -{ [false] = print, [tonumber] = 402, field = nil }
local v0 = function(p0, ...)
    if p0 then
        tonumber("hello", obj)
    else
        p0 -= p0
        p0()
    end
    for g1_0 in next("") do
        if { [false] = nil } then
            break
        end
    end
end
repeat
until pairs[false]
for g1_0, g1_1 in v0(ipairs:method(tonumber, v0)) do
    if tonumber.field then
        break
    end
end
local v1 = function(p1)
    local v2 = tonumber(nil, nil)
    p1(nil)
    repeat
        local v3, v4 = obj, obj()
    until t
end
for i2 = tonumber, true, nil do
    print()
    local v3, v4 = next, obj(129.25)
end
if function(p2, p3, p4)
    if t.field then
        tostring(t, "a-b")
    end
    for g5_0, g5_1 in v0(nil, math) do
        v1(p3, nil)
        if next then
            continue
        end
    end
    p2()
    return t
end then
    local function f2(p3, p4)
        return select, math
    end
end
return (if { ["a-b"] = nil, true } then v1.field else v1.field), table:run()
)LUA",
        R"LUA(
next += if table.field then math else tonumber(52.75, 889)
if ipairs[not 386] then
    repeat
    until #next(select)
    tonumber = if function(p0, p1)
    end then 970 > print else select[246]
    for i0 = math, obj[true], print() do
        i0()
        local function f1(p2, p3, p4)
        end
    end
end
table //= not (nil > next)
repeat
until table.field.field
local v0 = { tonumber.field, [print / ipairs] = #nil }
return function(p1, p2, p3)
    v0(select, print)
    ipairs(36)
    p1()
    return pairs
end, ... % {}
)LUA",
        R"LUA(
local function f0()
    ipairs = (if f0 then f0 else 37.75);
    return false, print;
end
(f0[false])();
repeat
    f0();
    if (...) then
        local v1 = ...;
        while v1() do
            if "hello" then
                break
            end
        end
        f0 = f0[nil][(tostring ^ false)];
    else
        f0[173](f0.field);
        for g1_0, g1_1 in tonumber(nil) do
            local v3 = (37 and true);
            if (-(-true)) then
            end
            for g4_0 in v3(tonumber) do
                if true then
                    continue
                end
            end
        end
    end
    local v1 = obj.field;
until ((("a-b" * true))[function(p1, p2, p3, ...)
next(nil, "");
f0(p1, 748);
return nil, p2;
end]);
local v1 = (if { k = f0, [obj:get(obj)] = (if nil then f0 else ""), [next.field] = string:method(), function(p1, p2, p3)
return p2, nil;
end } then (f0(string)) else string[853]());
local function f2(p3, p4, p5)
    tonumber.field(115);
    return ;
end
if function(p3, p4, ...)
    p4();
    p4(nil);
    f0(math, nil);
    return "a-b", 177.75;
end then
    if t then
    end
    for g3_0, g3_1 in v1(f0, f2) do
        f2(nil);
        f2();
    end
    for g3_0 in f0(true, false) do
        ipairs(true);
    end
end
for i3 = (if v1 then true else f0), (if t then "value" else "key") do
    repeat
        string();
        i3(nil);
        math();
    until tostring;
end
local v3 = ipairs[(#math)]({ x = nil, "x" });
f2 %= (if 67.25 then false else 69.75);
return (if next then ("key") else (if f0 then t else string)), tonumber[true];
)LUA"
    );

    CheckNoIntroducedGeneratedGlobals(source);
}

TEST_CASE("SETLIST closures remain declared before table elements", "[Decompiler][FuzzRegress][ForwardRef]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
local v0 = nil
local v1 = v0(151i, nil)
local v2 = #{
    function()
        return 27.714285714285715
    end,
    "\n",
    ["y"] = nil,
    ["data"] = ("value")["a-b"],
}
local v3 = true
local v4 = function()
    return false
end
local v5 = function()
    return "\n"
end
tostring(not 137, { { 420, ["a\nb"] = 189i, nil }, 19.714285714285715, -6i, 125 })
return true
)LUA");
}

TEST_CASE("Captured multi-locals remain declared", "[Decompiler][FuzzRegress][ForwardRef]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
local v0 = ...
local v1, v2, v3 = (...), { true }, (false == false)
v0()
for i4 = 13.5, print do
end
return v0[function(p4, ...)
    v1(obj, 220.25)
    v2()
    pairs(select, "a-b")
    return 134.25
end], v0:run()
)LUA");
}

TEST_CASE("Conditional loop bounds declare both values", "[Decompiler][FuzzRegress][ForwardRef]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
local function f0(p1, p2, p3)
    f0(print, next)
    return true, string
end
for i1 = 57, (if ... then f0.field else table.field), (if t * f0 then #true else function(...)
end) do
    if tostring.field then
        break
    end
end
return tonumber["hello"] / (if next then table else f0)
)LUA");
}

TEST_CASE("Loop body registers do not suppress later closure declarations", "[Decompiler][FuzzRegress][ForwardRef]") {
    CheckNoIntroducedGeneratedGlobals(R"LUA(
for g1_0 in pairs.field() do
end
for i0 = function(p0)
    p0()
    p0()
    p0(610, nil)
    return nil, 973
end, ... do
    tostring(82, math)
    table = math(nil, next)
end
)LUA");
}

TEST_CASE("Multiline string keys and leading newlines preserve bytes", "[Decompiler][Roundtrip][FuzzRegress]") {
    const std::string source = R"LUA(
local t = { ["\n"] = 3, ["a\nb"] = 7 }
t["x\ny"] = 11
print(t["\n"], t["a\nb"], t["x\ny"], "\nhello", "a\nb")
local function read(suffix) return t["a\nb" .. suffix] end
print(read(""))
)LUA";
    EnableLuauFFlagsOnce();
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    CHECK(fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")}).kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Captured branch tables retain phi declarations and identity", "[Decompiler][Roundtrip][FuzzRegress]") {
    const std::string source = R"LUA(
local function choose(flag, item)
    local selected = if flag then { item, 35 } else { item }
    local function read() return selected end
    print(rawequal(read(), selected), selected[1], selected[2])
    ;(function() return selected end)()[1] += 3
    print(rawequal(read(), selected), selected[1], selected[2])
    return read()[1]
end
print(choose(true, 6), choose(false, 6))
)LUA";
    EnableLuauFFlagsOnce();
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    CHECK(fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")}).kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Table constructor folding preserves self-referential key evaluation", "[Decompiler][Roundtrip][FuzzRegress]") {
    const std::string source = R"LUA(
local value = { ["a-b"] = "result" }
value[value["a-b"]] = 42
print(value.result)
)LUA";
    EnableLuauFFlagsOnce();
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Reconstruction preserves first error across expression boundaries", "[Decompiler][Roundtrip][FuzzRegress]") {
    const std::string source = GENERATE(
        "local value = {} value[value[\"a-b\"]] = (0)[0]", "local v0 = ({})[self(pairs, 129i)].value ~= (if (127i)[true] then true else nil).x",
        "local v0 = {true, \"\\n\"} string(nil, (0)[nil]) v0[nil] = \"\\nhello\"", "local v0 = -(-next(table, table)) while obj do end v0 *= #{true}",
        "local v0 = {\"key\", 189i, \"key\"} v0({}) v0[next(true).value] = {\"\"}",
        "local function f0(p1) f0 += nil end repeat if f0(ipairs, nil) then end f0 = table:run() until obj()",
        "local v1 = {} v0[(nil)[\"a-b\"].y[(0).value]] = {false ^ v1}",
        "local function f0(p1) end f0 = (if -(-(function(p1,p2) end + f0)) then f0[f0](\"x\") else f0()[...] ) local function f3() return f0 end",
        "local v0 = true local v1 = self(48.57142857142857 .. 31i).x v0[select(604).x[(nil).y]] = (nil)[v1(59.285714285714285)]",
        "local v0 = -(if 44 then 28i else 116i) obj(v0)", "repeat for i = function() end, (if tostring then t.field else table:get(table)) do end until obj()",
        "local v = not (3 ~= (true ~= -62i)) / 54 local function f() return (0)[0] end print(f()) v()",
        "(string[tonumber])[if print(true) then select else print]() for k,v in t() do end",
        "local function f() end local v = ({})[f:run(f)][{#next}] if v then print(next) end",
        R"LUA(local v0 = {
    (0 <= true)[{ true, nil, true, "x" }],
    ["x"] = nil,
    - - -nil,
    function() end,
})LUA",
        R"LUA(local v0 = {
    - -pairs(false, 100i),
    (0).value <= true,
    ["field"] = function() end,
    ["a-b"] = game,
})LUA",
        "tonumber += (... + print[ipairs()])",
        R"LUA(local v0, v1, v2 = print, ..., print(next)
local v3 = v1
local v4 = select:run(v0.field, v2.field / true)
return "hello", pairs())LUA",
        R"LUA(local v1 = -{ v0, ["x\n]"] = pairs, "hello", true }
local v2 = if (not (0).x)[{ next, v1 }] then -v0() else {})LUA",
        R"LUA(local v0 = {
    (65i)[27i].value,
    false .. 0 < string("hello"),
    ["field"] = false,
})LUA",
        R"LUA(repeat
    repeat
        repeat
        until false * 0
        local v0 = function() end
        v0 += "key"
    until next[ipairs[not print]]
until nil)LUA"
    );
    EnableLuauFFlagsOnce();
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    INFO(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), Luau::compile(""));
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), Luau::compile(""));
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Expression boundaries preserve successful execution order", "[Decompiler][Roundtrip][FuzzRegress]") {
    const std::string source = GENERATE(
        R"LUA(
local function read(label, value) print(label) return value end
local v = read("left", {value = 1}).value ~= (if read("condition", true) then {x = 2} else {x = 3}).x
print(v)
local t = {1, 2}
print("before store", t[1])
t[read("key", 3)] = read("value", 4)
print(t[3])
)LUA",
        R"LUA(
local function f() return f end
local saved = f
local n = 0
repeat
    print(f() == saved)
    f = function() return 7 end
    n += 1
until n == 2
print(f())
)LUA",
        R"LUA(
local function step() print("step") return true end
if string then repeat until step() end
print("after")
for i = 1, 2 do print(i) end
)LUA",
        R"LUA(
local function run(flag)
    local n = 0
    repeat
        for i = 1, (if flag then 2 else 3) do print(i) end
        n += 1
    until n == 2
    print("done", n)
end
run(true)
run(false)
)LUA",
        R"LUA(
local f = function() print("called") return 7 end
print(f())
saved = f
print(saved())
print("before loop")
for k, v in ipairs({4, 5}) do print(k, v) end
)LUA",
        R"LUA(
while (if function() end then (if not math then function() end else false) else -(-tostring)) do end
print("after nested condition")
)LUA",
        R"LUA(
local n = 0
repeat
    if n > 0 then print("body") end
    n += 1
until n == 2
print(n)
)LUA"
    );
    EnableLuauFFlagsOnce();
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO(verdict.original.trace);
    INFO(verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
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

TEST_CASE("Regress fuzz t18_1: run-once repeat wraps for with break", "[Decompiler][Roundtrip][FuzzRegress]") {
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
TEST_CASE("Regress dead-branch comparison must not drop its throwing evaluation", "[Decompiler][Roundtrip][FuzzRegress]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local n = 0
while { 1, 2 } do
    n = n + 1
    if n > 3 then break end
    if ("" < tostring) then continue end
end
return n
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The comparison (and its potential throw) must be preserved, not NOPed away.
    CHECK(result.decompilationOutput.find("< tostring") != std::string::npos);
}

// An INFINITE repeat (`repeat BODY until <false const>`, e.g. `until nil`) compiles to an
// unconditional back-edge. When the header carries a truthiness `if`, the repeat reconstruction used
// to mistake that inner `if` for the until-condition and DROP the body (with its effects/throws),
// emitting `repeat local v = <cond> until (not v)`. The body must survive. This asserts recompilation
// AND that the inner-if body (`table(nil)`) is preserved.
TEST_CASE("Regress infinite repeat-until must not drop its inner-if body", "[Decompiler][Roundtrip][FuzzRegress]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
repeat
    if string then
        table(nil)
    end
until nil
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The inner-if body (the throwing `table(nil)` call) must be preserved, not dropped.
    CHECK(result.decompilationOutput.find("table(nil)") != std::string::npos);
}

// A pure diamond `local v; if c then v = a else v = b end` (both arms exactly one assignment to the
// same target) must fold into the idiomatic Luau if-expression `local v = if c then a else b`, not the
// archaic C89 declare-then-assign-in-branches shape. The following `while` keeps the diamond arms as
// single assignments (a trailing statement would otherwise merge into each branch).
TEST_CASE("Fold pure diamond assignment into an if-expression declaration", "[Decompiler][Roundtrip][IfExpr]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local flag
if cond then flag = 1 else flag = 2 end
while running do
    step(flag)
end
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The diamond must render as a single `local ... = if ... then ... else ...` declaration, and the
    // archaic bare `local v0` split must be gone.
    CHECK(result.decompilationOutput.find("= if ") != std::string::npos);
    CHECK(result.decompilationOutput.find(" else ") != std::string::npos);
}

// A materialised short-circuit VALUE whose nested term shares a merge arm -- `local x = (a and b) and c
// or d` used later (here a concat, so `x` cannot inline into a terminal) -- lowers to a diamond where the
// trailing `or d` value block is reached from TWO truthiness edges. LiftControlFlow lifted that block
// into the first branch and marked it visited; the second branch then found it visited, lifted an empty
// body and DROPPED `x = d` on that path -- `x` read nil at the merge and `"x" .. x` threw "attempt to
// concatenate string with nil". The durable invariant: every path assigns the value, so the fallback
// (`""`) survives on the second edge -- either re-lifted into the branch (verbose) or folded into the
// `and/or` expression. This asserts recompilation AND that the fallback is not dropped.
TEST_CASE("Regress shared short-circuit value arm must not drop the fallback", "[Decompiler][Roundtrip][FuzzRegress]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function f(a, b)
    local plus = (a and b) and "+" or ""
    return "x" .. plus
end
return f(true, false)
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

// Same shared-arm short-circuit but consumed directly by `return` (so the terminal folders can collapse
// it): `return (a and b) and c or d` with register values. Must fold to the idiomatic single expression
// AND stay correct -- the fallback `d` must remain the final `or` term, not be dropped.
TEST_CASE("Regress shared short-circuit value folds when returned", "[Decompiler][Roundtrip][FuzzRegress]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function k(a, b, c, d)
    local x = (a and b) and c or d
    return x
end
return k
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // (a and b) and c or d simplifies to `a and (b and c) or d`; Luau lowers the inner `a and b and c`
    // to a right-nested `arg1 and arg2 or arg3` on the a-true path. The fallback `arg3` must survive.
    CHECK(result.decompilationOutput.find("arg1 and arg2 or arg3") != std::string::npos);
}

// DEEPER shape than the two above: the shared value block has a FURTHER truthiness test before the merge.
// `local r = a and (b or c) and d or e` used in a concat. The `and d` value block (a pure MOVE into r) is
// reached from BOTH the b-truthy and c-truthy edges, but its successor is the `if d` (`or e`) test, not the
// phi merge -- so the single-block IsDuplicableValueArm check (successor == merge) correctly rejects it and
// the second edge dropped `r`, which then read nil at the merge ("z" .. nil throws). The fix re-lifts the
// whole pure reconverging sub-region ({`r=d`, `if d`, `r=e`}) into the second branch. Invariant: `r` is
// assigned the `d` value (arg3) on BOTH shared edges, so it never reads nil. r is concat-consumed so it
// cannot inline into a terminal expression -- it materialises into a register on every path.
TEST_CASE("Regress deep shared short-circuit region must not drop the value", "[Decompiler][Roundtrip][FuzzRegress]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function f(a, b, c, d, e)
    local r = a and (b or c) and d or e
    return "z" .. tostring(r)
end
return f(true, false, true, 3, 9)
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The `d` value (arg3) must be materialised on both shared edges (b-truthy and c-truthy). The buggy
    // output assigned it on only the b-truthy edge and left the c-truthy edge empty -> exactly one `arg3`.
    const std::string &out = result.decompilationOutput;
    size_t arg3Count = 0;
    for (size_t p = out.find("arg3"); p != std::string::npos; p = out.find("arg3", p + 4))
        ++arg3Count;
    CHECK(arg3Count >= 2);
}

TEST_CASE("Regress generic-for capture survives sibling loop binding", "[Decompiler][FuzzRegress][GenericFor][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function iter()
    return next, {3}, nil
end
local saved
for a, b in iter() do
    saved = function() return a end
end
for c in iter() do
    print(c * c)
end
return saved()
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);

    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original trace: " << verdict.original.trace);
    INFO("decompiled trace: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "1\nreturn: 1\n");
}

TEST_CASE("Regress generic-for loop-carried noncapturing closure", "[Decompiler][FuzzRegress][GenericFor][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function iter()
    return next, {3}, nil
end
local saved
for a in iter() do
    saved = function() return 9 end
end
return saved()
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);

    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original trace: " << verdict.original.trace);
    INFO("decompiled trace: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "return: 9\n");
}

TEST_CASE("Regress fuzz t3_2: local keyed closure keeps owner and first error", "[Decompiler][FuzzRegress][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local v0 = {
    {  },
    ["x"] = function()
        return next
    end,
}
local v1 = - -not (86i)[63i]
local v2 = nil
local v3 = 216
math({ 143i })
return obj()
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK(result.decompilationOutput.find("local v0") != std::string::npos);

    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original trace: " << verdict.original.trace);
    INFO("decompiled trace: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Unrunnable);
    CHECK(verdict.original.trace == "error: attempt to index integer with integer\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
}
