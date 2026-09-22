//
// Created by Dottik on 21/9/2026.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "Luau/Compiler.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "Rewriters/ScopeAwareRenamer.hpp"
#include "Rewriters/ScopeBlockIntroducer.hpp"
#include "SourceGenerator/Generator.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Scope: redeclaration preserves an earlier captured binding", "[Decompiler][Scope][Regression]") {
    const auto LocalDecl = [](const std::string &name, const std::string &value) {
        return std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(value))
        );
    };
    const auto id = [](const std::string &name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    auto body = std::make_shared<BlockStatementNode>();
    body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{id("v0")}));
    auto closure =
        std::make_shared<FunctionDeclarationNode>("saved", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, body, true);
    closure->capturedNames.insert("v0");
    std::shared_ptr<Statement> capture = closure;
    bool captureInInitializer = false;
    bool renameCapture = false;
    bool recursiveBinding = false;
    SECTION("named closure") {}
    SECTION("renamed captured binding") { renameCapture = true; }
    SECTION("earlier local function captures its own binding") {
        closure->functionName = "v0";
        capture = LocalDecl("saved", "v0");
        recursiveBinding = true;
    }
    SECTION("inline closure") {
        closure->bAnonymousInline = true;
        closure->bIsLocalDeclaration = false;
        capture = std::make_shared<VariableDeclarationNode>(id("saved"), closure);
    }
    SECTION("closure inside a nested block") {
        closure->bAnonymousInline = true;
        closure->bIsLocalDeclaration = false;
        auto block = std::make_shared<BlockStatementNode>();
        block->bEmitAsDoBlock = true;
        block->body.push_back(std::make_shared<AssignmentStatementNode>(id("saved"), closure));
        capture = block;
    }
    SECTION("replacement initializer captures the previous binding") {
        closure->bAnonymousInline = true;
        closure->bIsLocalDeclaration = false;
        capture = LocalDecl("saved", "a");
        captureInInitializer = true;
    }
    auto call =
        std::make_shared<CallExpressionNode>(id("saved"), std::vector<std::shared_ptr<Expression>>{}, std::vector<std::shared_ptr<Expression>>{}, false, false);
    std::vector<std::shared_ptr<Statement>> stmts{
        LocalDecl("v0", "first"),  capture,
        LocalDecl("v1", "a"),      LocalDecl("v1", "b"),
        LocalDecl("v0", "second"), LocalDecl("v2", "a"),
        LocalDecl("v2", "b"),      LocalDecl("v3", "a"),
        LocalDecl("v3", "b"),      std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{call, id("v0")})
    };
    if (captureInInitializer) {
        stmts[4] = std::make_shared<VariableDeclarationNode>(id("v0"), closure);
        call->callee = id("v0");
        stmts.back() = std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{call, id("second")});
    }
    if (recursiveBinding) {
        stmts[0] = closure;
        stmts.back() = std::make_shared<ReturnStatementNode>(
            std::vector<std::shared_ptr<Expression>>{std::make_shared<BinaryExpressionNode>("==", call, id("saved")), id("v0")}
        );
    }
    if (renameCapture) {
        for (const auto &statement : stmts)
            ScopeAwareRenamer::RenameInStatement(statement, "v0", "value");
        CHECK(closure->capturedNames.contains("value"));
        CHECK_FALSE(closure->capturedNames.contains("v0"));
    }
    RootNode beforeRoot{stmts};
    SourceGenerator beforeGenerator{};
    const std::string before = beforeGenerator.GenerateSource(&beforeRoot);

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode afterRoot{stmts};
    SourceGenerator afterGenerator{};
    const std::string after = afterGenerator.GenerateSource(&afterRoot);
    INFO("rendered source:\n" << after);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(before), Luau::compile(after), {Luau::compile("first = 1 second = 2 a = 3 b = 4")});
    CHECK(verdict.original.trace == (recursiveBinding ? "return: true\t2\n" : "return: 1\t2\n"));
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}
