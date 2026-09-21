// Applies detector-proposed names within lexical bindings while blocking collisions and shadows.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

#include <cctype>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ScopeAwareRenamer {
  public:
    // Returns (autoName, newName) candidates whose binding lives in `scope`.
    using Detector = std::function<std::vector<std::pair<std::string, std::string>>(const std::vector<std::shared_ptr<Statement>> &)>;

    // When `includeParams` is set, a candidate whose autoName is a parameter of the enclosing function is
    // eligible: the parameter counts as that scope's single binding and the rename reaches its declaration
    // in the argument list. Off by default so passes that name locals-after-a-store (reverse-field, global
    // assignment) never rewrite a caller-facing parameter; opt in only where naming the argument is wanted
    // (e.g. `obj:SetAttribute("Range", arg1)` -> `range`).
    static void Run(std::vector<std::shared_ptr<Statement>> &root, const Detector &detect, bool includeParams = false) {
        bool usedReady = false;
        std::unordered_set<std::string> usedNames; // computed lazily: only if some scope has candidates
        ProcessScope(root, root, detect, usedNames, usedReady, includeParams, nullptr);
    }

    static void PruneStaleRenameComments(std::vector<std::shared_ptr<Statement>> &root) { PruneStaleRenameCommentsInScope(root, nullptr); }

    static bool IsBareIdentifier(const std::string &s) {
        if (s.empty())
            return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            return false;
        for (char c : s)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        return true;
    }

    // Lower-case the first letter of a back-propagated name (`BlahBlah` -> `blahBlah`). A local named
    // after a source (a field, a global) must not match that source's casing: an upper-cased source
    // (`Health`, `BlahBlah`) becomes a distinct local, so it can never shadow the field/global it was
    // named from. A name that is already lower-first is returned unchanged.
    static std::string LowerFirst(const std::string &s) {
        if (s.empty())
            return s;
        std::string out = s;
        out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
        return out;
    }

    // Rename references within one statement subtree while respecting closure shadowing.
    static void RenameInStatement(const std::shared_ptr<Statement> &stmt, const std::string &from, const std::string &to) {
        std::unordered_map<std::string, std::string> rename;
        rename.emplace(from, to);
        std::unordered_set<std::string> shadowed;
        RenameStmt(stmt, rename, shadowed);
    }

    // Collect every identifier name appearing in a statement subtree (used for collision checks).
    static void CollectIdentifierNames(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &out) {
        auto collect = [&](const std::shared_ptr<Identifier> &id) {
            if (id)
                out.insert(id->name);
        };
        WalkStmt(stmt, collect);
    }

  private:
    static void PruneStaleRenameCommentsInScope(std::vector<std::shared_ptr<Statement>> &scope, const std::shared_ptr<FunctionDeclarationNode> &enclosingFn) {
        std::unordered_map<std::string, int> bindings;
        for (const auto &statement : scope)
            CountScopeDecls(statement, bindings);
        if (enclosingFn)
            for (const auto &[_, argument] : enclosingFn->argumentsNames)
                if (argument)
                    CountDeclName(argument->argumentName, bindings);

        static constexpr std::string_view prefix = "Fission: INFO: binding '";
        std::erase_if(scope, [&](const std::shared_ptr<Statement> &statement) {
            const auto comment = std::dynamic_pointer_cast<CommentNode>(statement);
            if (!comment || !comment->bIsInformational || !comment->comment.starts_with(prefix))
                return false;
            const auto end = comment->comment.find('\'', prefix.size());
            return end != std::string::npos && !bindings.contains(comment->comment.substr(prefix.size(), end - prefix.size()));
        });

        std::vector<std::shared_ptr<FunctionDeclarationNode>> functions;
        for (const auto &statement : scope)
            CollectNestedFunctions(statement, functions);
        for (const auto &function : functions)
            if (function->lpFunctionBody)
                PruneStaleRenameCommentsInScope(function->lpFunctionBody->body, function);
    }

    static void ProcessScope(
        std::vector<std::shared_ptr<Statement>> &scope, std::vector<std::shared_ptr<Statement>> &root, const Detector &detect,
        std::unordered_set<std::string> &usedNames, bool &usedReady, bool includeParams, const std::shared_ptr<FunctionDeclarationNode> &enclosingFn
    ) {
        auto candidates = detect(scope);
        if (!candidates.empty()) {
            if (!usedReady) { // first scope with candidates: collect every identifier name once
                auto collect = [&](const std::shared_ptr<Identifier> &id) {
                    if (id)
                        usedNames.insert(id->name);
                };
                for (const auto &s : root)
                    WalkStmt(s, collect);
                usedReady = true;
            }

            std::unordered_map<std::string, int> scopeDecls;
            for (const auto &s : scope)
                CountScopeDecls(s, scopeDecls);
            // A parameter binds its name once for this scope. Counting it lets a candidate whose autoName is
            // a parameter pass the "bound exactly once" gate; the rename below also rewrites its declaration.
            if (includeParams && enclosingFn)
                for (const auto &[idx, arg] : enclosingFn->argumentsNames)
                    if (arg)
                        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(arg->argumentName); id && id->identifier)
                            scopeDecls[id->identifier->name]++;

            std::unordered_map<std::string, int> leafClaims;
            for (const auto &[var, leaf] : candidates)
                leafClaims[leaf]++;

            std::unordered_map<std::string, std::string> rename;
            for (const auto &[var, leaf] : candidates) {
                if (rename.contains(var))
                    continue;
                if (!IsValidIdentifierKey(leaf))
                    continue;
                if (IsBuiltinGlobal(leaf))
                    continue;
                if (scopeDecls[var] != 1)
                    continue;
                if (leafClaims[leaf] > 1)
                    continue;
                if (usedNames.contains(leaf))
                    continue;
                rename[var] = leaf;
                usedNames.insert(leaf); // claim it so later scopes don't reuse
            }
            if (!rename.empty()) {
                std::unordered_set<std::string> shadowed;
                for (const auto &s : scope)
                    RenameStmt(s, rename, shadowed);
                // Rename the enclosing function's parameter declarations too (their body references were
                // handled by the scope walk above).
                if (includeParams && enclosingFn)
                    for (const auto &[idx, arg] : enclosingFn->argumentsNames)
                        if (arg)
                            RenameExpr(arg->argumentName, rename, shadowed);
            }
        }

        // Recurse into each nested function as its own scope.
        std::vector<std::shared_ptr<FunctionDeclarationNode>> fns;
        for (const auto &s : scope)
            CollectNestedFunctions(s, fns);
        for (const auto &fn : fns)
            if (fn->lpFunctionBody)
                ProcessScope(fn->lpFunctionBody->body, root, detect, usedNames, usedReady, includeParams, fn);
    }

    static void CollectNestedFunctions(const std::shared_ptr<Statement> &stmt, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            CollectNestedFunctionsExpr(es->expression, out);
            return;
        }
        if (auto e = std::dynamic_pointer_cast<Expression>(stmt)) {
            CollectNestedFunctionsExpr(e, out);
            return;
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                CollectNestedFunctions(s, out);
            return;
        }
        if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            CollectNestedFunctionsExpr(vd->value, out);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            CollectNestedFunctionsExpr(asn->right, out);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CollectNestedFunctions(ifS->thenBranch, out);
            CollectNestedFunctions(ifS->elseBranch, out);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            CollectNestedFunctions(w->body, out);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            CollectNestedFunctions(r->body, out);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            CollectNestedFunctions(fnum->lpLoopBody, out);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            CollectNestedFunctionsExpr(fgen->generator, out);
            CollectNestedFunctions(fgen->body, out);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                CollectNestedFunctionsExpr(v, out);
            return;
        }
    }

    static void CollectNestedFunctionsExpr(const std::shared_ptr<Expression> &expr, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        if (!expr)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            out.push_back(fn); // a new scope; do NOT descend (recursion handles it)
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(call->callee, out);
            for (const auto &a : call->arguments)
                CollectNestedFunctionsExpr(a, out);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(nc->calledOn, out);
            for (const auto &a : nc->arguments)
                CollectNestedFunctionsExpr(a, out);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(mem->table, out);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(idx->left, out);
            CollectNestedFunctionsExpr(idx->right, out);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(bin->left, out);
            CollectNestedFunctionsExpr(bin->right, out);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(cmp->left, out);
            CollectNestedFunctionsExpr(cmp->right, out);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            CollectNestedFunctionsExpr(un->operand, out);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                CollectNestedFunctionsExpr(e, out);
            return;
        }
    }

    static void CountScopeDecls(const std::shared_ptr<Statement> &stmt, std::unordered_map<std::string, int> &out) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            CountDeclExpr(es->expression, out);
            return;
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            CountDeclName(decl->identifier, out);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (fn->bIsLocalDeclaration && IsBareIdentifier(fn->functionName))
                out[fn->functionName]++;
            return; // do NOT descend into the function body
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                CountScopeDecls(s, out);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CountScopeDecls(ifS->thenBranch, out);
            CountScopeDecls(ifS->elseBranch, out);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            CountScopeDecls(w->body, out);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            CountScopeDecls(r->body, out);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            CountDeclName(fnum->loopVariable, out);
            CountScopeDecls(fnum->lpLoopBody, out);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            for (const auto &lv : fgen->loopVariables)
                CountDeclName(lv, out);
            CountScopeDecls(fgen->body, out);
            return;
        }
    }

    static void CountDeclExpr(const std::shared_ptr<Expression> &expr, std::unordered_map<std::string, int> &out) {
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr); call && call->bIsLocalDeclaration) {
            for (const auto &r : call->rets)
                CountDeclName(r, out);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr); nc && nc->bIsLocalDeclaration) {
            for (const auto &r : nc->rets)
                CountDeclName(r, out);
        }
    }

    static void CountDeclName(const std::shared_ptr<Expression> &e, std::unordered_map<std::string, int> &out) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e); id && id->identifier)
            out[id->identifier->name]++;
    }

    static std::unordered_set<std::string>
    FunctionBoundKeys(const std::shared_ptr<FunctionDeclarationNode> &fn, const std::unordered_map<std::string, std::string> &keys) {
        std::unordered_set<std::string> bound;
        for (const auto &[idx, arg] : fn->argumentsNames) {
            if (!arg)
                continue;
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(arg->argumentName); id && id->identifier)
                if (keys.contains(id->identifier->name))
                    bound.insert(id->identifier->name);
        }
        std::unordered_map<std::string, int> bodyDecls;
        CountScopeDecls(fn->lpFunctionBody, bodyDecls);
        for (const auto &[name, n] : bodyDecls)
            if (keys.contains(name))
                bound.insert(name);
        return bound;
    }

    static void RenameStmt(
        const std::shared_ptr<Statement> &stmt, const std::unordered_map<std::string, std::string> &rename, const std::unordered_set<std::string> &shadowed
    ) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            RenameExpr(es->expression, rename, shadowed);
            return;
        }
        if (auto e = std::dynamic_pointer_cast<Expression>(stmt)) {
            RenameExpr(e, rename, shadowed);
            return;
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                RenameStmt(s, rename, shadowed);
            return;
        }
        if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            RenameExpr(vd->identifier, rename, shadowed);
            RenameExpr(vd->value, rename, shadowed);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            RenameExpr(asn->left, rename, shadowed);
            RenameExpr(asn->right, rename, shadowed);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            RenameExpr(ifS->condition, rename, shadowed);
            RenameStmt(ifS->thenBranch, rename, shadowed);
            RenameStmt(ifS->elseBranch, rename, shadowed);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            RenameExpr(w->condition, rename, shadowed);
            RenameStmt(w->body, rename, shadowed);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            RenameExpr(r->condition, rename, shadowed);
            RenameStmt(r->body, rename, shadowed);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            RenameExpr(fnum->loopVariable, rename, shadowed);
            RenameExpr(fnum->startVariable, rename, shadowed);
            RenameExpr(fnum->increaseBy, rename, shadowed);
            RenameExpr(fnum->maxIncreased, rename, shadowed);
            RenameStmt(fnum->lpLoopBody, rename, shadowed);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            for (const auto &lv : fgen->loopVariables)
                RenameExpr(lv, rename, shadowed);
            RenameExpr(fgen->generator, rename, shadowed);
            RenameExpr(fgen->state, rename, shadowed);
            RenameExpr(fgen->index, rename, shadowed);
            RenameStmt(fgen->body, rename, shadowed);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                RenameExpr(v, rename, shadowed);
            return;
        }
        if (auto fa = std::dynamic_pointer_cast<FunctionArgumentExpression>(stmt)) {
            RenameExpr(fa->argumentName, rename, shadowed);
            return;
        }
    }

    static void RenameExpr(
        const std::shared_ptr<Expression> &expr, const std::unordered_map<std::string, std::string> &rename, const std::unordered_set<std::string> &shadowed
    ) {
        if (!expr)
            return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr)) {
            if (id->identifier && !shadowed.contains(id->identifier->name)) {
                auto it = rename.find(id->identifier->name);
                if (it != rename.end())
                    id->identifier->name = it->second;
            }
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            const size_t separator = fn->functionName.find_first_of(".:");
            const std::string receiver = fn->functionName.substr(0, separator);
            if (!shadowed.contains(receiver)) {
                auto it = rename.find(receiver);
                if (it != rename.end())
                    fn->functionName.replace(0, receiver.size(), it->second);
            }
            const std::unordered_set<std::string> bound = FunctionBoundKeys(fn, rename);
            const std::unordered_set<std::string> *use = &shadowed;
            std::unordered_set<std::string> merged;
            if (!bound.empty()) {
                merged = shadowed;
                merged.insert(bound.begin(), bound.end());
                use = &merged;
            }
            std::unordered_set<std::string> renamedCaptures;
            for (const auto &name : fn->capturedNames) {
                const auto replacement = rename.find(name);
                renamedCaptures.insert(replacement != rename.end() && !use->contains(name) ? replacement->second : name);
            }
            fn->capturedNames = std::move(renamedCaptures);
            for (const auto &[idx, arg] : fn->argumentsNames)
                if (arg)
                    RenameExpr(arg->argumentName, rename, *use);
            RenameStmt(fn->lpFunctionBody, rename, *use);
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            RenameExpr(call->callee, rename, shadowed);
            for (const auto &a : call->arguments)
                RenameExpr(a, rename, shadowed);
            for (const auto &r : call->rets)
                RenameExpr(r, rename, shadowed);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            RenameExpr(nc->calledOn, rename, shadowed);
            for (const auto &a : nc->arguments)
                RenameExpr(a, rename, shadowed);
            for (const auto &r : nc->rets)
                RenameExpr(r, rename, shadowed);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            RenameExpr(mem->table, rename, shadowed);
            RenameExpr(mem->key, rename, shadowed);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            RenameExpr(idx->left, rename, shadowed);
            RenameExpr(idx->right, rename, shadowed);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            RenameExpr(bin->left, rename, shadowed);
            RenameExpr(bin->right, rename, shadowed);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            RenameExpr(cmp->left, rename, shadowed);
            RenameExpr(cmp->right, rename, shadowed);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            RenameExpr(un->operand, rename, shadowed);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                RenameExpr(e, rename, shadowed);
            return;
        }
    }

    template <class F> static void WalkStmt(const std::shared_ptr<Statement> &stmt, F &f) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            WalkExpr(es->expression, f);
            return;
        }
        if (auto e = std::dynamic_pointer_cast<Expression>(stmt)) {
            WalkExpr(e, f);
            return;
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                WalkStmt(s, f);
            return;
        }
        if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            WalkExpr(vd->identifier, f);
            WalkExpr(vd->value, f);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            WalkExpr(asn->left, f);
            WalkExpr(asn->right, f);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            WalkExpr(ifS->condition, f);
            WalkStmt(ifS->thenBranch, f);
            WalkStmt(ifS->elseBranch, f);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            WalkExpr(w->condition, f);
            WalkStmt(w->body, f);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            WalkExpr(r->condition, f);
            WalkStmt(r->body, f);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            WalkExpr(fnum->loopVariable, f);
            WalkExpr(fnum->startVariable, f);
            WalkExpr(fnum->increaseBy, f);
            WalkExpr(fnum->maxIncreased, f);
            WalkStmt(fnum->lpLoopBody, f);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            for (const auto &lv : fgen->loopVariables)
                WalkExpr(lv, f);
            WalkExpr(fgen->generator, f);
            WalkExpr(fgen->state, f);
            WalkExpr(fgen->index, f);
            WalkStmt(fgen->body, f);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                WalkExpr(v, f);
            return;
        }
        if (auto fa = std::dynamic_pointer_cast<FunctionArgumentExpression>(stmt)) {
            WalkExpr(fa->argumentName, f);
            return;
        }
    }

    template <class F> static void WalkExpr(const std::shared_ptr<Expression> &expr, F &f) {
        if (!expr)
            return;
        if (auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(expr)) {
            WalkExpr(conditional->condition, f);
            WalkExpr(conditional->thenExpr, f);
            WalkExpr(conditional->elseExpr, f);
            return;
        }
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr)) {
            f(id->identifier);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            for (const auto &[idx, arg] : fn->argumentsNames)
                if (arg)
                    WalkExpr(arg->argumentName, f);
            WalkStmt(fn->lpFunctionBody, f);
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            WalkExpr(call->callee, f);
            for (const auto &a : call->arguments)
                WalkExpr(a, f);
            for (const auto &r : call->rets)
                WalkExpr(r, f);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            WalkExpr(nc->calledOn, f);
            for (const auto &a : nc->arguments)
                WalkExpr(a, f);
            for (const auto &r : nc->rets)
                WalkExpr(r, f);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            WalkExpr(mem->table, f);
            WalkExpr(mem->key, f);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            WalkExpr(idx->left, f);
            WalkExpr(idx->right, f);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            WalkExpr(bin->left, f);
            WalkExpr(bin->right, f);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            WalkExpr(cmp->left, f);
            WalkExpr(cmp->right, f);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            WalkExpr(un->operand, f);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                WalkExpr(e, f);
            return;
        }
    }

    static bool IsValidIdentifierKey(const std::string &s) {
        if (!IsBareIdentifier(s))
            return false;
        static const std::unordered_set<std::string> kw = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",   "in",
                                                           "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while"};
        return !kw.contains(s);
    }

    // Renaming a local to one of these would shadow a built-in, so it is always
    // refused -- even a getter-derived name that would read nicely (a module
    // named `GetStats` keeps its auto-name rather than shadowing the `stats`
    // global). Covers the Luau base library + standard library tables, plus the
    // Roblox runtime globals and datatype constructors a ModuleScript relies on.
    static bool IsBuiltinGlobal(const std::string &s) {
        static const std::unordered_set<std::string> globals = {
            // Luau base library
            "_G", "_VERSION", "assert", "collectgarbage", "error", "gcinfo", "getfenv", "getmetatable", "ipairs", "loadstring", "newproxy", "next", "pairs",
            "pcall", "print", "rawequal", "rawget", "rawlen", "rawset", "require", "select", "setfenv", "setmetatable", "tonumber", "tostring", "type",
            "typeof", "unpack", "xpcall",
            // Luau standard library tables
            "bit32", "buffer", "coroutine", "debug", "math", "os", "string", "table", "task", "utf8", "vector",
            // Roblox runtime globals (incl. deprecated/function globals)
            "game", "workspace", "script", "shared", "plugin", "Enum", "delay", "spawn", "tick", "time", "wait", "warn", "settings", "stats", "version",
            "elapsedTime", "UserSettings", "PluginManager", "DebuggerManager",
            // Roblox datatype constructors
            "Instance", "Vector3", "Vector3int16", "Vector2", "Vector2int16", "CFrame", "Color3", "ColorSequence", "ColorSequenceKeypoint", "BrickColor",
            "UDim", "UDim2", "Ray", "Rect", "Region3", "Region3int16", "TweenInfo", "NumberRange", "NumberSequence", "NumberSequenceKeypoint",
            "PhysicalProperties", "Faces", "Axes", "DateTime", "Random", "Font", "OverlapParams", "RaycastParams", "PathWaypoint", "Content"
        };
        return globals.contains(s);
    }
};
