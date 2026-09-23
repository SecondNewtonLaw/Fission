// Rewrites class and module field closures as method declarations when ownership is unambiguous.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ASTRewriter.hpp"

#include <cctype>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class ClassMethodRewriter : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        std::unordered_set<std::string> classes;
        std::unordered_set<std::string> moduleTables;
        for (const auto &stmt : stmts) {
            CollectClassesFromStatement(stmt, classes);
            CollectModuleTableFromStatement(stmt, moduleTables);
        }

        if (classes.empty() && moduleTables.empty())
            return;

        // Single forward sweep. For each statement, optionally take the
        // closure from its RHS (literal) or from a prior statement in the
        // same block (named decl / register assignment) and rewrite the
        // current statement into a method declaration. Tombstoned statements
        // are nulled out and filtered at the end.
        std::vector<bool> removed(stmts.size(), false);
        for (size_t i = 0; i < stmts.size(); ++i) {
            if (removed[i])
                continue;
            auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[i]);
            if (!asn)
                continue;
            auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(asn->left);
            if (!mem || !mem->table || !mem->key)
                continue;
            auto tblId = std::dynamic_pointer_cast<IdentifierExpressionNode>(mem->table);
            if (!tblId || !tblId->identifier)
                continue;
            const bool isClass = classes.contains(tblId->identifier->name);
            const bool isModule = moduleTables.contains(tblId->identifier->name);
            if (!isClass && !isModule)
                continue;
            auto keyStr = std::dynamic_pointer_cast<StringLiteralNode>(mem->key);
            if (!keyStr)
                continue;
            if (keyStr->value.size() >= 2 && keyStr->value[0] == '_' && keyStr->value[1] == '_')
                continue;
            // dot form prints the key bare (`function T.key`), so it must be a
            // legal identifier; the colon path keeps existing (looser) behavior.
            if (!isClass && !IsValidIdentifierKey(keyStr->value))
                continue;

            std::shared_ptr<FunctionDeclarationNode> fn;
            std::optional<size_t> sourceIndex;
            std::string sourceName;

            if (auto direct = std::dynamic_pointer_cast<FunctionDeclarationNode>(asn->right)) {
                fn = direct;
            } else if (auto rhsId = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->right); rhsId && rhsId->identifier) {
                sourceName = rhsId->identifier->name;
                auto found = FindClosureSource(stmts, removed, i, sourceName);
                if (!found.has_value())
                    continue;
                sourceIndex = found->first;
                fn = found->second;
                // Bail out if the same identifier is read again after this
                // assignment; we can't safely splice the only definition.
                if (IsIdentifierReadAfter(stmts, removed, i, sourceName))
                    continue;
                // Also bail if anything between the closure declaration and this
                // assignment reads the identifier (e.g. an earlier closure that
                // captures it) -- splicing the decl away would leave it unbound.
                if (IsIdentifierReadBetween(stmts, removed, *sourceIndex, i, sourceName))
                    continue;
            } else {
                continue;
            }

            if (!fn || !fn->lpFunctionBody)
                continue;

            // A class-table field is always a method (`function T:m`). A module-table field is a method
            // only when its first parameter is used as a receiver (`p.field` / `p:m()` / `p[k]`); then it
            // also becomes a colon method with `self`. A free function (first param used otherwise) stays
            // dot syntax with its parameters intact. The `:`/`.` definition style is pure sugar, so the
            // conversion is semantically identical and recompilable regardless of call sites.
            bool useColon = isClass;
            if (!useColon && isModule && fn->argumentCount >= 1) {
                const std::string firstArgName = ExtractArgName(fn->argumentsNames, 0);
                if (!firstArgName.empty() && FirstParamIsReceiver(fn->lpFunctionBody, firstArgName))
                    useColon = true;
            }

            if (useColon) {
                // colon method: needs a first param to become `self`.
                if (fn->argumentCount < 1)
                    continue;
                const std::string firstArgName = ExtractArgName(fn->argumentsNames, 0);
                if (firstArgName.empty())
                    continue;
                RenameIdentifiersInBlock(fn->lpFunctionBody, firstArgName, "self");

                std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> shifted;
                for (int32_t k = 1; k < fn->argumentCount; ++k) {
                    auto it = fn->argumentsNames.find(k);
                    if (it != fn->argumentsNames.end())
                        shifted[k - 1] = it->second;
                }
                if (fn->bIsVarArg) {
                    auto vit = fn->argumentsNames.find(fn->argumentCount);
                    if (vit != fn->argumentsNames.end())
                        shifted[fn->argumentCount - 1] = vit->second;
                }
                fn->argumentsNames = std::move(shifted);
                fn->argumentCount -= 1;
                fn->functionName = tblId->identifier->name + ":" + keyStr->value;
            } else {
                // module table: dot declaration, params untouched.
                fn->functionName = tblId->identifier->name + "." + keyStr->value;
            }
            fn->bIsLocalDeclaration = false;
            fn->bAnonymousInline = false;

            stmts[i] = fn;
            if (sourceIndex.has_value())
                removed[*sourceIndex] = true;
        }

        // Compact tombstoned slots.
        std::vector<std::shared_ptr<Statement>> filtered;
        filtered.reserve(stmts.size());
        for (size_t i = 0; i < stmts.size(); ++i)
            if (!removed[i])
                filtered.push_back(std::move(stmts[i]));
        stmts = std::move(filtered);
    }

  private:
    static void CollectClassesFromStatement(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &out) {
        if (!stmt)
            return;
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!asn)
            return;
        auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(asn->left);
        if (!mem || !mem->table || !mem->key)
            return;
        auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key);
        if (!key || key->value != "__index")
            return;
        auto lhsId = std::dynamic_pointer_cast<IdentifierExpressionNode>(mem->table);
        auto rhsId = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->right);
        if (!lhsId || !rhsId || !lhsId->identifier || !rhsId->identifier)
            return;
        if (lhsId->identifier->name == rhsId->identifier->name)
            out.insert(lhsId->identifier->name);
    }

    // A `local X = {...}` / `X = {...}` table-literal bind marks `X` as a module
    // table eligible for dot-declaration sugar (`function X.fn`). Constructed
    // tables only -- random globals receiving a closure are left untouched.
    static void CollectModuleTableFromStatement(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &out) {
        if (!stmt)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
            if (id && id->identifier && std::dynamic_pointer_cast<TableLiteralNode>(decl->value))
                out.insert(id->identifier->name);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
            if (id && id->identifier && std::dynamic_pointer_cast<TableLiteralNode>(asn->right))
                out.insert(id->identifier->name);
            return;
        }
    }

    // True if `s` is a bare Luau identifier (so `function T.s` parses) and not a
    // reserved word. Conservative: a non-identifier key stays an assignment.
    static bool IsValidIdentifierKey(const std::string &s) {
        if (s.empty())
            return false;
        if (!(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_'))
            return false;
        for (char c : s)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_'))
                return false;
        static const std::unordered_set<std::string> kw = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",   "in",
                                                           "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while"};
        return !kw.contains(s);
    }

    // Walk backwards from `before` (exclusive). Returns (index, fnDecl) of
    // the nearest statement that defines `name` as a function literal.
    // Recognised shapes:
    //   `local function name(...) end`          -> FunctionDeclarationNode
    //   `local name = function(...) end`        -> VariableDeclarationNode
    //   `name = function(...) end`              -> AssignmentStatementNode
    static std::optional<std::pair<size_t, std::shared_ptr<FunctionDeclarationNode>>>
    FindClosureSource(const std::vector<std::shared_ptr<Statement>> &stmts, const std::vector<bool> &removed, size_t before, const std::string &name) {
        for (size_t j = before; j-- > 0;) {
            if (removed[j])
                continue;
            const auto &s = stmts[j];
            if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s); fn && fn->bIsLocalDeclaration && fn->functionName == name)
                return std::make_pair(j, fn);
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
                if (id && id->identifier && id->identifier->name == name)
                    if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(decl->value))
                        return std::make_pair(j, fn);
            }
            if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
                auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
                if (id && id->identifier && id->identifier->name == name)
                    if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(asn->right))
                        return std::make_pair(j, fn);
            }
            // the nearest binding is not a closure; an older closure is no longer the value read
            if (StatementRedefinesIdentifier(s, name))
                return std::nullopt;
        }
        return std::nullopt;
    }

    // True if `name` is read anywhere after `from` (exclusive), up to (but
    // not including) the next statement that redefines `name`. A redefinition
    // creates a fresh binding -- later reads no longer alias the source we
    // are about to consume, so they shouldn't block the rewrite. Reads on
    // the current statement's RHS don't count -- that's the use we're
    // consuming.
    static bool
    IsIdentifierReadAfter(const std::vector<std::shared_ptr<Statement>> &stmts, const std::vector<bool> &removed, size_t from, const std::string &name) {
        for (size_t j = from + 1; j < stmts.size(); ++j) {
            if (removed[j])
                continue;
            if (StatementReadsIdentifier(stmts[j], name))
                return true;
            if (StatementRedefinesIdentifier(stmts[j], name))
                return false;
        }
        return false;
    }

    // True if `name` is read in the statements strictly between `after` and `before`
    // (exclusive on both ends) -- e.g. an earlier closure that captures the source we
    // are about to splice away. A redefinition in that window rebinds the name, so
    // reads past it no longer alias our source and stop blocking the rewrite.
    static bool IsIdentifierReadBetween(
        const std::vector<std::shared_ptr<Statement>> &stmts, const std::vector<bool> &removed, size_t after, size_t before, const std::string &name
    ) {
        for (size_t j = after + 1; j < before && j < stmts.size(); ++j) {
            if (removed[j])
                continue;
            if (StatementReadsIdentifier(stmts[j], name))
                return true;
            if (StatementRedefinesIdentifier(stmts[j], name))
                return false;
        }
        return false;
    }

    static bool StatementRedefinesIdentifier(const std::shared_ptr<Statement> &stmt, const std::string &name) {
        if (!stmt)
            return false;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->bIsLocalDeclaration && fn->functionName == name)
            return true;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
            return id && id->identifier && id->identifier->name == name;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
            return id && id->identifier && id->identifier->name == name;
        }
        if (auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(stmt)) {
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(compound->left);
            return id && id->identifier && id->identifier->name == name;
        }
        return false;
    }

    static bool StatementReadsIdentifier(const std::shared_ptr<Statement> &stmt, const std::string &name) {
        if (!stmt)
            return false;
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            if (ExpressionReadsIdentifier(ifS->condition, name))
                return true;
            if (BlockReadsIdentifier(ifS->thenBranch, name))
                return true;
            if (BlockReadsIdentifier(ifS->elseBranch, name))
                return true;
            return false;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (HasArgNamed(fn->argumentsNames, name))
                return false;
            return BlockReadsIdentifier(fn->lpFunctionBody, name);
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt))
            return ExpressionReadsIdentifier(w->condition, name) || BlockReadsIdentifier(w->body, name);
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt))
            return ExpressionReadsIdentifier(r->condition, name) || BlockReadsIdentifier(r->body, name);
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt))
            return ExpressionReadsIdentifier(fnum->startVariable, name) || ExpressionReadsIdentifier(fnum->maxIncreased, name) ||
                   ExpressionReadsIdentifier(fnum->increaseBy, name) || BlockReadsIdentifier(fnum->lpLoopBody, name);
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt))
            return ExpressionReadsIdentifier(fgen->generator, name) || BlockReadsIdentifier(fgen->body, name);
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            // A bare-identifier LHS (`v42 = ...`) is a write, not a read, and
            // doesn't extend `name`'s live range. Only descend when the LHS is
            // a member/index expression -- then `name` may appear in the
            // table/key position as a genuine read (`t[name] = ...`).
            if (!std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left))
                if (ExpressionReadsIdentifier(asn->left, name))
                    return true;
            return ExpressionReadsIdentifier(asn->right, name);
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            return ExpressionReadsIdentifier(decl->value, name);
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt))
            return ExpressionReadsIdentifier(es->expression, name);
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                if (ExpressionReadsIdentifier(v, name))
                    return true;
            return false;
        }
        if (auto expr = std::dynamic_pointer_cast<Expression>(stmt))
            return ExpressionReadsIdentifier(expr, name);
        return false;
    }

    static bool BlockReadsIdentifier(const std::shared_ptr<BlockStatementNode> &block, const std::string &name) {
        if (!block)
            return false;
        for (const auto &s : block->body)
            if (StatementReadsIdentifier(s, name))
                return true;
        return false;
    }

    static bool ExpressionReadsIdentifier(const std::shared_ptr<Expression> &expr, const std::string &name) {
        if (!expr)
            return false;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr))
            return id->identifier && id->identifier->name == name;
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr))
            return ExpressionReadsIdentifier(mem->table, name) || ExpressionReadsIdentifier(mem->key, name);
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr))
            return ExpressionReadsIdentifier(idx->left, name) || ExpressionReadsIdentifier(idx->right, name);
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            if (ExpressionReadsIdentifier(call->callee, name))
                return true;
            for (const auto &a : call->arguments)
                if (ExpressionReadsIdentifier(a, name))
                    return true;
            return false;
        }
        if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            if (ExpressionReadsIdentifier(nameCall->calledOn, name))
                return true;
            for (const auto &a : nameCall->arguments)
                if (ExpressionReadsIdentifier(a, name))
                    return true;
            return false;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr))
            return ExpressionReadsIdentifier(bin->left, name) || ExpressionReadsIdentifier(bin->right, name);
        if (auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr))
            return ExpressionReadsIdentifier(compound->left, name) || ExpressionReadsIdentifier(compound->right, name);
        if (auto ifExpr = std::dynamic_pointer_cast<IfExpressionNode>(expr))
            return ExpressionReadsIdentifier(ifExpr->condition, name) || ExpressionReadsIdentifier(ifExpr->thenExpr, name) ||
                   ExpressionReadsIdentifier(ifExpr->elseExpr, name);
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr))
            return ExpressionReadsIdentifier(un->operand, name);
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                if (ExpressionReadsIdentifier(e, name))
                    return true;
            return false;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            if (HasArgNamed(fn->argumentsNames, name))
                return false;
            return BlockReadsIdentifier(fn->lpFunctionBody, name);
        }
        return false;
    }

    static std::string ExtractArgName(const std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> &args, int32_t index) {
        auto it = args.find(index);
        if (it == args.end() || !it->second)
            return "";
        auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(it->second->argumentName);
        if (!ident || !ident->identifier)
            return "";
        return ident->identifier->name;
    }

    static void RenameIdentifiersInBlock(const std::shared_ptr<BlockStatementNode> &block, const std::string &from, const std::string &to) {
        if (!block)
            return;
        for (auto &s : block->body)
            RenameIdentifiersInStatement(s, from, to);
    }

    static void RenameIdentifiersInStatement(const std::shared_ptr<Statement> &stmt, const std::string &from, const std::string &to) {
        if (!stmt)
            return;
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            RenameIdentifiersInExpression(ifS->condition, from, to);
            RenameIdentifiersInBlock(ifS->thenBranch, from, to);
            RenameIdentifiersInBlock(ifS->elseBranch, from, to);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (HasArgNamed(fn->argumentsNames, from))
                return;
            RenameIdentifiersInBlock(fn->lpFunctionBody, from, to);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            RenameIdentifiersInExpression(w->condition, from, to);
            RenameIdentifiersInBlock(w->body, from, to);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            RenameIdentifiersInExpression(r->condition, from, to);
            RenameIdentifiersInBlock(r->body, from, to);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            RenameIdentifiersInBlock(fnum->lpLoopBody, from, to);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            RenameIdentifiersInBlock(fgen->body, from, to);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            RenameIdentifiersInExpression(asn->left, from, to);
            RenameIdentifiersInExpression(asn->right, from, to);
            return;
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            RenameIdentifiersInExpression(decl->value, from, to);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            RenameIdentifiersInExpression(es->expression, from, to);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (auto &v : ret->returnValues)
                RenameIdentifiersInExpression(v, from, to);
            return;
        }
        if (auto expr = std::dynamic_pointer_cast<Expression>(stmt))
            RenameIdentifiersInExpression(expr, from, to);
    }

    static void RenameIdentifiersInExpression(const std::shared_ptr<Expression> &expr, const std::string &from, const std::string &to) {
        if (!expr)
            return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr)) {
            if (id->identifier && id->identifier->name == from)
                id->identifier->name = to;
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            RenameIdentifiersInExpression(mem->table, from, to);
            RenameIdentifiersInExpression(mem->key, from, to);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            RenameIdentifiersInExpression(idx->left, from, to);
            RenameIdentifiersInExpression(idx->right, from, to);
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            RenameIdentifiersInExpression(call->callee, from, to);
            for (auto &a : call->arguments)
                RenameIdentifiersInExpression(a, from, to);
            return;
        }
        if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            RenameIdentifiersInExpression(nameCall->calledOn, from, to);
            for (auto &a : nameCall->arguments)
                RenameIdentifiersInExpression(a, from, to);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            RenameIdentifiersInExpression(bin->left, from, to);
            RenameIdentifiersInExpression(bin->right, from, to);
            return;
        }
        if (auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            RenameIdentifiersInExpression(compound->left, from, to);
            RenameIdentifiersInExpression(compound->right, from, to);
            return;
        }
        if (auto ifExpr = std::dynamic_pointer_cast<IfExpressionNode>(expr)) {
            RenameIdentifiersInExpression(ifExpr->condition, from, to);
            RenameIdentifiersInExpression(ifExpr->thenExpr, from, to);
            RenameIdentifiersInExpression(ifExpr->elseExpr, from, to);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            RenameIdentifiersInExpression(un->operand, from, to);
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &e : tbl->expressions)
                RenameIdentifiersInExpression(e, from, to);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            if (HasArgNamed(fn->argumentsNames, from))
                return;
            RenameIdentifiersInBlock(fn->lpFunctionBody, from, to);
            return;
        }
    }

    // True if `name` is used as a receiver in the body: the base of a member access (`name.field`), an
    // index (`name[k]`), or a method call (`name:m()`). That marks the first parameter as `self`.
    static bool FirstParamIsReceiver(const std::shared_ptr<BlockStatementNode> &block, const std::string &name) { return BlockUsesAsReceiver(block, name); }

    static bool IsIdent(const std::shared_ptr<Expression> &e, const std::string &name) {
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e);
        return id && id->identifier && id->identifier->name == name;
    }

    static bool ExprUsesAsReceiver(const std::shared_ptr<Expression> &expr, const std::string &name) {
        if (!expr)
            return false;
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            if (IsIdent(mem->table, name))
                return true;
            return ExprUsesAsReceiver(mem->table, name) || ExprUsesAsReceiver(mem->key, name);
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            if (IsIdent(idx->left, name))
                return true;
            return ExprUsesAsReceiver(idx->left, name) || ExprUsesAsReceiver(idx->right, name);
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            if (IsIdent(nc->calledOn, name))
                return true;
            if (ExprUsesAsReceiver(nc->calledOn, name))
                return true;
            for (const auto &a : nc->arguments)
                if (ExprUsesAsReceiver(a, name))
                    return true;
            return false;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            if (ExprUsesAsReceiver(call->callee, name))
                return true;
            for (const auto &a : call->arguments)
                if (ExprUsesAsReceiver(a, name))
                    return true;
            return false;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr))
            return ExprUsesAsReceiver(bin->left, name) || ExprUsesAsReceiver(bin->right, name);
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr))
            return ExprUsesAsReceiver(un->operand, name);
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                if (ExprUsesAsReceiver(e, name))
                    return true;
            return false;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            if (HasArgNamed(fn->argumentsNames, name))
                return false; // shadowed by a nested function's own parameter
            return BlockUsesAsReceiver(fn->lpFunctionBody, name);
        }
        return false;
    }

    static bool StmtUsesAsReceiver(const std::shared_ptr<Statement> &stmt, const std::string &name) {
        if (!stmt)
            return false;
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt))
            return ExprUsesAsReceiver(ifS->condition, name) || BlockUsesAsReceiver(ifS->thenBranch, name) || BlockUsesAsReceiver(ifS->elseBranch, name);
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (HasArgNamed(fn->argumentsNames, name))
                return false;
            return BlockUsesAsReceiver(fn->lpFunctionBody, name);
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt))
            return ExprUsesAsReceiver(w->condition, name) || BlockUsesAsReceiver(w->body, name);
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt))
            return ExprUsesAsReceiver(r->condition, name) || BlockUsesAsReceiver(r->body, name);
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt))
            return BlockUsesAsReceiver(fnum->lpLoopBody, name);
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt))
            return ExprUsesAsReceiver(fgen->generator, name) || BlockUsesAsReceiver(fgen->body, name);
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            return ExprUsesAsReceiver(asn->left, name) || ExprUsesAsReceiver(asn->right, name);
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            return ExprUsesAsReceiver(decl->value, name);
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt))
            return ExprUsesAsReceiver(es->expression, name);
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                if (ExprUsesAsReceiver(v, name))
                    return true;
            return false;
        }
        return false;
    }

    static bool BlockUsesAsReceiver(const std::shared_ptr<BlockStatementNode> &block, const std::string &name) {
        if (!block)
            return false;
        for (const auto &s : block->body)
            if (StmtUsesAsReceiver(s, name))
                return true;
        return false;
    }

    static bool HasArgNamed(const std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> &args, const std::string &name) {
        for (const auto &[_, arg] : args) {
            if (!arg)
                continue;
            auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(arg->argumentName);
            if (id && id->identifier && id->identifier->name == name)
                return true;
        }
        return false;
    }
};
