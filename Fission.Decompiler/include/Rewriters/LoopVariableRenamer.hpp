// Names loop variables by convention while avoiding active nested bindings.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

class LoopVariableRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        std::unordered_set<std::string> active;
        WalkBlock(statements, active);
    }

  private:
    // The lifter's loop-var auto-names: `i_<digits>` (numeric) and `v<digits>[_<digits>]` (generic).
    static bool IsLoopAutoName(const std::string &s) {
        if (s.size() >= 3 && s[0] == 'i' && s[1] == '_') {
            for (size_t i = 2; i < s.size(); ++i)
                if (!std::isdigit(static_cast<unsigned char>(s[i])))
                    return false;
            return true;
        }
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

    static bool Taken(const std::string &c, const std::unordered_set<std::string> &active, const std::unordered_set<std::string> &bodyNames) {
        return active.count(c) || bodyNames.count(c);
    }

    // Prefer i through n, then append numeric suffixes.
    static std::string PickNumeric(const std::unordered_set<std::string> &active, const std::unordered_set<std::string> &bodyNames) {
        static const char *letters[] = {"i", "j", "k", "l", "m", "n"};
        for (const char *l : letters)
            if (!Taken(l, active, bodyNames))
                return l;
        for (int n = 2; n < 1000; ++n)
            for (const char *l : letters)
                if (const std::string c = std::string(l) + std::to_string(n); !Taken(c, active, bodyNames))
                    return c;
        return "";
    }

    // Convention name for generic-for variable `idx` of `numVars`; bumped (v -> v2 ...) until free.
    static std::string
    PickGeneric(size_t idx, size_t numVars, bool ipairsIter, const std::unordered_set<std::string> &active, const std::unordered_set<std::string> &bodyNames) {
        std::string base;
        if (numVars == 1)
            base = "v";
        else if (idx == 0)
            base = ipairsIter ? "i" : "k";
        else if (idx == 1)
            base = "v";
        else
            base = "v" + std::to_string(idx);
        if (!Taken(base, active, bodyNames))
            return base;
        for (int n = 2; n < 1000; ++n)
            if (const std::string c = base + std::to_string(n); !Taken(c, active, bodyNames))
                return c;
        return "";
    }

    static bool IsCallTo(const std::shared_ptr<Expression> &expr, const char *name) {
        auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr);
        if (!call)
            return false;
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callee);
        return id && id->identifier && id->identifier->name == name;
    }

    static std::string AutoName(const std::shared_ptr<Expression> &var) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(var); id && id->identifier && IsLoopAutoName(id->identifier->name))
            return id->identifier->name;
        return "";
    }

    void WalkBlock(std::vector<std::shared_ptr<Statement>> &stmts, std::unordered_set<std::string> &active) {
        std::vector<std::string> added;
        for (auto &s : stmts) {
            WalkStmt(s, active);
            AddBindings(s, active, added);
        }
        for (const auto &name : added)
            active.erase(name);
    }

    static void AddBinding(const std::shared_ptr<Expression> &expr, std::unordered_set<std::string> &active, std::vector<std::string> *added = nullptr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
            if (auto [it, inserted] = active.insert(id->identifier->name); inserted && added)
                added->push_back(*it);
    }

    static void AddBindings(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &active, std::vector<std::string> &added) {
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            AddBinding(decl->identifier, active, &added);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (fn->bIsLocalDeclaration && ScopeAwareRenamer::IsBareIdentifier(fn->functionName))
                if (auto [it, inserted] = active.insert(fn->functionName); inserted)
                    added.push_back(*it);
        } else if (auto expression = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration)
                for (const auto &ret : call->rets)
                    AddBinding(ret, active, &added);
            else if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration)
                for (const auto &ret : call->rets)
                    AddBinding(ret, active, &added);
        }
    }

    void WalkStmt(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &active) {
        if (!stmt)
            return;

        if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            std::unordered_set<std::string> active2 = active;
            if (const std::string from = AutoName(fn->loopVariable); !from.empty() && fn->lpLoopBody) {
                std::unordered_set<std::string> bodyNames;
                ScopeAwareRenamer::CollectIdentifierNames(fn->lpLoopBody, bodyNames);
                if (const std::string to = PickNumeric(active, bodyNames); !to.empty()) {
                    ScopeAwareRenamer::RenameInStatement(fn->loopVariable, from, to);
                    ScopeAwareRenamer::RenameInStatement(fn->lpLoopBody, from, to);
                    active2.insert(to);
                }
            }
            // bounds may hold closures; the body carries the loop nesting.
            WalkExpr(fn->startVariable, active);
            WalkExpr(fn->maxIncreased, active);
            WalkExpr(fn->increaseBy, active);
            if (fn->lpLoopBody)
                WalkBlock(fn->lpLoopBody->body, active2);
            return;
        }

        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            std::unordered_set<std::string> active2 = active;
            std::unordered_set<std::string> bodyNames;
            if (fg->body)
                ScopeAwareRenamer::CollectIdentifierNames(fg->body, bodyNames);
            const bool ipairsIter = IsCallTo(fg->generator, "ipairs");
            const size_t n = fg->loopVariables.size();
            for (size_t idx = 0; idx < n; ++idx) {
                const std::string from = AutoName(fg->loopVariables[idx]);
                if (from.empty())
                    continue;
                if (const std::string to = PickGeneric(idx, n, ipairsIter, active2, bodyNames); !to.empty()) {
                    ScopeAwareRenamer::RenameInStatement(fg->loopVariables[idx], from, to);
                    ScopeAwareRenamer::RenameInStatement(fg->body, from, to);
                    active2.insert(to);
                }
            }
            WalkExpr(fg->generator, active);
            if (fg->body)
                WalkBlock(fg->body->body, active2);
            return;
        }

        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            WalkExpr(w->condition, active);
            WalkStmt(w->body, active);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            WalkExpr(r->condition, active);
            WalkStmt(r->body, active);
            return;
        }
        if (auto ifs = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            WalkExpr(ifs->condition, active);
            WalkStmt(ifs->thenBranch, active);
            WalkStmt(ifs->elseBranch, active);
            return;
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            WalkBlock(b->body, active);
            return;
        }
        if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            WalkExpr(vd->value, active);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            WalkExpr(asn->right, active);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            WalkExpr(es->expression, active);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (auto &v : ret->returnValues)
                WalkExpr(v, active);
            return;
        }
        // a `local function`/expression-function statement: recurse into its body as a fresh scope.
        if (auto e = std::dynamic_pointer_cast<Expression>(stmt)) {
            WalkExpr(e, active);
            return;
        }
    }

    // Hunt for closures inside an expression. Register namespace is fresh, but lexical parent names remain occupied.
    void WalkExpr(const std::shared_ptr<Expression> &expr, const std::unordered_set<std::string> &active) {
        if (!expr)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            if (fn->lpFunctionBody) {
                std::unordered_set<std::string> fresh = active;
                for (const auto &[_, arg] : fn->argumentsNames)
                    if (arg)
                        AddBinding(arg->argumentName, fresh);
                WalkBlock(fn->lpFunctionBody->body, fresh);
            }
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            WalkExpr(call->callee, active);
            for (auto &a : call->arguments)
                WalkExpr(a, active);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            WalkExpr(nc->calledOn, active);
            for (auto &a : nc->arguments)
                WalkExpr(a, active);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            WalkExpr(mem->table, active);
            WalkExpr(mem->key, active);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            WalkExpr(idx->left, active);
            WalkExpr(idx->right, active);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            WalkExpr(bin->left, active);
            WalkExpr(bin->right, active);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            WalkExpr(cmp->left, active);
            WalkExpr(cmp->right, active);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            WalkExpr(un->operand, active);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &e : tbl->expressions)
                WalkExpr(e, active);
            return;
        }
    }
};
