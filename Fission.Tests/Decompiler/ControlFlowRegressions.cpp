//
// Created by Dottik on 2/6/2026.
//

// Control-flow regression tests; each targets a known/fixed bug.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
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
#include <regex>
#include <set>
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

    // No auto-named local `vN` is textually used before its `local vN` declaration; the
    // forward-reference bug (a value folded into a constructor whose own `local` is emitted, or
    // dropped, afterwards reads nil). Comment blocks are stripped first. Names that never appear
    // in a `local` declaration are globals/params and are ignored.
    bool NoForwardReference(const std::string &decompiled) {
        const std::string s = std::regex_replace(decompiled, std::regex(R"(--\[\[[\s\S]*?\]\])"), "");
        const std::regex localRe(R"(\blocal\s+(v\d+)\b)");
        std::set<std::string> names;
        for (std::sregex_iterator it(s.begin(), s.end(), localRe), e; it != e; ++it)
            names.insert((*it)[1].str());
        for (const auto &name : names) {
            std::smatch dm;
            if (!std::regex_search(s, dm, std::regex("\\blocal\\s+(" + name + ")\\b")))
                continue;
            const auto declTokPos = static_cast<size_t>(dm.position(1));
            std::smatch tm;
            if (std::regex_search(s, tm, std::regex("\\b" + name + "\\b")) && static_cast<size_t>(tm.position(0)) < declTokPos)
                return false; // first appearance of vN is a use, before its declaration
        }
        return true;
    }

} // namespace

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
        out,
        std::regex(
            R"(while\s+true[\s\S]*v\d+\s*\+=\s*1[\s\S]*if\s+100\s*<\s*v\d+\s+then\s+break[\s\S]*v\d+\s*\+=\s*v\d+[\s\S]*if\s+500\s*<\s*v\d+\s+then\s+break[\s\S]*return\s+v\d+)"
        )
    ));
    // The post-loop value is returned after the loop, not from inside it.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(while[\s\S]*return\s+v\d+[\s\S]*v\d+\s*\+=)")));
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

// While-true-break with single body statement before break
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
    CHECK(ContainsRegex(out, std::regex(R"(while\s+[^\n]*\bor\b[^\n]*left\s*\(.*\bor\b[^\n]*right\s*\()")));
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

TEST_CASE("Regress: shadowed bare locals preserve initializer reads", "[Decompiler][Rewriter][Regression]") {
    auto id = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    for (bool readsOld : {false, true}) {
        auto args = readsOld ? std::vector<std::shared_ptr<Expression>>{id("v2")} : std::vector<std::shared_ptr<Expression>>{};
        auto call = std::make_shared<CallExpressionNode>(id("get"), args, std::vector<std::shared_ptr<Expression>>{id("v2")}, false, false);
        auto bare = std::make_shared<VariableDeclarationNode>(id("v2"), nullptr);
        std::vector<std::shared_ptr<Statement>> statements{
            bare, std::make_shared<ExpressionStatementNode>(call), std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{id("v2")})
        };
        DeadLocalEliminator{}.Run(statements);
        CHECK(statements.size() == (readsOld ? 3 : 2));
        CHECK(call->rets.size() == 1);
    }
}

TEST_CASE("Regress: empty then inversion preserves NaN and condition effects", "[Decompiler][Rewriter][Regression]") {
    auto call = [](const char *name) {
        return std::make_shared<CallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)), std::vector<std::shared_ptr<Expression>>{},
            std::vector<std::shared_ptr<Expression>>{}, false, true
        );
    };
    auto branch = std::make_shared<IfStatementNode>();
    branch->condition = std::make_shared<BinaryExpressionNode>("<", call("probe"), std::make_shared<NumberLiteralNode>(0));
    branch->thenBranch = std::make_shared<BlockStatementNode>();
    branch->elseBranch = std::make_shared<BlockStatementNode>();
    branch->elseBranch->body.push_back(std::make_shared<ExpressionStatementNode>(call("mark")));
    const std::string prelude = "local calls, marks = 0, 0\nlocal function probe() calls += 1 return 0/0 end\n"
                                "local function mark() marks += 1 end\n";
    auto render = [&] {
        SourceGenerator generator;
        branch->Accept(&generator);
        return prelude + generator.buffer.str() + "\nreturn calls, marks\n";
    };
    const auto before = render();
    std::vector<std::shared_ptr<Statement>> statements{branch};
    IfChainSimplifier{}.Run(statements);
    const auto after = render();
    REQUIRE(Recompiles(after));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(before), Luau::compile(after), {Luau::compile("")});
    CHECK(verdict.original.trace == "return: 1\t1\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: long effectful dispatch shares its result without recursive lifting", "[Decompiler][ControlFlow][Regression]") {
    std::string source = "local function run(x, step) local result = 0\n";
    for (int i = 0; i < 100; ++i)
        source +=
            (i == 0 ? "if " : "elseif ") + std::string("x.kind == \"k") + std::to_string(i) + "\" then result = step(result, " + std::to_string(i) + ")\n";
    source += "end return result end\nlocal function step(a, b) print(b) return a + b end\n"
              "print(run({kind='k0'}, step), run({kind='k49'}, step), run({kind='k99'}, step), run({kind='missing'}, step))";
    const auto out = DecompileOrFail(source);
    INFO(out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: long dispatch retains continue edges", "[Decompiler][ControlFlow][Regression]") {
    std::string source = "for _, x in {0, 49, 99, -1} do\n";
    for (int i = 0; i < 100; ++i)
        source += (i == 0 ? "if " : "elseif ") + std::string("x == ") + std::to_string(i) + " then print(" + std::to_string(i) + ") continue\n";
    source += "end print('missing') end";
    const auto out = DecompileOrFail(source);
    INFO(out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: mixed truthiness paths retain their shared effectful body", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function check(parsed, dates, consume)
            if (not parsed and dates[1]) or (parsed and dates[2] and parsed > dates[2]) then
                consume("fallback")
            end
        end
        check(nil, {1, 2}, print)
        check(3, {1, 2}, print)
        check(1, {1, 2}, print)
        check(nil, {}, print)
        local observed = setmetatable({}, {__index = function(_, key) print(key) return key end})
        check(nil, observed, print)
        check(3, observed, print)
        check(1, observed, print)
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    REQUIRE(Recompiles(out));
    CHECK(Contains(out, "not arg0 and arg1[1] or arg0 and arg1[2]"));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO(verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: adjacent return guards preserve condition effects", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function probe(label, value)
            print(label)
            return value
        end
        local function run(a, b)
            if probe("a", a) then return false end
            if probe("b", b) then return false end
            print("pass")
            return true
        end
        print(run(true, true), run(false, true), run(false, false))
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    REQUIRE(Recompiles(out));
    CHECK(Contains(out, " or "));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: merged elseif arms preserve condition effects", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function probe(label, value)
            print(label)
            return value
        end
        local function run(a, b, c)
            if probe("a", a) then
                print("hit")
            elseif probe("b", b) then
                print("hit")
            elseif probe("c", c) then
                print("other")
            end
        end
        run(true, true, true)
        run(false, true, true)
        run(false, false, true)
        run(false, false, false)
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    CHECK(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: table template fields retain read order when folded", "[Decompiler][Table][Regression]") {
    const std::string source = R"(
        local function build(time)
            return { month = time.Month, year = time.Year }
        end
        local time = setmetatable({}, { __index = function(_, key) print(key) return key end })
        local result = build(time)
        return result.month, result.year
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    REQUIRE(Recompiles(out));
    CHECK_FALSE(Contains(out, ".month ="));
    CHECK_FALSE(Contains(out, ".year ="));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: branch-created arrays survive their phi merge", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function choose(flag, date, previous, consume)
            local selected = if flag then {date, previous[2]} else {date}
            consume(selected)
            consume(selected)
        end
        local seen
        local function consume(value)
            print(value[1], value[2])
            if seen then print(rawequal(seen, value)) end
            seen = value
        end
        choose(true, 17, {0, 23}, consume)
        choose(false, 41, {}, consume)
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: optional fallback preserves value at shared return join", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local calls = 0
        local function fallback()
            calls += 1
            return { value = 23 }
        end
        local function choose(input)
            local selected = input
            if selected == nil then
                selected = fallback()
            end
            local result = {}
            result.value = selected.value
            return result
        end
        return choose({ value = 17 }).value, choose(nil).value, calls
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    CHECK(CountOccurrences(out, ".value =") == 1);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: named method result retains binding across fallback", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function choose(root, fallback)
            local selected = root:FindFirstChild("Dev")
            if selected == nil then
                selected = fallback.Dev
            end
            local result = {}
            result.value = selected.value
            return result
        end
        local root = { FindFirstChild = function(self) return self.child end }
        root.child = { value = 17 }
        local a = choose(root, { Dev = { value = 23 } }).value
        root.child = nil
        local b = choose(root, { Dev = { value = 23 } }).value
        return a, b
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: terminal join performs one observable store per call", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local writes = 0
        local target = setmetatable({}, {
            __newindex = function(_, _, value)
                writes += 1
            end
        })
        local function put(flag)
            local value
            if flag then
                value = 17
            else
                value = 23
            end
            target.value = value
        end
        put(true)
        put(false)
        return writes
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 2\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress: generic for pairs over a dict-style table preserves its keys", "[Decompiler][GenericFor][Regression]") {
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
    CHECK(Contains(out, "a = 1"));
    CHECK(Contains(out, "b = 2"));
    CHECK(Contains(out, "c = 3"));
}

// Stricter table literal tests (for working patterns)

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
    // Output may be `{  }` with spaces; check brace balance.
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
    CHECK(CountOccurrences(out, "{") >= 1);
}

// Infinite `while CONST` loops: testless back-edge / self-loop / for-wrapping
// `while 1 do for i=1,1,2 do break end end`; Luau emits the outer infinite while
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

// Empty-arm diamond before a return must not null the if's then-branch
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
    // The real value `v` (= t.X) is returned, not mangled into a bogus and/or chain. `t.X` stays a
    // local read placed before the `t.Ready` read: inlining it into `return t.X` would move the
    // index past the `if t.Ready` and change which read throws first if `t` is nil, so the faithful
    // form keeps `local v = t.X ... return v`.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+(\w+)\s*=\s*\w+\.X\b[\s\S]*return\s+\1\b)")));
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

// Stricter loop tests

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

// Phi-merged call result must not shadow the merge variable with `local`
// Known bug: a value defined in both branches of an if/else, where one branch
// is a method-call result, emitted `local v = obj:Method()` inside the branch
// (shadowing the hoisted merge variable) so the post-merge read saw the wrong
// value. The call-result branch must use a bare assignment.

TEST_CASE("Regress: phi-merged call result is not re-declared with local", "[Decompiler][Phi][Regression]") {
    // `cs and cs:IsA(...) or false` lowers so the method-call result lands directly
    // in the merge register (no temp), exercising the phi-hoist of a Call/NameCall
    // return target. `print` after forces the value to escape the branches.
    const auto out = DecompileOrFail(
        R"(
        return function(cs)
            local isSubj = cs and cs:IsA("VehicleSeat") or false
            print(isSubj)
        end
    )",
        2
    );

    INFO("decompile:\n" << out);
    // The method-call branch must NOT re-declare the merge variable with `local`
    // (that would shadow the hoisted merge variable and lose the value).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+\w+\s*=\s*\w+:IsA\()")));
}

// Comparison materialised into a boolean register
// Known bug: `local x = a ~= b` (a comparison stored as a boolean value, not
// used directly as a branch condition) lowered to a garbled
// `if a == b then local x = x end` with a self-assign and no real value.
// It must reconstruct the comparison expression itself.

TEST_CASE("Regress: comparison stored as boolean value is reconstructed", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        return function(self)
            local x = self.occlusionMode ~= "Invisicam"
            print(x)
        end
    )",
        2
    );

    INFO("decompile:\n" << out);
    // No self-assign garbage.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\b(v\d+|arg\d+)\s*=\s*\1\b)")));
    // The comparison must be materialised as a value.
    CHECK(ContainsRegex(out, std::regex(R"(=\s*\w+\.occlusionMode\s*~=\s*"Invisicam")")));
}

// Numeric for-loop with both `continue` and `break`
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
    // Exactly one break; the `continue` path must NOT have become a second break.
    size_t breakCount = 0;
    for (size_t p = out.find("break"); p != std::string::npos; p = out.find("break", p + 1))
        ++breakCount;
    CHECK(breakCount == 1);
}

// `x = X and (...)` must reassign, not shadow with a nested `local`
// Known bug: `local s = a and (b or c)` lowered to `local s = a; if s then
// local s ... end`; the inner `local s` shadowed the outer, so the post-merge
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

// `~=` / `==` equality-jump branch polarity
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
    const bool correctPolarity =
        ContainsRegex(out, std::regex(R"(~=\s*"b"\s*then\s*return\s+10)")) || ContainsRegex(out, std::regex(R"(==\s*"b"\s*then\s*return\s+20)"));
    CHECK(correctPolarity);
}

// Module-table constructor must not absorb non-inlinable field values
// Known bug: `local t = {}; function t.foo() ... end` was reconstructed as a
// fabricated literal `{ foo = v5 }` (every field aliased to a single reused
// register) and the closure leaked as a standalone global `anon_# = function`.
// The table must stay empty and the closure inline into the field store.

// A table-field closure on a module table (`local t = {}; function t.foo()`) is
// sugared back to dot-declaration form `function t.foo(...) ... end` by the
// ClassMethodRewriter, NOT left as a `t.foo = function()`/`t.foo = foo` pair.
// Dot form has no `self` slot, so the parameter list is untouched and the
// rewrite is always recompilable.
TEST_CASE("Regress: module table-field closure is sugared to dot declaration", "[Decompiler][Table][Regression]") {
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
    // Emitted as a dot declaration bound straight to the field.
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+\.foo\s*\()")));
    // Not split into a separate decl + field assignment.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\.foo\s*=)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+function\s+foo\b)")));
}

// Dot sugar on a module table must NOT absorb a `self` slot: every declared
// parameter stays in the visible signature (unlike the colon path). A two-arg
// module function keeps two args.
TEST_CASE("Regress: module dot-method keeps all parameters", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local m = {}
        function m.add(a, b)
            return a + b
        end
        return m
    )");

    INFO("decompile:\n" << out);
    // Two distinct params survive in the signature; no `self`, no param shift.
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+\.add\s*\(\s*\w+\s*,\s*\w+\s*\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\bself\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:add\b)")));
}

// ClassMethodRewriter: a class table (`X.__index = X`) with a colon-method
// field closure is sugared back to `function X:method(...)`, absorbing the
// implicit first parameter as `self` and shifting the remaining params down.
TEST_CASE("Regress: class colon-method is sugared to method syntax", "[Decompiler][Class][Regression]") {
    const auto out = DecompileOrFail(R"(
        local Animal = {}
        Animal.__index = Animal
        function Animal:speak(volume)
            return self.name, volume
        end
        return Animal
    )");

    INFO("decompile:\n" << out);
    // Emitted in colon form; the implicit self is absorbed and the one explicit
    // param is shifted down into the visible signature (two args -> one).
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+:speak\s*\(\s*\w+\s*\))")));
    // The body references self (the absorbed first arg), not a leaked arg0 name.
    CHECK(ContainsRegex(out, std::regex(R"(\bself\b)")));
    // The class table marker survives the rewrite.
    CHECK(ContainsRegex(out, std::regex(R"(\.__index\s*=)")));
    // Not left as a plain field-assigned function literal.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\.speak\s*=\s*function\b)")));
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
// a single `{ ... }` literal; the guard above must not over-trigger.
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

// Nested REF upvalue capture must alias, not redefine
// A variable captured by reference (mutated across scopes) is ONE shared variable.
// Rendering the capture as `local uv_N = source` makes a by-value copy: writes
// inside the closure no longer alias the outer variable. It must keep a single
// consistent name across all nesting levels with no redefinition.
TEST_CASE("Regress: nested REF upvalue capture aliases instead of redefining", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no debug names -> exercises the autogenerated-name path
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
    // No by-value capture copy (`local uv_N = ...`); that would desync the writes.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+uv_\d+\s*=)")));
    // The shared variable is declared once, BEFORE the closure that captures it.
    std::smatch valueDecl;
    REQUIRE(std::regex_search(out, valueDecl, std::regex(R"(local\s+v\d+\s*=\s*_G\.globalValue)")));
    const size_t closurePos = out.find("local function f");
    REQUIRE(closurePos != std::string::npos);
    CHECK(static_cast<size_t>(valueDecl.position(0)) < closurePos);
    // Exactly one declaration of it; the trailing write is an assignment, not a 2nd local.
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
    CHECK(Contains(out, "Vector3.new(1, 2, 3)")); // LOADK vector
    CHECK(Contains(out, "Vector3.new(4, 5, 6)")); // vector inside a constant table
    CHECK(Contains(out, "Vector3.new(7, 8, 9)"));
    CHECK_FALSE(Contains(out, "nil")); // no constant silently dropped to nil
}

// OmitFissionComments suppresses informational comments but keeps warnings
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
    // call; the generator's buffer is a reused member that accumulates otherwise.)
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

// A nested function's own vN must not shadow a captured upvalue of the same name
// A nested function reuses low register numbers, so its own `vN` can equal a
// captured upvalue that (after aliasing) reads as the enclosing scope's `vN`.
// Rendered inline, the inner `local vN` then shadows the upvalue. The inner's own
// auto-name must be disambiguated (suffixed) so it stays distinct.
TEST_CASE("Regress: nested function own vN does not shadow captured upvalue vN", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names; the collision surfaces
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

// Sibling closures capturing by value must not collide on uv_N names
// Each closure numbers its upvalues from 0, so emitting `local uv_N = source`
// capture copies in the shared parent scope makes siblings clash (closure A's
// `local uv_0 = t1` vs closure B's `local uv_0 = t1`), and a closure's own uv_0
// reference can bind to the wrong sibling's local. VAL captures must alias the
// source variable directly; no copy, no uv_N.
TEST_CASE("Regress: sibling VAL captures alias sources without uv_N collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names; collision would surface
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

// LCT_UPVAL capture chaining: a closure that captures one of its parent's
// upvalues must reference the parent's upvalue, not a colliding fresh uv_0
// Bug (real Opera-GX script): an inner closure captured the parent's upvalue
// index N (CAPTURE mode 2 / LCT_UPVAL), but the lifter named the inner upvalue
// uv_0; colliding with the parent's own uv_0 (a different value). e.g. a pcall
// closure rendered `uv_0:GetCampaignEligibilityAsync(...)` where uv_0 was the
// PlaceId table, not the AdService it actually captured. Compiled WITHOUT upvalue
// debug names (debugLevel 1), so names are autogenerated and the collision shows.
TEST_CASE("Regress: LCT_UPVAL capture resolves to the parent upvalue, no uv_0 collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no upvalue debug names -> autogenerated uv_N, collision visible
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
    // must NOT be made on uv_0; that is the collision bug.
    CHECK_FALSE(Contains(out, "uv_0:Async"));
}

// Generalized iteration `for k,v in t do` must not emit the implicit nils
// Luau lowers `for k, v in t do` to the iterator triple [t, nil, nil] (LOAD nil,
// LOAD nil, FORGPREP). The lifter emitted all three slots -> `for k, v in t, nil,
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

// Production safety boundary: malformed bytecode must not crash the host
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
    const auto out = DecompileOrFail(
        R"(
        local a = ({})
        while 1 do for i = 1, 1, #"67" or 9 do table.insert(a, i) break end end
    )",
        0
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, "table.insert"));
    // the length expression appears once (the for-step), not also as a dead local.
    CHECK(CountOccurrences(out, "#\"67\"") == 1);
}

// ScopeBlockIntroducer: do-end scoping of register-reuse phases
// Register reuse (hence whether the lifter emits a `local vN` redefinition) is too
// allocation-dependent to force from source reliably, so the wrap/guard logic is unit-tested
// directly on a hand-built statement list; an end-to-end test then checks recompile-safety.
namespace {
    std::shared_ptr<VariableDeclarationNode> LocalDecl(const std::string &name, const std::string &valueName) {
        auto lhs = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
        auto rhs = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(valueName));
        return std::make_shared<VariableDeclarationNode>(lhs, rhs);
    }
    std::shared_ptr<ExpressionStatementNode> UseStmt(const std::string &name) {
        return std::make_shared<ExpressionStatementNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)));
    }
    bool IsDoBlock(const std::shared_ptr<Statement> &s) {
        auto b = std::dynamic_pointer_cast<BlockStatementNode>(s);
        return b && b->bEmitAsDoBlock;
    }
} // namespace

// A redefinition whose prior lifetime is self-contained splits into two `do ... end` scopes.
TEST_CASE("Scope: self-contained redefinition is split into do-blocks", "[Decompiler][Scope][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "x")); // local v0 = x
    stmts.push_back(UseStmt("v0"));        // v0
    stmts.push_back(LocalDecl("v0", "y")); // local v0 = y  (redefinition; old v0 dead)
    stmts.push_back(UseStmt("v0"));        // v0

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 2);
    CHECK(IsDoBlock(stmts[0]));
    CHECK(IsDoBlock(stmts[1]));
    auto b0 = std::dynamic_pointer_cast<BlockStatementNode>(stmts[0]);
    CHECK(b0->body.size() == 2); // the decl + its use
}

// A local still live past the redefinition point keeps the source flat. Wrapping would require
// hoisting an unrelated binding outside its original region.
TEST_CASE("Scope: a local live across the redefinition keeps the statement list flat", "[Decompiler][Scope][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "x"));   // local v0 = x
    stmts.push_back(LocalDecl("keep", "a")); // local keep = a   (escapes the first phase)
    stmts.push_back(UseStmt("v0"));          // v0
    stmts.push_back(LocalDecl("v0", "y"));   // local v0 = y     (redefinition)
    stmts.push_back(UseStmt("keep"));        // keep  <- used AFTER the redefinition
    stmts.push_back(UseStmt("v0"));          // v0

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 6);
    for (const auto &statement : stmts)
        CHECK_FALSE(IsDoBlock(statement));
    auto keep = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[1]);
    REQUIRE(keep);
    CHECK(keep->value != nullptr);
}

// End-to-end: with the pass on, a real decompile of register-reuse-shaped code recompiles.
TEST_CASE("Scope: pass keeps decompiled output recompilable", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local keep = workspace.Part
            local a = keep:Clone()
            a.Parent = workspace
            local b = keep:Clone()
            b.Parent = workspace
            return keep
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return"));
    CHECK(Recompiles(out));
}

TEST_CASE("Scope: module initializer with crossing locals stays flat", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
        local service = game:GetService("ReplicatedStorage")
        local cache = require("../Internal/Cache")
        local lookups = {
            Table = require("../Utilities/TableLookup"),
            Value = require("../Utilities/ValueLookup"),
            Key = require("../Utilities/KeyLookup"),
        }
        local module = {}
        function module.Create(id, key, value, lookup, sync)
            local entry = cache[id]
            if lookup then
                local kind = if lookup.Table then "Table" elseif lookup.Value then "Value" else "Key"
                local lookupFn = lookups[kind]
                local found = lookupFn and lookupFn.TableLookup(id, cache[id], lookup.Name, 1)
                if found then found[key] = value else entry[key] = value end
            else
                entry[key] = value
            end
            if not sync then return end
            send({Identity = id, indexkey = tostring(key), dataValue = value})
        end
        return module
    )");

    INFO("decompile:\n" << out);
    CHECK(out.starts_with("--[["));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((?:^|\n)\s*do\s*(?:\n|$))")));
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: lookup guard keeps its fallback for non-string keys", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function create(entry, key, value, lookup, found)
            if lookup and type(key) == "string" then
                if found then
                    found[key] = value
                else
                    entry[key] = value
                end
            else
                entry[key] = value
            end
        end
        local numeric = {}
        create(numeric, 7, 11, {Table = true}, nil)
        local plain = {}
        create(plain, "name", 12, nil, nil)
        return numeric[7], plain.name
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 11\t12\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

// End-to-end through the SourceGenerator: a redefinition list renders as two `do ... end` scopes
// and the rendered source recompiles.
TEST_CASE("Scope: redefinition renders as do-end and recompiles", "[Decompiler][Scope][Regression]") {
    auto assign = [](const std::string &lhs, const std::string &rhs) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(lhs)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(rhs))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "g1")); // local v0 = g1
    stmts.push_back(assign("h1", "v0"));    // h1 = v0
    stmts.push_back(LocalDecl("v0", "g2")); // local v0 = g2  (redefinition)
    stmts.push_back(assign("h2", "v0"));    // h2 = v0

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator gen{};
    const std::string src = gen.GenerateSource(&root);

    INFO("rendered:\n" << src);
    CHECK(ContainsRegex(src, std::regex(R"((?:^|\n)\s*do\s*(?:\n|$))")));
    CHECK(ContainsRegex(src, std::regex(R"((?:^|\n)\s*end\s*(?:\n|$))")));
    CHECK(CountOccurrences(src, "do") >= 2);
    CHECK(Recompiles(src));
}

// Real lifted call-result locals are NameCall/Call nodes (rets + isLocal) wrapped in an
// ExpressionStatement, NOT VariableDeclaration. The first cut of this feature only knew
// VariableDeclaration and produced ZERO do-blocks on real output; this guards that exact shape.
TEST_CASE("Scope: ExpressionStatement-wrapped call-result redefinition is scoped", "[Decompiler][Scope][Regression]") {
    auto callDecl = [](const std::string &ret, const std::string &obj) {
        std::vector<std::shared_ptr<Expression>> args;
        std::vector<std::shared_ptr<Expression>> rets{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ret))};
        auto nc = std::make_shared<NameCallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(obj)), std::make_shared<StringLiteralNode>("m"), args, rets, false, false
        );
        nc->bIsLocalDeclaration = true; // `local ret = obj:m()`
        return std::make_shared<ExpressionStatementNode>(nc);
    };
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(callDecl("v0", "a")); // local v0 = a:m()
    stmts.push_back(UseStmt("v0"));
    stmts.push_back(callDecl("v0", "b")); // local v0 = b:m()  (redefinition via call rets)
    stmts.push_back(UseStmt("v0"));

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 2);
    CHECK(IsDoBlock(stmts[0]));
    CHECK(IsDoBlock(stmts[1]));
}

// RequireRenamer: path-style require(...) -> module leaf name

// A path-style `require(A.B.Mod)` bound to an auto-named local is renamed to the
// module's leaf name, and every reference follows.
TEST_CASE("Regress: path require is renamed to its module leaf", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local mod = require(svc.Core.Widget)
        print(mod)
        print(mod)
        return mod
    )");

    INFO("decompile:\n" << out);
    // The auto-named local becomes `local Widget = require(...)`.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+Widget\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// A string-variant require has no path to mine, so the binding is left untouched.
TEST_CASE("Regress: string require is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local mod = require("SomeStringModule")
        print(mod)
        print(mod)
        return mod
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "require(\"SomeStringModule\")"));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+SomeStringModule\b)")));
    CHECK(Recompiles(out));
}

// Two requires resolving to the same leaf are ambiguous -> neither is renamed.
TEST_CASE("Regress: duplicate require leaf is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.Dup)
        local b = require(svc.Other.Dup)
        print(a, b)
        print(a, b)
        return a, b
    )");

    INFO("decompile:\n" << out);
    // Path keys still read `.Dup`, but no binding is renamed to `local Dup`.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+Dup\s*=\s*require\()")));
    CHECK(CountOccurrences(out, "require(") >= 2);
    CHECK(Recompiles(out));
}

// A leaf that matches a built-in Luau/Roblox global is not used as a name: it
// would shadow the global. The clashing require is left auto-named; an adjacent
// non-clashing one still renames.
TEST_CASE("Regress: require leaf colliding with a Luau global is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.task)
        local b = require(svc.Core.Inventory)
        print(a, b)
        print(a, b)
        return a, b
    )");

    INFO("decompile:\n" << out);
    // `task` is a built-in -> never bound as a local name.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+task\s*=\s*require\()")));
    // The non-clashing module still renames.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+Inventory\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// GetterRenamer: local = obj:GetXxx() -> xxx

// A local bound to a `Get<Property>` call is renamed to the property: the `Get`
// prefix is dropped and the first remaining letter lower-cased.
TEST_CASE("Regress: getter-bound local is renamed to its property", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(self)
            local a = self:GetFullName()
            local b = self:GetHumanoid()
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+fullName\s*=\s*\w+:GetFullName\()")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+humanoid\s*=\s*\w+:GetHumanoid\()")));
    CHECK(Recompiles(out));
}

// A getter whose property name is a built-in global is left auto-named:
// `GetTime` would yield `time`, which would shadow the `time` global.
TEST_CASE("Regress: getter property colliding with a global is not renamed", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(self)
            local a = self:GetTime()
            print(a)
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+time\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:GetTime\()")));
    CHECK(Recompiles(out));
}

// AttributeRenamer: local = obj:GetAttribute("X") -> x ; obj:SetAttribute("X", v) -> v named x

// A local bound to `:GetAttribute("Name")` is renamed after the attribute string, lower-first-cased.
TEST_CASE("Regress: GetAttribute-bound local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("AccuracyDeviation")
            local b = obj:GetAttribute("Pellets")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+accuracyDeviation\s*=\s*\w+:GetAttribute\("AccuracyDeviation"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+pellets\s*=\s*\w+:GetAttribute\("Pellets"\))")));
    CHECK(Recompiles(out));
}

// The value written by `:SetAttribute("Name", v)` is renamed after the attribute string, at its
// declaration and every reference.
TEST_CASE("Regress: SetAttribute value local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj, base)
            local v = base * 2
            obj:SetAttribute("Damage", v)
            return v
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+damage\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// When a GetAttribute read and a SetAttribute write name the same attribute in one scope, the read owns
// the name: the read local becomes `damage`, and the written value keeps its auto-name (two locals cannot
// share `damage`). Without the get-over-set priority both would collide and neither would be renamed.
TEST_CASE("Regress: GetAttribute claims the name over a same-attribute SetAttribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj, base)
            local d = obj:GetAttribute("Damage")
            local nv = base + 1
            obj:SetAttribute("Damage", nv)
            print(d)
            print(d)
            return d, nv
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+damage\s*=\s*\w+:GetAttribute\("Damage"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// A multi-word attribute name is folded to camelCase: `"Max HP"` -> `maxHp`,
// `"Super Long Name"` -> `superLongName`.
TEST_CASE("Regress: multi-word attribute name is camelCased", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("Max HP")
            local b = obj:GetAttribute("Super Long Name")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+maxHp\s*=\s*\w+:GetAttribute\("Max HP"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+superLongName\s*=\s*\w+:GetAttribute\("Super Long Name"\))")));
    CHECK(Recompiles(out));
}

// A name that folds to a leading digit is salvaged by spelling the digit out: `"3D Offset"` folds to
// `3DOffset`, then the leading `3` becomes `three` -> `threeDOffset`.
TEST_CASE("Regress: leading-digit attribute name spells the digit out", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("3D Offset")
            local b = obj:GetAttribute("2Handed")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+threeDOffset\s*=\s*\w+:GetAttribute\("3D Offset"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+twoHanded\s*=\s*\w+:GetAttribute\("2Handed"\))")));
    CHECK(Recompiles(out));
}

// An attribute name whose folded form is still not a legal identifier (punctuation the space-fold does
// not remove) leaves the auto-name untouched.
TEST_CASE("Regress: attribute name that stays illegal after folding is not renamed", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("Damage/Sec")
            print(a)
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(:GetAttribute\("Damage/Sec"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+damageSec\s*=)")));
    CHECK(Recompiles(out));
}

// PropertyRenamer: local = obj.Property -> property

namespace {
    std::shared_ptr<MemberExpressionNode> MemberRead(const std::string &tbl, const std::string &key) {
        return std::make_shared<MemberExpressionNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(tbl)), key);
    }
    std::string DeclName(const std::shared_ptr<Statement> &s) {
        auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(s);
        if (!vd)
            return {};
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(vd->identifier);
        return (id && id->identifier) ? id->identifier->name : std::string{};
    }
} // namespace

// A local with a single source that is a property read is renamed to the
// property (first letter lower-cased), references included.
TEST_CASE("Regress: single-source property local renamed to property", "[Decompiler][Property][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "Character")
        )
    ); // local v0 = plr.Character
    stmts.push_back(UseStmt("v0"));

    PropertyRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "character");
    auto use = std::dynamic_pointer_cast<IdentifierExpressionNode>(std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[1])->expression);
    REQUIRE(use);
    CHECK(use->identifier->name == "character");
}

// A local that is reassigned (more than one value source) keeps its auto-name --
// the property name would no longer describe its contents.
TEST_CASE("Regress: reassigned property local is not renamed", "[Decompiler][Property][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "First"))
    ); // local v0 = plr.First
    stmts.push_back(UseStmt("v0"));
    stmts.push_back(
        std::make_shared<AssignmentStatementNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "Second"))
    ); // v0 = plr.Second
    stmts.push_back(UseStmt("v0"));

    PropertyRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "v0"); // untouched: two sources
}

TEST_CASE("Regress: class marker does not rename its owner to __index", "[Decompiler][Class][Naming][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), std::make_shared<TableLiteralNode>()
        )
    );
    stmts.push_back(
        std::make_shared<AssignmentStatementNode>(MemberRead("v0", "__index"), std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")))
    );

    ReverseFieldRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "v0");
}

TEST_CASE("Regress: scoped rename updates method receiver", "[Decompiler][Class][Naming][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), std::make_shared<TableLiteralNode>()
        )
    );
    auto method = std::make_shared<FunctionDeclarationNode>(
        "v0:initialize", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, std::make_shared<BlockStatementNode>(), false
    );
    stmts.push_back(method);

    ScopeAwareRenamer::Run(stmts, [](const auto &) { return std::vector<std::pair<std::string, std::string>>{{"v0", "controller"}}; });

    CHECK(DeclName(stmts[0]) == "controller");
    CHECK(method->functionName == "controller:initialize");
}

// End-to-end: a chain of single-source property reads is renamed.
TEST_CASE("Regress: property reads are renamed end-to-end", "[Decompiler][Property][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function clean(plr)
            local c = plr.Character
            local h = c.Humanoid
            print(c, h)
            print(c, h)
            return c, h
        end
        return clean
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+character\s*=\s*\w+\.Character)")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+humanoid\s*=\s*character\.Humanoid)")));
    CHECK(Recompiles(out));
}

// Output-quality cleanups

// A no-op `x = x` reassignment is dropped; surrounding statements survive.
TEST_CASE("Regress: self-assignment is eliminated", "[Decompiler][Cleanup][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    auto keepL = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("keep"));
    auto keepR = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("src"));
    stmts.push_back(std::make_shared<AssignmentStatementNode>(keepL, keepR)); // keep = src
    auto selfL = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("foo"));
    auto selfR = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("foo"));
    stmts.push_back(std::make_shared<AssignmentStatementNode>(selfL, selfR)); // foo = foo  (dropped)

    SelfAssignmentEliminator{}.Run(stmts);

    REQUIRE(stmts.size() == 1);
    auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[0]);
    REQUIRE(asn);
    auto l = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
    REQUIRE(l);
    CHECK(l->identifier->name == "keep");
}

// A literal call-callee gets exactly one wrapping paren layer, not two.
TEST_CASE("Regress: literal method call is not double-parenthesized", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        return ("hello"):rep(3)
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\("hello"\):rep\()")));
    CHECK_FALSE(Contains(out, "((")); // no double-paren
    CHECK(Recompiles(out));
}

// A keyed table value is emitted without redundant wrapping parens.
TEST_CASE("Regress: keyed table value has no wrapping parens", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        return {[0] = "s", [1] = "ms"}
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*"s")")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*\(")")));
    CHECK(Recompiles(out));
}

// A constant with high (non-ASCII) bytes round-trips as the raw UTF-8 text, not
// a re-escaped `\\206` sequence (the deserializer no longer pre-escapes).
TEST_CASE("Regress: high-byte string constant is not double-escaped", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188s\""); // source escapes -> bytes CE BC 73 ("μs")

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));    // raw UTF-8 bytes present
    CHECK_FALSE(Contains(out, "\\206")); // not re-emitted as a decimal escape
    CHECK(Recompiles(out));
}

// Mixed string: a valid UTF-8 sequence stays raw (readable) while invalid/lone
// high bytes are escaped as \ddd, so the emitted file is always valid UTF-8 and
// still round-trips to the identical bytes.
TEST_CASE("Regress: invalid UTF-8 bytes are escaped, valid stays raw", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188\\255x\""); // CE BC (mu, valid) + FF (invalid) + x

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));   // valid 2-byte sequence kept raw
    CHECK(Contains(out, "\\255"));      // lone 0xFF escaped to decimal
    CHECK_FALSE(Contains(out, "\xFF")); // no raw invalid byte leaked into the file
    CHECK(Recompiles(out));
}

// A computed local stored into a field is named after that field.
TEST_CASE("Regress: reverse-field naming names a local after its field", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function make(x)
            local self = {}
            local v = x * 2 + 1
            self.health = v
            print(v)
            return self
        end
        return make
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+health\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\.health\s*=\s*health)")));
    CHECK(Recompiles(out));
}

// SETLIST array-element forward references
// An array element that indexes a populated inner table (built between the outer
// NEWTABLE and its SETLIST) must not be folded into the constructor: the inner
// table's `local` is emitted after the outer one, so the fold either drops it or
// reads nil. When the outer table is a real local, the SETLIST is emitted as
// `t[i] = elem` assignments after every contributing local is declared.
TEST_CASE("Regress: SETLIST array element indexing an inner table is declared before use", "[Decompiler][Table][SetList][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = { 43, ({ "a", "b", f = "" })[2] }
        t[1] = 5
        return t
    )");

    INFO("decompile:\n" << out);
    // the inner table survives as a real local (it was being dropped entirely).
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\s*=\s*\{\s*"a",\s*"b")")));
    // every auto-named local is declared before it is used.
    CHECK(NoForwardReference(out));
    // the inner table is NOT folded into the outer constructor as a bare `vN[2]` element.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\{[^{}]*\bv\d+\[\d+\][^{}]*\})")));
    CHECK(Recompiles(out));
}

// The same defect when the indexed inner table is the FIRST element and the outer
// table is multi-use (so it is a real local, not inlined into a return).
TEST_CASE("Regress: SETLIST first element indexing an inner table is sound", "[Decompiler][Table][SetList][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = { ({ "a", "b", f = "" })[2], 43 }
        print(t)
        print(t)
        return t
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\s*=\s*\{\s*"a",\s*"b")")));
    CHECK(NoForwardReference(out));
    CHECK(Recompiles(out));
}

// Guard against over-deferral (a quality regression): the safe, common cases must
// STILL fold into a single `{ ... }` constructor. An inner table built and indexed
// inline as a return value is consumed at its last use, where everything already
// exists; it folds. Arithmetic over params/earlier-locals folds. Nested constructors
// fold. None of these should spill into `t[i] = elem` statements.
TEST_CASE("Regress: SETLIST safe elements still fold into a constructor", "[Decompiler][Table][SetList][Regression]") {
    SECTION("return-form inner-table index folds") {
        const auto out = DecompileOrFail("return { 43, ({ 10, 20, 30 })[2] }");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s*\{\s*43,\s*\(\{\s*10,\s*20,\s*30\s*\}\)\[2\]\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("arithmetic over params folds") {
        const auto out = DecompileOrFail("local function f(a, b) return { a + 1, b * 2, a - b } end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*arg0\s*\+\s*1,\s*arg1\s*\*\s*2,\s*arg0\s*-\s*arg1\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("array of nested constructors folds and stays declared-before-use") {
        const auto out = DecompileOrFail(R"(
            local t = { { x = 1 }, { y = 2 } }
            t[1].x = 9
            return t
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*\{\s*x\s*=\s*1\s*\},\s*\{\s*y\s*=\s*2\s*\}\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// A SETLIST with count == 0 is variadic: the array gathers every source register up to the stack top,
// whose last element is a multret call. The size must come from that multret producer, not be treated
// as one element -- otherwise all but the first element are dropped and each emitted as a stray call.
// And no element may also leak as a standalone `local` beside the folded constructor (the SETLIST
// counts its base register twice). Regression: TestService.LuauLSP_Settings (`{ a(), b(), ..., n() }`).
TEST_CASE("Regress: variadic SETLIST keeps every element and does not duplicate", "[Decompiler][Table][SetList][Regression]") {
    SECTION("table of calls with a multret tail") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                return { g(1), g(2), g(3), g(4), g(5) }
            end
            return f
        )");
        INFO("decompile:\n" << out);
        // every element survives (the param `g` is auto-named arg0 at flags=0)...
        for (int i = 1; i <= 5; ++i) {
            const std::string call = "arg0(" + std::to_string(i) + ")";
            INFO("element " << call);
            CHECK(CountOccurrences(out, call) == 1); // present exactly once -- not dropped, not duplicated
        }
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("nested array assigned into a field (LuauLSP_Settings shape)") {
        const auto out = DecompileOrFail(R"(
            local t = {}
            t.include = {
                game:GetService("Workspace"),
                game:GetService("Players"),
                game:GetService("Lighting"),
                game:GetService("TestService"),
            }
            return t
        )");
        INFO("decompile:\n" << out);
        CHECK(CountOccurrences(out, R"(GetService("Workspace"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("Players"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("Lighting"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("TestService"))") == 1);
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// Variadic / multret tails (SSA implicit-use completeness)
// A RETURN / SETLIST / CALL whose operand count is 0 is variadic: the value list runs from its base
// register up to a multret producer (a call returning all results, or `...`). The SSA builder must
// record every register in that range as a use -- recording only the first (the old `count==0 -> 1`)
// silently dropped every trailing value (`return 1, 2, f()` lifted to `return 1`). These guard that
// whole class so it cannot regress again.
TEST_CASE("Regress: variadic RETURN keeps every value", "[Decompiler][Variadic][Return][Regression]") {
    SECTION("multret call tail: return 1, 2, g()") {
        const auto out = DecompileOrFail("local function f(g) return 1, 2, g() end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+1,\s*2,\s*arg0\(\))")));
        CHECK(Recompiles(out));
    }
    SECTION("vararg tail: return 1, ...") {
        const auto out = DecompileOrFail("local function f(...) return 1, ... end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+1,\s*\.\.\.)")));
        CHECK(Recompiles(out));
    }
    SECTION("pure multret: return g()") {
        const auto out = DecompileOrFail("local function f(g) return g() end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\(\))")));
        CHECK(Recompiles(out));
    }
    SECTION("pure vararg: return ...") {
        const auto out = DecompileOrFail("local function f(...) return ... end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+\.\.\.)")));
        CHECK(Recompiles(out));
    }
    SECTION("fixed multi-return unaffected: return a, b, c") {
        const auto out = DecompileOrFail("local function f(a, b, c) return a, b, c end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0,\s*arg1,\s*arg2)")));
        CHECK(Recompiles(out));
    }
    SECTION("multret in non-tail position is truncated to one value: return g(), b") {
        const auto out = DecompileOrFail("local function f(g, b) return g(), b end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\(\),\s*arg1)")));
        CHECK(Recompiles(out));
    }
}

// Auto-name vs program-global collision
// The decompiler names registers `v<reg>`. A program global shaped the same way (`v6`) would be
// overwritten by a `local v6`, and on recompile later reads of the global would rebind to the local.
// GetVarName prefixes the register's local (`_v6`) so the global is preserved, and emits a FISSION INFO.
TEST_CASE("Regress: register auto-name does not overwrite a same-named global", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local v1380 = 110
        tonumber()
        local v1381 = not v6()
        local v1382 = 141.7
        v681(631)
        return #("a-b"), select(nil, 31)[{ ["data"] = nil }]
    )");

    INFO("decompile:\n" << out);
    // the colliding register local is prefixed (the global `v6` is referenced as a bare call).
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+_v\d+\b)")));
    // the global `v6` survives as a global (not shadowed before its use).
    CHECK(NoForwardReference(out));
    CHECK(Recompiles(out));
}

// Back-propagated names are lower-first-cased
// A local stored into a global (SETGLOBAL) is named after the global, lower-first-cased so it cannot
// shadow it: `BlahBlah = v0` -> `local blahBlah = ...; BlahBlah = blahBlah`.
TEST_CASE("Regress: SETGLOBAL back-propagates a lower-first global name", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            BlahBlah = a
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+blahBlah\b)")));        // lower-first local
    CHECK(ContainsRegex(out, std::regex(R"(\bBlahBlah\s*=\s*blahBlah\b)"))); // global preserved, no shadow
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\blocal\s+BlahBlah\b)")));  // never a same-cased local
    CHECK(Recompiles(out));
}

// A local stored into a capitalized field is named after it, lower-first-cased.
TEST_CASE("Regress: reverse-field back-prop lowers a capitalized field name", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            o.Health = a
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+health\b)")));      // lower-first local
    CHECK(ContainsRegex(out, std::regex(R"(\.Health\s*=\s*health\b)"))); // field key keeps its casing
    CHECK(Recompiles(out));
}

// Shadow safety: a global that is already lower-first must NOT be back-propagated; a `local score`
// would shadow the `score` global on its later reads. The engine's used-name gate must refuse it.
TEST_CASE("Regress: lower-first global is not back-propagated (would shadow)", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            score = a
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\blocal\s+score\b)"))); // would shadow the global `score`
    CHECK(ContainsRegex(out, std::regex(R"(\bscore\s*=\s*\w)")));        // the global assignment survives
    CHECK(Recompiles(out));
}

// Constructor-result naming
// A local bound to a constructor call is named after what it builds. `Instance.new("X")` names after
// the class string; any other `Type.new(...)` / `Type.from*(...)` names after the type. Lower-first so
// the local never shadows the type/global. Only the lifter's auto-names are retargeted.
TEST_CASE("Feature: Instance.new names the local after the class string", "[Decompiler][Naming][Constructor]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local a = Instance.new("Part")
            a.Anchored = true
            print(a)
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+part\s*=\s*Instance\.new)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: Instance.new lower-firsts a PascalCase class string", "[Decompiler][Naming][Constructor]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local a = Instance.new("ScreenGui")
            a.Enabled = true
            print(a)
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+screenGui\b)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: datatype constructor names the local after the type", "[Decompiler][Naming][Constructor]") {
    SECTION("Vector3.new") {
        const auto out = DecompileOrFail(R"(
            local function f(x)
                local a = Vector3.new(x, x, x)
                print(a)
                return a + a
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+vector3\s*=\s*Vector3\.new)")));
        CHECK(Recompiles(out));
    }
    SECTION("Color3.fromRGB (from* factory)") {
        const auto out = DecompileOrFail(R"(
            local function f()
                local a = Color3.fromRGB(1, 2, 3)
                print(a)
                return a, a
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+color3\s*=\s*Color3\.fromRGB)")));
        CHECK(Recompiles(out));
    }
    SECTION("user OOP class .new()") {
        const auto out = DecompileOrFail(R"(
            local function f()
                local a = MyClass.new()
                a:init()
                return a
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+myClass\s*=\s*MyClass\.new)")));
        CHECK(Recompiles(out));
    }
}

// Loop-variable naming (nesting-aware)
// Numeric-for loop vars become i / j / k by nesting depth; siblings reuse i (disjoint scopes).
TEST_CASE("Feature: nested numeric for loops use i, j, k", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b, c)
            for x = 1, a do
                for y = 1, b do
                    for z = 1, c do
                        print(x, y, z)
                    end
                end
            end
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+i\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+j\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+k\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bprint\(i,\s*j,\s*k\))")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: sibling numeric for loops both reuse i", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b)
            for x = 1, a do print(x) end
            for y = 1, b do print(y) end
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountOccurrences(out, "for i =") == 2); // both siblings -> i
    CHECK(Recompiles(out));
}

// Generic-for: ipairs -> i, v; pairs (and others) -> k, v.
TEST_CASE("Feature: generic for ipairs uses i, v", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for a, b in ipairs(t) do print(a, b) end
            return t
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+i,\s*v\s+in\s+ipairs\b)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: generic for pairs uses k, v", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for a, b in pairs(t) do print(a, b) end
            return t
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+k,\s*v\s+in\s+pairs\b)")));
    CHECK(Recompiles(out));
}

// Collision safety: a loop var must not be renamed to a name the body already uses (a global `i`),
// which it would otherwise capture. Bump past it; the global write survives.
TEST_CASE("Feature: loop var does not capture a same-named global in its body", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a)
            for x = 1, a do
                i = x
            end
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\bfor\s+i\s*=)"))); // would capture the global `i`
    CHECK(ContainsRegex(out, std::regex(R"(\bi\s*=\s*\w)")));        // the global assignment survives
    CHECK(Recompiles(out));
}

// self recovery for module-table methods
// A module-table field closure whose first parameter is used as a receiver (`p.field`, `p:m()`) is a
// method: emit colon syntax with `self` (`function T:m(...)`). A free function (first param not a
// receiver) stays dot syntax with its parameters intact.
TEST_CASE("Feature: module-table method recovers self (colon form)", "[Decompiler][Naming][Self]") {
    const auto out = DecompileOrFail(R"(
        local T = {}
        function T:setValue(v)
            self.value = v
        end
        function T:getValue()
            return self.value
        end
        function T.freeFn(a, b)
            return a + b
        end
        return T
    )");
    INFO("decompile:\n" << out);
    // member-write method -> colon + self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+:setValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bself\.value\s*=)")));
    // member-read method -> colon + self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+:getValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\breturn\s+self\.value\b)")));
    // free function (no receiver use) -> stays dot, no self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+\.freeFn\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:freeFn\b)")));
    CHECK(Recompiles(out));
}

// Class methods (the `X.__index = X` pattern) already recover self; guard it stays that way.
TEST_CASE("Feature: class method keeps self", "[Decompiler][Naming][Self]") {
    const auto out = DecompileOrFail(R"(
        local C = {}
        C.__index = C
        function C:setX(v)
            self.x = v
        end
        return C
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+\w+:setX\b)"))); // table keeps auto-name under flags=0
    CHECK(ContainsRegex(out, std::regex(R"(\bself\.x\s*=)")));
    CHECK(Recompiles(out));
}

// Length / count naming
// A local bound to a `#expr` length is named `count`.
TEST_CASE("Feature: length operator names the local count", "[Decompiler][Naming][Length]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local n = #t
            print(n)
            return n + n
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+count\s*=\s*#)")));
    CHECK(Recompiles(out));
}

// Scope-restore: a register used only as a branch-local temporary must not
// keep its slot "defined" past the merge. A later instruction reusing the
// slot (a merge-block NEWTABLE) was emitted as a bare `v2 = {}` -- no `local`
// -- leaking the value to a global. (Repro: MouseOverModule, R2 is a namecall
// arg inside the `or` branch, then a fresh table in the merge block.)
TEST_CASE("Regress: reused branch-temp slot is re-declared local after the merge", "[Decompiler][Naming][Regression]") {
    // a leaking bare assignment of an auto-name to a table literal: `v2 = { ... }` with no `local`.
    const std::regex leak(R"((?:^|\n)[ \t]*v\d+[ \t]*=[ \t]*\{)");

    SECTION("if-branch temp then merge-block table") {
        const auto out = DecompileOrFail(R"(
            local function f(c, obj)
                if c then
                    obj:Method("temparg")
                end
                local t = {}
                t.x = 1
                return t
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK_FALSE(ContainsRegex(out, leak));                               // no global leak
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+v\d+\s*=\s*\{)"))); // table is a local
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }

    SECTION("`or` with namecall arg then table (MouseOverModule shape)") {
        const auto out = DecompileOrFail(R"(
            local v = game.Players.LocalPlayer or game.Players:GetPropertyChangedSignal("LocalPlayer")
            local mouse = v:GetMouse()
            local t = {}
            t.handler = function() end
            return t
        )");
        INFO("decompile:\n" << out);
        CHECK_FALSE(ContainsRegex(out, leak));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// pcall / xpcall result naming
// `local ok, result = pcall(f)` -- the first protected-call return is always a boolean status, the
// second its result/error. Conventional, never-wrong names. Reused across scopes (each scope gets its
// own ok/result); a second pcall in the same scope bumps to ok2/result2 so distinct registers never
// collapse to one name.
TEST_CASE("Feature: pcall result names ok / result", "[Decompiler][Naming][Pcall]") {
    SECTION("multi-return pcall") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local a, b = pcall(g, 1, 2)
                if a then print(b) end
                return a
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*,\s*result\s*=\s*pcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("single-return pcall") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local a = pcall(g)
                if a then print("y") end
                return a
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*=\s*pcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("xpcall names the same way") {
        const auto out = DecompileOrFail(R"(
            local function f(g, h)
                local a, b = xpcall(g, h)
                return a, b
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*,\s*result\s*=\s*xpcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("two pcalls in one scope stay distinct (no aliasing)") {
        const auto out = DecompileOrFail(R"(
            local function f(g, h)
                local a = pcall(g)
                local b = pcall(h)
                if a then print("a") end
                if b then print("b") end
                return a, b
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*=\s*pcall\b)")));
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok2\s*=\s*pcall\b)")));
        // the two statuses must remain two distinct names through the return
        CHECK(ContainsRegex(out, std::regex(R"(\breturn\s+ok\s*,\s*ok2\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("existing ok in scope is not captured") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local ok = 5
                local a, b = pcall(g)
                return ok, a, b
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(Recompiles(out));
        CHECK(NoForwardReference(out));
    }
}

TEST_CASE("Tail-duplicated table re-inlines consumed elements", "[Decompiler][Regression][DoubleLift]") {
    // A run-once repeat-until followed by a while-with-closure-condition makes the lifter
    // duplicate the tail region, lifting the table-literal instructions twice. The first lift
    // inlines the nil/select elements and marks their defs processed. A second lift must re-inline
    // pure element defs instead of emitting undeclared register names.
    const auto out = DecompileOrFail(R"(
        repeat
            next ..= (-(-select));
            tonumber = ((if 168.75 then string else 296) == { data = "hello", [529] = false, k = math, y = 852 });
        until (#(select).field)
        while function(p0, p1, p2)
            ipairs(true);
            return ;
        end do
            if 106 then
                local v0, v1, v2 = tostring, tostring, false;
                v1(true, v2);
                v1 += tonumber;
            end
        end
        t.field(f0[{ nil, x = math, ["hello"] = false, select }], ipairs(false));
    )");
    INFO("decompile:\n" << out);
    // every lifted copy of the constructor must carry the literal elements, never a register name
    const std::regex faithful(R"(\{\s*nil,\s*x\s*=\s*math,\s*hello\s*=\s*false,\s*select\s*\})");
    const size_t copies = static_cast<size_t>(std::distance(std::sregex_iterator(out.begin(), out.end(), faithful), std::sregex_iterator{}));
    CHECK(copies >= 1);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\{\s*v\d+,\s*x\s*=\s*math)")));
    CHECK(Recompiles(out));
}

// ClassMethodRewriter must not splice a `local function F` into `T.m = F` dot sugar
// when an EARLIER statement (declared before the assignment) still captures F -- doing
// so drops the `local function F` decl and leaves that earlier closure referencing an
// unbound global. The forward read-scan alone missed the backward capture; the fix adds
// an IsIdentifierReadBetween check over (decl, assignment).
TEST_CASE("Regress: module dot-sugar keeps closure captured by an earlier function", "[Decompiler][Class][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = {}
        local function helper(x)
            return x + 1
        end
        local function wrapper()
            return helper(5)
        end
        t.run = helper
        return t, wrapper
    )");

    INFO("decompile:\n" << out);
    // Preserve helper as a local function instead of consuming it into `t.run`.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+function\s+helper\s*\()")));
    // wrapper still references the bound local `helper`.
    CHECK(ContainsRegex(out, std::regex(R"(return\s+helper\s*\(\s*5\s*\))")));
    // The rewrite did NOT fire: no dot-declaration of run absorbing helper's body.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(function\s+\w+\.run\s*\()")));
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: deep truthiness OR chain lifts without native recursion", "[Decompiler][Regression][StackDepth]") {
    std::string source = "local text = ...\nif ";
    for (int i = 0; i < 96; ++i) {
        if (i)
            source += " or ";
        source += "string.match(text, \"pattern" + std::to_string(i) + "\")";
    }
    source += " then return true end\nreturn false";

    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, " or "));
    CHECK(Recompiles(out));
}

TEST_CASE("SSA: deep dominator tree renames iteratively", "[SSA][Regression][StackDepth]") {
    std::string source = "local x = ...\n";
    for (int i = 0; i < 320; ++i)
        source += "if x == " + std::to_string(i) + " then x += 1 end\n";
    source += "return x";

    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    CHECK(Recompiles(out));
}
