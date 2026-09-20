// Names auto-generated locals from fields they feed.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ReverseFieldRenamer {
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
    // `v12`, `v3`, `v7_2`: the lifter's auto-names. Only these are retargeted.
    static bool IsAutoName(const std::string &s) {
        if (s.size() < 2 || s[0] != 'v' || !std::isdigit(static_cast<unsigned char>(s[1])))
            return false;
        size_t i = 1;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
            ++i;
        if (i == s.size())
            return true;
        if (s[i] != '_')
            return false;
        ++i;
        if (i == s.size())
            return false;
        for (; i < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
        return true;
    }

    // The field name written to: `obj.field` / `obj["field"]`.
    static std::string FieldKey(const std::shared_ptr<Expression> &lhs) {
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(lhs)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key))
                return key->value;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(lhs)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(idx->right))
                return key->value;
        }
        return "";
    }

    static std::string FieldOwner(const std::shared_ptr<Expression> &lhs) {
        std::shared_ptr<Expression> owner;
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(lhs))
            owner = mem->table;
        else if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(lhs))
            owner = idx->left;
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(owner);
        return id && id->identifier ? id->identifier->name : "";
    }

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!asn)
            return;
        auto rhs = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->right);
        if (!rhs || !rhs->identifier || !IsAutoName(rhs->identifier->name))
            return;
        const std::string key = FieldKey(asn->left);
        if (key.empty() || (key == "__index" && FieldOwner(asn->left) == rhs->identifier->name))
            return;
        candidates.emplace_back(rhs->identifier->name, ScopeAwareRenamer::LowerFirst(key));
    }
};
