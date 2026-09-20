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
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                CollectPcalls(s, out);
        } else if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CollectPcalls(ifS->thenBranch, out);
            CollectPcalls(ifS->elseBranch, out);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            CollectPcalls(w->body, out);
        } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            CollectPcalls(r->body, out);
        } else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            CollectPcalls(fnum->lpLoopBody, out);
        } else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            CollectPcalls(fgen->body, out);
        }
    }

    // Nested function bodies reachable from this scope (each is its own register namespace / fresh scope).
    void CollectNestedFunctions(const std::shared_ptr<Statement> &stmt, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt))
            CollectNestedFunctionsExpr(es->expression, out);
        else if (auto e = std::dynamic_pointer_cast<Expression>(stmt))
            CollectNestedFunctionsExpr(e, out);
        else if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                CollectNestedFunctions(s, out);
        } else if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            CollectNestedFunctionsExpr(vd->value, out);
        else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            CollectNestedFunctionsExpr(asn->right, out);
        else if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CollectNestedFunctions(ifS->thenBranch, out);
            CollectNestedFunctions(ifS->elseBranch, out);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt))
            CollectNestedFunctions(w->body, out);
        else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt))
            CollectNestedFunctions(r->body, out);
        else if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt))
            CollectNestedFunctions(fnum->lpLoopBody, out);
        else if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            CollectNestedFunctionsExpr(fgen->generator, out);
            CollectNestedFunctions(fgen->body, out);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                CollectNestedFunctionsExpr(v, out);
        }
    }

    void CollectNestedFunctionsExpr(const std::shared_ptr<Expression> &expr, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        if (!expr)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            out.push_back(fn); // a new scope; recursion processes its body
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(call->callee, out);
            for (const auto &a : call->arguments)
                CollectNestedFunctionsExpr(a, out);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(nc->calledOn, out);
            for (const auto &a : nc->arguments)
                CollectNestedFunctionsExpr(a, out);
        } else if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr))
            CollectNestedFunctionsExpr(mem->table, out);
        else if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(idx->left, out);
            CollectNestedFunctionsExpr(idx->right, out);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(bin->left, out);
            CollectNestedFunctionsExpr(bin->right, out);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr))
            CollectNestedFunctionsExpr(un->operand, out);
        else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                CollectNestedFunctionsExpr(e, out);
        }
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
            CollectNestedFunctions(s, fns);
        for (const auto &fn : fns)
            if (fn->lpFunctionBody)
                ProcessScope(fn->lpFunctionBody->body);
    }
};
