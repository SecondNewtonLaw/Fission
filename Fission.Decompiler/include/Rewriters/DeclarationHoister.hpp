// Places generated register declarations in the lowest scope dominating every access.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"
#include "FissionAllocator.hpp"

#include <algorithm>
#include <memory>
#include <ranges>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DeclarationHoister {
  public:
    // true when `statement` may read or write `name`
    static bool Mentions(const std::shared_ptr<Statement> &statement, const std::string &name) {
        MentionSet set;
        CollectStatementMentions(statement, set);
        return set.everything || set.names.contains(name);
    }

    static bool ExpressionMentions(const std::shared_ptr<Expression> &expression, const std::string &name) {
        MentionSet set;
        CollectExprMentions(expression, set);
        return set.everything || set.names.contains(name);
    }

    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        std::vector<FunctionWork> functions{{&statements, {}}};
        for (size_t i = 0; i < functions.size(); ++i) {
            SplitRecursiveInitializers(*functions[i].body);
            m_excludedNames = functions[i].capturedNames;
            m_locallyCapturedNames.clear();
            const auto resetWalk = [&]() {
                m_scopes.clear();
                m_subtreeEnd.clear();
                m_mentions.clear();
                m_access.clear();
                m_bareAssigned.clear();
                m_bareAssignmentScopes.clear();
                m_declCount.clear();
                m_declScope.clear();
                m_scopedBindings.clear();
                m_accessSites.clear();
                m_readSites.clear();
                m_writeSites.clear();
                m_declSites.clear();
                m_nestedFunctions.clear();
                m_capturedNames.clear();
                m_walkOrder = 0;
                m_namedCaptureDiscovered = false;
            };

            resetWalk();
            int root = NewScope(/*parent*/ -1, /*isLoopBody*/ false, functions[i].body);
            WalkBlock(*functions[i].body, root);
            if (m_namedCaptureDiscovered) {
                resetWalk();
                root = NewScope(/*parent*/ -1, /*isLoopBody*/ false, functions[i].body);
                WalkBlock(*functions[i].body, root);
            }

            InsertMissingDeclarations();
            DemoteDeepDeclarations();
            InsertMultiDeclarationFallbacks();
            for (auto &scope : m_scopes)
                CoalesceAdjacentDeclarations(*scope.body);
            for (auto &scope : m_scopes)
                PredeclareRepeatConditionLocals(*scope.body);
            m_mentions.clear();
            functions.insert(functions.end(), m_nestedFunctions.begin(), m_nestedFunctions.end());
        }
    }

  private:
    struct FunctionWork {
        std::vector<std::shared_ptr<Statement>> *body;
        std::unordered_set<std::string> capturedNames;
    };

    struct ScopeInfo {
        int parent;
        int depth;
        bool isLoopBody; // a while/repeat/for body: a decl here re-runs every iteration
        std::vector<std::shared_ptr<Statement>> *body;
    };

    std::vector<ScopeInfo> m_scopes;
    // per scope, the id of its last descendant
    mutable std::vector<int> m_subtreeEnd;
    // what each queried statement mentions (StatementMentions); held by pointer so an entry never outlives its node
    struct MentionSet {
        std::unordered_set<std::string> names;
        bool everything = false;
    };
    std::unordered_map<std::shared_ptr<Statement>, MentionSet> m_mentions;
    // name -> the set of scope ids in which it is read or written
    std::unordered_map<std::string, std::unordered_set<int>> m_access;
    // names that appear as the lhs of a bare `name = expr` assignment. Only these are safe to insert
    // an orphan `local name` for: their presence proves the lifter is naming the register `vN`
    // consistently (a debug-named local whose uses merely read as `vN` has no such bare assignment,
    // and inserting for it would spuriously double-declare).
    std::unordered_set<std::string> m_bareAssigned;
    std::unordered_map<std::string, std::unordered_set<int>> m_bareAssignmentScopes;
    // name -> number of `local name` declarations seen, and the scope id of the (single) one. M3
    // demotion only touches names with exactly one decl (multiple decls == deliberate shadowing).
    std::unordered_map<std::string, int> m_declCount;
    std::unordered_map<std::string, int> m_declScope;
    std::unordered_map<std::string, std::unordered_set<int>> m_scopedBindings;
    std::unordered_map<std::string, std::vector<std::pair<int, size_t>>> m_accessSites;
    std::unordered_map<std::string, std::vector<std::pair<int, size_t>>> m_readSites;
    std::unordered_map<std::string, std::vector<std::pair<int, size_t>>> m_writeSites;
    std::unordered_map<std::string, std::vector<std::pair<int, size_t>>> m_declSites;
    bool m_recordingWrite{};
    std::vector<FunctionWork> m_nestedFunctions;
    std::unordered_set<std::string> m_excludedNames;
    std::unordered_set<std::string> m_locallyCapturedNames;
    std::unordered_set<std::string> m_capturedNames;
    bool m_namedCaptureDiscovered{};
    size_t m_walkOrder{};
    size_t m_currentOrder{};

    static void SplitRecursiveInitializers(std::vector<std::shared_ptr<Statement>> &statements) {
        for (size_t i = 0; i < statements.size(); ++i) {
            const auto &statement = statements[i];
            if (statement && statement->nodeKind == ASTNodeKind::VariableDeclaration) {
                const auto declaration = std::static_pointer_cast<VariableDeclarationNode>(statement);
                const auto name = DeclName(declaration);
                const auto function = declaration->value && declaration->value->nodeKind == ASTNodeKind::FunctionDeclarationNode
                                          ? std::static_pointer_cast<FunctionDeclarationNode>(declaration->value)
                                          : nullptr;
                if (function && function->capturedNames.contains(name)) {
                    auto value = declaration->value;
                    declaration->value.reset();
                    statements.insert(
                        statements.begin() + static_cast<std::ptrdiff_t>(++i), Fission::MakeShared<AssignmentStatementNode>(declaration->identifier, value)
                    );
                }
                continue;
            }
            ForEachChildBlock(statement, SplitRecursiveInitializers);
        }
    }

    void RecordDecl(const std::string &name, int scopeId) {
        if (IsTrackedBinding(name)) {
            m_declCount[name]++;
            m_declScope[name] = scopeId;
            m_declSites[name].emplace_back(scopeId, m_currentOrder);
        }
    }

    void RecordScopedBinding(const std::string &name, int scopeId) {
        if (IsTrackedBinding(name))
            m_scopedBindings[name].insert(scopeId);
    }

    int NewScope(int parent, bool isLoopBody, std::vector<std::shared_ptr<Statement>> *body) {
        const int depth = parent < 0 ? 0 : m_scopes[parent].depth + 1;
        m_scopes.push_back(ScopeInfo{parent, depth, isLoopBody, body});
        return static_cast<int>(m_scopes.size() - 1);
    }

    // A generated register local is `vN`, optionally with an upvalue-collision suffix (`vN_M`), a collision prefix (`_vN`),
    // or the reserved prefix of a debug name spelled like one.
    static bool IsRegisterName(std::string_view s) {
        if (s.starts_with(kReservedNamePrefix))
            s.remove_prefix(kReservedNamePrefix.size());
        while (!s.empty() && s[0] == '_')
            s.remove_prefix(1);
        if (s.size() < 2 || s[0] != 'v')
            return false;
        size_t i = 1;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9')
            ++i;
        if (i == s.size())
            return true;
        if (s[i++] != '_' || i == s.size())
            return false;
        for (; i < s.size(); ++i)
            if (s[i] < '0' || s[i] > '9')
                return false;
        return true;
    }

    bool IsOwnedRegisterName(const std::string &name) const { return IsRegisterName(name) && !m_excludedNames.contains(name); }

    bool IsTrackedBinding(const std::string &name) const {
        return (IsRegisterName(name) || m_locallyCapturedNames.contains(name)) && !m_excludedNames.contains(name);
    }

    void RecordAccess(const std::string &name, int scopeId) {
        if (IsOwnedRegisterName(name)) {
            m_access[name].insert(scopeId);
            m_accessSites[name].emplace_back(scopeId, m_currentOrder);
            (m_recordingWrite ? m_writeSites : m_readSites)[name].emplace_back(scopeId, m_currentOrder);
        }
    }

    void RecordTrackedAccess(const std::string &name, int scopeId) {
        if (IsTrackedBinding(name)) {
            m_access[name].insert(scopeId);
            m_accessSites[name].emplace_back(scopeId, m_currentOrder);
            (m_recordingWrite ? m_writeSites : m_readSites)[name].emplace_back(scopeId, m_currentOrder);
        }
    }

    void CollectWriteTarget(const std::shared_ptr<Expression> &e, int scopeId) {
        m_recordingWrite = e && e->nodeKind == ASTNodeKind::IdentifierExpression;
        CollectExpr(e, scopeId);
        m_recordingWrite = false;
    }

    static std::shared_ptr<Expression> LocalDeclCall(const std::shared_ptr<Statement> &stmt) {
        auto expression = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt);
        if (!expression)
            return nullptr;
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration && !call->rets.empty())
            return call;
        if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration && !call->rets.empty())
            return call;
        return nullptr;
    }

    static std::string SingleRetName(const std::vector<std::shared_ptr<Expression>> &rets, bool &multi) {
        multi = rets.size() != 1;
        if (multi)
            return {};
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(rets.front());
        if (!id || !id->identifier) {
            multi = true;
            return {};
        }
        return id->identifier->name;
    }

    static void DemoteLocal(std::shared_ptr<Statement> &stmt) {
        if (auto expression = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expression->expression))
                call->bIsLocalDeclaration = false;
            else if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(expression->expression))
                call->bIsLocalDeclaration = false;
        }
    }

    // collect every identifier read inside an expression as an access in `scopeId`.
    void CollectExpr(const std::shared_ptr<Expression> &e, int scopeId) {
        if (!e)
            return;
        switch (e->nodeKind) {
        case ASTNodeKind::IdentifierExpression:
            if (auto id = std::static_pointer_cast<IdentifierExpressionNode>(e); id->identifier && !id->identifier->bIsGlobal)
                RecordTrackedAccess(id->identifier->name, scopeId);
            break;
        case ASTNodeKind::Identifier:
            if (auto id = std::dynamic_pointer_cast<Identifier>(e))
                RecordAccess(id->name, scopeId);
            break;
        case ASTNodeKind::BinaryExpression:
        case ASTNodeKind::TableBinaryExpression: {
            auto b = std::static_pointer_cast<BinaryExpressionNode>(e);
            CollectExpr(b->left, scopeId);
            CollectExpr(b->right, scopeId);
            break;
        }
        case ASTNodeKind::CompoundAssignment: {
            auto b = std::static_pointer_cast<CompoundBinaryExpressionNode>(e);
            CollectExpr(b->left, scopeId);
            CollectExpr(b->right, scopeId);
            break;
        }
        case ASTNodeKind::UnaryExpression:
            CollectExpr(std::static_pointer_cast<UnaryExpressionNode>(e)->operand, scopeId);
            break;
        case ASTNodeKind::IfExpression: {
            auto conditional = std::static_pointer_cast<IfExpressionNode>(e);
            CollectExpr(conditional->condition, scopeId);
            CollectExpr(conditional->thenExpr, scopeId);
            CollectExpr(conditional->elseExpr, scopeId);
            break;
        }
        case ASTNodeKind::IndexExpression: {
            auto ix = std::static_pointer_cast<IndexExpressionNode>(e);
            CollectExpr(ix->left, scopeId);
            CollectExpr(ix->right, scopeId);
            break;
        }
        case ASTNodeKind::MemberExpression: {
            auto m = std::static_pointer_cast<MemberExpressionNode>(e);
            CollectExpr(m->table, scopeId);
            CollectExpr(m->key, scopeId);
            break;
        }
        case ASTNodeKind::CallExpression: {
            auto c = std::static_pointer_cast<CallExpressionNode>(e);
            CollectExpr(c->callee, scopeId);
            for (const auto &a : c->arguments)
                CollectExpr(a, scopeId);
            for (const auto &r : c->rets)
                CollectWriteTarget(r, scopeId);
            break;
        }
        case ASTNodeKind::MethodCallExpression: {
            auto c = std::static_pointer_cast<NameCallExpressionNode>(e);
            CollectExpr(c->calledOn, scopeId);
            CollectExpr(c->callWhat, scopeId);
            for (const auto &a : c->arguments)
                CollectExpr(a, scopeId);
            for (const auto &r : c->rets)
                CollectWriteTarget(r, scopeId);
            break;
        }
        case ASTNodeKind::LiteralValue:
            // only table literals carry sub-expressions
            if (auto t = AsLiteral<TableLiteralNode>(e))
                for (const auto &el : t->expressions)
                    CollectExpr(el, scopeId);
            break;
        case ASTNodeKind::FunctionDeclarationNode: {
            auto fn = std::static_pointer_cast<FunctionDeclarationNode>(e);
            if (fn->lpFunctionBody) {
                for (const auto &name : fn->capturedNames) {
                    if (!IsRegisterName(name) && !m_excludedNames.contains(name)) {
                        m_locallyCapturedNames.insert(name);
                        m_namedCaptureDiscovered = true;
                    }
                    RecordTrackedAccess(name, scopeId);
                    m_capturedNames.insert(name);
                }
                m_nestedFunctions.push_back({&fn->lpFunctionBody->body, BoundOutsideBody(*fn)});
            }
            break;
        }
        default:
            break;
        }
    }

    // walk a real scope: record decls + accesses at scopeId, descend into child scopes.
    void WalkBlock(std::vector<std::shared_ptr<Statement>> &stmts, int scopeId) {
        for (auto &s : stmts)
            WalkStmt(s, scopeId);
    }

    void WalkStmt(const std::shared_ptr<Statement> &s, int scopeId) {
        if (!s)
            return;
        m_currentOrder = m_walkOrder++;

        switch (s->nodeKind) {
        case ASTNodeKind::VariableDeclaration: {
            const auto decl = std::static_pointer_cast<VariableDeclarationNode>(s);
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier) {
                RecordDecl(id->identifier->name, scopeId);
                m_recordingWrite = true;
                RecordTrackedAccess(id->identifier->name, scopeId);
                m_recordingWrite = false;
            }
            CollectExpr(decl->value, scopeId);
            return;
        }
        case ASTNodeKind::AssignmentStatement: {
            const auto asn = std::static_pointer_cast<AssignmentStatementNode>(s);
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
                id && id->identifier && !id->identifier->bIsGlobal && IsTrackedBinding(id->identifier->name)) {
                m_bareAssigned.insert(id->identifier->name);
                m_bareAssignmentScopes[id->identifier->name].insert(scopeId);
            }
            CollectWriteTarget(asn->left, scopeId);
            CollectExpr(asn->right, scopeId);
            return;
        }
        case ASTNodeKind::CompoundAssignment: {
            const auto compound = std::static_pointer_cast<CompoundBinaryExpressionNode>(s);
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(compound->left);
                id && id->identifier && !id->identifier->bIsGlobal && IsTrackedBinding(id->identifier->name)) {
                m_bareAssigned.insert(id->identifier->name);
                m_bareAssignmentScopes[id->identifier->name].insert(scopeId);
            }
            CollectExpr(compound, scopeId);
            return;
        }
        case ASTNodeKind::ExpressionStatement: {
            const auto es = std::static_pointer_cast<ExpressionStatementNode>(s);
            const auto recordReturns = [&](const std::vector<std::shared_ptr<Expression>> &rets, bool local) {
                for (const auto &ret : rets) {
                    const auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(ret);
                    if (!id || !id->identifier)
                        continue;
                    const auto &name = id->identifier->name;
                    if (local) {
                        RecordDecl(name, scopeId);
                        RecordTrackedAccess(name, scopeId);
                    } else if (IsTrackedBinding(name)) {
                        m_bareAssigned.insert(name);
                        m_bareAssignmentScopes[name].insert(scopeId);
                    }
                }
            };
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression))
                recordReturns(call->rets, call->bIsLocalDeclaration);
            else if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression))
                recordReturns(call->rets, call->bIsLocalDeclaration);
            CollectExpr(es->expression, scopeId);
            return;
        }
        case ASTNodeKind::ReturnExpression: {
            const auto ret = std::static_pointer_cast<ReturnStatementNode>(s);
            for (const auto &v : ret->returnValues)
                CollectExpr(v, scopeId);
            return;
        }
        case ASTNodeKind::IfStatement: {
            const auto iff = std::static_pointer_cast<IfStatementNode>(s);
            CollectExpr(iff->condition, scopeId);
            if (iff->thenBranch) {
                const int c = NewScope(scopeId, m_scopes[scopeId].isLoopBody, &iff->thenBranch->body);
                WalkBlock(iff->thenBranch->body, c);
            }
            if (iff->elseBranch) {
                const int c = NewScope(scopeId, m_scopes[scopeId].isLoopBody, &iff->elseBranch->body);
                WalkBlock(iff->elseBranch->body, c);
            }
            return;
        }
        case ASTNodeKind::WhileStatement: {
            const auto w = std::static_pointer_cast<WhileStatementNode>(s);
            CollectExpr(w->condition, scopeId);
            if (w->body) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &w->body->body);
                WalkBlock(w->body->body, c);
            }
            return;
        }
        case ASTNodeKind::RepeatStatement: {
            const auto r = std::static_pointer_cast<RepeatStatementNode>(s);
            if (r->body) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &r->body->body);
                WalkBlock(r->body->body, c);
                CollectExpr(r->condition, c); // until-cond is inside the body scope (Luau)
            } else {
                CollectExpr(r->condition, scopeId);
            }
            return;
        }
        case ASTNodeKind::ForNumeric: {
            const auto fn = std::static_pointer_cast<ForNumericNode>(s);
            CollectExpr(fn->startVariable, scopeId);
            CollectExpr(fn->increaseBy, scopeId);
            CollectExpr(fn->maxIncreased, scopeId);
            if (fn->lpLoopBody) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &fn->lpLoopBody->body);
                if (auto lv = std::dynamic_pointer_cast<IdentifierExpressionNode>(fn->loopVariable); lv && lv->identifier)
                    RecordScopedBinding(lv->identifier->name, c);
                WalkBlock(fn->lpLoopBody->body, c);
            }
            return;
        }
        case ASTNodeKind::ForGeneral: {
            const auto fg = std::static_pointer_cast<ForGeneralNode>(s);
            CollectExpr(fg->generator, scopeId);
            CollectExpr(fg->state, scopeId);
            CollectExpr(fg->index, scopeId);
            if (fg->body) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &fg->body->body);
                for (const auto &lv : fg->loopVariables)
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(lv); id && id->identifier)
                        RecordScopedBinding(id->identifier->name, c);
                WalkBlock(fg->body->body, c);
            }
            return;
        }
        case ASTNodeKind::FunctionDeclarationNode: {
            const auto fdn = std::static_pointer_cast<FunctionDeclarationNode>(s);
            // `local function vN()` declares vN in this scope (it is a FunctionDeclarationNode, not a
            // VariableDeclarationNode); record it so we never insert a duplicate decl for it.
            if (fdn->bIsLocalDeclaration && IsTrackedBinding(fdn->functionName)) {
                RecordDecl(fdn->functionName, scopeId);
                RecordTrackedAccess(fdn->functionName, scopeId);
            }
            if (fdn->lpFunctionBody) {
                for (const auto &name : fdn->capturedNames) {
                    if (!IsRegisterName(name) && !m_excludedNames.contains(name)) {
                        m_locallyCapturedNames.insert(name);
                        m_namedCaptureDiscovered = true;
                    }
                    RecordTrackedAccess(name, scopeId);
                    m_capturedNames.insert(name);
                }
                m_nestedFunctions.push_back({&fdn->lpFunctionBody->body, BoundOutsideBody(*fdn)});
            }
            return;
        }
        case ASTNodeKind::BlockStatement: {
            const auto blk = std::static_pointer_cast<BlockStatementNode>(s);
            WalkBlock(blk->body, scopeId);
            return;
        }
        default:
            break;
        }
    }

    int LCA(int a, int b) const {
        while (a != b) {
            if (a < 0 || b < 0)
                return -1;
            if (m_scopes[a].depth > m_scopes[b].depth)
                a = m_scopes[a].parent;
            else if (m_scopes[b].depth > m_scopes[a].depth)
                b = m_scopes[b].parent;
            else {
                a = m_scopes[a].parent;
                b = m_scopes[b].parent;
            }
        }
        return a;
    }

    bool IsWithinScopedBinding(const std::string &name, int scopeId) const {
        const auto bindings = m_scopedBindings.find(name);
        if (bindings == m_scopedBindings.end())
            return false;
        return std::any_of(bindings->second.begin(), bindings->second.end(), [&](int bindingScope) { return IsAncestorOrSelf(bindingScope, scopeId); });
    }

    bool HasUnboundBareAssignment(const std::string &name) const {
        const auto assignments = m_bareAssignmentScopes.find(name);
        if (assignments == m_bareAssignmentScopes.end())
            return false;
        return std::any_of(assignments->second.begin(), assignments->second.end(), [&](int scopeId) { return !IsWithinScopedBinding(name, scopeId); });
    }

    static void InsertLeadingDeclaration(std::vector<std::shared_ptr<Statement>> &body, const std::string &name) {
        const auto position =
            std::find_if(body.begin(), body.end(), [](const auto &statement) { return !statement || statement->nodeKind != ASTNodeKind::Comment; });
        body.insert(position, Fission::MakeShared<VariableDeclarationNode>(Fission::MakeShared<Identifier>(name)));
    }

    bool AllAccessesCovered(const std::string &name) {
        const auto declarations = m_declSites.find(name);
        if (declarations == m_declSites.end())
            return false;
        return std::all_of(m_accessSites[name].begin(), m_accessSites[name].end(), [&](const auto &access) {
            if (IsWithinScopedBinding(name, access.first))
                return true;
            return std::any_of(declarations->second.begin(), declarations->second.end(), [&](const auto &declaration) {
                return declaration.second <= access.second && IsAncestorOrSelf(declaration.first, access.first);
            });
        });
    }

    // a declaration below the hoisted one would shadow it from the uncovered access; nested shadows keep their own binding
    void DemoteOutermostDeclarations(int target, const std::string &name) {
        if (m_capturedNames.contains(name))
            return;
        const auto declarations = m_declSites.find(name);
        if (declarations == m_declSites.end())
            return;
        const auto covered = [&](const std::pair<int, size_t> &read) {
            return IsWithinScopedBinding(name, read.first) || std::any_of(declarations->second.begin(), declarations->second.end(), [&](const auto &decl) {
                       return decl.second <= read.second && IsAncestorOrSelf(decl.first, read.first);
                   });
        };
        const auto &reads = m_readSites[name];
        const auto &writes = m_writeSites[name];
        // any write between them may be what the read sees
        const auto killed = [&](size_t from, const std::pair<int, size_t> &read) {
            return std::any_of(writes.begin(), writes.end(), [&](const auto &write) { return write.second > from && write.second < read.second; });
        };
        const auto inLoopBelowTarget = [&](int scopeId) {
            for (int cursor = scopeId; cursor >= 0 && cursor != target; cursor = m_scopes[cursor].parent)
                if (m_scopes[cursor].isLoopBody)
                    return true;
            return false;
        };
        std::unordered_set<int> scopes;
        for (const auto &[scopeId, order] : declarations->second)
            if (IsAncestorOrSelf(target, scopeId) && inLoopBelowTarget(scopeId) &&
                std::none_of(
                    declarations->second.begin(), declarations->second.end(),
                    [&](const auto &other) { return other.first != scopeId && IsAncestorOrSelf(target, other.first) && IsAncestorOrSelf(other.first, scopeId); }
                ) &&
                std::any_of(reads.begin(), reads.end(), [&](const auto &read) {
                    return read.second > order && read.first != scopeId && IsAncestorOrSelf(read.first, scopeId) && !covered(read) && !killed(order, read);
                }))
                scopes.insert(scopeId);
        for (const int scopeId : scopes)
            for (auto &stmt : *m_scopes[scopeId].body) {
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt); vd && vd->value && DeclName(vd) == name) {
                    stmt = Fission::MakeShared<AssignmentStatementNode>(vd->identifier, vd->value);
                } else if (auto call = LocalDeclCall(stmt)) {
                    bool multi = false;
                    const auto callName = call->nodeKind == ASTNodeKind::CallExpression
                                              ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                              : SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi);
                    if (callName == name)
                        DemoteLocal(stmt);
                }
            }
    }

    // for a name with no declaration anywhere, insert `local vN` at the lowest scope dominating all
    // its accesses, hoisted out of any loop body (a loop-carried value must be declared outside the
    // loop or it re-initialises each iteration).
    void InsertMissingDeclarations() {
        for (const auto &[name, scopeSet] : m_access) {
            if (scopeSet.empty())
                continue;

            std::unordered_set<int> unboundScopes;
            for (int scopeId : scopeSet)
                if (!IsWithinScopedBinding(name, scopeId))
                    unboundScopes.insert(scopeId);
            if (unboundScopes.empty())
                continue;

            if (AllAccessesCovered(name))
                continue;

            int target = *unboundScopes.begin();
            for (int s : unboundScopes)
                target = LCA(target, s);
            if (target < 0)
                continue;

            if (!HasUnboundBareAssignment(name) && !m_declSites.contains(name))
                continue;

            // Prefer PROMOTION: when the target block itself holds the first bare `name = expr` (and no
            // earlier statement references the name), rewrite it to `local name = expr` in place :
            // keeps decl+value as one node, which the downstream naming passes match on.
            const auto promote = [&](std::vector<std::shared_ptr<Statement>> &body) {
                for (auto &stmt : body) {
                    if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
                        asn && IsBareIdentifier(asn->left, name) && !ExpressionMentions(asn->right, name)) {
                        stmt = Fission::MakeShared<VariableDeclarationNode>(asn->left, asn->right);
                        return true;
                    }
                    if (StatementMentions(stmt, name))
                        return false; // a use precedes the assignment in this block; cannot promote here
                }
                return false;
            };
            // a loop body that writes before any read carries nothing; a captured binding stays out so iterations share it
            if (m_scopes[target].isLoopBody && !m_capturedNames.contains(name) && promote(*m_scopes[target].body))
                continue;

            // hoist above every loop body: a decl inside a loop re-runs per iteration
            while (target >= 0 && m_scopes[target].isLoopBody && m_scopes[target].parent >= 0)
                target = m_scopes[target].parent;
            if (target < 0)
                continue;

            DemoteOutermostDeclarations(target, name);
            auto *body = m_scopes[target].body;
            const bool promoted = promote(*body);

            // Otherwise the bare assignment lives in a deeper scope (e.g. a loop body while the value is
            // carried across iterations / read after the loop). Declare `local name` once at the target
            // scope so every use; including the deeper writes; binds to it instead of leaking a global.
            if (!promoted)
                InsertLeadingDeclaration(*body, name);
        }
    }

    // is `anc` an ancestor of (or equal to) scope `s` in the scope tree? The walk numbers scopes in preorder, so a
    // scope's subtree is the id range up to its last descendant.
    bool IsAncestorOrSelf(int anc, int s) const {
        if (m_subtreeEnd.size() != m_scopes.size()) {
            m_subtreeEnd.assign(m_scopes.size(), 0);
            for (int id = static_cast<int>(m_scopes.size()) - 1; id >= 0; --id) {
                m_subtreeEnd[id] = (std::max)(m_subtreeEnd[id], id);
                if (const int parent = m_scopes[id].parent; parent >= 0)
                    m_subtreeEnd[parent] = (std::max)(m_subtreeEnd[parent], m_subtreeEnd[id]);
            }
        }
        return anc >= 0 && s >= anc && s <= m_subtreeEnd[anc];
    }

    struct ScopeDeclarationIndex {
        std::unordered_map<std::string, size_t> declarations;
        std::unordered_map<std::string, size_t> mentions;
        size_t firstUnknown;
    };

    ScopeDeclarationIndex IndexScopeDeclarations(int scopeId) {
        const auto &body = *m_scopes[scopeId].body;
        ScopeDeclarationIndex index{{}, {}, body.size()};
        size_t lastDeclaration = 0;
        for (size_t i = 0; i < body.size(); ++i) {
            const auto &statement = body[i];
            std::string declaration;
            if (const auto variable = std::dynamic_pointer_cast<VariableDeclarationNode>(statement))
                declaration = DeclName(variable);
            else if (const auto call = LocalDeclCall(statement)) {
                bool multi = false;
                declaration = call->nodeKind == ASTNodeKind::CallExpression
                                  ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                  : SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi);
            }
            const auto count = m_declCount.find(declaration);
            const auto scope = m_declScope.find(declaration);
            if (count != m_declCount.end() && count->second == 1 && scope != m_declScope.end() && scope->second == scopeId)
                if (index.declarations.try_emplace(std::move(declaration), i).second)
                    lastDeclaration = (std::max)(lastDeclaration, i);
        }
        for (size_t i = 0; i < lastDeclaration; ++i) {
            const auto &statement = body[i];
            MentionSet collected;
            const auto cached = m_mentions.find(statement);
            if (cached == m_mentions.end())
                CollectStatementMentions(statement, collected);
            const auto &mentions = cached == m_mentions.end() ? collected : cached->second;
            if (mentions.everything)
                index.firstUnknown = (std::min)(index.firstUnknown, i);
            for (const auto &name : mentions.names)
                if (index.declarations.contains(name))
                    index.mentions.try_emplace(name, i);
        }
        return index;
    }

    // M3: a name with exactly one `local name = expr` declaration whose scope does NOT dominate all of
    // its accesses is placed too deep (a use sits above or beside the decl and reads a global/nil).
    // Hoist it: declare `local name` once at the access LCA and turn the original into a plain
    // assignment. Conservative gates keep this off deliberate shadowing and the interim-name-mismatch
    // case: single decl only; multiple declarations can represent intentional shadowing.
    void DemoteDeepDeclarations() {
        std::unordered_map<int, ScopeDeclarationIndex> indexes;
        for (const auto &[name, count] : m_declCount) {
            if (count != 1)
                continue; // multiple decls -> intentional shadowing, leave it
            const auto scopeIt = m_declScope.find(name);
            const auto accIt = m_access.find(name);
            if (scopeIt == m_declScope.end() || accIt == m_access.end())
                continue;
            const int declScope = scopeIt->second;
            // earlier reads bind to the enclosing loop variable of the same name, not to this shadowing local
            if (IsWithinScopedBinding(name, declScope))
                continue;

            int lca = *accIt->second.begin();
            for (int s : accIt->second)
                lca = LCA(lca, s);
            if (lca < 0)
                continue;

            // if the decl scope already dominates every access, placement is fine unless a use precedes
            // the decl within that same block; both are handled by targeting the access LCA and, when it
            // equals the decl scope, checking statement order below.
            const int target = lca;
            if (!IsAncestorOrSelf(target, declScope))
                continue; // decl not inside the LCA subtree -> unusual shape, skip
            if (target == declScope) {
                auto index = indexes.find(declScope);
                if (index == indexes.end())
                    index = indexes.emplace(declScope, IndexScopeDeclarations(declScope)).first;
                const auto declaration = index->second.declarations.find(name);
                const auto mention = index->second.mentions.find(name);
                if (declaration != index->second.declarations.end() && index->second.firstUnknown >= declaration->second &&
                    (mention == index->second.mentions.end() || mention->second >= declaration->second))
                    continue;
            }
            auto *declBody = m_scopes[declScope].body;
            // Find the declaration in its owning block.
            int declIdx = -1;
            bool callDecl = false;
            std::shared_ptr<FunctionDeclarationNode> functionDecl;
            for (int i = 0; i < static_cast<int>(declBody->size()); ++i)
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>((*declBody)[i]); vd && DeclName(vd) == name) {
                    declIdx = i;
                    break;
                } else if (
                    auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>((*declBody)[i]);
                    fdn && fdn->bIsLocalDeclaration && fdn->functionName == name && lca != declScope
                ) {
                    declIdx = i;
                    functionDecl = fdn;
                    break;
                } else if (auto call = LocalDeclCall((*declBody)[i])) {
                    bool multi = false;
                    const auto callName = call->nodeKind == ASTNodeKind::CallExpression
                                              ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                          : call->nodeKind == ASTNodeKind::MethodCallExpression
                                              ? SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi)
                                              : std::string{};
                    if (callName == name) {
                        declIdx = i;
                        callDecl = true;
                        break;
                    }
                }
            if (declIdx < 0)
                continue;

            if (target == declScope) {
                // same block: only demote if a use actually precedes the decl statement
                bool usePrecedes = false;
                for (int i = 0; i < declIdx; ++i)
                    if (StatementMentions((*declBody)[i], name)) {
                        usePrecedes = true;
                        break;
                    }
                if (!usePrecedes)
                    continue;
            }

            // rewrite: `local name = expr` becomes `name = expr`; declare `local name` at target front
            if (functionDecl)
                functionDecl->bIsLocalDeclaration = false;
            else if (callDecl)
                DemoteLocal((*declBody)[declIdx]);
            else {
                auto vd = std::static_pointer_cast<VariableDeclarationNode>((*declBody)[declIdx]);
                (*declBody)[declIdx] =
                    Fission::MakeShared<AssignmentStatementNode>(vd->identifier, vd->value ? vd->value : Fission::MakeShared<NilLiteralNode>());
            }
            bool alreadyDeclared = false;
            for (const auto &stmt : *m_scopes[target].body)
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt); vd && DeclName(vd) == name)
                    alreadyDeclared = true;
            if (!alreadyDeclared)
                InsertLeadingDeclaration(*m_scopes[target].body, name);
            indexes.clear();
        }
    }

    void InsertMultiDeclarationFallbacks() {
        auto *root = m_scopes.front().body;
        std::unordered_set<std::string> rootDeclarations;
        for (const auto &stmt : *root)
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
                if (const auto name = DeclName(decl); IsRegisterName(name))
                    rootDeclarations.insert(name);

        for (const auto &name : m_bareAssigned) {
            if (!HasUnboundBareAssignment(name) || m_declCount[name] <= 1 || rootDeclarations.contains(name) || AllAccessesCovered(name))
                continue;
            InsertLeadingDeclaration(*root, name);
            rootDeclarations.insert(name);
        }
    }

    static std::unordered_set<std::string> BoundOutsideBody(const FunctionDeclarationNode &fn) {
        auto names = fn.capturedNames;
        for (const auto &argument : fn.argumentsNames | std::views::values)
            if (auto id = argument ? std::dynamic_pointer_cast<IdentifierExpressionNode>(argument->argumentName) : nullptr; id && id->identifier)
                names.insert(id->identifier->name);
        return names;
    }

    static std::string DeclName(const std::shared_ptr<VariableDeclarationNode> &vd) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(vd->identifier); id && id->identifier)
            return id->identifier->name;
        return {};
    }

    static bool IsBareIdentifier(const std::shared_ptr<Expression> &e, const std::string &name) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e); id && id->identifier)
            return id->identifier->name == name;
        return false;
    }

    bool StatementMentions(const std::shared_ptr<Statement> &s, const std::string &name) {
        if (!s)
            return false;
        auto [entry, fresh] = m_mentions.try_emplace(s);
        if (fresh)
            CollectStatementMentions(s, entry->second);
        return entry->second.everything || entry->second.names.contains(name);
    }

    // Every name `s` may read or write, with `everything` for a node of unknown shape.
    static void CollectStatementMentions(const std::shared_ptr<Statement> &s, MentionSet &out) {
        if (!s)
            return;
        if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            CollectFunctionMentions(*fdn, out);
            return;
        }
        ForEachStatementExpression(s, [&](const std::shared_ptr<Expression> &e) { CollectExprMentions(e, out); });
        ForEachChildBlock(s, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &c : body)
                CollectStatementMentions(c, out);
        });
    }

    // a closure reaches outer locals only through captures; its own same-named locals are not outer uses
    static void CollectFunctionMentions(const FunctionDeclarationNode &fn, MentionSet &out) {
        if (!fn.lpFunctionBody)
            return;
        MentionSet inner;
        std::unordered_set<std::string> own;
        for (const auto &argument : fn.argumentsNames | std::views::values)
            if (auto id = argument ? std::dynamic_pointer_cast<IdentifierExpressionNode>(argument->argumentName) : nullptr; id && id->identifier)
                own.insert(id->identifier->name);
        for (const auto &c : fn.lpFunctionBody->body) {
            CollectStatementMentions(c, inner);
            CollectOwnDeclarations(c, own);
        }
        out.everything |= inner.everything;
        for (const auto &name : inner.names)
            if (!own.contains(name) || fn.capturedNames.contains(name))
                out.names.insert(name);
    }

    static void CollectOwnDeclarations(const std::shared_ptr<Statement> &s, std::unordered_set<std::string> &own) {
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            if (const auto name = DeclName(decl); !name.empty())
                own.insert(name);
            return;
        }
        ForEachChildBlock(s, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &c : body)
                CollectOwnDeclarations(c, own);
        });
    }

    static void CollectExprMentions(const std::shared_ptr<Expression> &e, MentionSet &out) {
        if (!e)
            return;
        switch (e->nodeKind) {
        case ASTNodeKind::IdentifierExpression:
            if (auto id = std::static_pointer_cast<IdentifierExpressionNode>(e); id->identifier)
                out.names.insert(id->identifier->name);
            return;
        case ASTNodeKind::Identifier:
            if (auto id = std::dynamic_pointer_cast<Identifier>(e))
                out.names.insert(id->name);
            return;
        case ASTNodeKind::BinaryExpression:
        case ASTNodeKind::TableBinaryExpression: {
            auto b = std::static_pointer_cast<BinaryExpressionNode>(e);
            CollectExprMentions(b->left, out);
            CollectExprMentions(b->right, out);
            return;
        }
        case ASTNodeKind::CompoundAssignment: {
            auto b = std::static_pointer_cast<CompoundBinaryExpressionNode>(e);
            CollectExprMentions(b->left, out);
            CollectExprMentions(b->right, out);
            return;
        }
        case ASTNodeKind::UnaryExpression:
            CollectExprMentions(std::static_pointer_cast<UnaryExpressionNode>(e)->operand, out);
            return;
        case ASTNodeKind::IfExpression: {
            auto i = std::static_pointer_cast<IfExpressionNode>(e);
            CollectExprMentions(i->condition, out);
            CollectExprMentions(i->thenExpr, out);
            CollectExprMentions(i->elseExpr, out);
            return;
        }
        case ASTNodeKind::IndexExpression: {
            auto ix = std::static_pointer_cast<IndexExpressionNode>(e);
            CollectExprMentions(ix->left, out);
            CollectExprMentions(ix->right, out);
            return;
        }
        case ASTNodeKind::MemberExpression: {
            auto m = std::static_pointer_cast<MemberExpressionNode>(e);
            CollectExprMentions(m->table, out);
            CollectExprMentions(m->key, out);
            return;
        }
        case ASTNodeKind::CallExpression: {
            auto c = std::static_pointer_cast<CallExpressionNode>(e);
            CollectExprMentions(c->callee, out);
            for (const auto &a : c->arguments)
                CollectExprMentions(a, out);
            for (const auto &r : c->rets)
                CollectExprMentions(r, out);
            return;
        }
        case ASTNodeKind::MethodCallExpression: {
            auto c = std::static_pointer_cast<NameCallExpressionNode>(e);
            CollectExprMentions(c->calledOn, out);
            CollectExprMentions(c->callWhat, out);
            for (const auto &a : c->arguments)
                CollectExprMentions(a, out);
            for (const auto &r : c->rets)
                CollectExprMentions(r, out);
            return;
        }
        case ASTNodeKind::LiteralValue:
            if (auto t = AsLiteral<TableLiteralNode>(e))
                for (const auto &el : t->expressions)
                    CollectExprMentions(el, out);
            return;
        case ASTNodeKind::FunctionDeclarationNode:
            CollectFunctionMentions(*std::static_pointer_cast<FunctionDeclarationNode>(e), out);
            return;
        case ASTNodeKind::VarArgExpression:
            return;
        default:
            out.everything = true;
            return;
        }
    }

    static bool ContinuesLoop(const std::shared_ptr<Statement> &s) {
        if (!s)
            return false;
        if (s->nodeKind == ASTNodeKind::ContinueStatement)
            return true;
        const auto anyIn = [](const std::shared_ptr<BlockStatementNode> &block) { return block && std::ranges::any_of(block->body, ContinuesLoop); };
        if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s))
            return anyIn(iff->thenBranch) || anyIn(iff->elseBranch);
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s))
            return anyIn(blk);
        return false; // nested loops own their continues; functions have their own bodies
    }

    // Luau rejects `continue` jumping over a local that the `until` condition reads. Declare such
    // locals ahead of the first continue; each iteration still starts them fresh.
    void PredeclareRepeatConditionLocals(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (auto &stmt : stmts) {
            auto loop = std::dynamic_pointer_cast<RepeatStatementNode>(stmt);
            if (!loop || !loop->body)
                continue;
            auto &body = loop->body->body;
            const auto firstContinue = std::ranges::find_if(body, ContinuesLoop);
            if (firstContinue == body.end())
                continue;
            std::vector<std::string> names;
            for (auto it = firstContinue + 1; it != body.end(); ++it) {
                if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(*it)) {
                    const auto name = DeclName(decl);
                    if (name.empty() || !ExpressionMentions(loop->condition, name))
                        continue;
                    *it = Fission::MakeShared<AssignmentStatementNode>(decl->identifier, decl->value ? decl->value : Fission::MakeShared<NilLiteralNode>());
                    names.push_back(name);
                } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(*it); fn && fn->bIsLocalDeclaration) {
                    if (!ExpressionMentions(loop->condition, fn->functionName))
                        continue;
                    fn->bIsLocalDeclaration = false;
                    names.push_back(fn->functionName);
                } else if (auto call = LocalDeclCall(*it)) {
                    bool multi = false;
                    const auto name = call->nodeKind == ASTNodeKind::CallExpression
                                          ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                          : SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi);
                    if (name.empty() || !ExpressionMentions(loop->condition, name))
                        continue;
                    DemoteLocal(*it);
                    names.push_back(name);
                }
            }
            for (auto &s : body) {
                if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
                    AssignContinueDeclarations(iff->thenBranch, loop->condition);
                    AssignContinueDeclarations(iff->elseBranch, loop->condition);
                } else if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
                    AssignContinueDeclarations(blk, loop->condition);
                }
            }
            for (auto name = names.rbegin(); name != names.rend(); ++name)
                body.insert(body.begin(), Fission::MakeShared<VariableDeclarationNode>(Fission::MakeShared<Identifier>(*name)));
        }
    }

    // A latch duplicated ahead of `continue` redeclares the until-condition locals in a nested scope; assign them instead.
    void AssignContinueDeclarations(const std::shared_ptr<BlockStatementNode> &block, const std::shared_ptr<Expression> &condition) {
        if (!block || !std::ranges::any_of(block->body, ContinuesLoop))
            return;
        const bool continuesHere = std::ranges::any_of(block->body, [](const auto &s) { return s && s->nodeKind == ASTNodeKind::ContinueStatement; });
        const auto named = [&](const std::string &name) { return continuesHere && !name.empty() && ExpressionMentions(condition, name); };
        for (auto &s : block->body) {
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                if (named(DeclName(decl)))
                    s = Fission::MakeShared<AssignmentStatementNode>(decl->identifier, decl->value ? decl->value : Fission::MakeShared<NilLiteralNode>());
            } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s); fn && fn->bIsLocalDeclaration) {
                if (named(fn->functionName))
                    fn->bIsLocalDeclaration = false;
            } else if (auto call = LocalDeclCall(s)) {
                bool multi = false;
                const auto name = call->nodeKind == ASTNodeKind::CallExpression
                                      ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                      : SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi);
                if (named(name))
                    DemoteLocal(s);
            } else if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
                AssignContinueDeclarations(iff->thenBranch, condition);
                AssignContinueDeclarations(iff->elseBranch, condition);
            } else if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
                AssignContinueDeclarations(blk, condition);
            }
        }
    }

    void CoalesceAdjacentDeclarations(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
            auto decl =
                stmts[i] && stmts[i]->nodeKind == ASTNodeKind::VariableDeclaration ? std::static_pointer_cast<VariableDeclarationNode>(stmts[i]) : nullptr;
            auto asn = stmts[i + 1] && stmts[i + 1]->nodeKind == ASTNodeKind::AssignmentStatement
                           ? std::static_pointer_cast<AssignmentStatementNode>(stmts[i + 1])
                           : nullptr;
            const auto name = decl ? DeclName(decl) : std::string{};
            auto lhs = asn && asn->left && asn->left->nodeKind == ASTNodeKind::IdentifierExpression
                           ? std::static_pointer_cast<IdentifierExpressionNode>(asn->left)
                           : nullptr;
            // `local f; f = function ... end` is what `local function f` means, self-references included
            if (auto fn = asn && asn->right && asn->right->nodeKind == ASTNodeKind::FunctionDeclarationNode
                              ? std::static_pointer_cast<FunctionDeclarationNode>(asn->right)
                              : nullptr;
                fn && fn->bAnonymousInline && decl && !decl->value && lhs && lhs->identifier && lhs->identifier->name == name) {
                fn->functionName = name;
                fn->bAnonymousInline = false;
                fn->bIsLocalDeclaration = true;
                stmts[i] = fn;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                continue;
            }
            if (decl && !decl->value && !name.empty() && lhs && lhs->identifier && lhs->identifier->name == name && !ExpressionMentions(asn->right, name)) {
                decl->value = asn->right;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                --i;
            }
        }
    }
};
