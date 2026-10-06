//
// Created by Dottik on 2/6/2026.
//

// Structural passes own statement vectors because visitors cannot replace parent children.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"

#include <memory>
#include <vector>

class ASTRewriter {
  public:
    virtual ~ASTRewriter() = default;

    void Run(std::vector<std::shared_ptr<Statement>> &statements) { RewriteBlock(statements); }

  protected:
    // Runs post-order after nested blocks.
    virtual void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) = 0;
    virtual void RewriteLoopStatements(std::vector<std::shared_ptr<Statement>> &) {}

    // A repeat condition shares its body's scope and must participate in use analysis.
    std::shared_ptr<Expression> m_tailScopeExpr;

    void RewriteBlock(std::vector<std::shared_ptr<Statement>> &stmts, const std::shared_ptr<Expression> &tailScope = nullptr) {
        for (auto &stmt : stmts) {
            if (auto fn = stmt && stmt->nodeKind == ASTNodeKind::FunctionDeclarationNode ? std::static_pointer_cast<FunctionDeclarationNode>(stmt) : nullptr;
                fn && fn->lpFunctionBody) {
                RewriteBlock(fn->lpFunctionBody->body);
                continue;
            }
            ForEachStatementExpression(stmt, [&](const std::shared_ptr<Expression> &expr) { RewriteExpression(expr); });
            const auto repeat = stmt && stmt->nodeKind == ASTNodeKind::RepeatStatement ? std::static_pointer_cast<RepeatStatementNode>(stmt) : nullptr;
            const bool loop = IsLoopStatement(stmt);
            ForEachChildBlock(stmt, [&](std::vector<std::shared_ptr<Statement>> &body) {
                RewriteBlock(body, repeat ? repeat->condition : nullptr);
                if (loop)
                    RewriteLoopStatements(body);
            });
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
        if (auto fn = expr->nodeKind == ASTNodeKind::FunctionDeclarationNode ? std::static_pointer_cast<FunctionDeclarationNode>(expr) : nullptr;
            fn && fn->lpFunctionBody)
            RewriteBlock(fn->lpFunctionBody->body);
        else
            ForEachSubExpression(expr, [&](const std::shared_ptr<Expression> &child) { RewriteExpression(child); });
    }
};
