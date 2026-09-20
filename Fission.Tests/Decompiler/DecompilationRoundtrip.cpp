#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeclarationHoister.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/IfExpressionFolder.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
#include <string>
#include <unordered_set>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

static std::string DecompileOrFail(const std::string &source) {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);

    REQUIRE(result.resultCode == DecompileResult::Success);
    return std::move(result.decompilationOutput);
}

static bool ContainsRegex(const std::string &source, const std::regex &pattern) { return std::regex_search(source, pattern); }

static size_t CountRegex(const std::string &source, const std::regex &pattern) {
    return static_cast<size_t>(std::distance(std::sregex_iterator(source.begin(), source.end(), pattern), std::sregex_iterator{}));
}

TEST_CASE("Roundtrip: simple return", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return 42");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*return\s+42\s*$)")));
}

TEST_CASE("Roundtrip: while loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*v\d+\s*\+=\s*1[\s\S]*return\s+v\d+)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*\breturn\s+v\d+[\s\S]*(?:until|end))")));
}

TEST_CASE("Roundtrip: nested calls", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return function(a, b, c) return math.max(a, math.min(b, c)) end");
    INFO("decompile:\n" << out);
    // the inner multret call is the last argument of the outer call, so it inlines directly; the
    // faithful reconstruction keeps both nested and needs no argument spilled to a local. (An earlier
    // over-count in the B==0 arg estimator forced b/c into `local vN = argK` temporaries; the
    // producer-scan estimate no longer invents those.)
    CHECK(ContainsRegex(out, std::regex(R"(return\s+math\.max\(arg0,\s*math\.min\((?:arg1|v\d+),\s*(?:arg2|v\d+)\)\))")));
}

TEST_CASE("Roundtrip: table literal", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(return { 1, 2, 3, "hello" })");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+\{\s*1,\s*2,\s*3,\s*"hello"\s*\})")));
}

TEST_CASE("Roundtrip: function declaration", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local function add(a, b)
            return a + b
        end
        return add(1, 2)
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+function\s+add\([^)]*\)[\s\S]*return\s+[^\n]*\+[^\n]*[\s\S]*end)")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+add\(1,\s*2\))")));
}

TEST_CASE("Roundtrip: numeric for loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local s = 0
        for i = 1, 10 do
            s = s + i
        end
        return s
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*for\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*1,\s*10,\s*1\s+do)")));
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*v\d+\s*\+=\s*[A-Za-z_][A-Za-z_0-9]*)")));
}

TEST_CASE("Roundtrip: variable assignment with binary expression", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return function(a, b) return a + b end");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\s*\+\s*arg1)")));
}

TEST_CASE("Roundtrip: if statement", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local x = math.random()
        if x > 0.5 then
            return 1
        end
        return 0
    )");

    INFO("decompile:\n" << out);
    // The inverted branch keeps the exact `math.random() > 0.5` comparison negated with `not`, not
    // the algebraically-flipped `<=`. `>` lowers to `LT(0.5, x)`; flipping to `<=` would reverse the
    // operands and, on NaN or a raising compare, change the result/error. `not (a > b)` recompiles to
    // the same LT with an inverted branch, so operand order and any raised error survive the roundtrip.
    CHECK(ContainsRegex(out, std::regex(R"(if\s+not\s*\(\s*math\.random\(\)\s*>\s*0\.5\s*\)\s+then\s+return\s+0\s+else\s+return\s+1\s+end)")));
}

TEST_CASE("Roundtrip: repeat-until loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        repeat
            i = i + 1
        until i >= 10
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(repeat\s+v\d+\s*\+=\s*1\s+until\s*\(10\s*<=\s*v\d+\)\s+return\s+v\d+)")));
    CHECK(CountRegex(out, std::regex(R"((?:^|\n)\s*return\b)")) == 1u);
}

TEST_CASE("IfExpressionFolder: descends into inline function expressions", "[Decompiler][Rewriter]") {
    auto makeFunction = [] {
        auto body = std::make_shared<BlockStatementNode>();
        auto x = std::make_shared<Identifier>("x");
        body->body.push_back(std::make_shared<VariableDeclarationNode>(x));
        auto branch = std::make_shared<IfStatementNode>();
        branch->condition = std::make_shared<BooleanLiteralNode>(true);
        branch->thenBranch = std::make_shared<BlockStatementNode>();
        branch->elseBranch = std::make_shared<BlockStatementNode>();
        auto lhs = std::make_shared<IdentifierExpressionNode>(x);
        branch->thenBranch->body.push_back(std::make_shared<AssignmentStatementNode>(lhs, std::make_shared<NilLiteralNode>()));
        branch->elseBranch->body.push_back(
            std::make_shared<AssignmentStatementNode>(std::make_shared<IdentifierExpressionNode>(x), std::make_shared<BooleanLiteralNode>(false))
        );
        body->body.push_back(branch);
        return std::make_shared<FunctionDeclarationNode>("", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, body, false);
    };

    auto localFn = makeFunction();
    std::vector<std::shared_ptr<Statement>> localStatements{
        std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("f")), localFn)
    };
    IfExpressionFolder{}.Run(localStatements);
    auto folded = std::dynamic_pointer_cast<IfExpressionNode>(std::dynamic_pointer_cast<VariableDeclarationNode>(localFn->lpFunctionBody->body.front())->value);
    REQUIRE(folded);
    CHECK(std::dynamic_pointer_cast<NilLiteralNode>(folded->thenExpr));
    CHECK(std::dynamic_pointer_cast<BooleanLiteralNode>(folded->elseExpr)->value == false);

    auto callback = makeFunction();
    auto call = std::make_shared<CallExpressionNode>(
        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("run")), std::vector<std::shared_ptr<Expression>>{callback},
        std::vector<std::shared_ptr<Expression>>{}, false, false
    );
    std::vector<std::shared_ptr<Statement>> callStatements{std::make_shared<ExpressionStatementNode>(call)};
    IfExpressionFolder{}.Run(callStatements);
    CHECK(std::dynamic_pointer_cast<IfExpressionNode>(std::dynamic_pointer_cast<VariableDeclarationNode>(callback->lpFunctionBody->body.front())->value));

    auto returned = makeFunction();
    std::vector<std::shared_ptr<Statement>> returnStatements{std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{returned})};
    IfExpressionFolder{}.Run(returnStatements);
    CHECK(std::dynamic_pointer_cast<IfExpressionNode>(std::dynamic_pointer_cast<VariableDeclarationNode>(returned->lpFunctionBody->body.front())->value));

    auto indexed = makeFunction();
    auto index = std::make_shared<IndexExpressionNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("t")), indexed);
    std::vector<std::shared_ptr<Statement>> indexStatements{std::make_shared<AssignmentStatementNode>(index, std::make_shared<NilLiteralNode>())};
    IfExpressionFolder{}.Run(indexStatements);
    CHECK(std::dynamic_pointer_cast<IfExpressionNode>(std::dynamic_pointer_cast<VariableDeclarationNode>(indexed->lpFunctionBody->body.front())->value));
}

TEST_CASE("Type inference: function annotations use valid Luau syntax", "[Decompiler][TypeInference]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode("local f = string.gmatch('abc', '.'); return f, f", DecompilerFlags::InferRobloxTypes);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK(result.decompilationOutput.find("(...any) -> ...any") != std::string::npos);
    CHECK(result.decompilationOutput.find(": function") == std::string::npos);
    const auto bytecode = Luau::compile(result.decompilationOutput);
    REQUIRE(!bytecode.empty());
    CHECK(bytecode.front() != '\0');
}

TEST_CASE("Deserializer: function bytecode types emit valid annotations", "[Decompiler][Deserializer]") {
    CHECK(Deserializer::GetBytecodeTypeName(LBC_TYPE_FUNCTION) == "(...any) -> ...any");
    CHECK(Deserializer::GetBytecodeTypeName(LBC_TYPE_FUNCTION | LBC_TYPE_OPTIONAL_BIT) == "((...any) -> ...any)?");
    const auto bytecode =
        Luau::compile("return function(callback: " + Deserializer::GetBytecodeTypeName(LBC_TYPE_FUNCTION | LBC_TYPE_OPTIONAL_BIT) + ") return callback end");
    REQUIRE(!bytecode.empty());
    CHECK(bytecode.front() != '\0');
}

TEST_CASE("DeclarationHoister: coalesces only safe adjacent assignments", "[Decompiler][Rewriter]") {
    auto id = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    auto run = [](std::vector<std::shared_ptr<Statement>> statements) {
        DeclarationHoister{}.Run(statements);
        return statements;
    };
    {
        auto x = id("v40");
        auto s = run(
            {std::make_shared<VariableDeclarationNode>(x, nullptr),
             std::make_shared<AssignmentStatementNode>(id("v40"), std::make_shared<NumberLiteralNode>(1))}
        );
        auto d = std::dynamic_pointer_cast<VariableDeclarationNode>(s.front());
        REQUIRE(d);
        CHECK(d->value);
        CHECK(s.size() == 1);
    }
    {
        auto s = run({std::make_shared<VariableDeclarationNode>(id("v40"), nullptr), std::make_shared<AssignmentStatementNode>(id("v40"), id("v40"))});
        CHECK(s.size() == 2);
    }
    {
        auto fnBody = std::make_shared<BlockStatementNode>();
        fnBody->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{id("v40")}));
        auto fn =
            std::make_shared<FunctionDeclarationNode>("", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, fnBody, false);
        auto s = run({std::make_shared<VariableDeclarationNode>(id("v40"), nullptr), std::make_shared<AssignmentStatementNode>(id("v40"), fn)});
        CHECK(s.size() == 2);
    }
    {
        auto s = run(
            {std::make_shared<VariableDeclarationNode>(id("v40"), nullptr), std::make_shared<ExpressionStatementNode>(std::make_shared<NilLiteralNode>()),
             std::make_shared<AssignmentStatementNode>(id("v40"), std::make_shared<NumberLiteralNode>(1))}
        );
        CHECK(s.size() == 3);
    }
    {
        auto loop = std::make_shared<WhileStatementNode>();
        loop->body = std::make_shared<BlockStatementNode>();
        loop->body->body = {
            std::make_shared<VariableDeclarationNode>(id("v40"), nullptr),
            std::make_shared<AssignmentStatementNode>(id("v40"), std::make_shared<NumberLiteralNode>(1))
        };
        std::vector<std::shared_ptr<Statement>> s{loop};
        DeclarationHoister{}.Run(s);
        auto d = std::dynamic_pointer_cast<VariableDeclarationNode>(loop->body->body.front());
        REQUIRE(d);
        CHECK(d->value);
    }
    {
        auto value = std::make_shared<IfExpressionNode>(std::make_shared<BooleanLiteralNode>(true), id("v40"), std::make_shared<NumberLiteralNode>(1));
        auto s = run({std::make_shared<VariableDeclarationNode>(id("v40"), nullptr), std::make_shared<AssignmentStatementNode>(id("v40"), value)});
        CHECK(s.size() == 2);
    }
}

TEST_CASE("DeclarationHoister: hoists branch writes with suffixed register names", "[Decompiler][Rewriter]") {
    auto id = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };

    auto branch = std::make_shared<IfStatementNode>();
    branch->condition = std::make_shared<BooleanLiteralNode>(true);
    branch->thenBranch = std::make_shared<BlockStatementNode>();
    branch->elseBranch = std::make_shared<BlockStatementNode>();
    branch->thenBranch->body.push_back(std::make_shared<VariableDeclarationNode>(id("v11"), std::make_shared<NumberLiteralNode>(1)));
    branch->thenBranch->body.push_back(std::make_shared<VariableDeclarationNode>(id("v12_62"), std::make_shared<NumberLiteralNode>(1)));
    branch->elseBranch->body.push_back(std::make_shared<AssignmentStatementNode>(id("v11"), std::make_shared<NumberLiteralNode>(2)));
    branch->elseBranch->body.push_back(std::make_shared<VariableDeclarationNode>(id("v12_62"), std::make_shared<NumberLiteralNode>(2)));
    std::vector<std::shared_ptr<Statement>> statements{
        std::make_shared<VariableDeclarationNode>(id("v7_62"), std::make_shared<NumberLiteralNode>(0)), branch,
        std::make_shared<AssignmentStatementNode>(id("v7_62"), std::make_shared<NumberLiteralNode>(3)),
        std::make_shared<AssignmentStatementNode>(id("v12_62"), std::make_shared<NumberLiteralNode>(3)),
        std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{id("v11"), id("v7_62"), id("v12_62")})
    };

    DeclarationHoister{}.Run(statements);

    std::unordered_set<std::string> rootDeclarations;
    for (const auto &stmt : statements)
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            if (auto name = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); name && name->identifier)
                rootDeclarations.insert(name->identifier->name);
    CHECK(rootDeclarations.contains("v11"));
    CHECK(rootDeclarations.contains("v7_62"));
    CHECK(rootDeclarations.contains("v12_62"));
    CHECK(std::dynamic_pointer_cast<AssignmentStatementNode>(branch->thenBranch->body.front()));
}

TEST_CASE("IfExpressionFolder: preserves initializer scope and prefers positive conditions", "[Decompiler][Rewriter]") {
    auto id = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    for (bool readsSelf : {false, true}) {
        auto condition = id("flag");
        auto branch = std::make_shared<IfStatementNode>();
        branch->condition = std::make_shared<UnaryExpressionNode>("not ", condition);
        branch->thenBranch = std::make_shared<BlockStatementNode>();
        branch->elseBranch = std::make_shared<BlockStatementNode>();
        auto thenValue = readsSelf ? std::static_pointer_cast<Expression>(id("value")) : std::make_shared<NilLiteralNode>();
        auto elseValue = std::make_shared<BooleanLiteralNode>(false);
        branch->thenBranch->body.push_back(std::make_shared<AssignmentStatementNode>(id("value"), thenValue));
        branch->elseBranch->body.push_back(std::make_shared<AssignmentStatementNode>(id("value"), elseValue));
        auto declaration = std::make_shared<VariableDeclarationNode>(id("value"), nullptr);
        std::vector<std::shared_ptr<Statement>> statements{declaration, branch};
        IfExpressionFolder{}.Run(statements);
        CHECK(statements.size() == (readsSelf ? 2 : 1));
        auto value = readsSelf ? std::dynamic_pointer_cast<AssignmentStatementNode>(statements.back())->right
                               : std::dynamic_pointer_cast<VariableDeclarationNode>(statements.front())->value;
        auto folded = std::dynamic_pointer_cast<IfExpressionNode>(value);
        REQUIRE(folded);
        CHECK(folded->condition == condition);
        CHECK(folded->thenExpr == elseValue);
        CHECK(folded->elseExpr == thenValue);
    }
}

TEST_CASE("ASTRewriter: reaches closures under indexed assignment targets", "[Decompiler][Rewriter]") {
    auto condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("flag"));
    auto branch = std::make_shared<IfStatementNode>();
    branch->condition = std::make_shared<UnaryExpressionNode>("not ", condition);
    branch->thenBranch = std::make_shared<BlockStatementNode>();
    branch->elseBranch = std::make_shared<BlockStatementNode>();
    branch->elseBranch->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto body = std::make_shared<BlockStatementNode>();
    body->body.push_back(branch);
    auto fn = std::make_shared<FunctionDeclarationNode>("", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, body, false);
    auto call = std::make_shared<CallExpressionNode>(fn, std::vector<std::shared_ptr<Expression>>{}, std::vector<std::shared_ptr<Expression>>{}, false, true);
    auto lhs = std::make_shared<IndexExpressionNode>(call, std::make_shared<StringLiteralNode>("key"));
    auto block = std::make_shared<BlockStatementNode>();
    block->body.push_back(std::make_shared<AssignmentStatementNode>(lhs, std::make_shared<NilLiteralNode>()));
    std::vector<std::shared_ptr<Statement>> statements{block};
    IfChainSimplifier{}.Run(statements);
    CHECK(branch->condition == condition);
    CHECK_FALSE(branch->elseBranch);
    REQUIRE(branch->thenBranch);
    CHECK(branch->thenBranch->body.size() == 1);
}
