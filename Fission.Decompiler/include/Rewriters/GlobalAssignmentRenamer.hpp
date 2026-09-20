// Names auto-generated locals from globals they feed without shadowing those globals.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class GlobalAssignmentRenamer {
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

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!asn)
            return;
        // LHS is a bare identifier (`Global = ...`), not a field / index store and not itself an auto-name.
        auto lhs = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
        if (!lhs || !lhs->identifier)
            return;
        const std::string &globalName = lhs->identifier->name;
        if (!ScopeAwareRenamer::IsBareIdentifier(globalName) || IsAutoName(globalName))
            return;
        // RHS is an auto-named local being stored into the global.
        auto rhs = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->right);
        if (!rhs || !rhs->identifier || !IsAutoName(rhs->identifier->name))
            return;
        candidates.emplace_back(rhs->identifier->name, ScopeAwareRenamer::LowerFirst(globalName));
    }
};
