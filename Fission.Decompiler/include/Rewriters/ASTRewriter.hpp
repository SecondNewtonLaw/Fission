//
// Created by Dottik on 2/6/2026.
//

// Structural passes own statement vectors because visitors cannot replace parent children.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <memory>
#include <vector>

class ASTRewriter {
  public:
    virtual ~ASTRewriter() = default;

    void Run(std::vector<std::shared_ptr<Statement>> &statements) { RewriteBlock(statements); }

  protected:
    // Runs post-order after nested blocks.
    virtual void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) = 0;

    // A repeat condition shares its body's scope and must participate in use analysis.
    std::shared_ptr<Expression> m_tailScopeExpr;

    void RewriteBlock(std::vector<std::shared_ptr<Statement>> &stmts, const std::shared_ptr<Expression> &tailScope = nullptr) {
        for (auto &stmt : stmts) {
            if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
                RewriteExpression(ifS->condition);
                if (ifS->thenBranch)
                    RewriteBlock(ifS->thenBranch->body);
                if (ifS->elseBranch)
                    RewriteBlock(ifS->elseBranch->body);
            } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->lpFunctionBody) {
                RewriteBlock(fn->lpFunctionBody->body);
            } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt); w && w->body) {
                RewriteExpression(w->condition);
                RewriteBlock(w->body->body);
            } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt); r && r->body) {
                RewriteExpression(r->condition);
                RewriteBlock(r->body->body, r->condition);
            } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt); fnum && fnum->lpLoopBody) {
                RewriteExpression(fnum->startVariable);
                RewriteExpression(fnum->maxIncreased);
                RewriteExpression(fnum->increaseBy);
                RewriteBlock(fnum->lpLoopBody->body);
            } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt); fgen && fgen->body) {
                RewriteExpression(fgen->generator);
                RewriteExpression(fgen->state);
                RewriteExpression(fgen->index);
                RewriteBlock(fgen->body->body);
            } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
                RewriteExpression(asn->left);
                RewriteExpression(asn->right);
            } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
                RewriteExpression(decl->value);
            } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
                RewriteExpression(es->expression);
            } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
                for (auto &v : ret->returnValues)
                    RewriteExpression(v);
            } else if (auto block = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
                RewriteBlock(block->body);
            }
        }
        const auto saved = m_tailScopeExpr;
        m_tailScopeExpr = tailScope;
        RewriteStatements(stmts);
        m_tailScopeExpr = saved;
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
        } else if (auto bin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            RewriteExpression(bin->left);
            RewriteExpression(bin->right);
        } else if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            RewriteExpression(index->left);
            RewriteExpression(index->right);
        } else if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            RewriteExpression(member->table);
            RewriteExpression(member->key);
        } else if (auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(expr)) {
            RewriteExpression(conditional->condition);
            RewriteExpression(conditional->thenExpr);
            RewriteExpression(conditional->elseExpr);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            RewriteExpression(un->operand);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &e : tbl->expressions)
                RewriteExpression(e);
        }
    }
};
