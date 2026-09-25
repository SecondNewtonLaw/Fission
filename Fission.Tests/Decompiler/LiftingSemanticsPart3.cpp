//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "LiftingSemanticsTestSupport.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeclarationHoister.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/LoopVariableRenamer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace lifting_semantics_test;

TEST_CASE("Lift: numeric `for` emits a proper loop-variable identifier", "[Decompiler][NumericFor]") {
    // Regression for `for 1 = 1, n, 1 do ... end` and `for err_not_reg = ...`.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local s = 0
            for i = 1, n do
                s = s + i
            end
            return s
        end
    )");

    INFO("decompile:\n" << out);
    REQUIRE(Contains(out, "for "));
    CHECK_FALSE(Contains(out, "for 1 ="));
    CHECK_FALSE(Contains(out, "err_not_reg"));

    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // Identifier regex already constrains the first char; the assertion is
    // structural sanity.
    CHECK(std::isalpha(static_cast<unsigned char>(varName.front())) != 0);
}

TEST_CASE("Lift: numeric `for` body re-uses the same loop-variable name as the header", "[Decompiler][NumericFor]") {
    // Prevent the regression where the header rendered as `for 1 = ...` while
    // the body referenced the loop var as `i_3`. Index a table by the loop
    // variable so the body must reference it explicitly.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local t = {}
            for i = 1, n do
                t[i] = i
            end
            return t
        end
    )");

    INFO("decompile:\n" << out);
    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // The loop-var token should appear at least once more (inside the body).
    CHECK(CountOccurrences(out, varName) >= 2);
}

// Generic for loops

TEST_CASE("Lift: generic for over `pairs` emits `for k, v in pairs(t)`", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for k, v in pairs(t) do
                print(k, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "pairs("));
}

TEST_CASE("Lift: generic for over `ipairs` survives lift", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for i, v in ipairs(t) do
                print(i, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "ipairs("));
}

// Tables
// Dict-style key-value table literal reconstruction remains a known
// limitation (SETTABLEKS after NEWTABLE may be fragmented by SSA/CFG).
// List-style (SETLIST) reconstruction works reliably.

TEST_CASE("Lift: empty table literal has balanced braces", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{  }` with spaces; check brace balance.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: list-style table literal preserves numeric elements", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {10, 20, 30}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{ 10, 20, 30 }` with space after `{`.
    CHECK(CountOccurrences(out, "10") >= 1);
    CHECK(CountOccurrences(out, "20") >= 1);
    CHECK(CountOccurrences(out, "30") >= 1);
    // All three elements survive.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    // No key-value syntax for plain list elements.
    CHECK_FALSE(Contains(out, "[1]"));
}

TEST_CASE("Lift: nested table literal preserves inner list tables", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {{1, 2}, {3, 4}}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // All four elements survive.
    CHECK(CountOccurrences(out, "1") >= 1);
    CHECK(CountOccurrences(out, "2") >= 1);
    CHECK(CountOccurrences(out, "3") >= 1);
    CHECK(CountOccurrences(out, "4") >= 1);
    // At least two brace pairs (outer + inner).
    CHECK(CountOccurrences(out, "{") >= 3);
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: absurd mixed SETLIST table literal terminates and preserves edge elements", "[Decompiler][Table][SETLIST][Stress]") {
    std::stringstream source;
    source << "return function(a, b)\nlocal t = {";
    for (int i = 1; i <= 260; ++i) {
        if (i > 1)
            source << ", ";
        switch (i % 10) {
        case 0:
            source << "{ " << i << ", key = \"v" << i << "\", nested = {" << (i + 1) << ", " << (i + 2) << "} }";
            break;
        case 1:
            source << "a + " << i;
            break;
        case 2:
            source << "b and \"s" << i << "\" or nil";
            break;
        case 3:
            source << "function(x) return x + " << i << " end";
            break;
        case 4:
            source << "{ [\"dyn" << i << "\"] = a, " << i << " }";
            break;
        case 5:
            source << "not b";
            break;
        case 6:
            source << "(a * " << i << ") % 7";
            break;
        case 7:
            source << "\"edge" << i << "\"";
            break;
        case 8:
            source << "nil";
            break;
        default:
            source << i;
            break;
        }
    }
    source << ", absurdKey = { tail = 9999, flag = true }, [a] = b }\nreturn t\nend";

    const auto out = DecompileOrFail(source.str());

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "260"));
    CHECK(Contains(out, "9999"));
    CHECK(Contains(out, "function"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\.)")));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\[)")));
}

TEST_CASE("Lift: table literal as return value preserves content", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            return {1, 2, 3}
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return {"));
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    CHECK(Contains(out, "3"));
}

TEST_CASE("Lift: table literal with expression elements preserves operators", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            local t = {a + 1, b * 2, a - b}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "+"));
    CHECK(Contains(out, "*"));
    CHECK(Contains(out, "-"));
    CHECK(Contains(out, "a"));
    CHECK(Contains(out, "b"));
}

TEST_CASE("Lift: table literal preserves aliases across later register reuse", "[Decompiler][Table][Alias]") {
    const auto out = DecompileOrFail(R"(
        local nested = {1}
        print(nested)
        local g = rat()
        local t = {nested}
        return g, t
    )");

    INFO("decompile:\n" << out);
    static const std::regex ratThenSelfTable(R"(local\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s*rat\(\)\s+local\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*\{\s*\1\s*\})");
    CHECK_FALSE(std::regex_search(out, ratThenSelfTable));
    CHECK(Contains(out, "rat()"));
    CHECK(Contains(out, "{ 1 }"));
}

TEST_CASE("Lift: DUPTABLE nil template values do not index constants out of range", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return { a = nil }
    )");

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(return\s+\{\s*a\s*=\s*nil\s*\})")));
}

TEST_CASE("Lift: dict-style table keys are preserved", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {x = 1, y = 2, z = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "x = 1"));
    CHECK(Contains(out, "y = 2"));
    CHECK(Contains(out, "z = 3"));
}

TEST_CASE("Lift: mixed list/dict table literal preserves both elements and keys", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {1, 2, key = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    CHECK(Contains(out, "key = 3"));
}

TEST_CASE("Lift: AssignmentStatementNode RHS not gratuitously parenthesised", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local x = 5
            x = x + 1
            return x
        end
    )");

    INFO("decompile:\n" << out);
    // The previous always-wrap behaviour emitted `x = (x + 1)`. The
    // precedence-aware pass must drop the redundant outer parens.
    CHECK_FALSE(Contains(out, "x = (x + 1)"));
}

TEST_CASE("Lift: parallel swap preserves saved value", "[Decompiler][Assignment]") {
    const std::string pairSwap = R"(
        local function pair(x)
            return x, x + 1
        end
        local first, second = pair(5)
        first, second = second, first
        return first, second
    )";
    const auto checkBehavior = [](const std::string &source, const std::string &expected) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string originalBc = Luau::compile(source, opts);
        const std::string decompiledBc = Luau::compile(DecompileOrFail(source), opts);
        const std::string emptyPrelude = Luau::compile("", opts);
        const auto original = fuzz::RunLuauTrace(originalBc, emptyPrelude);
        const auto decompiled = fuzz::RunLuauTrace(decompiledBc, emptyPrelude);
        CHECK(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(decompiled.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == expected);
        CHECK(decompiled.trace == original.trace);
    };
    checkBehavior(pairSwap, "return: 6\t5\n");
    checkBehavior("local first, second = 5, 6\nfirst, second = second, first\nreturn first, second\n", "return: 6\t5\n");
}

TEST_CASE("Lift: generated-name shadowing preserves global bindings", "[Decompiler][DeclarationHoister][Semantic]") {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const auto check = [&](const std::string &source, const std::string &preludeSource, const std::string &localName, const std::string &globalName,
                           const std::string &expected) {
        const auto output = DecompileOrFail(source, opts.optimizationLevel, static_cast<DecompilerFlags>(0), 1);
        INFO("decompiled output:\n" << output);
        CHECK(std::regex_search(output, std::regex("local\\s+" + localName + R"(\b)")));
        CHECK_FALSE(std::regex_search(output, std::regex("local\\s+" + globalName + R"(\b)")));

        const auto prelude = Luau::compile(preludeSource, opts);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, opts), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(output, opts), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == expected);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };

    check("local x if flag then x = 1 else x = 2 end sink = x return v0, x", "v0 = 9 flag = true", "_v0", "v0", "return: 9\t1\n");
    check(
        "local outer = { keep } local function f() local x if flag then x = 1 else x = 2 end sink = x return v0_0, x end "
        "sinkOuter = outer local a, b = f() return a, b, outer[1]",
        "v0_0 = 9 flag = true keep = 77", "_v0_0", "v0_0", "return: 9\t1\t77\n"
    );
}

TEST_CASE("Lift: loop variable naming preserves outer header bindings", "[Decompiler][LoopBinding]") {
    const auto identifier = [](const std::string &name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    for (const bool numeric : {false, true}) {
        const std::string originalName = numeric ? "i_4" : "v4";
        auto variable = identifier(originalName);
        auto header = identifier(originalName);
        auto bodyUse = identifier(originalName);
        auto body = std::make_shared<BlockStatementNode>();
        body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{bodyUse}));
        std::vector<std::shared_ptr<Statement>> statements;
        if (numeric) {
            auto loop = std::make_shared<ForNumericNode>();
            loop->loopVariable = variable;
            loop->startVariable = header;
            loop->maxIncreased = identifier(originalName);
            loop->increaseBy = identifier(originalName);
            loop->lpLoopBody = body;
            statements.push_back(loop);
        } else {
            auto loop = std::make_shared<ForGeneralNode>();
            loop->loopVariables = {variable};
            loop->generator = header;
            loop->state = identifier(originalName);
            loop->index = identifier(originalName);
            loop->body = body;
            statements.push_back(loop);
        }
        LoopVariableRenamer{}.Run(statements);
        CHECK(header->identifier->name == originalName);
        CHECK(variable->identifier->name != originalName);
        CHECK(bodyUse->identifier->name == variable->identifier->name);
    }
}

TEST_CASE("Lift: captured anonymous closure keeps one binding", "[Decompiler][ClosureBinding]") {
    const auto check = [](const std::string &source) {
        const auto out = DecompileOrFail(source);
        INFO(out);
        const auto prelude = Luau::compile("", Luau::CompileOptions{1, 2});
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, Luau::CompileOptions{1, 2}), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(out, Luau::CompileOptions{1, 2}), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: 5\t9\n");
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };
    check(R"(
        local v0 = (function(x) return x + 1 end)
        local before = v0(4)
        local function later() return v0(8) end
        return before, later()
    )");
    check(R"(
        local offset = tonumber('1')
        local v0 = (function(x) return x + offset end)
        local before = v0(4)
        local function later() return v0(8) end
        return before, later()
    )");
}

TEST_CASE("Lift: reassigned captured closure keeps original binding", "[Decompiler][ClosureBinding][Regression]") {
    const std::string source = R"(
        local function f() return 1 end
        local function read() return f end
        f = function() return 2 end
        return read()()
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    CheckSameTrace(source, out);
}

TEST_CASE("Lift: `if` condition has no redundant outer parentheses around a bare comparison", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            if a == b then return 1 end
            return 0
        end
    )");

    INFO("decompile:\n" << out);
    // `if (a == b) then` form must not appear; the comparison is the entire
    // condition and needs no extra grouping.
    CHECK_FALSE(Contains(out, "if (a == b)"));
}

TEST_CASE("Lift: associative `or` chain has no redundant inner parentheses", "[Decompiler][Parens]") {
    // Same-operator chains under associative ops do not need inner parens.
    // Function-arg names are not yet propagated by the lifter (locals render
    // as `argN`), so the test matches the chain in a name-agnostic way.
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, sink)
            sink(a or b or c)
        end
    )");

    INFO("decompile:\n" << out);
    // The literal `(X or Y) or Z` form (grouping that the associativity-aware
    // emitter is supposed to drop) must not appear.
    static const std::regex leftAssocGrouping(R"(\([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\) or )");
    static const std::regex rightAssocGrouping(R"( or \([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\))");
    CHECK_FALSE(std::regex_search(out, leftAssocGrouping));
    CHECK_FALSE(std::regex_search(out, rightAssocGrouping));
    // Output should contain at least one `X or Y` pair.
    static const std::regex flatChain(R"([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*)");
    CHECK(std::regex_search(out, flatChain));
}

TEST_CASE("Lift: empty else block is omitted", "[Decompiler][Readability]") {
    const auto out = DecompileOrFail(R"(
        return function(value)
            if value then
                print(value)
            else
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(std::regex_search(out, std::regex(R"(else\s*\n\s*end)")));
    CHECK(Contains(out, "if "));
    CHECK(Contains(out, "print("));
}

TEST_CASE("IfChainSimplifier removes only empty else blocks", "[Decompiler][Readability]") {
    auto condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("value"));
    auto thenBranch = std::make_shared<BlockStatementNode>();
    thenBranch->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto emptyElse = std::make_shared<BlockStatementNode>();
    auto emptyIf = std::make_shared<IfStatementNode>();
    emptyIf->condition = condition;
    emptyIf->thenBranch = thenBranch;
    emptyIf->elseBranch = emptyElse;
    std::vector<std::shared_ptr<Statement>> statements{emptyIf};
    IfChainSimplifier{}.Run(statements);
    CHECK(emptyIf->elseBranch == nullptr);
    CHECK(emptyIf->condition == condition);
    CHECK(emptyIf->thenBranch == thenBranch);

    auto nonemptyElse = std::make_shared<BlockStatementNode>();
    nonemptyElse->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto keptIf = std::make_shared<IfStatementNode>();
    keptIf->condition = condition;
    keptIf->thenBranch = thenBranch;
    keptIf->elseBranch = nonemptyElse;
    statements = {keptIf};
    IfChainSimplifier{}.Run(statements);
    CHECK(keptIf->elseBranch == nonemptyElse);
}

TEST_CASE("IfChainSimplifier inverts empty then without rewriting relational condition", "[Decompiler][Readability]") {
    auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("value"));
    auto right = std::make_shared<NumberLiteralNode>(0.0);
    auto condition = std::make_shared<BinaryExpressionNode>("<", left, right);
    auto emptyThen = std::make_shared<BlockStatementNode>();
    auto elseBranch = std::make_shared<BlockStatementNode>();
    elseBranch->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto ifNode = std::make_shared<IfStatementNode>();
    ifNode->condition = condition;
    ifNode->thenBranch = emptyThen;
    ifNode->elseBranch = elseBranch;
    std::vector<std::shared_ptr<Statement>> statements{ifNode};
    IfChainSimplifier{}.Run(statements);
    auto inverted = std::dynamic_pointer_cast<UnaryExpressionNode>(ifNode->condition);
    REQUIRE(inverted);
    CHECK(inverted->op == "not ");
    CHECK(inverted->operand == condition);
    CHECK(ifNode->thenBranch == elseBranch);
    CHECK(ifNode->elseBranch == nullptr);
}

TEST_CASE("IfChainSimplifier merges identical elseif arms and keeps trailing else", "[Decompiler][Readability]") {
    auto makeBody = [] {
        auto body = std::make_shared<BlockStatementNode>();
        body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
        return body;
    };
    auto outer = std::make_shared<IfStatementNode>();
    outer->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("a"));
    outer->thenBranch = makeBody();
    auto inner = std::make_shared<IfStatementNode>();
    inner->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("b"));
    inner->thenBranch = makeBody();
    inner->elseBranch = std::make_shared<BlockStatementNode>();
    outer->elseBranch = std::make_shared<BlockStatementNode>();
    outer->elseBranch->body.push_back(inner);
    std::vector<std::shared_ptr<Statement>> statements{outer};
    IfChainSimplifier{}.Run(statements);
    auto condition = std::dynamic_pointer_cast<BinaryExpressionNode>(outer->condition);
    REQUIRE(condition);
    CHECK(condition->op == "or");
    CHECK(std::dynamic_pointer_cast<IdentifierExpressionNode>(condition->left)->identifier->name == "a");
    CHECK(std::dynamic_pointer_cast<IdentifierExpressionNode>(condition->right)->identifier->name == "b");
    CHECK(outer->elseBranch == inner->elseBranch);
}

TEST_CASE("Lift: FastWait keeps short-circuit duration separate from timer", "[Decompiler][ShortCircuit][Regression]") {
    std::ifstream fixture{FISSION_SOURCE_DIR "/Samples/EncodedRoblox/FAST_WAIT_BRANCH.txt", std::ios::binary};
    REQUIRE(fixture);
    std::stringstream encoded;
    encoded << fixture.rdbuf();

    Decompiler decompiler{};
    const auto flags = DecompilerFlags::OptimizeIR | DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
    const auto result = decompiler.DecompileRobloxBytecode(DecodeBase64(encoded.str()), flags);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompile:\n" << result.decompilationOutput);
    INFO("IR:\n" << result.irOutput);

    const auto functionStart = result.decompilationOutput.find("local function fastWait");
    REQUIRE(functionStart != std::string::npos);
    const auto function = result.decompilationOutput.substr(functionStart);
    std::smatch duration;
    std::smatch timer;
    REQUIRE(std::regex_search(function, duration, std::regex(R"(local\s+(\w+): number = tonumber\(arg0\))")));
    REQUIRE(std::regex_search(function, timer, std::regex(R"(local\s+(\w+) = tick\(\))")));
    CHECK(Contains(function, std::string(" = ") + timer[1].str() + " + (" + duration[1].str() + " or 0.03333333333333333)"));
    CHECK(Contains(function, std::string("coroutine.yield() - ") + timer[1].str()));
    CHECK(Contains(function, ": thread = coroutine.running()"));
    CHECK_FALSE(Contains(function, "v2 + (v2 or"));
    CHECK_FALSE(Contains(function, "\n    do\n"));
    CHECK_FALSE(Contains(result.decompilationOutput, ": table"));
}

TEST_CASE("Lift: ItemSpawn successful guards continue to later checks", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    const auto tickStart = out.find("local serverTimeNow");
    const auto tickEnd = out.find("Triggered:Connect", tickStart);
    REQUIRE(tickStart != std::string::npos);
    REQUIRE(tickEnd != std::string::npos);
    const auto tick = out.substr(tickStart, tickEnd - tickStart);

    const auto weatherIndent = LineIndentContaining(tick, "RequiredWeather");
    const auto oreIndent = LineIndentContaining(tick, "RequiredOre");
    const auto eventIndent = LineIndentContaining(tick, "RequiredEvent");
    const auto limitIndent = LineIndentContaining(tick, "LimitedAmount");
    REQUIRE(weatherIndent != std::string::npos);
    REQUIRE(oreIndent != std::string::npos);
    REQUIRE(eventIndent != std::string::npos);
    REQUIRE(limitIndent != std::string::npos);
    CHECK_FALSE(std::regex_search(tick, std::regex(R"(\belseif[^\n]*RequiredOre)")));
    CHECK(oreIndent == weatherIndent);
    CHECK(eventIndent == weatherIndent);
    CHECK(limitIndent == weatherIndent);
}

TEST_CASE("Lift: ItemSpawn active weather still invokes collection", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    const auto promptStart = out.find("Triggered:Connect");
    const auto promptEnd = out.find("SetState(false, true)", promptStart);
    REQUIRE(promptStart != std::string::npos);
    REQUIRE(promptEnd != std::string::npos);
    const auto prompt = out.substr(promptStart, promptEnd - promptStart);

    const auto weatherIndent = LineIndentContaining(prompt, "RequiredWeather");
    const auto invokeIndent = LineIndentContaining(prompt, "InvokeServer");
    REQUIRE(weatherIndent != std::string::npos);
    REQUIRE(invokeIndent != std::string::npos);
    CHECK(invokeIndent == weatherIndent);
}

TEST_CASE("Lift: ItemSpawn method closures remain local", "[Decompiler][Closure][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    CHECK_FALSE(std::regex_search(out, std::regex(R"((^|\n)(Construct|Start|Stop)\s*=\s*function)")));
}

TEST_CASE("Lift: rename comments only describe surviving suffixed locals", "[Decompiler][AutoName][Regression]") {
    std::ifstream fixture{FISSION_SOURCE_DIR "/Samples/EncodedRoblox/MODULE_LOADER_STALE_SUFFIX.txt", std::ios::binary};
    REQUIRE(fixture);
    std::stringstream encoded;
    encoded << fixture.rdbuf();

    Decompiler decompiler{};
    const auto flags = DecompilerFlags::OptimizeIR | DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
    const auto result = decompiler.DecompileRobloxBytecode(DecodeBase64(encoded.str()), flags);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompile:\n" << result.decompilationOutput);

    CHECK(Contains(result.decompilationOutput, "local ok, result = pcall(require, v)"));
    CHECK_FALSE(Contains(result.decompilationOutput, "has been suffixed to avoid shadowing"));
}

TEST_CASE("Lift: nested auto names do not shadow visible locals", "[Decompiler][AutoName][Regression]") {
    const std::string source = R"(
        local v0 = {}
        local v1 = {}
        local v2 = {}
        function v0.start(arg0)
            local screen = nil
            local enabled
            for outerKey, outerValue in pairs(arg0) do
                if screen == nil then
                    screen = outerValue
                end
                for innerKey, innerValue in pairs(outerValue) do
                    v1[innerKey] = innerValue
                end
            end
            table.sort({}, function(left, right)
                local temporary = left + 1
                return temporary < right
            end)
            enabled = false
            local callback = function()
                return enabled
            end
            return callback, screen
        end
        return v0, v2
    )";
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 0;
    opts.debugLevel = 1;
    const auto out = DecompileVanillaOrFail(Luau::compile(source, opts));
    INFO("decompile:\n" << out);

    const auto start = out.find("function v0.start");
    REQUIRE(start != std::string::npos);
    const auto function = out.substr(start);
    CHECK_FALSE(std::regex_search(function, std::regex(R"(\blocal\s+v2\b)")));
    CHECK_FALSE(std::regex_search(function, std::regex(R"(for\s+\w+\s*,\s*v2\s+in)")));
    CHECK(CompilesOk(out));

    AnalyzedFunction names{};
    names.nameSuffix = "_4";
    names.enclosingNames = {"v2", "v2_4"};
    CHECK(names.DisambiguateOwnName("v2") == "v2_4_2");
    CHECK(names.DisambiguateOwnName("v2") == "v2_4_2");
}
