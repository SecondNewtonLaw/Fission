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

TEST_CASE("Replay: SETLIST constructor stops before observed table mutation", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local seen
local function observe(x)
    seen = x[3]
    return 4
end
local t = {1, 2}
local key = observe(t)
t[3] = 7
t[key] = 8
return seen)LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 1);
    }
}

TEST_CASE("Replay: shared closures in SETLIST keep identity", "[Decompiler][ReplayRegress][Semantics]") {
    CheckSemanticParity(
        R"LUA(local function make()
    return function() return false end
end
local t = {make(), make()}
return t[1] == t[2], t[1]())LUA",
        2, 1
    );
}

TEST_CASE("Replay: repeat condition preserves failing lookup order", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "repeat until print[\"\"].field.field[tostring.field(nil, ...)]";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        const Luau::CompileOptions options{optimization, 2};
        fuzz::EnableLuauFlags();
        const auto decompiled = fuzz::FullDecompile(source);
        REQUIRE(decompiled.code == DecompileResult::Success);
        INFO(decompiled.output);
        const auto prelude = Luau::compile(fuzz::kSemPreludes[0], options);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, options), prelude);
        const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(decompiled.output, options), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Error);
        CHECK(reconstructed.status == original.status);
        CHECK(reconstructed.trace == original.trace);
    }
}

TEST_CASE("Replay: repeat condition preserves observable lookup order", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local emit = print
local events = {}
local receiver = setmetatable({}, {
    __index = function()
        events[#events + 1] = "receiver"
        return { field = { field = { hit = true } } }
    end,
})
local target = setmetatable({}, {
    __index = function()
        events[#events + 1] = "callee"
        return function()
            events[#events + 1] = "call"
            return "hit"
        end
    end,
})
repeat
until receiver[""].field.field[target.field(nil, ...)]
emit(table.concat(events, ",")))LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 2);
    }
}

TEST_CASE("Replay: dead closure capture does not rename live local", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function f0(...)
    if (if pairs("hello", obj) then f0 else (-"")) then
    end
end
while ... do
    f0 ..= ((f0))
end
repeat
    f0(0)
until not 0
repeat
    local function f1()
        f0 = "key"
    end
until function(p1, p2, p3)
end
)LUA";
    fuzz::EnableLuauFlags();
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        const Luau::CompileOptions options{optimization, 2};
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), options);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO(result.decompilationOutput);
        const auto originalBytecode = Luau::compile(source, options);
        const auto reconstructedBytecode = Luau::compile(result.decompilationOutput, options);
        for (size_t fixture = 0; fixture < fuzz::kSemPreludeCount; ++fixture) {
            INFO("fixture " << fixture);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[fixture], options);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            REQUIRE(original.status != fuzz::SemTrace::Status::Timeout);
            REQUIRE(original.status != fuzz::SemTrace::Status::LoadFailed);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
}

TEST_CASE("Replay: raising expressions keep order across comparison and table assignment", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string sources[] = {
        R"LUA(local v0 = "key"
local function f1(p2, p3, p4, ...)
    return print
end
local v2 = ({ f1, [nil] = 228.25 } < (not f1:run(v0)))
return { nil, 23.75 }, string:method("key"))LUA",
        R"LUA(local v0 = {}
local v1 = v0[true][(true)[20i]]
v0[-{}] = (if false then true else v1).x
return -obj)LUA",
    };
    fuzz::EnableLuauFlags();
    for (const auto &source : sources) {
        INFO("source " << source);
        for (int optimization = 0; optimization <= 2; ++optimization) {
            INFO("optimization " << optimization);
            const Luau::CompileOptions options{optimization, 2};
            Decompiler decompiler{};
            const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), options);
            REQUIRE(result.resultCode == DecompileResult::Success);
            INFO(result.decompilationOutput);
            const auto originalBytecode = Luau::compile(source, options);
            const auto reconstructedBytecode = Luau::compile(result.decompilationOutput, options);
            for (size_t fixture = 0; fixture < fuzz::kSemPreludeCount; ++fixture) {
                INFO("fixture " << fixture);
                const auto prelude = Luau::compile(fuzz::kSemPreludes[fixture], options);
                const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
                const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
                REQUIRE(original.status == fuzz::SemTrace::Status::Error);
                CHECK(reconstructed.status == original.status);
                CHECK(reconstructed.trace == original.trace);
            }
        }
    }
}

TEST_CASE("Replay: empty while keeps its entry test", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function c()
    return false
end
while (c() or nil) do
end
print("done"))LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 2);
    }
}

TEST_CASE("Loop: shared outer header does not duplicate an inner loop tail", "[Decompiler][ReplayRegress][ControlFlow]") {
    const std::string source = R"LUA(while true do
    while a do
        print("first")
        if x then
            continue
        end
    end
    while b do
        local function f0(...)
            return next, f0
        end
        for i = tonumber, f0:get() do
            local function f2()
                i()
            end
            for j = 1, 3 do
                i(true)
                if x then
                    continue
                end
            end
        end
    end
    while c do
        print("third")
    end
end
print("after"))LUA";
    fuzz::EnableLuauFlags();
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{optimization, 2});
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO(result.decompilationOutput);
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
        const auto first = result.decompilationOutput.find("local function f0");
        REQUIRE(first != std::string::npos);
        CHECK(result.decompilationOutput.find("local function f0", first + 1) == std::string::npos);
    }
}

TEST_CASE("Loop: while break before repeat condition stays an entry test", "[Decompiler][ReplayRegress][ControlFlow]") {
    const std::string source = R"LUA(repeat
    while t:set() do
        next()
        if pairs then
            break
        end
    end
    if select[ipairs] then
        break
    end
until #"hello" ~= (if ipairs then nil else t))LUA";
    fuzz::EnableLuauFlags();
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{optimization, 2});
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO(result.decompilationOutput);
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    }
}

TEST_CASE("Loop: for variable name does not claim preceding if value", "[Decompiler][ReplayRegress][ControlFlow]") {
    const std::string source = R"LUA(obj += (nil)[(if table("a-b", math) then true else ipairs[false])]
for g0_0 in ipairs("x") do
    if "value" then
        continue
    end
end)LUA";
    fuzz::EnableLuauFlags();
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{optimization, 2});
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO(result.decompilationOutput);
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    }
}

TEST_CASE("Loop: if value before generic for retains both phi inputs", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(obj = 0
function choose()
    return true
end
proxy = setmetatable({}, { __index = function(_, key)
    return key and 2 or 3
end })
obj += proxy[(if choose() then true else false)]
for g0_0 in ipairs({}) do
    if "value" then
        continue
    end
end
return obj)LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 2);
    }
}

TEST_CASE("Replay: shared join after continue branch stays outside the branch", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function f0()
    print("called")
end
repeat
    if tonumber then
    else
        string(true, 957)
        local v1 = true <= true
        v1("value", "a-b")
        if ... then
            continue
        end
    end
    local v1, v2 = (if tonumber then 40.5 else 56.5), f0(344)
    for g3_0 in pairs({}) do
        v1 = { y = nil, "key" }
    end
until "x"
print("done"))LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 2);
    }
}

TEST_CASE("Replay: continue skips shared body before later loop", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function f0()
    print("called")
end
repeat
    if tonumber then
    else
        string(true, 957)
        local v1 = true <= true
        v1("value", "a-b")
        if ... then
            continue
        end
    end
    local v1, v2 = (if tonumber then 40.5 else 56.5), f0(344)
    for g3_0 in pairs({}) do
        v1 = { y = nil, "key" }
    end
until "x"
local counter = 0
while counter < 0 do
    counter += 1
end
print("done"))LUA";
    for (int optimization = 0; optimization <= 2; ++optimization) {
        INFO("optimization " << optimization);
        CheckSemanticParity(source, optimization, 2);
    }
}

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

TEST_CASE("Replay: closures capturing one variable at different versions share its name", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local x = function() end\nif ... then\n    repeat\n        x = tostring\n        if function() return x end then break end\n"
                               "    until ...\nelse\n    local function f4() x(f4) end\nend\nx += 1";
    replay_divergence::CheckSemanticParity(source, 2, 2);
    const replay_divergence::CompileLevels levels(2, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("x_2") == std::string::npos);
    CHECK(result.output.find("x += 1") != std::string::npos);
}

TEST_CASE("Replay: a nested loop that never ends is not the exit of a shared-header wrap", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local v0 = ...\nrepeat\n    while type(v0) == \"table\" and v0.y do\n        print(0)\n    end\n"
                               "    for k, v in pairs({}) do\n        while v() do\n            print(1)\n        end\n"
                               "        repeat\n            print(2)\n        until false\n    end\nuntil type(v0) ~= \"table\" or v0.x\nreturn 1";
    replay_divergence::CheckSemanticParity(source, 2, 1);
    const replay_divergence::CompileLevels levels(2, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.ends_with("        return 1\n    end\nend\n"));
}

TEST_CASE("Replay: varargs and repeat-header arguments keep declarations in place", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a, b = ...\nif type(a) ~= \"table\" then\n    return\nend\nrepeat\n    while a.y do\n        repeat\n"
                               "            print(true, 99)\n        until ...\n    end\n    for i = a.f or 1, 2 do\n        print(i)\n    end\n"
                               "until a.x ~= 1\nreturn a[b]";
    replay_divergence::CheckSemanticParity(source, 2, 1);
    const replay_divergence::CompileLevels levels(2, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("local v0, v1 = ...") != std::string::npos);
    CHECK(result.output.find("print(true, 99)") != std::string::npos);
}

TEST_CASE("Replay: multiple results bind as one multi-target assignment", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function z(f, n)
    local x = 0
    for i = 1, n do
        if i > 2 then
            local q = i * 2
            print(q)
        else
            local p
            x, p = f(i)
            print(p + x)
        end
    end
    return x
end
local t = {}
t.x, t.y = ...
local b, c = select(2, ...)
print(z(function(i) return i, i + 1 end, 4), t.x, t.y, b, c))LUA";
    for (int opt = 0; opt <= 2; ++opt)
        for (int debug = 1; debug <= 2; ++debug)
            replay_divergence::CheckSemanticParity(source, opt, debug);
    const replay_divergence::CompileLevels levels(1, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("x, p = f(i)") != std::string::npos);
    CHECK(result.output.find("t.x, t.y = ...") != std::string::npos);
    CHECK(result.output.find("local b, c = select(2, ...)") != std::string::npos);
    CHECK(result.output.find("({ ... })") == std::string::npos);
}

TEST_CASE("Replay: a negation leading a constructor's list evaluates its operand once", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a = -(if nil then 1 else \"x\")\nlocal t = ({ not a, 1 })[nil]\nlocal b = {}\nprint(t, b)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
    const replay_divergence::CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("not -\"x\"") == result.output.rfind("not -\"x\""));
}

TEST_CASE("Replay: a closure key folds into the constructor", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local t = { g, [function() end] = function() end, 2 }\nprint(#t)";
    replay_divergence::CheckSemanticParity(source, 1, 1);
    const replay_divergence::CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("[function()") != std::string::npos);
    CHECK(result.output.find("v0[2] = 2") == std::string::npos);
}

TEST_CASE("Replay: a long tail after an arm that continues into a compound until runs on both paths", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n = 0\nrepeat\n  n += 1\n  if n % 2 == 0 then\n    print(n)\n  else\n    print(-n)\n    if n == 3 then continue end\n  end\n"
                               "  for i = 1, 2 do\n    for j = 1, 2 do print(i, j) end\n  end\nuntil n > 5 and n < 100";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: the code after an arm whose loop never exits stays after the if", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n = 0\nif n > 1 then\n  repeat\n    for i = 1, 2 do print(i) end\n  until nil\nelse\n  n = -n\nend\nprint(n)\n"
                               "for i = 1, 2 do print(i) end\nwhile n < 3 do n += 1 end\nrepeat n -= 1 until n < 0\nif n then print(n) end\nreturn n";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 2);
}

TEST_CASE("Replay: an inlined call's result read by a later compound assignment is not scoped away", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n = select(\"#\", ...) + 2\nlocal function f()\n  print(1)\n  return \"a\"\nend\nfor _ = 1, n do\n  local s = f()\n"
                               "  for i = 1, n do\n    s ..= tostring(i)\n  end\n  print(s)\nend";
    replay_divergence::CheckSemanticParity(source, 2, 1);
    const replay_divergence::CompileLevels levels(2, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("..=") != std::string::npos);
}

TEST_CASE("Replay: a nested if-expression inside a condition stays one condition", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a, b, c = ...\nif a and (if (if b then nil else true) then 1 else c[1]) then\n  print(1)\nelse\n  print(2)\nend\nprint(3)\nreturn 4";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 2);
    const replay_divergence::CompileLevels levels(0, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("(if (if b then nil else true) then 1 else c[1])") != std::string::npos);
}

TEST_CASE("Replay: an inner repeat sharing its header still lets the outer until exit", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n, m = 0, 0\nrepeat\n  repeat\n    for _ in pairs({}) do end\n    n += 1\n  until n % 2 == 0 and (if m then true else false)\n"
                               "  m += 1\nuntil (if m > 3 then 1 else nil)\nprint(n, m)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 2);
    const replay_divergence::CompileLevels levels(0, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("continue") == std::string::npos);
}

TEST_CASE("Replay: a break out of a repeat sharing its header with an inner while runs the tail once", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n = 0\nrepeat\n  while function() end do\n    break\n  end\n  local v0 = n\n"
                               "  if n == 2 and function() v0 = 1 end then\n    break\n  end\n  n += 1\nuntil n > 10\n"
                               "local f = function() return 1 end\nfor i = 1, 3, f() do\n  n += i\nend\nprint(n, (if n > 3 then \"big\" else \"small\"))";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 2);
    const replay_divergence::CompileLevels levels(2, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    const auto first = result.output.find("for i");
    REQUIRE(first != std::string::npos);
    CHECK(result.output.find("for i", first + 1) == std::string::npos);
}

TEST_CASE("Replay: inlined parameters sharing a debug name stay apart while both values live", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local function f0(p1, p2)\n  print(\"x\")\n  return\nend\nlocal function f1(p2, p3, p4)\n  print(p2.y, p3, p4)\nend\n"
                               "f1({ y = f0(\"\", print) }, (if {} then {} else 1))";
    replay_divergence::CheckSemanticParity(source, 2, 2);
}

TEST_CASE("Replay: a loop-body local read after the loop binds to the hoisted declaration", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local v0, v1 = nil, (if pairs then math[false] else -math.pi)\nrepeat\n  v1 = \"s\"\n"
                               "  local v3, v4 = next, v1:upper()\nuntil {}\nprint(v1)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: an until test after an always-breaking for keeps its operands", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local n = 0\nrepeat\n  n += 1\n  for i = 1, n do\n    print(i)\n    break\n  end\n  if 345 then\n    continue\n  end\n"
                               "until ({ 1, 2, 3 })[(if n > 2 then 2 else 5)]\nprint(n)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: an elseif reached from every term of an and-chain runs when an early term fails", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a, b = tonumber(\"1\"), tonumber(\"x\")\n"
                               "local v0 = if a and b and function() end then 1 elseif (if a then true else b) then 2 else 3\nprint(v0)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: an else region shared by an and-chain stays conditional when the then arm returns past it", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local v0, a, b = { field = false }, tonumber(\"1\"), nil\nif tostring and (if a then { x = 1 } else b) then\n  print(1)\nelse\n"
                               "  print(2)\n  if not v0.field then\n    for g in pairs({ 7 }) do\n      print(g)\n    end\n  end\n  print(3)\nend";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 2);
}

TEST_CASE("Replay: closures capturing one local on both arms of an if share its name", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local v0 = { method = function() return 1 end }
local v1 = 2
v1, v0 = (if tostring then true else nil), v0
if print then
    v0, v1 = v0:method(v1), { 3 }
    v1, v1 = ..., function()
        return v1
    end
else
    local v3 = function()
        return v1
    end
end
local v2, v3 = type(v1), function()
    return v1
end
print(v2, type(v3()))
)LUA";
    replay_divergence::CheckSemanticParity(source, 0, 2);
    const replay_divergence::CompileLevels levels(0, 2);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("v1_") == std::string::npos);
}

TEST_CASE("Replay: an empty if after a repeat sharing its header with an inner while stays out of the loop", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a, b, c, d, e = tonumber(\"x\"), tostring, tonumber(\"2\"), { field = 1 }, tonumber(\"1\")\nrepeat\n  while a do\n    if b then\n      break\n    end\n  end\n"
                               "until c\nif d.field then\nelse\nend\nif e then\n  print(1)\nelse\n  print(2)\nend\nreturn 1";
    for (int opt = 0; opt <= 2; ++opt) {
        replay_divergence::CheckSemanticParity(source, opt, 1);
        const replay_divergence::CompileLevels levels(opt, 1);
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        CHECK(result.output.find("while true") == std::string::npos);
        CHECK(result.output.find("until ") != std::string::npos);
    }
}

TEST_CASE("Replay: a closure stored inside a loop and read after it stays visible past the loop", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local function f0()\nend\nrepeat\n  f0 = function()\n    return 1\n  end\n  if f0 then\n    break\n  end\nuntil f0()\nprint(f0())";
    for (int opt = 0; opt <= 2; ++opt)
        for (int debug = 1; debug <= 2; ++debug)
            replay_divergence::CheckSemanticParity(source, opt, debug);
}

TEST_CASE("Replay: a tail after a nested continue runs on the path that skips the continue", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local n, m, a, b, c = 0, 2, tostring, tostring, tonumber("x")
while n < 3 do
    n += 1
    if a then
        if b then
            print("x")
            if c then
                continue
            end
        end
        for i = 1, m do
            print(i)
        end
    else
        print("y")
    end
    print("z")
end
)LUA";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: a falsy term in a guarded and-or selector falls through to the default", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local function f(k, key, d, t)
    return t and (k == 1 and t.Level1 or k == 2 and t.Level2 or k == 3 and t.Level3 or d[key]) or d[key]
end
print(f(1, "k", { k = 9 }, { Level2 = 5 }), f(2, "k", { k = 9 }, { Level2 = 5 }), f(4, "k", { k = 9 }, {}), f(1, "k", { k = 9 }, nil))
)LUA";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: a loop shared by two arms of a run-once repeat runs on both", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(local n = 0
local function w()
    n += 1
    return n < 3
end
local function g(x, a, b, c, d)
    if x then
        repeat
            if a then
                print(0)
                if b then
                    break
                end
            elseif c then
                break
            end
            while w() do
                print(1)
            end
            if d then
                print(3)
            end
        until "a-b"
    else
        print(2)
    end
end
g(true, true, nil, nil, true)
return true)LUA";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: breaks nested in arms keep a run-once repeat around them", "[Decompiler][ReplayRegress]") {
    const std::string source = R"LUA(local v0 = function(p0, p1, p2)
for i3 = p1("hello"), 294, (-"hello") do
    if select then
        continue
    end
end
while (if p1(select) then print else (print <= "key")) do
    local v3 = (#242.5);
end
return p0(), { y = nil, "a-b" };
end;
((if next then "" else select))(..., (v0)[next(next)]);
while v0[{ data = v0, [t] = print }]:method(v0:set(string, table)) do
    if (not {  }) then
    end
end
v0 //= nil;
if (v0:run()) then
    repeat
        if (false - nil) then
            v0();
            if select then
                break
            end
        else
            if obj then
                break
            end
        end
        while tostring:set(print) do
            v0("a-b", tostring);
            tonumber();
        end
        if ... then
            v0();
        end
    until "a-b";
    while (false) do
        if v0[243.75] then
            v0(string, v0);
            print(nil, nil);
            v0();
        else
            tonumber(ipairs, nil);
            select();
            if table then
                break
            end
        end
        while v0:run("x", 106.25) do
            v0(false);
            v0(243.25);
        end
    end
else
    v0 = (tonumber[true]);
    for i1 = (-(-select)), (if "a-b" then v0 else "key"), tostring() do
        pairs(nil, 382);
        for i2 = obj, print, "key" do
            i2(nil, "key");
            v0(346);
        end
    end
end
return true;)LUA";
    const replay_divergence::CompileLevels levels(2, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(fuzz::LuauCompiles(result.output));
}

TEST_CASE("Replay: a closure-keyed constructor stored to a global keeps every item","[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local o = { field = 1, method = function() return 2 end }\nresult = { o.field, [function() end] = { y = 160 }, o:method(), [\"k\"] = 3 }\nprint(#result, result.k)";
    for (int opt = 0; opt <= 2; ++opt)
        replay_divergence::CheckSemanticParity(source, opt, 1);
}

TEST_CASE("Replay: a named closure keyed between list items folds into the constructor","[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = "local a = ...\nlocal f = function()\n    return 1\nend\nprint(#{ a, [\"\\n\"] = f, nil, true })";
    replay_divergence::CheckSemanticParity(source, 1, 1);
    const replay_divergence::CompileLevels levels(1, 1);
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    CHECK(result.output.find("[2] = nil") == std::string::npos);
}

TEST_CASE("Replay: a closure before an if-loop remains visible after it", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(t({ nil })
local v0 = function(p0)
end
if ... then
    for i1 = "value", "" do
    end
end
for g1_0 in v0.field() do
end)LUA";
    const replay_divergence::CompileLevels levels(2, 2);
    const Luau::CompileOptions options{2, 2};
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    const auto prelude = Luau::compile(fuzz::kSemPreludes[1], options);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, options), prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(result.output, options), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Replay: callee lookup precedes effectful argument", "[Decompiler][ReplayRegress][Semantics]") {
    const std::string source = R"LUA(string.field.field(..., (false):set(..., print.field))
repeat
    while not v0 do
        local function f1(...)
        end
        local v2 = v0
        for i3 = {}, v2 ~= true do
            v2("")
        end
    end
until table[""].field.field)LUA";
    const replay_divergence::CompileLevels levels(2, 2);
    const Luau::CompileOptions options{2, 2};
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(source);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    const auto prelude = Luau::compile(fuzz::kSemPreludes[0], options);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, options), prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(result.output, options), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Replay: values and effects keep their evaluation count and order", "[Decompiler][ReplayRegress][Semantics]") {
    const std::vector<std::string> sources{
        // a duplicated loop tail keeps the generic-for's iterator call inside its header
        "local n = 0\nwhile n < 2 do\n    n += 1\n    if n == 1 then\n    else\n        print(1)\n        if n > 5 then\n            continue\n        end\n    end\n"
        "    for a, b in pairs({ n }) do\n        print(a, b)\n    end\nend",
        // a value read before a loop is read once, not on every iteration
        "local b = t.field\nlocal n = 0\nrepeat\n    b(n)\n    n += 1\nuntil n > 2",
        // a constructor field stays behind an earlier store of the same key
        "local t = { data = nil, data = print(1), x = 220, field = print(2) }\nprint(t)",
        // a local an inlined body declares is still read by the repeat's `until`
        "local function get(t)\n    print(t[1])\n    return t[1]\nend\nlocal n = 0\nrepeat\n    n += 1\n    local v = get({ n })\n    local w = print(n)\n"
        "until v >= 3\nprint(n)",
        // an inlined argument named like the caller's parameter does not hide it
        "local function f0(p1, p2)\n    p1(1)\n    return p2\nend\nlocal function f1(p2)\n    f0(print, ipairs)\n    p2(2)\nend\nf1(print)",
        // a call inlined into its own argument reads the outer argument before redeclaring it
        "local function f0(p1)\n    p1(1)\n    return print\nend\nlocal n = 0\nrepeat\n    f0(f0(print))\n    n += 1\n    f0(print)\nuntil n > 1",
    };
    for (const auto &source : sources)
        for (const int optimization : {1, 2})
            for (const int debug : {1, 2})
                replay_divergence::CheckSemanticParity(source, optimization, debug);

    // an effect skipped for a later store stays ahead when that store does not fold; every fixture raises, after the trace
    const std::string carried = "local v0 = { tostring }\nlocal v1 = game.value\nv0[-pairs(1)] = \"h\"\ntostring[(19i).field] = 22\n"
                                "local v3 = string(print())\nv0[451] = v3(v1)";
    const replay_divergence::CompileLevels levels(2, 2);
    const Luau::CompileOptions options{2, 2};
    fuzz::EnableLuauFlags();
    const auto result = fuzz::FullDecompile(carried);
    REQUIRE(result.code == DecompileResult::Success);
    INFO(result.output);
    for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
        INFO("fixture " << i);
        const auto prelude = Luau::compile(fuzz::kSemPreludes[i], options);
        const auto original = fuzz::RunLuauTrace(Luau::compile(carried, options), prelude);
        const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(result.output, options), prelude);
        CHECK(reconstructed.status == original.status);
        CHECK(reconstructed.trace == original.trace);
    }
}
