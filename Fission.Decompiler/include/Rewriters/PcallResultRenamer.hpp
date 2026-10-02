// Names pcall and xpcall results `ok` and `result`, with per-scope collision suffixes.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class PcallResultRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) { ProcessScope(statements); }

  private:
    // The lifter's register auto-names: `vN` / `vN_M`.
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
        for (++i; i < s.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(s[i])))
                return false;
        return i > 0;
    }

    static bool IsProtectedCall(const std::shared_ptr<Expression> &callee) {
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(callee);
        if (!id || !id->identifier)
            return false;
        const std::string &n = id->identifier->name;
        return n == "pcall" || n == "xpcall" || n == "ypcall";
    }

    static std::string AutoNameOf(const std::shared_ptr<Expression> &e) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e); id && id->identifier && IsAutoName(id->identifier->name))
            return id->identifier->name;
        return "";
    }

    // The ordered return-target auto-names of one pcall local-declaration (slot empty -> "").
    static bool PcallRets(const std::shared_ptr<Statement> &stmt, std::vector<std::string> &rets) {
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression);
                call && call->bIsLocalDeclaration && !call->rets.empty() && IsProtectedCall(call->callee)) {
                for (const auto &r : call->rets)
                    rets.push_back(AutoNameOf(r));
                return true;
            }
            return false;
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(decl->value); call && IsProtectedCall(call->callee)) {
                rets.push_back(AutoNameOf(decl->identifier));
                return true;
            }
        }
        return false;
    }

    // Append numeric suffixes until the name is unclaimed and unused.
    static std::string Pick(const std::string &base, const std::unordered_set<std::string> &claimed, const std::unordered_set<std::string> &taken) {
        auto free = [&](const std::string &c) { return !claimed.count(c) && !taken.count(c); };
        if (free(base))
            return base;
        for (int n = 2; n < 1000; ++n)
            if (const std::string c = base + std::to_string(n); free(c))
                return c;
        return "";
    }

    // Collect pcall return-name lists in this scope (descends control-flow blocks, NOT nested functions).
    void CollectPcalls(const std::shared_ptr<Statement> &stmt, std::vector<std::vector<std::string>> &out) {
        if (!stmt)
            return;
        if (std::vector<std::string> rets; PcallRets(stmt, rets)) {
            out.push_back(std::move(rets));
            return;
        }
        ForEachChildBlock(stmt, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &s : body)
                CollectPcalls(s, out);
        });
    }

    void ProcessScope(std::vector<std::shared_ptr<Statement>> &scope) {
        std::vector<std::vector<std::string>> pcalls;
        for (const auto &s : scope)
            CollectPcalls(s, pcalls);

        if (!pcalls.empty()) {
            std::unordered_set<std::string> taken; // names already used somewhere in the scope
            for (const auto &s : scope)
                ScopeAwareRenamer::CollectIdentifierNames(s, taken);

            std::unordered_set<std::string> claimed;
            std::unordered_map<std::string, std::string> rename;
            for (const auto &rets : pcalls) {
                for (size_t idx = 0; idx < rets.size() && idx < 2; ++idx) {
                    const std::string &from = rets[idx];
                    if (from.empty() || rename.contains(from)) // non-auto slot, or a duplicated-block reuse
                        continue;
                    const std::string base = (idx == 0) ? "ok" : "result";
                    if (const std::string to = Pick(base, claimed, taken); !to.empty()) {
                        rename[from] = to;
                        claimed.insert(to);
                    }
                }
            }
            for (const auto &[from, to] : rename)
                for (const auto &s : scope)
                    ScopeAwareRenamer::RenameInStatement(s, from, to);
        }

        std::vector<std::shared_ptr<FunctionDeclarationNode>> fns;
        for (const auto &s : scope)
            ScopeAwareRenamer::CollectNestedFunctions(s, fns);
        for (const auto &fn : fns)
            if (fn->lpFunctionBody)
                ProcessScope(fn->lpFunctionBody->body);
    }
};
