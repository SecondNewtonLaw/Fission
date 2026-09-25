//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "ReplayDivergenceTestSupport.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>

using replay_divergence::CheckSemanticParity;
using replay_divergence::CompileLevels;

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
    for (auto at = output.find("elseif arg0 > 0 then\n        print(\"up\")"); at != std::string::npos;
         at = output.find("elseif arg0 > 0 then\n        print(\"up\")", at + 1))
        ++chains;
    CHECK(chains == 2);
    CHECK(output.find("not (arg0 > 0)") == std::string::npos);
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
    for (const char *expected :
         {"t.v += x", "t.w -= 1", "t[k] *= x", "t[2] //= 2", "get().v += x", "t.w = t.w + x", "G += 1", "total += ", "return total, tostring("})
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
    for (const char *expected :
         {"setmetatable({  }, {\n    __index = function(", "setmetatable({  }, nil)", "rawlen({ 1, 2 })", "{ k = tostring(1) }", "{ k = tostring(2), 5 }",
          "print({ k = { j = tick() } })", "({ k = tick() }, tick())", "{ a = tick(), b = { c = tick() } }",
          "(rawlen({ tick(), tick() }), select(\"#\", { k = tick() }))"})
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
        CHECK(
            output.find("    elseif arg0 == 2 then\n        v1 = advance(v1, 2)\n    else\n        v1 = advance(v1, 3)\n    end\n    return v1\n") !=
            std::string::npos
        );
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
    CHECK(
        output.find("if arg0 == 1 then\n        print(1)\n    elseif arg0 == 2 then\n        print(2)\n    else\n        print(3)\n    end\nend") !=
        std::string::npos
    );
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

TEST_CASE("Compiler audit: variadic SETLIST keeps the stored key after a captured local changes", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(c)
    local k = if c then "x" else "y"
    local function change() k = "z"; return 1 end
    local function multi() return 7, 8, 9 end
    local t = {[k] = 5, if c then change() else 2, multi()}
    return t.x, t.y, t.z, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: variadic SETLIST keeps a computed key with effects", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(c)
    local k = "x"
    local function key() k = "z"; return "x" end
    local function multi() return 7, 8, 9 end
    local t = {[key()] = 5, if c then 1 else 2, multi()}
    return t.x, t.z, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: variadic SETLIST keeps a method call key", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function f(c)
    local calls = 0
    local object = {}
    function object:key() calls += 1; return "x" end
    local function multi() return 7, 8, 9 end
    local t = {[object:key()] = 5, if c then 1 else 2, multi()}
    return calls, t.x, t[1], t[2], t[3], t[4]
end
return f(true), f(false))LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: an O2 inlined return skips a numeric for natural exit", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function probe(limit)
    for i = 1, limit do
        if i == 2 then return 100 + i end
    end
    return -1
end
local limits = {3, 1}
print(probe(limits[1]), probe(limits[2])))LUA";
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: an O2 inlined return skips a generic for natural exit", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function probe(items)
    for _, value in ipairs(items) do
        if value == 2 then return 100 + value end
    end
    return -1
end
local items = {{1, 2, 3}, {1}}
print(probe(items[1]), probe(items[2])))LUA";
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: an O2 inlined nil return remains distinct from a loop break", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function probe(limit, stop)
    for i = 1, limit do
        if i == stop then break end
        if i == 2 then return nil end
    end
    return -1
end
local limits = {3, 3}
print(probe(limits[1], 4), probe(limits[2], 1)))LUA";
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: O2 inline exits keep pairs and custom iterator results", "[Decompiler][ReplayRegress][Semantics]") {
    const auto pairsSource = R"LUA(local function probe(items)
    for key, value in pairs(items) do
        if key == "hit" then return value end
    end
    return -1
end
local items = {{hit = 7}, {miss = 1}}
print(probe(items[1]), probe(items[2])))LUA";
    const auto customSource = R"LUA(local function iter(state, index)
    local nextIndex = index + 1
    if nextIndex > #state then return nil end
    return nextIndex, state[nextIndex]
end
local function probe(items)
    for _, value in iter, items, 0 do
        if value == 2 then return 100 + value end
    end
    return -1
end
local items = {{1, 2, 3}, {1}}
print(probe(items[1]), probe(items[2])))LUA";
    CheckSemanticParity(pairsSource, 2, 1);
    CheckSemanticParity(pairsSource, 2, 2);
    CheckSemanticParity(customSource, 2, 1);
    CheckSemanticParity(customSource, 2, 2);
}

TEST_CASE("Compiler audit: O2 nested for inline exits skip both natural paths", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local function probe(limit)
    for i = 1, limit do
        for j = 1, limit do
            if i == 2 and j == 2 then return i * 10 + j end
        end
    end
    return -1
end
print(probe(3), probe(1)))LUA";
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Compiler audit: a closure used only as an if-expression condition is truthy", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local calls = 0
local function choose()
    calls += 1
    return calls == 2
end
if choose() or (if choose() then function() end else nil) then
    while next({}) do
        print("body")
    end
    print("then")
else
    print("else")
end)LUA";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 1, 2);
    CheckSemanticParity(source, 2, 1);
    CheckSemanticParity(source, 2, 2);
    std::string capturingSource = source;
    capturingSource.replace(capturingSource.find("function() end"), sizeof("function() end") - 1, "function() return calls end");
    CheckSemanticParity(capturingSource, 1, 1);
    CheckSemanticParity(capturingSource, 1, 2);
    CheckSemanticParity(capturingSource, 2, 1);
    CheckSemanticParity(capturingSource, 2, 2);
}

TEST_CASE("Compiler audit: long call chains lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local calls = 0\nlocal function f() calls += 1 return f end\nf";
    for (int i = 0; i < 70; ++i)
        source += "()";
    source += "\nprint(calls)";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);

    std::string methods = "local t = {count = 0}\nfunction t:step() self.count += 1 return self end\nt";
    for (int i = 0; i < 70; ++i)
        methods += ":step()";
    methods += "\nprint(t.count)";
    CheckSemanticParity(methods, 1, 1);
    CheckSemanticParity(methods, 2, 1);
    const auto ordered = "local function f(n) print(n) return f end\nf(1)(2)(3)";
    CheckSemanticParity(ordered, 1, 1);
    CheckSemanticParity(ordered, 2, 1);
}

TEST_CASE("Compiler audit: long unary chains lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "print(";
    for (int i = 0; i < 70; ++i)
        source += "not ";
    source += "next({}))";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: right-nested binary chains lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local x = tonumber('1')\nprint(";
    for (int i = 0; i < 70; ++i)
        source += "(x + ";
    source += "x";
    for (int i = 0; i < 70; ++i)
        source += ")";
    source += ")";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: constant-index table chains lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local t = {}\nt[1] = t\nprint(t";
    for (int i = 0; i < 70; ++i)
        source += "[1]";
    source += " == t)";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: mixed unary and binary trees lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local x = tonumber('1')\nprint(";
    for (int i = 0; i < 70; ++i)
        source += "-(x + ";
    source += "x";
    for (int i = 0; i < 70; ++i)
        source += ")";
    source += ")";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: mixed table and unary trees lift without expression depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    std::string source = "local t = {}\nt[1] = t\nsetmetatable(t, {__unm = function(x) return x end})\nprint(";
    std::string expression = "t";
    for (int i = 0; i < 70; ++i)
        expression = "(-" + expression + ")[1]";
    source += expression + " == t)";
    CheckSemanticParity(source, 1, 1);
    CheckSemanticParity(source, 2, 1);
}

TEST_CASE("Compiler audit: nested table constructors keep distant SETLIST stores", "[Decompiler][ReplayRegress][Semantics]") {
    for (const int depth : {27, 70}) {
        std::string source = "local t = ";
        source.append(depth, '{');
        source += "1";
        source.append(depth, '}');
        source += "\nprint(t";
        for (int i = 0; i < depth; ++i)
            source += "[1]";
        source += ")";
        CheckSemanticParity(source, 1, 1);
        CheckSemanticParity(source, 2, 1);
    }
}

TEST_CASE("Compiler audit: alternating table reads and calls lift without depth loss", "[Decompiler][ReplayRegress][Semantics]") {
    for (const int depth : {35, 70}) {
        std::string source = "local t = {}\nt[1] = function() return t end\nprint(t";
        for (int i = 0; i < depth; ++i)
            source += "[1]()";
        source += " == t)";
        CheckSemanticParity(source, 1, 1);
        CheckSemanticParity(source, 2, 1);
    }
}
