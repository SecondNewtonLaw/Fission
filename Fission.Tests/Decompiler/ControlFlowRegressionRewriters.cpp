//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

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

using namespace control_flow_test;

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

TEST_CASE("Dead local analysis refreshes names between runs", "[Decompiler][Rewriter][Regression]") {
    auto id = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    auto declaration = std::make_shared<VariableDeclarationNode>(id("value"), id("source"));
    auto result = std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{id("value")});
    std::vector<std::shared_ptr<Statement>> statements{declaration, result};
    DeadLocalEliminator eliminator;
    eliminator.Run(statements);
    REQUIRE(statements.size() == 2);
    result->returnValues.front() = id("other");
    eliminator.Run(statements);
    CHECK(statements.size() == 1);
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
    CHECK(Contains(out, "not parsed and dates[1] or parsed and dates[2]"));
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
    CHECK(CountOccurrences(out, "value = selected") == 1);
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

// Phi-merged call result must not shadow the merge variable with `local`
// Known bug: a value defined in both branches of an if/else, where one branch
// is a method-call result, emitted `local v = obj:Method()` inside the branch
// (shadowing the hoisted merge variable) so the post-merge read saw the wrong
// value. The call-result branch must use a bare assignment.
