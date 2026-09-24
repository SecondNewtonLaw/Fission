//
// Created by Dottik on 23/9/2026.
//

// Names `x:GetService("Name")`, `x:FindFirstChild("Name")`, `x:WaitForChild("Name")` and
// `require(x:WaitForChild("Name"))` results after the child they look up.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class ChildLookupRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        ScopeAwareRenamer::Run(statements, [](const std::vector<std::shared_ptr<Statement>> &scope) {
            std::vector<std::pair<std::string, std::string>> candidates;
            for (size_t i = 0; i < scope.size(); ++i) {
                std::string alias;
                if (i > 0)
                    if (const auto previous = std::dynamic_pointer_cast<VariableDeclarationNode>(scope[i - 1])) {
                        const auto name = std::dynamic_pointer_cast<IdentifierExpressionNode>(previous->identifier);
                        const auto value = std::dynamic_pointer_cast<IdentifierExpressionNode>(previous->value);
                        if (name && name->identifier && value && value->identifier && value->identifier->name == "require")
                            alias = name->identifier->name;
                    }
                CollectCandidate(scope[i], candidates, alias);
            }
            return candidates;
        });
    }

  private:
    // Roblox instance names may hold spaces or punctuation; only plain identifiers can name a local.
    static bool IsPlainIdentifier(const std::string &s) {
        if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
            return false;
        for (const char c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                return false;
        return true;
    }

    static std::string MethodName(const std::shared_ptr<NameCallExpressionNode> &call) {
        const auto method = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callWhat);
        return method && method->identifier ? method->identifier->name : "";
    }

    static std::string LiteralArgument(const std::vector<std::shared_ptr<Expression>> &arguments) {
        if (arguments.size() != 1)
            return "";
        const auto literal = std::dynamic_pointer_cast<StringLiteralNode>(arguments[0]);
        return literal && IsPlainIdentifier(literal->value) ? literal->value : "";
    }

    static std::string ChildName(const std::shared_ptr<Expression> &value, std::string_view requireAlias) {
        if (const auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(value)) {
            const auto method = MethodName(call);
            return method == "GetService" || method == "FindFirstChild" || method == "WaitForChild" ? LiteralArgument(call->arguments) : "";
        }
        const auto call = std::dynamic_pointer_cast<CallExpressionNode>(value);
        const auto callee = call ? std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callee) : nullptr;
        if (!callee || !callee->identifier || (callee->identifier->name != "require" && callee->identifier->name != requireAlias) ||
            call->arguments.size() != 1)
            return "";
        const auto lookup = std::dynamic_pointer_cast<NameCallExpressionNode>(call->arguments[0]);
        if (!lookup)
            return "";
        const auto method = MethodName(lookup);
        return method == "WaitForChild" || method == "FindFirstChild" ? LiteralArgument(lookup->arguments) : "";
    }

    static void
    CollectCandidate(const std::shared_ptr<Statement> &stmt, std::vector<std::pair<std::string, std::string>> &candidates, std::string_view requireAlias) {
        const auto propose = [&](const std::shared_ptr<Expression> &target, const std::shared_ptr<Expression> &value) {
            const auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(target);
            if (!id || !id->identifier)
                return;
            if (const auto child = ChildName(value, requireAlias); !child.empty())
                candidates.emplace_back(id->identifier->name, child);
        };
        if (const auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (const auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression);
                call && call->bIsLocalDeclaration && call->rets.size() == 1)
                propose(call->rets[0], call);
            else if (
                const auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); call && call->bIsLocalDeclaration && call->rets.size() == 1
            )
                propose(call->rets[0], call);
        } else if (const auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            propose(decl->identifier, decl->value);
        }
    }
};
