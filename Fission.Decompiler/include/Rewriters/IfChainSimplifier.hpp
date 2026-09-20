//
// Created by Dottik on 2/6/2026.
//

// Flips inverted branches so nested else blocks render as elseif chains.

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <memory>
#include <string>

class IfChainSimplifier : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts) {
            auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt);
            if (!ifS)
                continue;
            if (ifS->thenBranch && ifS->thenBranch->body.empty() && ifS->elseBranch && !ifS->elseBranch->body.empty()) {
                if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition); u && u->op == "not ")
                    ifS->condition = u->operand;
                else
                    ifS->condition = std::make_shared<UnaryExpressionNode>("not ", ifS->condition);
                std::swap(ifS->thenBranch, ifS->elseBranch);
            }
            if (ifS->elseBranch && ifS->elseBranch->body.empty())
                ifS->elseBranch.reset();
            if (!ifS->thenBranch || ifS->thenBranch->body.empty() || !ifS->elseBranch || ifS->elseBranch->body.empty())
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
            while (ifS->elseBranch && ifS->elseBranch->body.size() == 1) {
                auto inner = std::dynamic_pointer_cast<IfStatementNode>(ifS->elseBranch->body.front());
                if (!inner || !SameBody(ifS->thenBranch, inner->thenBranch))
                    break;
                ifS->condition = std::make_shared<BinaryExpressionNode>("or", ifS->condition, inner->condition);
                ifS->elseBranch = inner->elseBranch;
            }
        }
        for (size_t i = 0; i + 1 < stmts.size();) {
            auto first = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
            auto next = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
            if (first && next && !first->elseBranch && first->thenBranch && first->thenBranch->body.size() == 1 &&
                std::dynamic_pointer_cast<ReturnStatementNode>(first->thenBranch->body.front())) {
                if (SameBody(first->thenBranch, next->thenBranch)) {
                    first->condition = std::make_shared<BinaryExpressionNode>("or", first->condition, next->condition);
                    first->elseBranch = next->elseBranch;
                    stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    continue;
                }
                if (SameBody(first->thenBranch, next->elseBranch)) {
                    first->condition = std::make_shared<BinaryExpressionNode>("or", first->condition, InvertCondition(next->condition));
                    first->elseBranch = next->thenBranch;
                    stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    continue;
                }
            }
            ++i;
        }
    }

  private:
    static std::string RenderBody(const std::shared_ptr<BlockStatementNode> &body) {
        SourceGenerator generator;
        if (body)
            for (const auto &stmt : body->body)
                if (stmt)
                    stmt->Accept(&generator);
        return generator.buffer.str();
    }

    static bool SameBody(const std::shared_ptr<BlockStatementNode> &lhs, const std::shared_ptr<BlockStatementNode> &rhs) {
        return lhs && rhs && !lhs->body.empty() && RenderBody(lhs) == RenderBody(rhs);
    }

    // `not X` -> X; comparisons flip; anything else is wrapped in `not (...)`.
    static std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond) {
        if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); u && u->op == "not ")
            return u->operand;
        if (auto b = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
            std::string inv;
            if (b->op == "==")
                inv = "~=";
            else if (b->op == "~=")
                inv = "==";
            if (!inv.empty())
                return std::make_shared<BinaryExpressionNode>(inv, b->left, b->right);
        }
        return std::make_shared<UnaryExpressionNode>("not ", cond);
    }
};
