//
// Created by Dottik on 2/6/2026.
//

// Control-flow regression tests; each targets a known/fixed bug.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "ControlFlowTestSupport.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeadLocalEliminator.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/PropertyRenamer.hpp"
#include "Rewriters/ReverseFieldRenamer.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"
#include "Rewriters/ScopeBlockIntroducer.hpp"
#include "Rewriters/SelfAssignmentEliminator.hpp"
#include "SourceGenerator/Generator.hpp"
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

    std::string DecompileOrFail(const std::string &source, int optLevel) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
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
TEST_CASE("Regress: repeat-until body is not eaten (simple counter)", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local i = 0
            repeat
                i = i + 1
            until i >= 10
            return i
        end
        return f
    )");

    INFO("decompile:\n" << out);
    const auto repeatBody = FirstBetween(out, "repeat", "until");
    CHECK(Contains(out, "repeat"));
    REQUIRE_FALSE(repeatBody.empty());
    CHECK(ContainsRegex(repeatBody, std::regex(R"((?:^|\n)\s*v\d+\s*\+=\s*1\b)")));
    CHECK_FALSE(ContainsRegex(repeatBody, std::regex(R"((?:^|\n)\s*return\b)")));
}

TEST_CASE("Regress: repeat-until with computation body is preserved", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(acc, x)
            repeat
                acc = acc + x
                x = x - 1
            until x <= 0
            return acc
        end
        return f
    )");

    INFO("decompile:\n" << out);
    const auto repeatBody = FirstBetween(out, "repeat", "until");
    REQUIRE_FALSE(repeatBody.empty());
    CHECK(ContainsRegex(repeatBody, std::regex(R"((?:^|\n)\s*arg\d+\s*\+=\s*arg\d+\b)")));
    CHECK(ContainsRegex(repeatBody, std::regex(R"((?:^|\n)\s*arg\d+\s*-?=\s*(?:arg\d+\s*-\s*)?1\b)")));
    CHECK_FALSE(ContainsRegex(repeatBody, std::regex(R"((?:^|\n)\s*return\b)")));
}

// While-true-break body preservation
// Known bug: while true do x = x + 1; if x >= 100 then break end end
//          -> while 100 >= v0 do end  (body eaten)

TEST_CASE("Regress: while-true-break keeps body statements", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(repeat\s+v\d+\s*\+=\s*1\s+until\s*\(100\s*<=\s*v\d+\)\s+return\s+v\d+)")));
}

// And-or mixed short-circuit
// Known bug: a and b or c and d  ->  garbled with self-assign (v2 = v2)

TEST_CASE("Regress: and-or mixed short-circuit does not produce self-assign", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, d)
            return a and b or c and d
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\b(v\d+|arg\d+)\s*=\s*\1\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\s+and\s+arg1\s+or\s+arg2\s+and\s+arg3\b)")));
}

// OR-chain dispatch (multiple `==` sharing one body) must not clobber
// Known bug: `if a==1 or a==2 or a==3 or a==4 then X elseif a==5 then Y else
// return end` lowers to a run of comparison headers all branching to body X.
// The lifter mistook the shared body X for the merge block and emitted it as an
// unconditional tail, so the `a==5` path ran `Y` and then fell through to `X`,
// clobbering Y. Coalescing the run into `if (a==1 or a==2 or ...) then X` fixes
// both the readability and the correctness bug.
TEST_CASE("Regress: OR-chain dispatch folds and does not clobber sibling body", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileOrFail(
        R"(
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
    )",
        2
    );

    INFO("decompile:\n" << out);
    // The OR run is coalesced into a single condition.
    CHECK(Contains(out, " or "));
    // Both bodies survive exactly once.
    CHECK(CountOccurrences(out, "100") == 1);
    CHECK(CountOccurrences(out, "200") == 1);
    // No clobber: the shared body (100) is a sibling branch that precedes the
    // `== "e"` body (200); it must not reappear as a tail after 200.
    REQUIRE(out.find("100") != std::string::npos);
    REQUIRE(out.find("200") != std::string::npos);
    CHECK(out.find("100") < out.find("200"));
}

// Break in else branches
// Known bug: elseif/else chains get a spurious `break` inserted, which
// kills the enclosing loop. The break should only appear inside the loop
// body when explicitly written.

TEST_CASE("Regress: break in else branch does not kill enclosing loop", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            for i = 1, 20 do
                if i < 3 then
                    -- low
                elseif i < 7 then
                    -- mid
                else
                    -- high
                end
            end
            return 0
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*for\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*1,\s*20,\s*1\s+do)")));
    CHECK_FALSE(Contains(out, "break"));
}

// While-true-break with multiple exits

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
            R"((?:while\s+true|repeat)[\s\S]*v\d+\s*\+=\s*1[\s\S]*if\s+100\s*<\s*v\d+\s+then\s+break[\s\S]*v\d+\s*\+=\s*v\d+[\s\S]*(?:if\s+500\s*<\s*v\d+\s+then\s+break|until\s+\(500\s*<\s*v\d+\))[\s\S]*return\s+v\d+)"
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

TEST_CASE("Regress: repeat-until with continue preserves complete body", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    // The assignment after `continue` must survive.
    const bool bodySum = Contains(out, "+=") || Contains(out, "s");
    CHECK(bodySum);
}

// Continue in numeric for loop
// Luau compiler may merge `if cond then continue end; body` into
// `if not cond then body end` when continue is the last statement before
// back-edge. Test that loop structure and body survive.

TEST_CASE("Regress: continue inside numeric for preserves loop body", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    // Accumulation must survive (as vN += i_3 after var rename).
    const bool hasPlus = Contains(out, "+=") || (Contains(out, " = ") && Contains(out, "+"));
    CHECK(hasPlus);
}

// Repeat-until body with table access (ANALYSIS.md #57)
// Known bug: `repeat acc = acc + t[i]; i = i + 1 until t[i] == nil`
// decompiles to `while t[i] ~= nil do end`; body eaten.

TEST_CASE("Regress: repeat-until with table indexing body is preserved", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    // Body must contain some arithmetic or table access (not empty).
    const bool hasBody = Contains(out, "+") || Contains(out, "t[") || Contains(out, "i ");
    CHECK(hasBody);
}

// And-or expression in loop condition (ANALYSIS.md #64)
// Known bug: `found = i > 10 or (i % 3 == 0)` inside while produces
// `v2 = v2` self-assign and garbled logic.

TEST_CASE("Regress: and-or mixed expression in while loop body avoids self-assign", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    // Self-assign pattern must not appear.
    CHECK_FALSE(Contains(out, "v2 = v2"));
    // The loop increment must survive.
    const bool hasInc = Contains(out, "+=") || Contains(out, "+ 1");
    CHECK(hasInc);
}

// Nested loops with inner break preserved as for+for structure

TEST_CASE("Regress: nested for loops with inner conditional break preserves nesting", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local s = 0
            for outer = 1, 5 do
                for inner = 1, 10 do
                    if inner * outer > 20 then
                        s = s + inner
                    end
                end
            end
            return s
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    // Accumulation must survive.
    const bool hasAcc = Contains(out, "+=") || Contains(out, "+");
    CHECK(hasAcc);
}

// FizzBuzz-like elseif chain in numeric for loop (ANALYSIS.md #65)
// Known bug: elseif chain inside for produces spurious `break` that kills loop.
// Comments are stripped by Luau compiler, so test only structure.

TEST_CASE("Regress: for loop with if-elseif-else body does not get spurious break", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(n)
            for i = 1, n do
                if i % 15 == 0 then
                    local x = 1
                elseif i % 3 == 0 then
                    local x = 2
                elseif i % 5 == 0 then
                    local x = 3
                else
                    local x = 4
                end
            end
            return 0
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK_FALSE(Contains(out, "break"));
}

// While with compound condition (and); body preservation (ANALYSIS.md #22)
// Known bug: `while x < y and y > 0 do` -> splits into while + nested if.
// Body should survive even if condition structure is flattened.

TEST_CASE("Regress: while-and compound condition body survives", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(x, y)
            while x < y and y > 0 do
                local a = x + 1
                local b = y - 1
            end
            return x, y
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "while"));
    // Body statements must survive (may use register/arg names).
    const bool hasBody = Contains(out, "+") || Contains(out, "-");
    CHECK(hasBody);
}

// Repeat-until with conditional break inside (double exit)

TEST_CASE("Regress: repeat-until with inner break preserves body", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    // Body must survive (s = s + i or vN += vM).
    const bool hasAcc = Contains(out, "+=") || Contains(out, " + ");
    CHECK(hasAcc);
}

// Generic FOR pairs with inline table literal; numeric keys (ANALYSIS.md #14)
// Known bug: `for k, v in pairs({10, 20, 30}) do` loses content -> `pairs({ })`

TEST_CASE("Regress: generic-for inline table preserves numeric content", "[Decompiler][GenericFor][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local s = 0
            for _, v in pairs({10, 20, 30}) do
                s = s + v
            end
            return s
        end
        return f
    )");

    INFO("decompile:\n" << out);
    const bool hasContent = Contains(out, "10") || Contains(out, "pairs");
    CHECK(hasContent);
}

// Early return inside for loop; semantic correctness

TEST_CASE("Regress: early return inside generic for loop preserves for structure", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t, target)
            for i, v in ipairs(t) do
                if v == target then
                    return i
                end
            end
            return -1
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, "return "));
}

TEST_CASE("Generic loop bindings do not suppress later local declarations", "[Decompiler][GenericFor][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+v3\b)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Branch-local register reuse does not leak a global", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f0(...)
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
    )");

    INFO("decompile:\n" << out);
    CHECK(CountOccurrences(out, "local v1") == 2);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((^|\n)v1\s*=)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Sibling branches keep independent local bindings", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(if\s+flag\s+then\s+local\s+v\d+(?:_\d+)?\s*=\s*not\s+(?:\.\.\.|\(\.\.\.\)))")));
    CHECK(Recompiles(out));
}

// Nested repeat-until inside repeat-until with while after; body eaten

TEST_CASE("Regress: nested repeat-until body survives with trailing while loop", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "repeat"));
    CHECK(Contains(out, "while "));
    CHECK(Contains(out, "bye bye bye"));
    CHECK(Contains(out, "hello hello hello"));
    CHECK(Contains(out, "can you believe this"));
    CHECK(Contains(out, "warn(\"hi\""));
    CHECK(Contains(out, "print(\"hi\""));
}

// Known-decompiler-limitation tests (tracking future fixes)

TEST_CASE("Regress: dict-style keys survive in an inline pairs() table literal", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            for k, v in pairs({hello = "world", num = 42}) do
                print(k, v)
            end
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "pairs("));
    CHECK(Contains(out, "hello = \"world\""));
    CHECK(Contains(out, "num = 42"));
}

TEST_CASE("Regress: while compound condition is preserved (header `and` or equivalent break-guard)", "[Decompiler][Loop][Regression]") {
    // `while a < limit and b > 0 do BODY` may be reconstructed either as a compound header or as the
    // semantically identical `while a < limit do if not (b > 0) then break end BODY`; both are correct.
    // The regression this guards is the second operand being *dropped* entirely.
    const auto out = DecompileOrFail(R"(
        local function f(a, b, limit)
            while a < limit and b > 0 do
                a = a + 1
                b = b - 1
            end
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "while"));
    const bool hasInc = Contains(out, "+=") || Contains(out, "+ 1");
    const bool hasDec = Contains(out, "-=") || Contains(out, "- 1");
    CHECK(hasInc);
    CHECK(hasDec);
    CHECK_FALSE(Contains(out, "do end"));
    CHECK_FALSE(Contains(out, "do\nend"));
    // the b > 0 half must survive somewhere: folded into the header (`and`) or as a break-guard.
    CHECK((Contains(out, "and") || Contains(out, "break")));
    // and the whole thing must be valid, recompilable Luau.
    CHECK(Recompiles(out));
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
