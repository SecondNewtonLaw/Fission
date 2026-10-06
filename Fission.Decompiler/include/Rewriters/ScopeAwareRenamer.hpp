// Applies detector-proposed names within lexical bindings while blocking collisions and shadows.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"

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

    static void CollectGlobalNames(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &out) {
        auto collect = [&](const std::shared_ptr<Identifier> &id) {
            if (id && id->bIsGlobal)
                out.insert(id->name);
        };
        WalkStmt(stmt, collect);
    }

    // Closures reachable from `stmt` without entering another closure; each is a scope of its own.
    static void CollectNestedFunctions(const std::shared_ptr<Statement> &stmt, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        ForEachPart(stmt, [&](const auto &s) { CollectNestedFunctions(s, out); }, [&](const auto &e) { CollectNestedFunctionsExpr(e, out); });
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

    template <typename Visit> static void ForEachLoopBinding(const std::shared_ptr<Statement> &stmt, Visit &&visit) {
        if (stmt->nodeKind == ASTNodeKind::ForNumeric)
            visit(std::static_pointer_cast<ForNumericNode>(stmt)->loopVariable);
        else if (stmt->nodeKind == ASTNodeKind::ForGeneral)
            for (const auto &lv : std::static_pointer_cast<ForGeneralNode>(stmt)->loopVariables)
                visit(lv);
    }

    // Visits every expression `stmt` owns, loop bindings included, then each statement nested in its blocks.
    template <class OnStatement, class OnExpression>
    static void ForEachPart(const std::shared_ptr<Statement> &stmt, OnStatement &&onStatement, OnExpression &&onExpression) {
        if (!stmt)
            return;
        if (stmt->nodeKind == ASTNodeKind::ExpressionStatement) {
            const auto es = std::static_pointer_cast<ExpressionStatementNode>(stmt);
            onExpression(es->expression);
            return;
        }
        if (auto e = AsExpression(stmt)) {
            onExpression(e);
            return;
        }
        if (stmt->nodeKind == ASTNodeKind::FunctionArgument) {
            const auto fa = std::static_pointer_cast<FunctionArgumentExpression>(stmt);
            onExpression(fa->argumentName);
            return;
        }
        ForEachLoopBinding(stmt, onExpression);
        ForEachStatementExpression(stmt, onExpression);
        ForEachChildBlock(stmt, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &s : body)
                onStatement(s);
        });
    }

    static void CollectNestedFunctionsExpr(const std::shared_ptr<Expression> &expr, std::vector<std::shared_ptr<FunctionDeclarationNode>> &out) {
        if (expr && expr->nodeKind == ASTNodeKind::FunctionDeclarationNode)
            out.push_back(std::static_pointer_cast<FunctionDeclarationNode>(expr)); // a new scope; do NOT descend (recursion handles it)
        else if (expr)
            ForEachSubExpression(expr, [&](const std::shared_ptr<Expression> &child) { CollectNestedFunctionsExpr(child, out); });
    }

    static void CountScopeDecls(const std::shared_ptr<Statement> &stmt, std::unordered_map<std::string, int> &out) {
        if (!stmt)
            return;
        if (stmt->nodeKind == ASTNodeKind::ExpressionStatement) {
            const auto es = std::static_pointer_cast<ExpressionStatementNode>(stmt);
            CountDeclExpr(es->expression, out);
            return;
        }
        if (stmt->nodeKind == ASTNodeKind::VariableDeclaration) {
            const auto decl = std::static_pointer_cast<VariableDeclarationNode>(stmt);
            CountDeclName(decl->identifier, out);
            return;
        }
        if (stmt->nodeKind == ASTNodeKind::FunctionDeclarationNode) {
            const auto fn = std::static_pointer_cast<FunctionDeclarationNode>(stmt);
            if (fn->bIsLocalDeclaration && IsBareIdentifier(fn->functionName))
                out[fn->functionName]++;
            return; // do NOT descend into the function body
        }
        ForEachLoopBinding(stmt, [&](const std::shared_ptr<Expression> &binding) { CountDeclName(binding, out); });
        ForEachChildBlock(stmt, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &s : body)
                CountScopeDecls(s, out);
        });
    }

    static void CountDeclExpr(const std::shared_ptr<Expression> &expr, std::unordered_map<std::string, int> &out) {
        if (!expr)
            return;
        if (expr->nodeKind == ASTNodeKind::CallExpression) {
            const auto call = std::static_pointer_cast<CallExpressionNode>(expr);
            if (!call->bIsLocalDeclaration)
                return;
            for (const auto &r : call->rets)
                CountDeclName(r, out);
        } else if (expr->nodeKind == ASTNodeKind::MethodCallExpression) {
            const auto nc = std::static_pointer_cast<NameCallExpressionNode>(expr);
            if (!nc->bIsLocalDeclaration)
                return;
            for (const auto &r : nc->rets)
                CountDeclName(r, out);
        }
    }

    static void CountDeclName(const std::shared_ptr<Expression> &e, std::unordered_map<std::string, int> &out) {
        if (e && e->nodeKind == ASTNodeKind::IdentifierExpression) {
            const auto id = std::static_pointer_cast<IdentifierExpressionNode>(e);
            if (id->identifier)
                out[id->identifier->name]++;
        }
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
        ForEachPart(stmt, [&](const auto &s) { RenameStmt(s, rename, shadowed); }, [&](const auto &e) { RenameExpr(e, rename, shadowed); });
    }

    static void RenameExpr(
        const std::shared_ptr<Expression> &expr, const std::unordered_map<std::string, std::string> &rename, const std::unordered_set<std::string> &shadowed
    ) {
        if (!expr)
            return;
        if (expr->nodeKind == ASTNodeKind::IdentifierExpression) {
            const auto id = std::static_pointer_cast<IdentifierExpressionNode>(expr);
            if (id->identifier && !shadowed.contains(id->identifier->name)) {
                auto it = rename.find(id->identifier->name);
                if (it != rename.end())
                    id->identifier->name = it->second;
            }
            return;
        }
        if (expr->nodeKind == ASTNodeKind::FunctionDeclarationNode) {
            const auto fn = std::static_pointer_cast<FunctionDeclarationNode>(expr);
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
        ForEachSubExpression(expr, [&](const std::shared_ptr<Expression> &child) { RenameExpr(child, rename, shadowed); });
    }

    template <class F> static void WalkStmt(const std::shared_ptr<Statement> &stmt, F &f) {
        ForEachPart(stmt, [&](const auto &s) { WalkStmt(s, f); }, [&](const auto &e) { WalkExpr(e, f); });
    }

    template <class F> static void WalkExpr(const std::shared_ptr<Expression> &expr, F &f) {
        if (!expr)
            return;
        if (expr->nodeKind == ASTNodeKind::IdentifierExpression) {
            const auto id = std::static_pointer_cast<IdentifierExpressionNode>(expr);
            f(id->identifier);
            return;
        }
        if (expr->nodeKind == ASTNodeKind::FunctionDeclarationNode) {
            const auto fn = std::static_pointer_cast<FunctionDeclarationNode>(expr);
            for (const auto &[idx, arg] : fn->argumentsNames)
                if (arg)
                    WalkExpr(arg->argumentName, f);
            WalkStmt(fn->lpFunctionBody, f);
            return;
        }
        ForEachSubExpression(expr, [&](const std::shared_ptr<Expression> &child) { WalkExpr(child, f); });
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
