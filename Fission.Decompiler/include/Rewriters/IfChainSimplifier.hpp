//
// Created by Dottik on 2/6/2026.
//
// Flips inverted nested ifs (`if v ~= A then <cont> else <body>`) into positive form
// (`if v == A then <body> else <cont>`). The generator renders an `else` that is one
// `if` as `elseif`, so the chain collapses. Inversion is semantics-preserving.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <string>

class IfChainSimplifier : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts) {
            auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt);
            if (!ifS || !ifS->thenBranch || ifS->thenBranch->body.empty() || !ifS->elseBranch || ifS->elseBranch->body.empty())
                continue;

            bool condNegated = false;
            if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition); u && u->op == "not ")
                condNegated = true;
            if (auto b = std::dynamic_pointer_cast<BinaryExpressionNode>(ifS->condition); b && b->op == "~=")
                condNegated = true;

            const bool thenLeadsWithIf = std::dynamic_pointer_cast<IfStatementNode>(ifS->thenBranch->body.front()) != nullptr;
            const bool elseLeadsWithIf = std::dynamic_pointer_cast<IfStatementNode>(ifS->elseBranch->body.front()) != nullptr;
            if (condNegated && thenLeadsWithIf && !elseLeadsWithIf) {
                ifS->condition = InvertCondition(ifS->condition);
                std::swap(ifS->thenBranch, ifS->elseBranch);
            }
        }
    }

  private:
    // `not X` → X; comparisons flip; anything else is wrapped in `not (...)`.
    static std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond) {
        if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); u && u->op == "not ")
            return u->operand;
        if (auto b = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
            std::string inv;
            if (b->op == "==")
                inv = "~=";
            else if (b->op == "~=")
                inv = "==";
            else if (b->op == "<")
                inv = ">=";
            else if (b->op == ">")
                inv = "<=";
            else if (b->op == "<=")
                inv = ">";
            else if (b->op == ">=")
                inv = "<";
            if (!inv.empty())
                return std::make_shared<BinaryExpressionNode>(inv, b->left, b->right);
        }
        return std::make_shared<UnaryExpressionNode>("not ", cond);
    }
};
