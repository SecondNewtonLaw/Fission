// Names getter results from the property after a Get prefix.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class GetterRenamer {
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
    // `Get<Upper>rest` / `get<Upper>rest` -> `<lower>rest`; "" if not a getter.
    static std::string GetterLeaf(const std::string &method) {
        if (method.size() <= 3)
            return "";
        const bool getPrefix = method.compare(0, 3, "Get") == 0 || method.compare(0, 3, "get") == 0;
        if (!getPrefix || !std::isupper(static_cast<unsigned char>(method[3])))
            return "";
        std::string rest = method.substr(3);
        rest[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(rest[0])));
        return rest;
    }

    // Method name of a `obj:Method()` namecall.
    static std::string NameCallMethod(const std::shared_ptr<NameCallExpressionNode> &nc) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(nc->callWhat); id && id->identifier)
            return id->identifier->name;
        return "";
    }

    // Callee name of a `obj.Method()` / `Method()` plain call.
    static std::string CallMethod(const std::shared_ptr<CallExpressionNode> &call) {
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(call->callee)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key))
                return key->value;
        }
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callee); id && id->identifier)
            return id->identifier->name;
        return "";
    }

    // From a getter call expression, return its leaf name (or "").
    static std::string CallExprLeaf(const std::shared_ptr<Expression> &expr) {
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr))
            return GetterLeaf(NameCallMethod(nc));
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr))
            return GetterLeaf(CallMethod(call));
        return "";
    }

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        if (!stmt)
            return;
        // `local v = obj:GetX()` / `local v = obj.GetX()` as ExpressionStatement.
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression); nc && nc->bIsLocalDeclaration && nc->rets.size() == 1) {
                if (const std::string leaf = GetterLeaf(NameCallMethod(nc)); !leaf.empty())
                    if (auto rid = std::dynamic_pointer_cast<IdentifierExpressionNode>(nc->rets[0]); rid && rid->identifier)
                        candidates.emplace_back(rid->identifier->name, leaf);
            } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); call && call->bIsLocalDeclaration && call->rets.size() == 1) {
                if (const std::string leaf = GetterLeaf(CallMethod(call)); !leaf.empty())
                    if (auto rid = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->rets[0]); rid && rid->identifier)
                        candidates.emplace_back(rid->identifier->name, leaf);
            }
            return;
        }
        // `local v = obj:GetX()` as a VariableDeclaration.
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                if (const std::string leaf = CallExprLeaf(decl->value); !leaf.empty())
                    candidates.emplace_back(id->identifier->name, leaf);
            return;
        }
        // split form: `v = obj:GetX()`.
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (auto lid = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); lid && lid->identifier)
                if (const std::string leaf = CallExprLeaf(asn->right); !leaf.empty())
                    candidates.emplace_back(lid->identifier->name, leaf);
            return;
        }
    }
};
