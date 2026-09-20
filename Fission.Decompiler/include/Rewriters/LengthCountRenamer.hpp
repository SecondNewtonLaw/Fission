// Names one unambiguous length result `count` per scope.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class LengthCountRenamer {
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

    // A `#expr` length unary.
    static bool IsLength(const std::shared_ptr<Expression> &expr) {
        auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr);
        return un && un->op == "#";
    }

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        if (!stmt)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier && IsAutoName(id->identifier->name))
                if (IsLength(decl->value))
                    candidates.emplace_back(id->identifier->name, "count");
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (auto lid = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); lid && lid->identifier && IsAutoName(lid->identifier->name))
                if (IsLength(asn->right))
                    candidates.emplace_back(lid->identifier->name, "count");
            return;
        }
    }
};
