// Removes bare `x = x`; member assignments and declarations can have semantics.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <vector>

class SelfAssignmentEliminator : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        std::vector<std::shared_ptr<Statement>> kept;
        kept.reserve(stmts.size());
        for (auto &stmt : stmts) {
            if (IsSelfAssign(stmt))
                continue;
            kept.push_back(std::move(stmt));
        }
        stmts = std::move(kept);
    }

  private:
    static bool IsSelfAssign(const std::shared_ptr<Statement> &stmt) {
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!asn)
            return false;
        auto l = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
        auto r = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->right);
        return l && r && l->identifier && r->identifier && !l->identifier->bIsGlobal && !r->identifier->bIsGlobal && l->identifier->name == r->identifier->name;
    }
};
