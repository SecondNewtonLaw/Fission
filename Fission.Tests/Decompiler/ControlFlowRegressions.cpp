//
// Created by Dottik on 2/6/2026.
//

// Control-flow regression tests; each targets a known/fixed bug.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "ControlFlowTestSupport.hpp"
#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
#include <string>

namespace control_flow_test {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::string DecompileOrFail(const std::string &source, int optLevel, int debugLevel) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = debugLevel;
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    bool ContainsRegex(const std::string &haystack, const std::regex &pattern) { return std::regex_search(haystack, pattern); }

    // true if `src` recompiles to valid bytecode (first byte 0 = Luau error marker).
    bool Recompiles(const std::string &src) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string bc = Luau::compile(src, opts);
        return !bc.empty() && bc[0] != '\0';
    }

    std::string FirstBetween(const std::string &haystack, const std::string &begin, const std::string &end) {
        const size_t beginPos = haystack.find(begin);
        if (beginPos == std::string::npos)
            return {};

        const size_t bodyPos = beginPos + begin.size();
        const size_t endPos = haystack.find(end, bodyPos);
        if (endPos == std::string::npos)
            return {};

        return haystack.substr(bodyPos, endPos - bodyPos);
    }

    size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
        if (needle.empty())
            return 0;
        size_t count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

} // namespace control_flow_test

using namespace control_flow_test;

// Repeat-until body preservation
// Known bug: repeat i = i + 1 until i >= 10  ->  while 10 >= v0 do end
// The loop body (i = i + 1) is completely eaten.
TEST_CASE("Regress: repeat-until body is not eaten (simple counter)", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local i = 0
            repeat
                i = i + 1
            until i >= 10
            return i
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: repeat-until with computation body is preserved", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(acc, x)
            repeat
                acc = acc + x
                x = x - 1
            until x <= 0
            return acc
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(0,3),f(7,1),f(-2,5)))DRIVER"
    );
}

TEST_CASE("Regress: while-true-break keeps body statements", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local x = 0
            while true do
                x = x + 1
                if x >= 100 then
                    break
                end
            end
            return x
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: and-or mixed short-circuit does not produce self-assign", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        return function(a, b, c, d)
            return a and b or c and d
        end
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,a in {false,true} do for _,b in {false,true} do for _,c in {false,true} do for _,d in {false,true} do print(f(a,b,c,d)) end end end end)DRIVER"
    );
}

TEST_CASE("Regress: OR-chain dispatch folds and does not clobber sibling body", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function pick(mode)
            local r
            if mode == "a" or mode == "b" or mode == "c" or mode == "d" then
                r = 100
            elseif mode == "e" then
                r = 200
            else
                return -1
            end
            return r
        end
        return pick
    )LUA",
        R"DRIVER(local f=__integration_subject(); for _,mode in {"a","b","c","d","e","unknown"} do print(f(mode)) end)DRIVER"
    );
}

TEST_CASE("Regress: break in else branch does not kill enclosing loop", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            for i = 1, 20 do
                if i < 3 then
                    print("low",i)
                elseif i < 7 then
                    print("mid",i)
                else
                    print("high",i)
                    break
                end
            end
            return 0
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: while-true-break with multiple exit conditions preserves body", "[Decompiler][Loop][Regression]") {
    const std::string source = R"(
        local function f()
            local x = 0
            local y = 0
            while true do
                x = x + 1
                if x > 100 then break end
                y = y + x
                if y > 500 then break end
            end
            return y
        end
        return f()
    )";
    const auto out = DecompileOrFail(source);

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(
        out,
        std::regex(
            R"((?:while\s+true|repeat)[\s\S]*x\s*\+=\s*1[\s\S]*if\s+x\s*>\s*100\s+then\s+break[\s\S]*y\s*\+=\s*x[\s\S]*(?:if\s+y\s*>\s*500\s+then\s+break|until\s+\(?y\s*>\s*500\)?)[\s\S]*return\s+y)"
        )
    ));
    CHECK(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 528\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

// Repeat-until with continue
// Known bug (from stress test): body of repeat-until with continue is split

TEST_CASE("Regress: repeat-until with continue preserves complete body", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local i = 0
            local s = 0
            repeat
                i = i + 1
                if i % 2 == 0 then
                    continue
                end
                s = s + i
            until i >= 10
            return s
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: continue inside numeric for preserves loop body", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local s = 0
            for i = 1, 10 do
                if i % 2 == 0 then
                    continue
                end
                s = s + i
            end
            return s
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: repeat-until with table indexing body is preserved", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t)
            local i = 1
            local acc = 0
            repeat
                acc = acc + t[i]
                i = i + 1
            until t[i] == nil
            return acc
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f({3}),f({1,2,3}),f({-4,8})))DRIVER"
    );
}

TEST_CASE("Regress: and-or mixed expression in while loop body avoids self-assign", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local i = 0
            local found = false
            while not found do
                i = i + 1
                found = i > 10 or i % 3 == 0
            end
            return i
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: nested for loops with inner conditional break preserves nesting", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local s = 0
            for outer = 1, 5 do
                for inner = 1, 10 do
                    if inner * outer > 20 then
                        s = s + inner
                        break
                    end
                end
            end
            return s
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: for loop with if-elseif-else body does not get spurious break", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(n)
            for i = 1, n do
                if i % 15 == 0 then
                    print(1)
                elseif i % 3 == 0 then
                    print(2)
                elseif i % 5 == 0 then
                    print(3)
                else
                    print(4)
                end
            end
            return 0
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(0)); print(f(16)))DRIVER"
    );
}

TEST_CASE("Regress: while-and compound condition body survives", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(x, y)
            while x < y and y > 0 do
                local a = x + 1
                local b = y - 1
                x, y = a, b
            end
            return x, y
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(0,4)); print(f(4,0)); print(f(5,2)))DRIVER"
    );
}

TEST_CASE("Regress: repeat-until with inner break preserves body", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local i = 0
            local s = 0
            repeat
                if i > 100 then break end
                s = s + i
                i = i + 1
            until i >= 1000
            return s
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: generic-for inline table preserves numeric content", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local s = 0
            for _, v in pairs({10, 20, 30}) do
                s = s + v
            end
            return s
        end
        return f
    )LUA",
        R"DRIVER(print(__integration_subject()()))DRIVER"
    );
}

TEST_CASE("Regress: early return inside generic for loop preserves for structure", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t, target)
            for i, v in ipairs(t) do
                if v == target then
                    return i
                end
            end
            return -1
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f({},7),f({3,7,9},7),f({3,7},8)))DRIVER"
    );
}

TEST_CASE("Generic loop bindings do not suppress later local declarations", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        for a in first() do
            for b in second() do
                for c in third() do
                    for d in fourth() do
                    end
                end
            end
        end

        for i = 1, 2 do
            local a, b, c = one(), two(), three()
            sink(a.field ~= nil, b, c)
        end
    )LUA",
        R"DRIVER(for _,name in {"first","second","third","fourth"} do _G[name]=function() print(name); return ipairs({1,2}) end end; one=function() return {field=true} end; two=function() return 2 end; three=function() return 3 end; sink=print; __integration_subject())DRIVER"
    );
}

TEST_CASE("Branch-local register reuse does not leak a global", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f0(...)
            print("call")
        end

        if flag then
            local x = if cond then f0() else other()
            sink(x)
        end

        local y = if next then 1 else function()
            f0()
        end
        sink(y)
        return y
    )LUA",
        R"DRIVER(other=function() return 9 end; sink=function(v) if type(v)=="function" then v(); print("function") else print(v) end end; for _,enabled in {false,true} do flag=enabled; cond=enabled; next=enabled; local y=__integration_subject(); sink(y) end)DRIVER"
    );
}

TEST_CASE("Sibling branches keep independent local bindings", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        if first then
            repeat
                local value = make()
                sink(value)
            until done
        end

        if flag then
            local value = not ...
            sink(value)
        else
            local value, secondValue, thirdValue = other(), second(), third()
            sink(value)
        end
        finish()
    )LUA",
        R"DRIVER(done=true; make=function() return 7 end; other=function() return 8 end; second=function() return 9 end; third=function() return 10 end; sink=print; finish=function() print("finish") end; for _,enabled in {false,true} do first=enabled; flag=enabled; __integration_subject(true) end)DRIVER"
    );
}

TEST_CASE("Regress: nested repeat-until body survives with trailing while loop", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(x)
            repeat
                repeat
                    error("bye bye bye!")
                until x
                warn("hi")
                print("hi")
                repeat
                    error("hello hello hello!")
                until x
            until x
            while x do
                warn("bye")
                warn("can you believe this?")
            end
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(pcall(f,true)); print(pcall(f,false)))DRIVER"
    );
}

TEST_CASE("Regress: dict-style keys survive in an inline pairs() table literal", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            for k, v in pairs({hello = "world", num = 42}) do
                print(k, v)
            end
        end
        return f
    )LUA",
        R"DRIVER(__integration_subject()())DRIVER"
    );
}

TEST_CASE("Regress: while compound condition is preserved (header `and` or equivalent break-guard)", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(a, b, limit)
            while a < limit and b > 0 do
                a = a + 1
                b = b - 1
            end
            return a, b
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(0,5,3)); print(f(4,0,9)); print(f(10,2,5)))DRIVER"
    );
}

TEST_CASE("Regress: while-or preserves short-circuit order", "[Decompiler][Loop][Regression]") {
    const std::string source = R"(
        local function run(first, second)
            trace = ""
            local function left()
                trace ..= "l"
                return second
            end
            local function right()
                trace ..= "r"
                return false
            end
            local iterations = 0
            while first or left() or right() do
                trace ..= "b"
                first = false
                iterations += 1
                if iterations == 2 then break end
            end
            return trace
        end
        return run(true, false), run(false, true), run(false, false)
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    CHECK(Recompiles(out));

    const std::string originalBc = Luau::compile(source);
    const std::string decompiledBc = Luau::compile(out);
    const std::string preludeBc = Luau::compile("");
    REQUIRE(!originalBc.empty());
    REQUIRE(!decompiledBc.empty());
    const auto verdict = fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: \"blr\"\t\"lblb\"\t\"lr\"\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: short-circuit fallback survives an effectful terminal join", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function choose(a, b, c)
            local value = a and b or c
            print(value)
        end
        choose(false, "B", "C")
        choose(true, false, "C")
        choose(true, nil, "C")
        choose(true, "B", "C")
        local checks = 0
        local item = { IsA = function(self, kind)
            checks += 1
            return false
        end }
        local target = {}
        local function assign(obj)
            local value = obj and obj:IsA("VehicleSeat") or false
            target.value = value
        end
        assign(nil)
        assign(item)
        return target.value, checks
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}
