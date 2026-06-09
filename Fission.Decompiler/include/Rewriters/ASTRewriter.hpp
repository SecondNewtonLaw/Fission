//
// Created by Dottik on 2/6/2026.
//
// Base for the structural AST post-passes (the visitor pattern can't restructure a
// parent's statement vector, so rewriters take the owning vector directly).
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <memory>
#include <vector>

class ASTRewriter {
  public:
    virtual ~ASTRewriter() = default;

    void Run(std::vector<std::shared_ptr<Statement>> &statements) { RewriteBlock(statements); }

  protected:
    // subclass hook. called post-order, so nested blocks are already rewritten.
    virtual void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) = 0;

    void RewriteBlock(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (auto &stmt : stmts) {
            if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
                if (ifS->thenBranch)
                    RewriteBlock(ifS->thenBranch->body);
                if (ifS->elseBranch)
                    RewriteBlock(ifS->elseBranch->body);
            } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->lpFunctionBody) {
                RewriteBlock(fn->lpFunctionBody->body);
            } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt); w && w->body) {
                RewriteBlock(w->body->body);
            } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt); r && r->body) {
                RewriteBlock(r->body->body);
            } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt); fnum && fnum->lpLoopBody) {
                RewriteBlock(fnum->lpLoopBody->body);
            } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt); fgen && fgen->body) {
                RewriteBlock(fgen->body->body);
            } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
                RewriteExpression(asn->right);
            } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
                RewriteExpression(decl->value);
            } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
                RewriteExpression(es->expression);
            } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
                for (auto &v : ret->returnValues)
                    RewriteExpression(v);
            }
        }
        RewriteStatements(stmts);
    }

    // descend into function bodies reachable through expressions (closures, methods, inline call-arg closures).
    void RewriteExpression(const std::shared_ptr<Expression> &expr) {
        if (!expr)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr); fn && fn->lpFunctionBody) {
            RewriteBlock(fn->lpFunctionBody->body);
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            RewriteExpression(call->callee);
            for (auto &a : call->arguments)
                RewriteExpression(a);
        } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            RewriteExpression(nameCall->calledOn);
            for (auto &a : nameCall->arguments)
                RewriteExpression(a);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            RewriteExpression(bin->left);
            RewriteExpression(bin->right);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            RewriteExpression(un->operand);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &e : tbl->expressions)
                RewriteExpression(e);
        }
    }
};
