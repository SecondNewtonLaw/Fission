//
// Variation matrix for ConditionFolder and the and-chain fold: several source forms that lower to the
// same shapes, at every optimization/debug level, through both the default and the OptimizeIR pipeline.
//

#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include <catch2/catch_test_macros.hpp>
#include <format>
#include <string>

namespace {
    // O0/O1/O2 x D0/D1/D2 for the default pipeline, then the OptimizeIR pipeline via integration_test::Check.
    void CheckMatrix(const std::string &source, const std::string &driver, bool checkOptimizeIr = true) {
        fuzz::EnableLuauFlags();
        for (const int optimization : {0, 1, 2}) {
            for (const int debug : {0, 1, 2}) {
                INFO("O" << optimization << " D" << debug << "\n" << source);
                const Luau::CompileOptions options{optimization, debug};
                const auto bytecode = Luau::compile(source, options);
                REQUIRE_FALSE(bytecode.empty());
                REQUIRE(bytecode.front() != '\0');
                Decompiler decompiler{};
                const auto result = decompiler.DecompileVanillaBytecode(bytecode, DecompilerFlags::AutoNameVariables);
                REQUIRE(result.resultCode == DecompileResult::Success);
                integration_test::CheckOutput(bytecode, result.decompilationOutput, options, "math.randomseed(0)", driver);
            }
        }
        if (checkOptimizeIr)
            integration_test::Check(source, driver);
    }

    const std::string kAndChainSource = R"LUA(
local function ac2(a, b) return a and b end
local function ac3(a, b, c) return a and b and c end
local function ac5(a, b, c, d, e) return a and b and c and d and e end
local function acConst(a, b) return a and b and true end
local function acNil(a, b) return a and b and nil end
local function acFalse(a, b) return a and b and false end
local function acCall(a, b, f) return a and b and f() end
local function acIndex(a, b, t) return a and b and t.key end
local function acTable(a, b) return { value = a and b } end
local function acArg(a, b, f) return f(a and b) end
local function acMixed(a, b, c, d) return (a and b) or (c and d) end
return ac2, ac3, ac5, acConst, acNil, acFalse, acCall, acIndex, acTable, acArg, acMixed
)LUA";

    const std::string kAndChainDriver = R"LUA(
local ac2, ac3, ac5, acConst, acNil, acFalse, acCall, acIndex, acTable, acArg, acMixed = __integration_subject()
local function show(...)
    local n = select('#', ...)
    local parts = {}
    for i = 1, n do
        local v = select(i, ...)
        parts[i] = tostring(v) .. ':' .. type(v)
    end
    print(table.concat(parts, '|'))
end
show(ac2(true, 1), ac2(false, 1), ac2(nil, 1), ac2(true, nil))
show(ac3(true, true, 3), ac3(true, false, 3), ac3(1, 2, 3), ac3(nil, 2, 3))
show(ac5(1, 2, 3, 4, 5), ac5(1, nil, 3, 4, 5), ac5(nil, 2, 3, 4, 5))
show(acConst(true, 1), acConst(false, 1), acConst(nil, 1))
show(acNil(true, 1), acNil(true, nil), acNil(false, 1))
show(acFalse(true, 1), acFalse(true, nil), acFalse(false, 1))
local calls = 0
local function f() calls = calls + 1 return 'F' end
show(acCall(true, true, f), calls)
show(acCall(true, false, f), calls)
show(acCall(false, true, f), calls)
local t = { key = 'K' }
show(acIndex(true, true, t), acIndex(true, false, t), acIndex(true, true, {}))
local tb = acTable(true, 'V'); show(tb.value, acTable(false, 'V').value)
local seen = 'unset'
local function g(v) seen = v return v end
show(acArg(true, 'A', g), seen)
show(acArg(false, 'A', g), seen)
show(acMixed(true, 'B', true, 'D'), acMixed(false, nil, true, 'D'), acMixed(false, nil, false, 'D'))
)LUA";

    const std::string kWhileGuardSource = R"LUA(
local function wg2(a, b)
    local n = 0
    while a > 0 and b > 0 do
        a = a - 1
        b = b - 1
        n = n + 1
    end
    return a, b, n
end
local function wg3(a, b, c)
    local n = 0
    while a > 0 and b > 0 and c > 0 do
        a = a - 1
        n = n + 1
    end
    return a, n
end
local function wgCall(t, f)
    local n = 0
    while t.left > 0 and f(t) do
        t.left = t.left - 1
        n = n + 1
    end
    return n
end
local function wgContinue(a, b)
    local n = 0
    while a > 0 and b > 0 do
        a = a - 1
        if a % 2 == 0 then
            n = n + 1
            continue
        end
        b = b - 1
    end
    return a, b, n
end
local function wgBreak(a, b)
    while a > 0 and b > 0 do
        if a == 3 then
            break
        end
        a = a - 1
    end
    return a, b
end
local function wgNested(a, b)
    local n = 0
    while a > 0 and b > 0 do
        for i = 1, 2 do
            n = n + i
        end
        a = a - 1
        b = b - 1
    end
    return n
end
return wg2, wg3, wgCall, wgContinue, wgBreak, wgNested
)LUA";

    const std::string kWhileGuardDriver = R"LUA(
local wg2, wg3, wgCall, wgContinue, wgBreak, wgNested = __integration_subject()
print(wg2(4, 3))
print(wg2(0, 5))
print(wg2(2, 0))
print(wg2(7, 9))
print(wg3(5, 4, 3))
print(wg3(5, 0, 3))
local gate = true
local function f(t) local r = gate and t.left < 4; gate = not gate; return r end
print(wgCall({ left = 6 }, f))
print(wgContinue(7, 5))
print(wgContinue(4, 9))
print(wgBreak(9, 2))
print(wgBreak(2, 9))
print(wgNested(3, 4))
)LUA";

    const std::string kNegationSource = R"LUA(
local function nEq(a, b) return not (a == b) end
local function nNeq(a, b) return not (a ~= b) end
local function nOr(a, b) return not (not a or not b) end
local function nAnd(a, b) return not (not a and not b) end
local function nMixed(a, b, c) return not (not a or b or not c) end
local function nCond(a, b)
    if not (not a or not b) then
        return 'both'
    end
    return 'not'
end
local function nWhile(a, b)
    local n = 0
    while not (not a or not b) do
        n = n + 1
        a = false
    end
    return n
end
local function nValue(a, b)
    local v = not (not a or not b)
    return v, type(v)
end
return nEq, nNeq, nOr, nAnd, nMixed, nCond, nWhile, nValue
)LUA";

    const std::string kNegationDriver = R"LUA(
local nEq, nNeq, nOr, nAnd, nMixed, nCond, nWhile, nValue = __integration_subject()
local function show(...)
    local n = select('#', ...)
    local parts = {}
    for i = 1, n do
        local v = select(i, ...)
        parts[i] = tostring(v) .. ':' .. type(v)
    end
    print(table.concat(parts, '|'))
end
local values = { nil, false, true, 0, '', 'x', 1 }
for i = 1, 7 do
    for j = 1, 7 do
        local a, b = values[i], values[j]
        show(nEq(a, b), nNeq(a, b), nOr(a, b), nAnd(a, b))
    end
end
show(nMixed(true, false, true), nMixed(false, true, true), nMixed(true, true, false))
show(nCond(true, true), nCond(true, false), nCond(nil, true))
show(nWhile(true, true), nWhile(false, true), nWhile(1, 'x'))
show(nValue(true, true), nValue(true, nil), nValue(nil, nil))
)LUA";

    const std::string kOneShotSource = R"LUA(
local function osPlain(n)
    local acc = 0
    repeat
        acc = acc + n
        n = n - 1
    until true
    return acc
end
local function osGuard(c)
    local r = 0
    repeat
        if not c then break end
        r = 1
    until true
    return r
end
local function osGuardNeg(c)
    local r = 0
    repeat
        if c then break end
        r = 1
    until true
    return r
end
local function osBodyBreak(n)
    local r = 0
    repeat
        r = r + 1
        if r >= n then break end
        r = r + 10
    until true
    return r
end
local function osBodyContinue(n)
    local r = 0
    repeat
        r = r + 1
        if r < n then continue end
        r = r + 100
    until true
    return r
end
return osPlain, osGuard, osGuardNeg, osBodyBreak, osBodyContinue
)LUA";

    const std::string kOneShotDriver = R"LUA(
local osPlain, osGuard, osGuardNeg, osBodyBreak, osBodyContinue = __integration_subject()
print(osPlain(3), osPlain(0), osPlain(-2))
print(osGuard(true), osGuard(false), osGuard(nil), osGuard(0))
print(osGuardNeg(true), osGuardNeg(false), osGuardNeg(nil))
print(osBodyBreak(1), osBodyBreak(2), osBodyBreak(5))
print(osBodyContinue(1), osBodyContinue(2), osBodyContinue(3))
)LUA";
} // namespace

TEST_CASE("Matrix: and-chain return forms", "[Decompiler][Condition][Matrix][Integration]") { CheckMatrix(kAndChainSource, kAndChainDriver); }

TEST_CASE("Matrix: while guard forms", "[Decompiler][Condition][Matrix][Integration]") { CheckMatrix(kWhileGuardSource, kWhileGuardDriver); }

TEST_CASE("Matrix: negation forms", "[Decompiler][Condition][Matrix][Integration]") { CheckMatrix(kNegationSource, kNegationDriver); }

TEST_CASE("Matrix: one-shot repeat forms", "[Decompiler][Condition][Matrix][Integration]") { CheckMatrix(kOneShotSource, kOneShotDriver); }

TEST_CASE("Matrix: dead empty if with constant conditions", "[Decompiler][Condition][Matrix][Integration]") {
    const std::string source = R"LUA(
local function de(x)
    if not 1 then end
    if nil then end
    if false then end
    if true then return 1 end
    if x then return 2 end
    return 3
end
return de
)LUA";
    const std::string driver = "local de=__integration_subject(); print(de(true), de(false), de(nil))";
    CheckMatrix(source, driver);
}

TEST_CASE("Matrix: many folded shapes in one decompile", "[Decompiler][Condition][Matrix]") {
    fuzz::EnableLuauFlags();
    constexpr int kCopies = 100;
    // Table fields rather than locals: the decompiler does not preserve arbitrary source scopes, so a few
    // hundred local functions would exceed Luau's 200-local compiler limit in the emitted chunk.
    std::string source = "local sink = {}\nlocal function take(...) sink[#sink + 1] = select('#', ...) end\n";
    for (int i = 0; i < kCopies; ++i) {
        source += "sink[#sink + 1] = function(a, b, c, d, e) return a and b and c and d and e end\n"
                  "take(sink[#sink](1, 2, 3, 4, 5), sink[#sink](1, nil, 3, 4, 5))\n";
        source += "sink[#sink + 1] = function(a, b, c)\n"
                  "    local n = 0\n"
                  "    while a > 0 and b > 0 and c > 0 do\n"
                  "        a = a - 1\n"
                  "        b = b - 1\n"
                  "        n = n + 1\n"
                  "    end\n"
                  "    return n\n"
                  "end\n"
                  "take(sink[#sink](4, 3, 2), sink[#sink](0, 1, 1))\n";
        source += "sink[#sink + 1] = function(a, b)\n"
                  "    if not (not a or not b) then\n"
                  "        return not (a == b)\n"
                  "    end\n"
                  "    return not (not a and not b)\n"
                  "end\n"
                  "take(sink[#sink](true, true), sink[#sink](nil, true))\n";
        source += "sink[#sink + 1] = function(n)\n"
                  "    local acc = 0\n"
                  "    repeat\n"
                  "        acc = acc + n\n"
                  "        n = n - 1\n"
                  "    until true\n"
                  "    return acc\n"
                  "end\n"
                  "take(sink[#sink](3))\n";
    }
    source += "return #sink\n";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, DecompilerFlags::AutoNameVariables, Luau::CompileOptions{1, 1});
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled bytes: " << result.decompilationOutput.size());
    const auto &output = result.decompilationOutput;
    // every shape must fold, at scale, without disturbing the rest of the chunk.
    CHECK(output.find("return arg0 and arg1 and arg2 and arg3 and arg4") != std::string::npos);
    CHECK(output.find("while arg0 > 0 and arg1 > 0 and arg2 > 0 do") != std::string::npos);
    CHECK(output.find("arg0 ~= arg1") != std::string::npos);
    CHECK(output.find("until true") == std::string::npos);
    const auto recompiled = Luau::compile(output, Luau::CompileOptions{1, 1});
    REQUIRE_FALSE(recompiled.empty());
    REQUIRE(recompiled.front() != '\0');
}
