//
// Created by Dottik on 25/9/2026.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "LiftingSemanticsTestSupport.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

static std::string DecompileWith(const std::string &source, int optimization, int debug, DecompilerFlags flags) {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    const Luau::CompileOptions options{optimization, debug};
    auto result = decompiler.DecompileTestCode(source, flags, options);
    INFO(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    return std::move(result.decompilationOutput);
}

static void CheckTraceWith(const std::string &source, int optimization, int debug, DecompilerFlags flags = static_cast<DecompilerFlags>(0)) {
    const auto output = DecompileWith(source, optimization, debug, flags);
    const Luau::CompileOptions options{optimization, debug};
    const auto prelude = Luau::compile("", options);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, options), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(output, options), prelude);
    INFO("O" << optimization << " debug " << debug << "\nsource:\n" << source << "\ndecompiled:\n" << output);
    INFO("original:\n" << original.trace << "decompiled:\n" << decompiled.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

static constexpr const char *kValueForms = R"LUA(
local F = {}
F[#F + 1] = function(a, b, c, d) return (a or b) and c end
F[#F + 1] = function(a, b, c, d) return (a or b) and (c and d) end
F[#F + 1] = function(a, b, c, d) return a and (b or c) and d end
F[#F + 1] = function(a, b, c, d) return (a or (b and c)) and d end
F[#F + 1] = function(a, b, c, d) return (not a or b) and c end
F[#F + 1] = function(a, b, c, d) return (a or not b) and c end
F[#F + 1] = function(a, b, c, d) return (a == b or c) and d end
F[#F + 1] = function(a, b, c, d) return (a ~= nil or b) and c end
F[#F + 1] = function(a, b, c, d) return (a and b == c) or d end
F[#F + 1] = function(a, b, c, d) return (a or b) and c, (a or c) and d end
F[#F + 1] = function(a, b, c, d) return ((a or b) or c) and d end
F[#F + 1] = function(a, b, c, d) return (a or 1) and b end
F[#F + 1] = function(a, b, c, d) return (a or b) and 5 end
F[#F + 1] = function(a, b, c, d) return a ~= b and c or d end
F[#F + 1] = function(a, b, c, d) return a and (b or c) or d end
F[#F + 1] = function(a, b, c, d) return (not a) and (not b) or c end
F[#F + 1] = function(a, b, c, d) return ((a or b) and c or d) and a end
F[#F + 1] = function(a, b, c, d) return if a then (b or c) and d else b end
F[#F + 1] = function(a, b, c, d) local v = (a or b) and c return v end
F[#F + 1] = function(a, b, c, d) if a then return a ~= b and c or d end return "no" end
local V = { false, true, nil, 0 }
for n = 1, #F do
    local row = {}
    for p = 1, 4 do for q = 1, 4 do for r = 1, 4 do
        local v1, v2 = F[n](V[p], V[q], V[r], V[(p * r + q) % 4 + 1])
        row[#row + 1] = tostring(v1) .. "/" .. tostring(v2)
    end end end
    print(n, table.concat(row, " "))
end
)LUA";

TEST_CASE("Check pending P1: shared short-circuit arms returned directly keep every path", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    CheckTraceWith(kValueForms, optimization, 1);
}

static constexpr const char *kLoopArms = R"LUA(
local out
local function L(s) out[#out + 1] = tostring(s) end
local F = {}
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) local i = 0 while i < 3 do i += 1 if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) local i = 0 repeat i += 1 if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) until i >= 3 return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then L("a") if b then L("x") break end elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for _, v in { 1, 2, 3 } do if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. v) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") else L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") continue else L("c") end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break else L("y") end elseif b then L("b") continue elseif c then L("c") break end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) local r = 0 for i = 1, 3 do if a then if c then r = -1 break end r += 1 elseif b then r += 10 continue end r += 100 end return r end
F[#F + 1] = function(a, b, c) local i = 0 while true do i += 1 if i > 3 then break end if a then if c then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
local V = { false, true, nil }
for n = 1, #F do
    out = {}
    for p = 1, 3 do for q = 1, 3 do for r = 1, 3 do
        L("=" .. tostring(F[n](V[p], V[q], V[r])))
    end end end
    print(n, table.concat(out, " "))
end
)LUA";

TEST_CASE("Check pending P2: loop tails shared with a nested break arm survive a continue arm", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    CheckTraceWith(kLoopArms, optimization, 1);
}

TEST_CASE("Check pending P3: move-elided inline arguments do not redeclare loop-carried locals", "[Decompiler][CheckPending][Semantics]") {
    const std::string source = GENERATE(
        std::string("local out = {}\nlocal function L(s) out[#out + 1] = s end\n"
                    "local function f(b) local x = 1 if b then x = x + 10 end while x > 5 do x -= 4 L(x) end return x end\n"
                    "print(f(true), f(false), table.concat(out, ','))"),
        std::string("local out = {}\nlocal function L(s) out[#out + 1] = s end\n"
                    "local function f(c) local x = c + 0 repeat x -= 4 L(x) until x <= 5 return x end\n"
                    "print(f(11), f(1), #out)"),
        std::string("local out = {}\nlocal function L(s) out[#out + 1] = s end\n"
                    "local function f(c) local x = c + 0 while x > 5 do x -= 4 L(x) L(x) end return x end\n"
                    "print(f(11), f(1), #out)")
    );
    CheckTraceWith(source, 2, 2);
}

TEST_CASE("Check pending P4: generated parameter names do not capture globals", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 1, 2);
    CheckTraceWith(
        "arg0, arg1, arg2 = 'GA0', 'GA1', 'GA2'\nlocal function p1(a, b) return a, b, arg0, arg1, arg2 end\nprint(p1(1, 2))\n"
        "local function p2(...) local a = ... return a, arg0 end\nprint(p2('va'))",
        optimization, debug
    );
}

TEST_CASE("Check pending P5: prefixed generated names stay local", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const std::string source = GENERATE(
        std::string("v0, v1, v2 = 1, 2, 3\nlocal function g(x) local y = x .. '!' return y end\nprint(g('a'), v0, v1, v2)\n"
                    "do local m = { 1 } end\nprint(type(rawget(_G, '_v0')), type(rawget(_G, '_v1')), type(rawget(_G, '_v2')))"),
        std::string("v0, v1, v2, v3, v4, v5, v6 = 'g0', 'g1', 'g2', 'g3', 'g4', 'g5', 'g6'\n"
                    "local function f(x) local a = x .. '!' local b = a .. '?' print(v0, v1, v2, v3, v4, v5, v6) return b end\nprint(f('p'))\n"
                    "do local math = { floor = function() return 'shadow' end } end\n"
                    "print(math.floor(1.5), type(rawget(_G, '_v1')), type(rawget(_G, '_v2')), type(rawget(_G, '_v3')))")
    );
    CheckTraceWith(source, optimization, debug);
}

TEST_CASE("Check pending P6: IR constant propagation keeps closure-written locals", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    const std::string source = GENERATE(
        std::string("local a = 0\nlocal function set() a = 5 end\nset()\nprint(a)"),
        std::string("local function upv() local a = 1 local function set(v) a = v end local function get() return a end set(5) return get(), a end\nprint(upv())"),
        std::string("local cnt = 0\nlocal fs = {}\nfor i = 1, 3 do fs[i] = function() cnt += i return cnt end end\nprint(fs[1](), fs[2](), fs[3](), cnt)")
    );
    CheckTraceWith(source, optimization, 1, DecompilerFlags::OptimizeIR);
}

TEST_CASE("Check pending P7: class self and forward references survive decompilation", "[Decompiler][CheckPending][Class][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    std::vector<std::pair<Luau::FValue<bool> *, bool>> saved;
    for (auto *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strcmp(flag->name, "DebugLuauUserDefinedClasses") == 0 || std::strcmp(flag->name, "DebugLuauUserDefinedClassesRuntime") == 0) {
            saved.emplace_back(flag, flag->value);
            flag->value = true;
        }
    REQUIRE(saved.size() == 2);
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const std::string source = GENERATE(
        std::string("class C function make(self) return C.new({}) end function tag(self) return 'c' end end return C.new({}):make():tag()"),
        std::string("class C function make(self) return C end end return C.new({}):make() == C"),
        std::string("class A function f(self) return B end end class B function g(self) return 'b' end end return A.new({}):f().new({}):g()")
    );
    CheckTraceWith(source, optimization, debug);
    for (auto [flag, value] : saved)
        flag->value = value;
}

TEST_CASE("Check pending P8: automatic names do not shadow globals read in scope", "[Decompiler][CheckPending][Semantics]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const std::string source = GENERATE(
        std::string("local folder = { FindFirstChild = function(self, c) return { Name = c } end, WaitForChild = function(self, c) return { Name = c } end }\n"
                    "script = { Name = 'real-script' }\nworkspace = { Name = 'real-ws' }\n"
                    "local function f() local a = folder:FindFirstChild('script') local b = folder:WaitForChild('print') local c = folder:FindFirstChild('workspace') "
                    "print(a.Name, b.Name, c.Name, script.Name, workspace.Name) end\nf()\n"
                    "local function h() local s = folder:FindFirstChild('tostring') return s.Name, tostring(1) end\nprint(h())"),
        std::string("local services = {}\ngame = { GetService = function(self, n) services[n] = services[n] or { Name = n } return services[n] end }\n"
                    "Players = 'GLOBAL-Players'\nlocal function f() local p = game:GetService('Players') return p.Name, Players end\nprint(f())")
    );
    CheckTraceWith(source, optimization, debug, DecompilerFlags::AutoNameVariables | DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes);
}

TEST_CASE("Check pending N1-N5, N8: compiler lowerings read back in their source form", "[Decompiler][CheckPending][Semantics]") {
    const std::string source = R"LUA(local function f(x) return x end
print(f(1))
local t = {}
t[#t + 1] = f(2)
for i = 1, 3 do print(i) end
local n = f(4)
if n > 3 then print("gt") end
while n >= 2 do n -= 1 end
print(`v={n} 100%`, `{f(5)}{"lit"}\{`, t[1]))LUA";
    const auto output = DecompileWith(source, 1, 1, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK(lifting_semantics_test::Contains(output, "print(f(1))"));
    CHECK(lifting_semantics_test::Contains(output, "[#v1 + 1] = f(2)"));
    CHECK(lifting_semantics_test::Contains(output, "= 1, 3 do"));
    CHECK(lifting_semantics_test::Contains(output, " > 3 then"));
    CHECK(lifting_semantics_test::Contains(output, " >= 2 do"));
    CHECK(lifting_semantics_test::Contains(output, " 100%`"));
    CHECK(lifting_semantics_test::Contains(output, "lit\\{`"));
    CHECK_FALSE(lifting_semantics_test::Contains(output, ":format("));
    const int optimization = GENERATE(0, 1, 2);
    CheckTraceWith(source, optimization, 1);
}

TEST_CASE("Check pending N4 O0: nested arguments keep the callee inline", "[Decompiler][CheckPending][Semantics]") {
    const std::string source = R"LUA(print(type(rawget(_G, "x")), type(rawget(_G, "y"))))LUA";
    const auto output = DecompileWith(source, 0, 1, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK_FALSE(lifting_semantics_test::Contains(output, "= print"));
    CHECK(lifting_semantics_test::Contains(output, "print(type(rawget(_G, \"x\")), type(rawget(_G, \"y\")))"));
    CheckTraceWith(source, 0, 1);
    const std::string mutatingArgument = R"LUA(
local original = print
print = function(a, b) original("before", a, b) end
local function change()
    print = function(a, b) original("after", a, b) end
    return "x"
end
print(change(), type(rawget(_G, "missing")))
)LUA";
    for (int optimization : {0, 1, 2})
        CheckTraceWith(mutatingArgument, optimization, 1);
}

TEST_CASE("Check pending: the most negative integer constant renders as valid source", "[Decompiler][CheckPending][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    Luau::BytecodeBuilder builder{};
    const uint32_t main = builder.beginFunction(0, false);
    const int32_t constant = builder.addConstantInteger(std::numeric_limits<int64_t>::min());
    REQUIRE(constant >= 0);
    builder.emitAD(LOP_LOADK, 0, static_cast<int16_t>(constant));
    builder.emitABC(LOP_RETURN, 0, 2, 0);
    builder.endFunction(1, 0);
    builder.setMainFunction(main);
    builder.finalize();
    const auto bytecode = builder.getBytecode();
    const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    INFO(output);
    const auto compiled = Luau::compile(output);
    REQUIRE_FALSE(compiled.empty());
    REQUIRE(compiled.front() != '\0');
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(compiled, prelude);
    INFO("original: " << original.trace << "reconstructed: " << reconstructed.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Check pending N9: debug level 2 names locals and parameters from debug info", "[Decompiler][CheckPending][Naming][Semantics]") {
    const std::string source = R"LUA(local function pick(first, second, third)
    local value
    repeat
        value = first and 1
        if not value then break end
        value = second or 2
    until true
    return value, third
end
local function walk(items)
    local total, count = 0, #items
    for index = 1, count do total += items[index] end
    for key, item in pairs(items) do total += key * item end
    local shadow = total
    do local shadow = shadow + 1 total = shadow end
    return total, shadow
end
print(pick(true, false), pick(false, true), pick(true, 5))
print(walk({ 3, 4, 5 })))LUA";
    const auto output = DecompileWith(source, 1, 2, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK(lifting_semantics_test::Contains(output, "local function pick(first, second, third)"));
    CHECK(lifting_semantics_test::Contains(output, "local value = first and 1"));
    CHECK(lifting_semantics_test::Contains(output, "return value, third"));
    CHECK(lifting_semantics_test::Contains(output, "local total = 0"));
    CHECK(lifting_semantics_test::Contains(output, "local count = #items"));
    CHECK(lifting_semantics_test::Contains(output, "for index = 1, count do"));
    CHECK(lifting_semantics_test::Contains(output, "for key, item in pairs(items) do"));
    CHECK(lifting_semantics_test::Contains(output, "local shadow = total"));
    const int optimization = GENERATE(0, 1, 2);
    CheckTraceWith(source, optimization, 2);
}

TEST_CASE("Check pending P9: vanilla decompilation keeps vector.create", "[Decompiler][CheckPending]") {
    const int optimization = GENERATE(0, 1, 2);
    const auto output = DecompileWith("local v = vector.create(1, 2, 3)\nprint(v, v.x)", optimization, 1, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK_FALSE(lifting_semantics_test::Contains(output, "Vector3"));
    CHECK(lifting_semantics_test::Contains(output, "vector.create"));
}

TEST_CASE("Check pending N6: export table reconstructs value and function declarations", "[Decompiler][CheckPending][Export]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const std::string source = "export local x = 5\nexport local y = 10\nexport function add(a, b) return a + b + x end\ny = add(y, 1)";
    const auto output = DecompileWith(source, optimization, debug, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK(lifting_semantics_test::Contains(output, "export local x"));
    CHECK(lifting_semantics_test::Contains(output, "export local y"));
    CHECK(lifting_semantics_test::Contains(output, "export local y = 10"));
    CHECK(lifting_semantics_test::Contains(output, "export function add("));
    CHECK_FALSE(lifting_semantics_test::Contains(output, "table.freeze"));
    CHECK_FALSE(lifting_semantics_test::Contains(output, "__EXP"));
    CHECK_FALSE(lifting_semantics_test::Contains(output, "\nlocal x = 5\n"));
    const auto compiled = Luau::compile(output, Luau::CompileOptions{optimization, debug});
    REQUIRE_FALSE(compiled.empty());
    CHECK(compiled.front() != '\0');
}

TEST_CASE("Check pending N6: deferred export stores and ordinary frozen tables", "[Decompiler][CheckPending][Export]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const auto exported = DecompileWith("local function make() return 7 end\nexport local value = make()\n"
                                        "export function get() return value end\nvalue = get() + 1", optimization, debug,
                                        static_cast<DecompilerFlags>(0));
    INFO(exported);
    CHECK(lifting_semantics_test::Contains(exported, "export local value"));
    CHECK(lifting_semantics_test::Contains(exported, "export function get("));
    CHECK_FALSE(lifting_semantics_test::Contains(exported, "table.freeze"));
    const auto compiled = Luau::compile(exported, Luau::CompileOptions{optimization, debug});
    REQUIRE_FALSE(compiled.empty());
    CHECK(compiled.front() != '\0');

    const auto ordinary = DecompileWith("local t = {answer = 42}\nreturn table.freeze(t)", optimization, debug,
                                        static_cast<DecompilerFlags>(0));
    INFO(ordinary);
    CHECK(lifting_semantics_test::Contains(ordinary, "table.freeze"));
    CHECK_FALSE(lifting_semantics_test::Contains(ordinary, "export local"));
}

TEST_CASE("Check pending N6: generated parameter shadow keeps export table form", "[Decompiler][CheckPending][Export]") {
    const auto output = DecompileWith("export local arg0 = 5\nexport function f(value) return arg0 + value end", 0, 0,
                                      static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK(lifting_semantics_test::Contains(output, "table.freeze"));
    const auto compiled = Luau::compile(output, Luau::CompileOptions{0, 0});
    REQUIRE_FALSE(compiled.empty());
    CHECK(compiled.front() != '\0');
}

TEST_CASE("Check pending N6: sole export function precedes its table", "[Decompiler][CheckPending][Export]") {
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    const auto output = DecompileWith("export function only() return 5 end", optimization, debug, static_cast<DecompilerFlags>(0));
    INFO(output);
    CHECK(lifting_semantics_test::Contains(output, "export function only("));
    CHECK_FALSE(lifting_semantics_test::Contains(output, "table.freeze"));
    const auto compiled = Luau::compile(output, Luau::CompileOptions{optimization, debug});
    REQUIRE_FALSE(compiled.empty());
    CHECK(compiled.front() != '\0');
}
