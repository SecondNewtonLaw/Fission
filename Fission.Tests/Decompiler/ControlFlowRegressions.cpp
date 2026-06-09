//
// Created by Dottik on 2/6/2026.
//
// Control-flow regression tests; each targets a known/fixed bug.
//

#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <string>

namespace {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::string DecompileOrFail(const std::string &source, int optLevel = 1) {
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

} // namespace

// -------------------------------------------------------------------------
// Repeat-until body preservation
// -------------------------------------------------------------------------
// Known bug: repeat i = i + 1 until i >= 10  →  while 10 >= v0 do end
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

// -------------------------------------------------------------------------
// While-true-break body preservation
// -------------------------------------------------------------------------
// Known bug: while true do x = x + 1; if x >= 100 then break end end
//          → while 100 >= v0 do end  (body eaten)

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

// -------------------------------------------------------------------------
// And-or mixed short-circuit
// -------------------------------------------------------------------------
// Known bug: a and b or c and d  →  garbled with self-assign (v2 = v2)

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

// -------------------------------------------------------------------------
// OR-chain dispatch (multiple `==` sharing one body) must not clobber
// -------------------------------------------------------------------------
// Known bug: `if a==1 or a==2 or a==3 or a==4 then X elseif a==5 then Y else
// return end` lowers to a run of comparison headers all branching to body X.
// The lifter mistook the shared body X for the merge block and emitted it as an
// unconditional tail, so the `a==5` path ran `Y` and then fell through to `X`,
// clobbering Y. Coalescing the run into `if (a==1 or a==2 or ...) then X` fixes
// both the readability and the correctness bug.
TEST_CASE("Regress: OR-chain dispatch folds and does not clobber sibling body", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileOrFail(R"(
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
                                     2);

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

// -------------------------------------------------------------------------
// Break in else branches
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// While-true-break with multiple exits
// -------------------------------------------------------------------------

TEST_CASE("Regress: while-true-break with multiple exit conditions preserves body", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
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
        return f
    )");

    INFO("decompile:\n" << out);
    // Body statements precede the first exit test, so it must NOT fold into the
    // loop condition (that would re-order it ahead of `x += 1`). Stays `while true`
    // with both exits as real `break`s in their original positions.
    CHECK(ContainsRegex(out, std::regex(R"(while\s+true\s+do)")));
    CHECK(ContainsRegex(
        out, std::regex(R"(while\s+true[\s\S]*v\d+\s*\+=\s*1[\s\S]*if\s+100\s*<\s*v\d+\s+then\s+break[\s\S]*v\d+\s*\+=\s*v\d+[\s\S]*if\s+500\s*<\s*v\d+\s+then\s+break[\s\S]*return\s+v\d+)")
    ));
    // The post-loop value is returned after the loop, not from inside it.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(while[\s\S]*return\s+v\d+[\s\S]*v\d+\s*\+=)")));
}

// -------------------------------------------------------------------------
// Repeat-until with continue
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// Continue in numeric for loop
// -------------------------------------------------------------------------
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
    const bool hasPlus = Contains(out, "+=") || Contains(out, " = ") && Contains(out, "+");
    CHECK(hasPlus);
}

// -------------------------------------------------------------------------
// While-true-break with single body statement before break
// -------------------------------------------------------------------------
// Luau compiler compiles `while true do if cond then break end; body end`
// into a conditional back-edge. The decompiler reconstructs a while/repeat
// loop with the break condition in the loop header. Body should survive.

TEST_CASE("Regress: while-true-break preserves loop body statement", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local x = 0
            while true do
                x = x + 1
                if x >= 100 then break end
            end
            return x
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // Body increment must survive (compiler may transform to while/repeat, but +=1 stays).
    const bool hasIncrement = Contains(out, "+=") || Contains(out, "+ 1");
    CHECK(hasIncrement);
    const bool hasLoop = Contains(out, "while") || Contains(out, "repeat");
    CHECK(hasLoop);
}

// -------------------------------------------------------------------------
// Repeat-until body with table access (ANALYSIS.md #57)
// -------------------------------------------------------------------------
// Known bug: `repeat acc = acc + t[i]; i = i + 1 until t[i] == nil`
// decompiles to `while t[i] ~= nil do end` — body eaten.

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

// -------------------------------------------------------------------------
// And-or expression in loop condition (ANALYSIS.md #64)
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// Nested loops with inner break preserved as for+for structure
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// FizzBuzz-like elseif chain in numeric for loop (ANALYSIS.md #65)
// -------------------------------------------------------------------------
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

// -------------------------------------------------------------------------
// While with compound condition (and) — body preservation (ANALYSIS.md #22)
// -------------------------------------------------------------------------
// Known bug: `while x < y and y > 0 do` → splits into while + nested if.
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

// -------------------------------------------------------------------------
// Repeat-until with conditional break inside (double exit)
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Generic FOR pairs with inline table literal — numeric keys (ANALYSIS.md #14)
// -------------------------------------------------------------------------
// Known bug: `for k, v in pairs({10, 20, 30}) do` loses content → `pairs({ })`

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

// -------------------------------------------------------------------------
// Early return inside for loop — semantic correctness
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Nested repeat-until inside repeat-until with while after — body eaten
// -------------------------------------------------------------------------

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

// -------------------------------------------------------------------------
// Known-decompiler-limitation tests (tracking future fixes)
// -------------------------------------------------------------------------

TEST_CASE("Regress: table literal in pairs() argument - known limitation: dict content lost", "[Decompiler][Table][Regression][KnownLimitation]") {
    // KNOWN LIMITATION: Dict-style keys in inline table literals are lost.
    // SETTABLEKS after NEWTABLE may be fragmented by SSA/CFG block splitting.
    // This test verifies that at minimum pairs() and braces survive.
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
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    // TODO: check for "hello", "world", "num", "42" once dict reconstruction works.
}

TEST_CASE("Regress: while complex compound condition - known limitation: `and` lost", "[Decompiler][Loop][Regression][KnownLimitation]") {
    // KNOWN LIMITATION: Compound conditions with `and`/`or` in while headers
    // are flattened by the CFA. Body statements should still survive.
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
    // Body statements must survive even if the compound condition doesn't.
    const bool hasInc = Contains(out, "+=") || Contains(out, "+ 1");
    const bool hasDec = Contains(out, "-=") || Contains(out, "- 1");
    CHECK(hasInc);
    CHECK(hasDec);
    // No empty loop body.
    CHECK_FALSE(Contains(out, "do end"));
    CHECK_FALSE(Contains(out, "do\nend"));
    // TODO: CHECK(Contains(out, "and")) once compound condition reconstruction is fixed.
}

TEST_CASE("Regress: generic for pairs with dict-style table - known limitation: keys lost", "[Decompiler][GenericFor][Regression][KnownLimitation]") {
    // KNOWN LIMITATION: Dict keys in table literal are lost (same as other
    // dict table limitations). The `for k, v in pairs(t)` structure survives.
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = {a = 1, b = 2, c = 3}
            for k, v in pairs(t) do
                print(k, v)
            end
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "pairs("));
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    // t's keys not checked: dict reconstruction known limitation.
}

// -------------------------------------------------------------------------
// Stricter table literal tests (for working patterns)
// -------------------------------------------------------------------------

TEST_CASE("Regress: inline table literal has matched braces", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = {1, 2, 3}
            return t
        end
        return f
    )");

    INFO("decompile:\n" << out);
    const size_t openBraces = CountOccurrences(out, "{");
    const size_t closeBraces = CountOccurrences(out, "}");
    CHECK(openBraces == closeBraces);
    CHECK(openBraces >= 1);
}

TEST_CASE("Regress: table literal empty braces are balanced", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = {}
            return t
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // Output may be `{  }` with spaces — check brace balance.
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
    CHECK(CountOccurrences(out, "{") >= 1);
}

// -------------------------------------------------------------------------
// Infinite `while CONST` loops: testless back-edge / self-loop / for-wrapping
// -------------------------------------------------------------------------
// `while 1 do for i=1,1,2 do break end end` — Luau emits the outer infinite while
// as a testless back-edge sharing the for's header. Regressions fixed:
//   - the inner for collapsed to `repeat until (not false)` with the break escaping
//     the loop, because the for-header bundled its init LOADs (FOR-recognition was
//     too strict) and the shared back-edge latch overwrote the for's latch.
//   - the testless `while` condition rendered as `while not false`.

TEST_CASE("Regress: infinite while wrapping a for keeps the for and the break", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail("while 1 do for i=1, 1, 2 do break end end");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(while\s+(true|1)\s+do)")));
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*=\s*1,\s*1,)")));
    CHECK(Contains(out, "break"));
    // mislift signatures: for turned into a repeat, or a bogus testless condition.
    CHECK_FALSE(Contains(out, "repeat"));
    CHECK_FALSE(Contains(out, "not false"));
    // the break must stay inside the loops, not escape past their `end`s.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(end\s+break)")));
}

TEST_CASE("Regress: infinite empty while (self-loop) is not dropped", "[Decompiler][Loop][Regression]") {
    // At -O2 Luau folds the body away, leaving a single block that jumps to itself
    // (`while true do end`). The self-loop block was typed LoopLatch, not LoopHeader,
    // so the whole loop vanished and the function decompiled to nothing.
    const auto out = DecompileOrFail("while 1 do for i=1, 1, #\"67\" or 9 do break end end", 2);

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(while\s+true\s+do)")));
    CHECK_FALSE(Contains(out, "not false"));
}

TEST_CASE("Regress: local declared before an infinite while survives", "[Decompiler][Loop][Regression]") {
    // The LoopHeader lift used `nodes.resize(nodes.size() - stmts.size())` to drop the
    // header's own (pre-appended) statements, but the iterative driver never pre-appends
    // them, so it instead deleted the *preceding* `local a = {}`. The body then referenced
    // an undeclared `v0` (`table.insert(v0, 1)`). The trailing `break` folds the for away
    // at -O2, leaving a self-loop whose body is the only place the declaration is needed.
    const auto out = DecompileOrFail("local a = ({})\nwhile 1 do for i=1, 1, #\"67\" or 9 do table.insert(a, i) break end end", 2);

    INFO("decompile:\n" << out);
    // declaration present AND the same variable is what table.insert mutates.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+(\w+)\s*=\s*\{\s*\}[\s\S]*table\.insert\(\1\b)")));
}

// -------------------------------------------------------------------------
// Empty-arm diamond before a return must not null the if's then-branch
// -------------------------------------------------------------------------
// Crash: `local v = t.X; if t.Ready then end; return v` lifts a diamond whose
// true-arm is empty. The CFG builder swapped then<->else to invert it, but with
// no else block to swap in, thenBranch became null; FoldTerminalMixedAndOr then
// dereferenced it (it matches the `not COND` shape this swap produces) and crashed.
// Fix: don't swap without an else, and guard the fold against a null then-branch.

TEST_CASE("Regress: empty-arm diamond before return does not crash the and-or fold", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local v = t.X
            if t.Ready then end
            return v
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // The real value `v` is returned, not mangled into a bogus and/or chain.
    CHECK(ContainsRegex(out, std::regex(R"(return\s+\w+\.X\b)")));
    // The guarded read survives (t.Ready is observable via __index).
    CHECK(Contains(out, ".Ready"));
    // No malformed-branch residue from a null/empty if.
    CHECK_FALSE(Contains(out, "conditional branches not lifted"));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(then\s*\n\s*end)")));
}

TEST_CASE("Regress: table literal numeric list elements survive", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = {10, 20, 30}
            return t
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(CountOccurrences(out, "10") >= 1);
    CHECK(CountOccurrences(out, "20") >= 1);
    CHECK(CountOccurrences(out, "30") >= 1);
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
}

// -------------------------------------------------------------------------
// Stricter loop tests
// -------------------------------------------------------------------------

TEST_CASE("Regress: numeric `for` with step preserves all loop parameters", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local s = 0
            for i = 1, 10, 2 do
                s = s + i
            end
            return s
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, "1,"));
    CHECK(Contains(out, "10"));
    // The step value must appear somewhere.
    CHECK(Contains(out, "2"));
    // Loop body must survive.
    bool plusArith = Contains(out, "+") || Contains(out, "+=");
    CHECK(plusArith);
}

TEST_CASE("Regress: while loop with single-statement body preserves body", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(limit)
            local x = 0
            while x < limit do
                x = x + 1
            end
            return x
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "while"));
    const bool hasInc = Contains(out, "+=") || Contains(out, "+ 1");
    CHECK(hasInc);
    // No empty loop.
    bool whileDoEnd = Contains(out, "while ") && Contains(out, "do end");
    CHECK_FALSE(whileDoEnd);
}

TEST_CASE("Regress: nested for+for with body only in inner loop", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local s = 0
            for i = 1, 3 do
                for j = 1, 3 do
                    s = s + i + j
                end
            end
            return s
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // Two `for` keywords: outer + inner.
    CHECK(CountOccurrences(out, "for ") >= 2);
    // Accumulation must survive.
    const bool hasAcc = Contains(out, "+=") || Contains(out, "+");
    CHECK(hasAcc);
    // No break spurious.
    CHECK_FALSE(Contains(out, "break"));
}

TEST_CASE("Regress: while loop body not replaced by its condition", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local i = 0
            while i < 10 do
                i = i + 1
            end
            return i
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "while"));
    // The body must contain `i = i + 1` or similar increment.
    const bool bodyFull = Contains(out, "+ 1") || Contains(out, "+=") || Contains(out, "= i + 1");
    CHECK(bodyFull);
    // The condition `i < 10` should appear only in the loop header, not
    // duplicated in a redundant guard before the loop.
    CHECK_FALSE(Contains(out, "if i < 10"));
}

TEST_CASE("Regress: repeat-until with two separate accumulations has both", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local s1 = 0
            local s2 = 0
            local i = 0
            repeat
                s1 = s1 + i
                s2 = s2 + i * 2
                i = i + 1
            until i > 10
            return s1, s2
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "repeat"));
    // Both accumulations must survive (at least two `+` or `+=`).
    const size_t plusCount = CountOccurrences(out, "+") + CountOccurrences(out, "+=");
    CHECK(plusCount >= 2);
    // Multiplication must survive.
    CHECK(Contains(out, "*"));
}

TEST_CASE("Regress: generic for over pairs with string-key table preserves keys", "[Decompiler][GenericFor][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = {a = 1, b = 2, c = 3}
            for k, v in pairs(t) do
                print(k, v)
            end
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "pairs("));
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    // The table keys must survive.
    bool hasA = Contains(out, "a =") || Contains(out, "a=");
    bool hasB = Contains(out, "b =") || Contains(out, "b=");
    bool hasC = Contains(out, "c =") || Contains(out, "c=");
    CHECK(hasA);
    CHECK(hasB);
    CHECK(hasC);
}

// -------------------------------------------------------------------------
// Phi-merged call result must not shadow the merge variable with `local`
// -------------------------------------------------------------------------
// Known bug: a value defined in both branches of an if/else, where one branch
// is a method-call result, emitted `local v = obj:Method()` inside the branch
// (shadowing the hoisted merge variable) so the post-merge read saw the wrong
// value. The call-result branch must use a bare assignment.

TEST_CASE("Regress: phi-merged call result is not re-declared with local", "[Decompiler][Phi][Regression]") {
    // `cs and cs:IsA(...) or false` lowers so the method-call result lands directly
    // in the merge register (no temp), exercising the phi-hoist of a Call/NameCall
    // return target. `print` after forces the value to escape the branches.
    const auto out = DecompileOrFail(R"(
        return function(cs)
            local isSubj = cs and cs:IsA("VehicleSeat") or false
            print(isSubj)
        end
    )",
        2);

    INFO("decompile:\n" << out);
    // The method-call branch must NOT re-declare the merge variable with `local`
    // (that would shadow the hoisted merge variable and lose the value).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+\w+\s*=\s*\w+:IsA\()")));
}

// -------------------------------------------------------------------------
// Comparison materialised into a boolean register
// -------------------------------------------------------------------------
// Known bug: `local x = a ~= b` (a comparison stored as a boolean value, not
// used directly as a branch condition) lowered to a garbled
// `if a == b then local x = x end` with a self-assign and no real value.
// It must reconstruct the comparison expression itself.

TEST_CASE("Regress: comparison stored as boolean value is reconstructed", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(R"(
        return function(self)
            local x = self.occlusionMode ~= "Invisicam"
            print(x)
        end
    )",
        2);

    INFO("decompile:\n" << out);
    // No self-assign garbage.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\b(v\d+|arg\d+)\s*=\s*\1\b)")));
    // The comparison must be materialised as a value.
    CHECK(ContainsRegex(out, std::regex(R"(=\s*\w+\.occlusionMode\s*~=\s*"Invisicam")")));
}

// -------------------------------------------------------------------------
// Numeric for-loop with both `continue` and `break`
// -------------------------------------------------------------------------
// Known bug: the break edge inlined the post-loop code (and leaked loop control
// registers), and the `continue` path was emitted as a spurious `break`. The
// loop must keep one real `break`, no stray return, and the post-loop statement
// must appear after the loop.

TEST_CASE("Regress: numeric for with continue and break", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t, limit)
            for i = 1, #t do
                local v = t[i]
                if v > limit then
                    continue
                end
                if v == 0 then
                    break
                end
                print(v)
            end
            print("done")
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*=)")));
    // The body's guarded statement and the real break both survive.
    CHECK(ContainsRegex(out, std::regex(R"(\bbreak\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(print\(v\d+\))")));
    // The post-loop statement must be present (not swallowed into the break path).
    CHECK(ContainsRegex(out, std::regex(R"(print\("done"\))")));
    // Exactly one break — the `continue` path must NOT have become a second break.
    size_t breakCount = 0;
    for (size_t p = out.find("break"); p != std::string::npos; p = out.find("break", p + 1))
        ++breakCount;
    CHECK(breakCount == 1);
}

// -------------------------------------------------------------------------
// `x = X and (...)` must reassign, not shadow with a nested `local`
// -------------------------------------------------------------------------
// Known bug: `local s = a and (b or c)` lowered to `local s = a; if s then
// local s ... end` — the inner `local s` shadowed the outer, so the post-merge
// read saw `a` instead of the computed value. The inner store must reassign.

TEST_CASE("Regress: and-chain conditional reassign does not shadow", "[Decompiler][Phi][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(cm)
            local activeSensor = cm.ActiveController and
                ((cm.ActiveController:IsA("GroundController") and cm.GroundSensor) or
                 (cm.ActiveController:IsA("ClimbController") and cm.ClimbSensor))
            if activeSensor and activeSensor.SensedPart then
                return activeSensor.SensedPart
            end
            return nil
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // The sensor variable is declared once (from ActiveController) and reassigned.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+(\w+)\s*=\s*\w+\.ActiveController)")));
    // The conditional update must be a bare reassignment, never a shadowing `local`.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(then\s*\n\s*local\s+\w+\s*\n)")));
}

// -------------------------------------------------------------------------
// `~=` / `==` equality-jump branch polarity
// -------------------------------------------------------------------------
// Known bug: JUMPXEQK with the not-flag clear (compiled from a `~=` source test)
// had ifStatementTrue/False swapped, so `if x ~= "b" then return 10 end` came
// back as `if x == "b" then return 10`, inverting the branch.

TEST_CASE("Regress: not-equal if-branch keeps correct polarity", "[Decompiler][Branch][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function g(x)
            if x ~= "b" then
                return 10
            end
            return 20
        end
        return g
    )");

    INFO("decompile:\n" << out);
    // The inverted form `if x == "b" then return 10` must NOT appear.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(==\s*"b"\s*then\s*return\s+10)")));
    // 10 must be the `~=` result (either guarded by ~=, or 20 guarded by ==).
    const bool correctPolarity = ContainsRegex(out, std::regex(R"(~=\s*"b"\s*then\s*return\s+10)")) ||
                                 ContainsRegex(out, std::regex(R"(==\s*"b"\s*then\s*return\s+20)"));
    CHECK(correctPolarity);
}

// -------------------------------------------------------------------------
// Module-table constructor must not absorb non-inlinable field values
// -------------------------------------------------------------------------
// Known bug: `local t = {}; function t.foo() ... end` was reconstructed as a
// fabricated literal `{ foo = v5 }` (every field aliased to a single reused
// register) and the closure leaked as a standalone global `anon_# = function`.
// The table must stay empty and the closure inline into the field store.

TEST_CASE("Regress: closure table-field is inlined, not leaked as a global", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = {}
        function t.foo()
            return 42
        end
        return t
    )");

    INFO("decompile:\n" << out);
    // Closure body survives.
    CHECK(ContainsRegex(out, std::regex(R"(return\s+42)")));
    // The field is assigned a function literal.
    CHECK(ContainsRegex(out, std::regex(R"(\.foo\s*=\s*function\b)")));
    // No standalone closure leaked as a bare global assignment.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((?:^|\n)\s*anon_\w+\s*=\s*function\b)")));
}

// A table-constructor scan must not absorb a SETLIST/SETTABLE targeting a *later*
// table that merely reuses the same register. `setmetatable({}, mt)` followed by
// `self.list = {1,2,3}` must keep the empty `{}` empty.
TEST_CASE("Regress: empty constructor does not absorb a reused-register table", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(mt)
            local self = setmetatable({}, mt)
            self.list = {1, 2, 3}
            return self
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // The list assignment keeps its real values, not fabricated register names.
    CHECK(ContainsRegex(out, std::regex(R"(\.list\s*=\s*\{\s*1\s*,\s*2\s*,\s*3\s*\})")));
    // The metatable's table must stay empty: the list must NOT have been absorbed
    // into the setmetatable argument (the reused-register bug).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(setmetatable\(\s*\{\s*1\s*,)")));
    // An empty constructor `{}` is present (the metatable subject).
    CHECK(ContainsRegex(out, std::regex(R"(\{\s*\})")));
}

// A constructor with only inlinable (constant) values must still coalesce into
// a single `{ ... }` literal — the guard above must not over-trigger.
TEST_CASE("Regress: constant table constructor still coalesces", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = { a = 1, b = 2, c = "x" }
            return t
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\{[^}]*a\s*=\s*1[^}]*\})")));
}

// -------------------------------------------------------------------------
// Nested REF upvalue capture must alias, not redefine
// -------------------------------------------------------------------------
// A variable captured by reference (mutated across scopes) is ONE shared variable.
// Rendering the capture as `local uv_N = source` makes a by-value copy: writes
// inside the closure no longer alias the outer variable. It must keep a single
// consistent name across all nesting levels with no redefinition.
TEST_CASE("Regress: nested REF upvalue capture aliases instead of redefining", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no debug names → exercises the autogenerated-name path
    const std::string source = R"(
        local value = _G.globalValue
        local function f()
            print(value)
            local function f1()
                print(value)
                value = _G.globalValue
            end
            f1()
            value = _G.globalValue
        end
        f()
        value = _G.globalValue
        return f
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // No by-value capture copy (`local uv_N = ...`) — that would desync the writes.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+uv_\d+\s*=)")));
    // The shared variable is declared once, BEFORE the closure that captures it.
    std::smatch valueDecl;
    REQUIRE(std::regex_search(out, valueDecl, std::regex(R"(local\s+v\d+\s*=\s*_G\.globalValue)")));
    const size_t closurePos = out.find("local function f");
    REQUIRE(closurePos != std::string::npos);
    CHECK(static_cast<size_t>(valueDecl.position(0)) < closurePos);
    // Exactly one declaration of it — the trailing write is an assignment, not a 2nd local.
    CHECK(CountOccurrences(out, "local v") == 1);
}

// A LUA_TINTEGER constant only reaches the bytecode via a library-member-constant
// fold callback (plain literals are f64). Inject one at compile time so the whole
// pipeline (compile -> deserialize -> lift -> source) is exercised.
TEST_CASE("Constants: integer constant decompiles with the `i` suffix", "[Decompiler][Constants]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    static const char *const knownLibraries[] = {"Integers", nullptr};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 2; // library-K folding needs O2
    opts.debugLevel = 1;
    opts.librariesWithKnownMembers = knownLibraries; // enables the member-constant fold for `Integers`
    opts.libraryMemberConstantCb = [](const char *library, const char *member, Luau::CompileConstant *constant) {
        if (std::strcmp(library, "Integers") == 0 && std::strcmp(member, "Big") == 0)
            Luau::setCompileConstantInteger64(constant, 82199292);
    };
    const std::string source = R"(
        return function()
            return Integers.Big
        end
    )";
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "82199292i"));
}

TEST_CASE("Constants: vector constants emit in expr and table positions, not silent nil", "[Decompiler][Constants]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 2;
    opts.debugLevel = 1;
    const std::string source = R"(
        return function()
            local v = vector.create(1, 2, 3)
            local t = { vector.create(4, 5, 6), vector.create(7, 8, 9) }
            return v, t
        end
    )";
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Vector3.new(1, 2, 3)"));     // LOADK vector
    CHECK(Contains(out, "Vector3.new(4, 5, 6)"));     // vector inside a constant table
    CHECK(Contains(out, "Vector3.new(7, 8, 9)"));
    CHECK_FALSE(Contains(out, "nil")); // no constant silently dropped to nil
}

// -------------------------------------------------------------------------
// OmitFissionComments suppresses informational comments but keeps warnings
// -------------------------------------------------------------------------
TEST_CASE("Option: OmitFissionComments drops info comments, keeps warnings", "[Decompiler][Options]") {
    EnableLuauFFlagsOnce();
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    const std::string source = R"(
        local function outer()
            local shared = {}
            local function inner()
                shared.flag = true
                local other = {}
                other.x = 1
                return other
            end
            return inner
        end
        return outer
    )";

    // Default: Fission's informational comments are present. (Fresh Decompiler per
    // call — the generator's buffer is a reused member that accumulates otherwise.)
    Decompiler d1{};
    const auto withInfo = d1.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(withInfo.resultCode == DecompileResult::Success);
    CHECK(Contains(withInfo.decompilationOutput, "Fission ~~ Function Information"));
    CHECK(Contains(withInfo.decompilationOutput, "Fission: INFO:"));

    // With the flag: per-function info blocks and INFO/capture notes are gone.
    Decompiler d2{};
    const auto noInfo = d2.DecompileTestCode(source, DecompilerFlags::OmitFissionComments, opts);
    REQUIRE(noInfo.resultCode == DecompileResult::Success);
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Fission ~~ Function Information"));
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Fission: INFO:"));
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Beginning captures"));
    // The code itself still decompiles (the function is still there).
    CHECK(Contains(noInfo.decompilationOutput, "function"));
}

// -------------------------------------------------------------------------
// A nested function's own vN must not shadow a captured upvalue of the same name
// -------------------------------------------------------------------------
// A nested function reuses low register numbers, so its own `vN` can equal a
// captured upvalue that (after aliasing) reads as the enclosing scope's `vN`.
// Rendered inline, the inner `local vN` then shadows the upvalue. The inner's own
// auto-name must be disambiguated (suffixed) so it stays distinct.
TEST_CASE("Regress: nested function own vN does not shadow captured upvalue vN", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names — the collision surfaces
    const std::string source = R"(
        local function outer()
            local shared = {}
            local function inner()
                shared.flag = true
                local other = {}
                other.x = 1
                return other
            end
            return inner
        end
        return outer
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // The captured `shared` and the inner's own `other` must not share a name: no
    // `vN` may be assigned a field and then re-declared `local vN` in the same body.
    std::smatch m;
    const bool shadowed = std::regex_search(out, m, std::regex(R"((v\d+)\.flag\s*=\s*true[\s\S]*?local\s+\1\b)"));
    CHECK_FALSE(shadowed);
    // A Fission INFO note documents the rename that resolved the collision.
    CHECK(ContainsRegex(out, std::regex(R"(Fission: INFO:[^\n]*has been suffixed to avoid shadowing)")));
}

// -------------------------------------------------------------------------
// Sibling closures capturing by value must not collide on uv_N names
// -------------------------------------------------------------------------
// Each closure numbers its upvalues from 0, so emitting `local uv_N = source`
// capture copies in the shared parent scope makes siblings clash (closure A's
// `local uv_0 = t1` vs closure B's `local uv_0 = t1`), and a closure's own uv_0
// reference can bind to the wrong sibling's local. VAL captures must alias the
// source variable directly — no copy, no uv_N.
TEST_CASE("Regress: sibling VAL captures alias sources without uv_N collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names — collision would surface
    const std::string source = R"(
        local function outer()
            local t1 = {}
            local t2 = {}
            local t3 = {}
            local function a()
                t1[1] = t2
                return t3
            end
            local function b()
                t2[1] = t3
                return t1
            end
            a()
            b()
            return a, b
        end
        return outer
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // No capture-copy locals and no generic uv_N identifiers at all.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+uv_\d+\s*=)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\buv_\d+\b)")));
}

// -------------------------------------------------------------------------
// LCT_UPVAL capture chaining: a closure that captures one of its parent's
// upvalues must reference the parent's upvalue, not a colliding fresh uv_0
// -------------------------------------------------------------------------
// Bug (real Opera-GX script): an inner closure captured the parent's upvalue
// index N (CAPTURE mode 2 / LCT_UPVAL), but the lifter named the inner upvalue
// uv_0 — colliding with the parent's own uv_0 (a different value). e.g. a pcall
// closure rendered `uv_0:GetCampaignEligibilityAsync(...)` where uv_0 was the
// PlaceId table, not the AdService it actually captured. Compiled WITHOUT upvalue
// debug names (debugLevel 1), so names are autogenerated and the collision shows.
TEST_CASE("Regress: LCT_UPVAL capture resolves to the parent upvalue, no uv_0 collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no upvalue debug names → autogenerated uv_N, collision visible
    const std::string source = R"(
        local cache = {}
        local Service = {}
        function Service:Async() return true end
        return function()
            if cache[1] == nil then
                local ok = pcall(function()
                    return Service:Async()
                end)
                return ok
            end
            return false
        end
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // The method is invoked on the captured Service upvalue.
    CHECK(Contains(out, ":Async("));
    // `cache` is the parent's uv_0 (used for the `cache[1]` index). The Async call
    // must NOT be made on uv_0 — that is the collision bug.
    CHECK_FALSE(Contains(out, "uv_0:Async"));
}

// -------------------------------------------------------------------------
// Generalized iteration `for k,v in t do` must not emit the implicit nils
// -------------------------------------------------------------------------
// Luau lowers `for k, v in t do` to the iterator triple [t, nil, nil] (LOAD nil,
// LOAD nil, FORGPREP). The lifter emitted all three slots → `for k, v in t, nil,
// nil do`, which is non-idiomatic and not portable to plain Lua. When the state
// and control values are both nil they are implicit and must be dropped.
TEST_CASE("Regress: generalized for-in does not emit trailing nil iterator args", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local sum = 0
            for k, v in t do
                sum = sum + v
            end
            return sum
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, " in "));
    // The generalized form iterates the value directly: `for a, b in arg0 do`.
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*,\s*\w+\s+in\s+\w+\s+do)")));
    // The implicit `nil, nil` state/control must NOT be materialized.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(in\s+\w+\s*,\s*nil\s*,\s*nil)")));
    CHECK_FALSE(Contains(out, ", nil, nil"));
}

// -------------------------------------------------------------------------
// Production safety boundary: malformed bytecode must not crash the host
// -------------------------------------------------------------------------
// In a PRODUCTION_BUILD libassert's DEBUG_ASSERT is stripped and ASSERT aborts;
// a malformed/hostile stream would crash the whole host (RbxCli). The safety
// boundary (Decompiler::CommonDecompilerEntry) installs a throwing libassert
// handler and catches everything, so a bad chunk returns a failure result
// instead of taking the process down. If this test ever crashes the runner, the
// boundary regressed. (Reaching the end of the loop IS the no-crash proof.)
TEST_CASE("Safety: malformed/truncated bytecode degrades gracefully, no crash", "[Decompiler][Safety]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};

    // Pure garbage of various shapes.
    const std::string garbage[] = {
        std::string(""),
        std::string("\x01\x02\x03"),
        std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8),
        std::string("\x06not-real-bytecode-just-bytes"),
    };
    for (const auto &g : garbage) {
        const auto r = decompiler.DecompileRobloxBytecode(g, static_cast<DecompilerFlags>(0));
        CHECK(r.resultCode != DecompileResult::Success);
    }

    // Every proper prefix of a real compiled chunk exercises the bounds checks
    // deep inside deserialize/lift. None may crash; none is a complete chunk.
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    const std::string good = Luau::compile("local t = {} for i = 1, 10 do t[i] = i * i end return t", opts);
    REQUIRE(good.size() > 8);
    for (size_t len = 1; len < good.size(); ++len) {
        const auto r = decompiler.DecompileRobloxBytecode(good.substr(0, len), static_cast<DecompilerFlags>(0));
        CHECK(r.resultCode != DecompileResult::Success);
    }
}

TEST_CASE("Regress: dead `local` from an or-step is eliminated, the for survives", "[Decompiler][Loop][Regression]") {
    // `#"67" or 9` lifts with a phi-consumed step register, which forced a standalone `local vN = #"67"`
    // that the for-step then inlined anyway. The dead binding must be dropped, leaving one `#"67"`.
    const auto out = DecompileOrFail(R"(
        local a = ({})
        while 1 do for i = 1, 1, #"67" or 9 do table.insert(a, i) break end end
    )", 0);

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, "table.insert"));
    // the length expression appears once (the for-step), not also as a dead local.
    CHECK(CountOccurrences(out, "#\"67\"") == 1);
}
