#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string CheckSemanticParity(const std::string &source) {
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
        return result.output;
    }
} // namespace

TEST_CASE("Fuzz run 35687267534 evaluates call arguments once", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    local function f0(p1, p2, ...)
        print(select)
        return "key", 138.25
    end
    (nil)(f0("", tonumber))
until (if nil then (if t:set(nil) then ... else ("key" .. obj)) else next:set("x", tostring))
string = obj(string)
for g0_0, g0_1 in select(t) do
    local v2 = ...
end
local v0, v1 = (next)(obj.field), table(((if 284 then "key" else false) * (tonumber)))
v0, v1 = nil, (not 324)
v1 = { select, [false] = false, nil }
local v2 = ((-(-{ k = "hello", [true] = tonumber })) - v0)
return (not obj(next)))LUA");
}

TEST_CASE("Fuzz run 35687267534 preserves open vararg call arity", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    t(obj:set("key"), ...);
    pairs ^= pairs[650];
    tostring(table, { x = t, true, math, [709] = false });
until ({ true, y = 61.5 } // next());
tostring({ [613] = "x", 228.5, "" }, ipairs.field);
((#math))[((if tonumber then string else nil) ~= string)](...);
local function f0(p1, p2)
    local v3 = function(p3, p4, p5)
        p1();
        return ;
    end;
    local v4, v5, v6 = "", nil, "value";
    f0, v5 = "value", "value";
    return "key", t;
end
for i1 = (false and f0), f0:method(true), ipairs:set(next) do
    ipairs(i1);
    local v2, v3, v4 = string, t, i1;
    i1(ipairs, true);
    if ... then
        continue
    end
end
return string.field[f0[true]];)LUA");
}

TEST_CASE("Fuzz run 35687267534 preserves empty vararg diagnostics", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    pairs(...)
    if ipairs["value"]((-(-false)), (print)) then
        continue
    end
until next[tonumber].field:method()
for g0_0, g0_1 in math(nil) do
end
while (-(-tonumber)) do
    for i0 = obj(true), next:method(333, "x"), { field = "a-b", [obj] = pairs, [850] = select, field = "x" } do
        i0(print)
        i0()
        i0("value", ipairs)
    end
    for g0_0 in next(print) do
        tonumber(next, true)
        if print[""] then
            g0_0(table, t)
        else
            g0_0("key", g0_0)
            if string then
                break
            end
        end
    end
    repeat
    until (function(p0, p1, p2, ...)
        return
    end)
    if (tonumber:set()) then
        continue
    end
end
local v0 = next("value")
local v1 = (... * (v0:run("x", pairs)))
if false then
end
v0, v0 = nil, (if v0 then "x" else 186.25)
local function f2()
    f2()
    return string, "value"
end
return function(p3)
    math(t, ipairs)
    obj(225.75, obj)
    t()
    return
end)LUA");
}

TEST_CASE("Fuzz run 35687267534 preserves computed table-key order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = { true }
v0[(834)["a\nb"][true]] = false
v0[(-(36.57142857142857).value)] = true
select[(if { 867 } then ipairs("\nhello", 598) else true)] = - - -v0(nil, false)
v0[v0((false)[v0], (-"hello"))] = #function()
    return function()
        return true
    end
end
return if "hello" then v0() else -8i, v0)LUA");
}

TEST_CASE("Fuzz run 35687267534 keeps statements before infinite nested loop", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(repeat
    (tonumber)(...)
until tonumber[808]()
local v0 = 170
local v1, v2 = true, v0(print)
while function(p3)
    return
end do
    while true do
    end
end
v1 = (if function(p3, p4, p5)
    return obj, 677
end then {  } else (not select.field[nil]))
return v0.field)LUA");
}

TEST_CASE("Fuzz run 35687267534 keeps outer while body before repeat tail", "[Decompiler][FuzzRegress][Semantics]") {
    const auto output = CheckSemanticParity(R"LUA(repeat
    while function(...)
        pairs(nil)
        return tostring, "value"
    end do
        ipairs(nil, nil)
        repeat
        until nil
        local v0 = (if false then nil else 518)
    end
    (nil)(string)
    for g0_0, g0_1 in tonumber(next, obj) do
        math("x")
        local function f2(p3)
            p3()
            return
        end
        g0_1, g0_1 = "x", t
    end
until {  }
pairs -= select:run()
t[""](math.field, {  })
return {  })LUA");
    const auto closureBody = output.find("pairs(nil)");
    REQUIRE(closureBody != std::string::npos);
    CHECK(output.find("pairs(nil)", closureBody + 1) == std::string::npos);
}

TEST_CASE("Nested repeats sharing a header preserve the outer condition", "[Decompiler][FuzzRegress][Semantics]") {
    const std::string source = R"LUA(local function mark(value)
    print(value)
    return value
end

local count = 0
repeat
    repeat
        count += 1
    until mark(count >= 2)
    count += 1
until if mark(false) then false else mark(count >= 5)

print(count)
return count)LUA";
    CheckSemanticParity(source);

    Decompiler decompiler{};
    const auto result =
        decompiler.DecompileTestCode(source, DecompilerFlags::CaptureCFGGraph | DecompilerFlags::FissionDebugNotes, kOptions);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("CFG bytes: " << result.cfgGraph.size());
    CHECK(result.cfgGraph.find("<B>WHY</B>") != std::string::npos);
    CHECK(result.cfgGraph.find("back-edge reaches B1 through inner-loop exit B6") != std::string::npos);
    CHECK(result.cfgGraph.find("preserve inner latch B4") != std::string::npos);
}

TEST_CASE("Fuzz run 35687267534 keeps throwing initializer before later expressions", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(local v0 = - -t.x
local v1 = if nil then -"a-b" else -(148i)
local v2 = "a-b"
v0()
ipairs()
local v3 = function()
    return true
end
local v4 = 69.14285714285714
v3[({ v1, 74.28571428571429 } % nil)] = if v4 + false then (if "value" then -56i else 18) else nil
return self(830), v4("hello"))LUA");
}

TEST_CASE("Fuzz run 35687267534 evaluates constructor elements once in order", "[Decompiler][FuzzRegress][Semantics]") {
    CheckSemanticParity(R"LUA(table(not -nil, { (73i)[434], 172i })
local v0 = tonumber
local v1 = v0(v0(), function()
    return nil
end)
v1[function()
    return 309
end] = (nil)[(35.285714285714285 <= 172i)]
v1(function()
    return 117i // -63i
end <= "key")
return nil, {  })LUA");
}
