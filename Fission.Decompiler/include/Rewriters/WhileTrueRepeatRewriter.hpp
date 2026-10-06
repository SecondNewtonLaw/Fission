//
// Created by Dottik on 10/1/2026.
//

// `while true do X if c then break end end` and `repeat X until c` compile to identical bytecode; prefer repeat.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "FissionAllocator.hpp"
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <vector>

class WhileTrueRepeatRewriter : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts) {
            auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt);
            if (!w || !w->body || w->body->body.empty())
                continue;
            auto lit = AsLiteral<BooleanLiteralNode>(w->condition);
            if (!lit || !lit->value)
                continue;
            auto tail = std::dynamic_pointer_cast<IfStatementNode>(w->body->body.back());
            if (!tail || tail->elseBranch || !tail->thenBranch || tail->thenBranch->body.size() != 1 ||
                !std::dynamic_pointer_cast<BreakStatementNode>(tail->thenBranch->body.front()))
                continue;
            // `continue` in a repeat runs the until test; in the while it skips it
            if (HasOwnContinue(w->body->body))
                continue;

            auto repeat = Fission::MakeShared<RepeatStatementNode>();
            repeat->condition = tail->condition;
            repeat->body = w->body;
            repeat->body->body.pop_back();
            stmt = repeat;
        }
    }

  private:
    static bool HasOwnContinue(const std::vector<std::shared_ptr<Statement>> &stmts) {
        for (const auto &stmt : stmts) {
            if (std::dynamic_pointer_cast<ContinueStatementNode>(stmt))
                return true;
            if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
                if ((ifS->thenBranch && HasOwnContinue(ifS->thenBranch->body)) || (ifS->elseBranch && HasOwnContinue(ifS->elseBranch->body)))
                    return true;
            } else if (auto block = std::dynamic_pointer_cast<BlockStatementNode>(stmt); block && HasOwnContinue(block->body)) {
                return true;
            }
        }
        return false;
    }
};
