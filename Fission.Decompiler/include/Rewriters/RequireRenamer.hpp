// Names path-based require results from their module leaf.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

class RequireRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        ScopeAwareRenamer::Run(statements, [](const std::vector<std::shared_ptr<Statement>> &scope) {
            std::vector<std::pair<std::string, std::string>> candidates;
            for (const auto &s : scope)
                CollectCandidate(s, candidates);
            return candidates;
        });
    }

  private:
    static std::string LeafName(const std::shared_ptr<Expression> &pathExpr) {
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(pathExpr)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key))
                return key->value;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(pathExpr)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(idx->right))
                return key->value;
        }
        return "";
    }

    static bool IsRequireCallee(const std::shared_ptr<Expression> &callee) {
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(callee);
        return id && id->identifier && id->identifier->name == "require";
    }

    // The single `require(<path>)` arg must be a path (member/index chain), not a
    // string literal. Returns the leaf module name, or "" if not a path require.
    static std::string PathRequireLeaf(const std::shared_ptr<CallExpressionNode> &call) {
        if (!call || !IsRequireCallee(call->callee) || call->arguments.size() != 1)
            return "";
        return LeafName(call->arguments[0]);
    }

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); call && call->bIsLocalDeclaration && call->rets.size() == 1) {
                if (const std::string leaf = PathRequireLeaf(call); !leaf.empty())
                    if (auto rid = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->rets[0]); rid && rid->identifier)
                        candidates.emplace_back(rid->identifier->name, leaf);
            }
            return;
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(decl->value))
                    if (const std::string leaf = PathRequireLeaf(call); !leaf.empty())
                        candidates.emplace_back(id->identifier->name, leaf);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (auto lid = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); lid && lid->identifier)
                if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(asn->right))
                    if (const std::string leaf = PathRequireLeaf(call); !leaf.empty())
                        candidates.emplace_back(lid->identifier->name, leaf);
            return;
        }
    }
};
