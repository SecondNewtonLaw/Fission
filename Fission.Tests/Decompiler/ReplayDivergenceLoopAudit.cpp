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
}

TEST_CASE("Compiler audit: a multi-value constructor tail survives a reused element register", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local function multi() return "m1", "m2" end
print(#{multi()})
local t = {multi(), multi()}
print(#t, t[1], t[2], t[3]))LUA");
}

TEST_CASE("Compiler audit: a table value remains available after a later initializer", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(
        R"LUA(local a = {x = 1} or nil
local b = tostring(#"abc")
print(type(a), a and a.x, b))LUA",
        1, 1
    );
}

TEST_CASE("Compiler audit: the right operand of a register and/or stays eagerly evaluated", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(
        R"LUA(local calls = 0
local function f() calls += 1 return calls end
local a = f()
local b = tostring or a
print(b == tostring, calls))LUA",
        1, 1
    );
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

TEST_CASE("Compiler audit: a closure chosen by and/or keeps its value", "[Decompiler][ReplayRegress][Semantics]") {
    const auto source = R"LUA(local enabled = tonumber("1")
local disabled = tonumber("x")
local h = enabled and function() return "a" end or print
local g = disabled and function() return "b" end or tostring
print(type(h), h == print, h and h(), g == tostring)
)LUA";
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
