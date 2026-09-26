//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>

namespace ForwardDeclarationRegressions {
    std::string Decompile(const std::string &source) {
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        return result.output;
    }
} // namespace ForwardDeclarationRegressions

TEST_CASE("Nested constructor key keeps later closure declaration in scope", "[Decompiler][ForwardDeclaration][Table]") {
    const std::string source = R"LUA(local outer = {"x"}
local value = function() return 1 end
outer[{y = true, field = "key", value}] = 7
print(outer)
return outer)LUA";
    fuzz::EnableLuauFlags();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{2, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Numeric loop assignment does not read its own declaration", "[Decompiler][ForwardDeclaration][Loop]") {
    const std::string source = R"LUA(for i = 1, 2 do
    if i then
        i = i.field
    end
    print(i)
end)LUA";
    fuzz::EnableLuauFlags();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{2, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Shared repeat exit test stays outside inner break guard", "[Decompiler][ForwardDeclaration][Loop]") {
    const std::string source = R"LUA(repeat
    repeat
        if nil then break end
    until f0[150].field
    if f0[table].field then
        ipairs(tonumber, ipairs)
    end
    if f0:set(191, true) then
        break
    end
until (if ... then f0[tonumber] else f0[pairs]) + ...)LUA";
    fuzz::EnableLuauFlags();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{2, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Nested repeat keeps numeric loop variable name through phi", "[Decompiler][ForwardDeclaration][Loop]") {
    const std::string source = R"LUA(for g in generator() do
    for i = false, function() return g end do
        repeat
            if "x" then continue end
        until function() return g end
        for k, v in g(false, i) do
            k(g)
        end
        i()
    end
end)LUA";
    fuzz::EnableLuauFlags();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{2, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Repeated merge call keeps closure argument bound", "[Decompiler][ForwardDeclaration][Loop]") {
    const std::string source = R"LUA(local x
repeat
    x = select
    f(function() return x end, if { t, k = 660 } then {} else {})
until x)LUA";
    fuzz::EnableLuauFlags();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{2, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
}

TEST_CASE("Batch 4 repeat branch keeps a declaration visible after the branch", "[Decompiler][ForwardDeclaration][Batch4]") {
    const std::string source = R"LUA(while (#function(...)
local v0 = obj["a-b"];
if (if t then "value" else "") then
    ipairs();
    obj();
    v0();
end
v0(667);
return "key", "a-b";
end) do
    local v0, v1, v2 = { field = string }, tostring:run(tonumber, tostring), (if print.field then 496 else ...);
    if "x" then
    else
        repeat
        until (math).field;
        v2[string]();
    end
    v2 *= (if (-(-106.75)) then tonumber["hello"] else function(p3, p4, p5)
return table, 87;
end);
    if ((-(-v1:method()))) then
        break
    end
end
tonumber -= (if t[132.25] then tostring[30.25] else (select));
tonumber[169][next()](pairs[math[next]], ipairs[nil].field);
tostring = ...;
print *= obj[math];
obj.field();
local function f0()
    f0 -= (string < "value");
    print.field(f0:get());
    return f0;
end
repeat
until ...;
for g1_0, g1_1 in f0[tostring]((if "x" then 179.25 else nil)) do
    for g3_0, g3_1 in g1_1(false) do
    end
    t();
end
(tostring)(...);
return { field = true, x = true, data = 175.5, [nil] = string }, (-(#""));)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string bytecode;

    REQUIRE(fuzz::LuauCompiles(output, &bytecode));
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(output, &source));
}

TEST_CASE("Branch-assigned repeat value remains live after its defining branch", "[Decompiler][ForwardDeclaration][Semantics]") {
    const std::string source = R"LUA(local function choose(flag)
local value
repeat
    if flag then
        value = 1
    else
        value = 41
    end
    value += 1
until true
return value
end
return choose(true), choose(false)
)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string originalBytecode;
    std::string reconstructedBytecode;
    std::string prelude;

    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 2\t42\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Captured repeat value remains declared outside the loop", "[Decompiler][ForwardDeclaration][Batch123]") {
    const std::string source = R"LUA(local v0, v1 = 128.75, tonumber(table:run(933, nil));
repeat
    v1 = ...;
    v0 = obj.field;
    for g2_0 in v1() do
        repeat
            local v3 = (if ipairs then v1.field else (not false));
        until (not v3);
        if (if v0[obj] then table() else ...) then
            g2_0(453);
        end
    end
until v0;
local v2 = string;
local v3 = function(p3)
v1();
return ;
end;
return function(...)
return ;
end;)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string bytecode;

    REQUIRE(fuzz::LuauCompiles(output, &bytecode));
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(output, &source));
}

TEST_CASE("Closure created after a repeat observes its updated local", "[Decompiler][ForwardDeclaration][Semantics]") {
    const std::string source = R"LUA(local count = 0
local value = 5
repeat
    count += 1
    value = count + 40
until count == 3
local function read()
    return value
end
return read()
)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string originalBytecode;
    std::string reconstructedBytecode;
    std::string prelude;

    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 43\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Numeric loop does not revisit its consumed continuation", "[Decompiler][ForwardDeclaration][Batch4]") {
    const std::string source = R"LUA(while ... do
    local v0 = (nil):get(print[pairs]);
    (("hello" + true))();
end
repeat
until (math[tostring]:set(..., (tonumber)) * false);
for g0_0 in pairs[print]() do
    for i1 = (nil ~= "x"), obj, string.field do
        local v2 = i1();
        for i3 = nil, "hello" do
            v2(i1);
        end
        local v3 = pairs.field;
    end
    local v1 = (math).field;
end
repeat
    local v0 = t[40].field;
    v0 = v0:run(print, nil);
    local v1 = 696;
    if tonumber[false][v1:method(false)] then
        break
    end
until ...;
repeat
    pairs(true);
until ((next));
local v0 = function()
for g0_0, g0_1 in pairs() do
    select(t, print);
    local v2 = g0_0:set(nil, true);
    g0_0(g0_1);
end
(t)((779));
for g0_0, g0_1 in pairs() do
    local v2, v3, v4 = "", "x", g0_1();
    g0_1 *= nil;
    v4();
end
return 11;
end;
for i1 = t:set(), v0.field, v0:run(nil) do
    local v2 = v0.field;
    for i3 = v2, print, string do
        pairs("x");
        pairs(nil);
        i3(tonumber, 29.25);
        if print then
            continue
        end
    end
    v0(false);
    if { data = "key", obj } then
        break
    end
end
local v1 = (if (#true) then (tonumber.field >= (-(-print))) else pairs(true));
if (print).field then
    local v2 = v1.field;
    for i3 = nil, nil, "a-b" do
        if obj then
            break
        end
    end
else
    v0();
    v1 ..= "";
    repeat
        string();
        v1("x");
        obj(print);
        if v1 then
            break
        end
    until (-(-"hello"));
end
local function f2(p3, ...)
    p3(next, table);
    f2(nil, "value");
    return v1, print;
end
return ((-(-t)) + ...);)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string bytecode;

    REQUIRE(fuzz::LuauCompiles(output, &bytecode));
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(output, &source));
}

TEST_CASE("Numeric loop preserves its shared continuation on every exit", "[Decompiler][ForwardDeclaration][Semantics]") {
    const std::string source = R"LUA(local function run(enabled, first, last, breakAt)
    local total = 0
    if enabled then
        for i = first, last do
            total += i
            if i == breakAt then
                break
            end
        end
    else
        total = -1
    end
    return total + 100
end
return run(true, 2, 1, 9), run(true, 1, 3, 9), run(true, 1, 3, 2), run(false, 1, 3, 2)
)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string originalBytecode;
    std::string reconstructedBytecode;
    std::string prelude;

    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 100\t106\t103\t99\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Compound assignments retain a declaration across control-flow scopes", "[Decompiler][ForwardDeclaration][Batch123]") {
    const std::vector<std::string> sources{
        R"LUA(local v0 = pairs((tonumber < next));
if (v0:get())() then
    math = next;
    repeat
        while ipairs[function(p1, p2, p3)
p3(p1);
pairs(string, table);
return "hello", print;
end] do
            v0();
        end
    until (not obj[t].field);
else
end
v0 -= (not { [string] = math, field = 453 });
return ipairs, (not { [705] = "a-b", [print] = tonumber, k = nil });)LUA",
        R"LUA(local function f0()
    return t;
end
local v1 = (function(p1, p2, p3)
for g4_0 in p1() do
end
return ;
end);
(math)();
obj((print[math] or ((if 472 then 32 else "key") % t.field)), math.field[v1][function(p2, p3, ...)
v1(p2);
p2(v1);
return tonumber, string;
end]);
if ... then
    if { (nil - v1), data = math } then
        math.field();
        f0 -= ...;
    else
        local v2, v3, v4 = (if table then nil else "x"), 900, nil;
    end
else
    if nil then
    else
    end
    v1(tostring:get(true, 78.5), { field = select });
    v1 /= function(p2, p3, ...)
return "value";
end;
end
local v2, v3, v4 = { (-(-190)), k = ... }, (#next[159].field), v1.field(({ 404, [tonumber] = print, field = f0, field = 421 } ~= false), ipairs:method());
return ((if obj then tonumber else t)), v2[nil][v3.field];)LUA",
    };

    for (const auto &source : sources) {
        const auto output = ForwardDeclarationRegressions::Decompile(source);
        std::string bytecode;
        REQUIRE(fuzz::LuauCompiles(output, &bytecode));
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(output, &source));
    }
}

TEST_CASE("Captured-local hoisting does not shadow a same-named global", "[Decompiler][ForwardDeclaration][Semantics]") {
    const std::string source = R"LUA(value = function()
    return "global"
end
local function outer()
    if true then
        local value = function()
            return "local"
        end
        value()
    end
    local function read()
        return value()
    end
    return read()
end
return outer()
)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string originalBytecode;
    std::string reconstructedBytecode;
    std::string prelude;

    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: \"global\"\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Captured local and same-named global keep separate bindings", "[Decompiler][ForwardDeclaration][Semantics]") {
    const std::string source = R"LUA(value = 99
local read
do
    local value = tonumber("5")
    read = function()
        return value
    end
end
return value, read()
)LUA";
    const auto output = ForwardDeclarationRegressions::Decompile(source);
    std::string originalBytecode;
    std::string reconstructedBytecode;
    std::string prelude;

    REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
    REQUIRE(fuzz::LuauCompiles(output, &reconstructedBytecode));
    REQUIRE(fuzz::LuauCompiles("", &prelude));
    const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "return: 99\t5\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}
