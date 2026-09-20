// Names constructor results from the constructed type or Instance class string.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class ConstructorResultRenamer {
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

    static bool IsCapitalized(const std::string &s) { return !s.empty() && std::isupper(static_cast<unsigned char>(s[0])); }

    // `new`, or a `from<Upper>...` factory (`fromRGB`, `fromMatrix`, ...).
    static bool IsConstructorMethod(const std::string &m) {
        if (m == "new")
            return true;
        return m.size() > 4 && m.compare(0, 4, "from") == 0 && std::isupper(static_cast<unsigned char>(m[4]));
    }

    // String key of a `a.b` member access (`mem->key` is a StringLiteralNode).
    static std::string MemberName(const std::shared_ptr<MemberExpressionNode> &mem) {
        if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key))
            return key->value;
        return "";
    }

    // From a `<Type>.new(...)` / `<Type>.from*(...)` call, return the local name to use (or "").
    static std::string ConstructorLeaf(const std::shared_ptr<Expression> &expr) {
        auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr);
        if (!call)
            return "";
        auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(call->callee);
        if (!mem)
            return "";
        auto base = std::dynamic_pointer_cast<IdentifierExpressionNode>(mem->table);
        if (!base || !base->identifier)
            return "";
        const std::string &type = base->identifier->name;
        const std::string method = MemberName(mem);
        if (!IsCapitalized(type) || !IsConstructorMethod(method))
            return "";

        // `Instance.new("Class")`: the class string is the real type.
        if (type == "Instance" && method == "new" && !call->arguments.empty())
            if (auto s = std::dynamic_pointer_cast<StringLiteralNode>(call->arguments[0]); s && ScopeAwareRenamer::IsBareIdentifier(s->value))
                return ScopeAwareRenamer::LowerFirst(s->value);

        return ScopeAwareRenamer::LowerFirst(type);
    }

    static void CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates) {
        if (!stmt)
            return;
        // `local v = Type.new(...)` as a VariableDeclaration (value is the call).
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier && IsAutoName(id->identifier->name))
                if (const std::string leaf = ConstructorLeaf(decl->value); !leaf.empty())
                    candidates.emplace_back(id->identifier->name, leaf);
            return;
        }
        // `local v = Type.new(...)` rendered as a call-with-return ExpressionStatement.
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); call && call->bIsLocalDeclaration && call->rets.size() == 1)
                if (auto rid = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->rets[0]); rid && rid->identifier && IsAutoName(rid->identifier->name))
                    if (const std::string leaf = ConstructorLeaf(es->expression); !leaf.empty())
                        candidates.emplace_back(rid->identifier->name, leaf);
            return;
        }
        // split form: `v = Type.new(...)`.
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (auto lid = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); lid && lid->identifier && IsAutoName(lid->identifier->name))
                if (const std::string leaf = ConstructorLeaf(asn->right); !leaf.empty())
                    candidates.emplace_back(lid->identifier->name, leaf);
            return;
        }
    }
};
