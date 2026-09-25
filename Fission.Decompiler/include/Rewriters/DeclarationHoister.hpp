// Places generated register declarations in the lowest scope dominating every access.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DeclarationHoister {
  public:
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
                m_declSites.clear();
                m_nestedFunctions.clear();
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
    std::unordered_map<std::string, std::vector<std::pair<int, size_t>>> m_declSites;
    std::vector<FunctionWork> m_nestedFunctions;
    std::unordered_set<std::string> m_excludedNames;
    std::unordered_set<std::string> m_locallyCapturedNames;
    bool m_namedCaptureDiscovered{};
    size_t m_walkOrder{};
    size_t m_currentOrder{};

    static void SplitRecursiveInitializers(std::vector<std::shared_ptr<Statement>> &statements) {
        for (size_t i = 0; i < statements.size(); ++i) {
            const auto &statement = statements[i];
            if (auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(statement)) {
                const auto name = DeclName(declaration);
                const auto function = std::dynamic_pointer_cast<FunctionDeclarationNode>(declaration->value);
                if (function && function->capturedNames.contains(name)) {
                    auto value = declaration->value;
                    declaration->value.reset();
                    statements.insert(statements.begin() + static_cast<std::ptrdiff_t>(++i), std::make_shared<AssignmentStatementNode>(declaration->identifier, value));
                }
                continue;
            }
            if (auto branch = std::dynamic_pointer_cast<IfStatementNode>(statement)) {
                if (branch->thenBranch)
                    SplitRecursiveInitializers(branch->thenBranch->body);
                if (branch->elseBranch)
                    SplitRecursiveInitializers(branch->elseBranch->body);
            } else if (auto loop = std::dynamic_pointer_cast<WhileStatementNode>(statement); loop && loop->body) {
                SplitRecursiveInitializers(loop->body->body);
            } else if (auto loop = std::dynamic_pointer_cast<RepeatStatementNode>(statement); loop && loop->body) {
                SplitRecursiveInitializers(loop->body->body);
            } else if (auto loop = std::dynamic_pointer_cast<ForNumericNode>(statement); loop && loop->lpLoopBody) {
                SplitRecursiveInitializers(loop->lpLoopBody->body);
            } else if (auto loop = std::dynamic_pointer_cast<ForGeneralNode>(statement); loop && loop->body) {
                SplitRecursiveInitializers(loop->body->body);
            } else if (auto block = std::dynamic_pointer_cast<BlockStatementNode>(statement)) {
                SplitRecursiveInitializers(block->body);
            }
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

    // A generated register local is `vN`, optionally with an upvalue-collision suffix (`vN_M`).
    static bool IsRegisterName(const std::string &s) {
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
        }
    }

    void RecordTrackedAccess(const std::string &name, int scopeId) {
        if (IsTrackedBinding(name)) {
            m_access[name].insert(scopeId);
            m_accessSites[name].emplace_back(scopeId, m_currentOrder);
        }
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
                RecordAccess(id->identifier->name, scopeId);
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
                CollectExpr(r, scopeId);
            break;
        }
        case ASTNodeKind::MethodCallExpression: {
            auto c = std::static_pointer_cast<NameCallExpressionNode>(e);
            CollectExpr(c->calledOn, scopeId);
            CollectExpr(c->callWhat, scopeId);
            for (const auto &a : c->arguments)
                CollectExpr(a, scopeId);
            for (const auto &r : c->rets)
                CollectExpr(r, scopeId);
            break;
        }
        case ASTNodeKind::LiteralValue:
            // only table literals carry sub-expressions
            if (auto t = std::dynamic_pointer_cast<TableLiteralNode>(e))
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
                }
                m_nestedFunctions.push_back({&fn->lpFunctionBody->body, fn->capturedNames});
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

        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier) {
                RecordDecl(id->identifier->name, scopeId);
                RecordTrackedAccess(id->identifier->name, scopeId);
            }
            CollectExpr(decl->value, scopeId);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
                id && id->identifier && !id->identifier->bIsGlobal && IsOwnedRegisterName(id->identifier->name)) {
                m_bareAssigned.insert(id->identifier->name);
                m_bareAssignmentScopes[id->identifier->name].insert(scopeId);
            }
            CollectExpr(asn->left, scopeId);
            CollectExpr(asn->right, scopeId);
            return;
        }
        if (auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(s)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(compound->left);
                id && id->identifier && !id->identifier->bIsGlobal && IsOwnedRegisterName(id->identifier->name)) {
                m_bareAssigned.insert(id->identifier->name);
                m_bareAssignmentScopes[id->identifier->name].insert(scopeId);
            }
            CollectExpr(compound, scopeId);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
            const auto recordReturns = [&](const std::vector<std::shared_ptr<Expression>> &rets, bool local) {
                bool multi = false;
                const auto name = SingleRetName(rets, multi);
                if (name.empty())
                    return;
                if (local) {
                    RecordDecl(name, scopeId);
                    RecordTrackedAccess(name, scopeId);
                } else if (IsOwnedRegisterName(name)) {
                    m_bareAssigned.insert(name);
                    m_bareAssignmentScopes[name].insert(scopeId);
                }
            };
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression))
                recordReturns(call->rets, call->bIsLocalDeclaration);
            else if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression))
                recordReturns(call->rets, call->bIsLocalDeclaration);
            CollectExpr(es->expression, scopeId);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (const auto &v : ret->returnValues)
                CollectExpr(v, scopeId);
            return;
        }
        if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
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
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            CollectExpr(w->condition, scopeId);
            if (w->body) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &w->body->body);
                WalkBlock(w->body->body, c);
            }
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            if (r->body) {
                const int c = NewScope(scopeId, /*isLoopBody*/ true, &r->body->body);
                WalkBlock(r->body->body, c);
                CollectExpr(r->condition, c); // until-cond is inside the body scope (Luau)
            } else {
                CollectExpr(r->condition, scopeId);
            }
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
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
        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
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
        if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
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
                }
                m_nestedFunctions.push_back({&fdn->lpFunctionBody->body, fdn->capturedNames});
            }
            return;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            WalkBlock(blk->body, scopeId);
            return;
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
        return std::any_of(bindings->second.begin(), bindings->second.end(), [&](int bindingScope) {
            return IsAncestorOrSelf(bindingScope, scopeId);
        });
    }

    bool HasUnboundBareAssignment(const std::string &name) const {
        const auto assignments = m_bareAssignmentScopes.find(name);
        if (assignments == m_bareAssignmentScopes.end())
            return false;
        return std::any_of(assignments->second.begin(), assignments->second.end(), [&](int scopeId) {
            return !IsWithinScopedBinding(name, scopeId);
        });
    }

    static void InsertLeadingDeclaration(std::vector<std::shared_ptr<Statement>> &body, const std::string &name) {
        const auto position = std::find_if(body.begin(), body.end(), [](const auto &statement) {
            return !statement || statement->nodeKind != ASTNodeKind::Comment;
        });
        body.insert(position, std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
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

            bool covered = false;
            if (const auto declarations = m_declSites.find(name); declarations != m_declSites.end()) {
                covered = std::all_of(m_accessSites[name].begin(), m_accessSites[name].end(), [&](const auto &access) {
                    if (IsWithinScopedBinding(name, access.first))
                        return true;
                    return std::any_of(declarations->second.begin(), declarations->second.end(), [&](const auto &declaration) {
                        return declaration.second <= access.second && IsAncestorOrSelf(declaration.first, access.first);
                    });
                });
            }
            if (covered)
                continue;

            int target = *unboundScopes.begin();
            for (int s : unboundScopes)
                target = LCA(target, s);
            if (target < 0)
                continue;

            // hoist above every loop body: a decl inside a loop re-runs per iteration
            while (target >= 0 && m_scopes[target].isLoopBody && m_scopes[target].parent >= 0)
                target = m_scopes[target].parent;
            if (target < 0)
                continue;

            if (!HasUnboundBareAssignment(name) && !m_declSites.contains(name))
                continue;

            // Prefer PROMOTION: when the target block itself holds the first bare `name = expr` (and no
            // earlier statement references the name), rewrite it to `local name = expr` in place :
            // keeps decl+value as one node, which the downstream naming passes match on.
            auto *body = m_scopes[target].body;
            bool promoted = false;
            for (auto &stmt : *body) {
                if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt); asn && IsBareIdentifier(asn->left, name)) {
                    stmt = std::make_shared<VariableDeclarationNode>(asn->left, asn->right);
                    promoted = true;
                    break;
                }
                if (StatementMentions(stmt, name))
                    break; // a use precedes the assignment in this block; cannot promote here
            }

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

    // M3: a name with exactly one `local name = expr` declaration whose scope does NOT dominate all of
    // its accesses is placed too deep (a use sits above or beside the decl and reads a global/nil).
    // Hoist it: declare `local name` once at the access LCA and turn the original into a plain
    // assignment. Conservative gates keep this off deliberate shadowing and the interim-name-mismatch
    // case: single decl only; multiple declarations can represent intentional shadowing.
    void DemoteDeepDeclarations() {
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
            auto *declBody = m_scopes[declScope].body;
            // Find the declaration in its owning block.
            int declIdx = -1;
            bool callDecl = false;
            for (int i = 0; i < static_cast<int>(declBody->size()); ++i)
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>((*declBody)[i]); vd && DeclName(vd) == name) {
                    declIdx = i;
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
            if (callDecl)
                DemoteLocal((*declBody)[declIdx]);
            else {
                auto vd = std::static_pointer_cast<VariableDeclarationNode>((*declBody)[declIdx]);
                (*declBody)[declIdx] = std::make_shared<AssignmentStatementNode>(
                    vd->identifier, vd->value ? vd->value : std::make_shared<NilLiteralNode>()
                );
            }
            bool alreadyDeclared = false;
            for (const auto &stmt : *m_scopes[target].body)
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt); vd && DeclName(vd) == name)
                    alreadyDeclared = true;
            if (!alreadyDeclared)
                InsertLeadingDeclaration(*m_scopes[target].body, name);
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
            if (!HasUnboundBareAssignment(name) || m_declCount[name] <= 1 || rootDeclarations.contains(name))
                continue;
            InsertLeadingDeclaration(*root, name);
            rootDeclarations.insert(name);
        }
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

    // does this statement (recursively) reference `name` anywhere? used only to decide promotion
    // safety, so it errs toward "yes" by scanning all sub-expressions and nested blocks.
    bool StatementMentionsDeep(const std::shared_ptr<Statement> &s, const std::string &name) {
        if (!s)
            return false;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s))
            return ExprMentions(decl->identifier, name) || ExprMentions(decl->value, name);
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s))
            return ExprMentions(asn->left, name) || ExprMentions(asn->right, name);
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s))
            return ExprMentions(es->expression, name);
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (const auto &v : ret->returnValues)
                if (ExprMentions(v, name))
                    return true;
            return false;
        }
        if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
            if (ExprMentions(iff->condition, name))
                return true;
            if (iff->thenBranch)
                for (const auto &c : iff->thenBranch->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            if (iff->elseBranch)
                for (const auto &c : iff->elseBranch->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            if (ExprMentions(w->condition, name))
                return true;
            if (w->body)
                for (const auto &c : w->body->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            if (r->body)
                for (const auto &c : r->body->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return ExprMentions(r->condition, name);
        }
        if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
            if (ExprMentions(fn->startVariable, name) || ExprMentions(fn->increaseBy, name) || ExprMentions(fn->maxIncreased, name))
                return true;
            if (fn->lpLoopBody)
                for (const auto &c : fn->lpLoopBody->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            if (ExprMentions(fg->generator, name) || ExprMentions(fg->state, name) || ExprMentions(fg->index, name))
                return true;
            if (fg->body)
                for (const auto &c : fg->body->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            if (fdn->lpFunctionBody)
                for (const auto &c : fdn->lpFunctionBody->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            for (const auto &c : blk->body)
                if (StatementMentionsDeep(c, name))
                    return true;
            return false;
        }
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

    // Every name StatementMentionsDeep / ExprMentions would report, with `everything` for a node they treat as mentioning all.
    void CollectStatementMentions(const std::shared_ptr<Statement> &s, MentionSet &out) {
        if (!s)
            return;
        const auto block = [&](const std::shared_ptr<BlockStatementNode> &body) {
            if (body)
                for (const auto &c : body->body)
                    CollectStatementMentions(c, out);
        };
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            CollectExprMentions(decl->identifier, out);
            CollectExprMentions(decl->value, out);
        } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            CollectExprMentions(asn->left, out);
            CollectExprMentions(asn->right, out);
        } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
            CollectExprMentions(es->expression, out);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (const auto &v : ret->returnValues)
                CollectExprMentions(v, out);
        } else if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
            CollectExprMentions(iff->condition, out);
            block(iff->thenBranch);
            block(iff->elseBranch);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            CollectExprMentions(w->condition, out);
            block(w->body);
        } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            block(r->body);
            CollectExprMentions(r->condition, out);
        } else if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
            CollectExprMentions(fn->startVariable, out);
            CollectExprMentions(fn->increaseBy, out);
            CollectExprMentions(fn->maxIncreased, out);
            block(fn->lpLoopBody);
        } else if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            CollectExprMentions(fg->generator, out);
            CollectExprMentions(fg->state, out);
            CollectExprMentions(fg->index, out);
            block(fg->body);
        } else if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            block(fdn->lpFunctionBody);
        } else if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            block(blk);
        }
    }

    void CollectExprMentions(const std::shared_ptr<Expression> &e, MentionSet &out) {
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
            if (auto t = std::dynamic_pointer_cast<TableLiteralNode>(e))
                for (const auto &el : t->expressions)
                    CollectExprMentions(el, out);
            return;
        case ASTNodeKind::FunctionDeclarationNode: {
            auto fn = std::static_pointer_cast<FunctionDeclarationNode>(e);
            if (fn->lpFunctionBody)
                for (const auto &c : fn->lpFunctionBody->body)
                    CollectStatementMentions(c, out);
            return;
        }
        default:
            out.everything = true;
            return;
        }
    }

    bool ExprMentions(const std::shared_ptr<Expression> &e, const std::string &name) {
        if (!e)
            return false;
        switch (e->nodeKind) {
        case ASTNodeKind::IdentifierExpression:
            if (auto id = std::static_pointer_cast<IdentifierExpressionNode>(e); id->identifier)
                return id->identifier->name == name;
            return false;
        case ASTNodeKind::Identifier:
            if (auto id = std::dynamic_pointer_cast<Identifier>(e))
                return id->name == name;
            return false;
        case ASTNodeKind::BinaryExpression:
        case ASTNodeKind::TableBinaryExpression: {
            auto b = std::static_pointer_cast<BinaryExpressionNode>(e);
            return ExprMentions(b->left, name) || ExprMentions(b->right, name);
        }
        case ASTNodeKind::CompoundAssignment: {
            auto b = std::static_pointer_cast<CompoundBinaryExpressionNode>(e);
            return ExprMentions(b->left, name) || ExprMentions(b->right, name);
        }
        case ASTNodeKind::UnaryExpression:
            return ExprMentions(std::static_pointer_cast<UnaryExpressionNode>(e)->operand, name);
        case ASTNodeKind::IfExpression: {
            auto i = std::static_pointer_cast<IfExpressionNode>(e);
            return ExprMentions(i->condition, name) || ExprMentions(i->thenExpr, name) || ExprMentions(i->elseExpr, name);
        }
        case ASTNodeKind::IndexExpression: {
            auto ix = std::static_pointer_cast<IndexExpressionNode>(e);
            return ExprMentions(ix->left, name) || ExprMentions(ix->right, name);
        }
        case ASTNodeKind::MemberExpression: {
            auto m = std::static_pointer_cast<MemberExpressionNode>(e);
            return ExprMentions(m->table, name) || ExprMentions(m->key, name);
        }
        case ASTNodeKind::CallExpression: {
            auto c = std::static_pointer_cast<CallExpressionNode>(e);
            if (ExprMentions(c->callee, name))
                return true;
            for (const auto &a : c->arguments)
                if (ExprMentions(a, name))
                    return true;
            for (const auto &r : c->rets)
                if (ExprMentions(r, name))
                    return true;
            return false;
        }
        case ASTNodeKind::MethodCallExpression: {
            auto c = std::static_pointer_cast<NameCallExpressionNode>(e);
            if (ExprMentions(c->calledOn, name) || ExprMentions(c->callWhat, name))
                return true;
            for (const auto &a : c->arguments)
                if (ExprMentions(a, name))
                    return true;
            for (const auto &r : c->rets)
                if (ExprMentions(r, name))
                    return true;
            return false;
        }
        case ASTNodeKind::LiteralValue:
            if (auto t = std::dynamic_pointer_cast<TableLiteralNode>(e))
                for (const auto &el : t->expressions)
                    if (ExprMentions(el, name))
                        return true;
            return false;
        case ASTNodeKind::FunctionDeclarationNode: {
            auto fn = std::static_pointer_cast<FunctionDeclarationNode>(e);
            if (fn->lpFunctionBody)
                for (const auto &c : fn->lpFunctionBody->body)
                    if (StatementMentionsDeep(c, name))
                        return true;
            return false;
        }
        default:
            return true;
        }
    }

    static bool ContinuesLoop(const std::shared_ptr<Statement> &s) {
        if (!s)
            return false;
        if (s->nodeKind == ASTNodeKind::ContinueStatement)
            return true;
        const auto anyIn = [](const std::shared_ptr<BlockStatementNode> &block) {
            return block && std::ranges::any_of(block->body, ContinuesLoop);
        };
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
                    if (name.empty() || !ExprMentions(loop->condition, name))
                        continue;
                    *it = std::make_shared<AssignmentStatementNode>(decl->identifier, decl->value ? decl->value : std::make_shared<NilLiteralNode>());
                    names.push_back(name);
                } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(*it); fn && fn->bIsLocalDeclaration) {
                    if (!ExprMentions(loop->condition, fn->functionName))
                        continue;
                    fn->bIsLocalDeclaration = false;
                    names.push_back(fn->functionName);
                } else if (auto call = LocalDeclCall(*it)) {
                    bool multi = false;
                    const auto name = call->nodeKind == ASTNodeKind::CallExpression
                                          ? SingleRetName(std::static_pointer_cast<CallExpressionNode>(call)->rets, multi)
                                          : SingleRetName(std::static_pointer_cast<NameCallExpressionNode>(call)->rets, multi);
                    if (name.empty() || !ExprMentions(loop->condition, name))
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
                body.insert(body.begin(), std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(*name)));
        }
    }

    // A latch duplicated ahead of `continue` redeclares the until-condition locals in a nested scope; assign them instead.
    void AssignContinueDeclarations(const std::shared_ptr<BlockStatementNode> &block, const std::shared_ptr<Expression> &condition) {
        if (!block || !std::ranges::any_of(block->body, ContinuesLoop))
            return;
        const bool continuesHere = std::ranges::any_of(block->body, [](const auto &s) { return s && s->nodeKind == ASTNodeKind::ContinueStatement; });
        const auto named = [&](const std::string &name) { return continuesHere && !name.empty() && ExprMentions(condition, name); };
        for (auto &s : block->body) {
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
                if (named(DeclName(decl)))
                    s = std::make_shared<AssignmentStatementNode>(decl->identifier, decl->value ? decl->value : std::make_shared<NilLiteralNode>());
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
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[i + 1]);
            const auto name = decl ? DeclName(decl) : std::string{};
            auto lhs = asn ? std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left) : nullptr;
            // `local f; f = function ... end` is what `local function f` means, self-references included
            if (auto fn = asn ? std::dynamic_pointer_cast<FunctionDeclarationNode>(asn->right) : nullptr;
                fn && fn->bAnonymousInline && decl && !decl->value && lhs && lhs->identifier && lhs->identifier->name == name) {
                fn->functionName = name;
                fn->bAnonymousInline = false;
                fn->bIsLocalDeclaration = true;
                stmts[i] = fn;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                continue;
            }
            if (decl && !decl->value && !name.empty() && lhs && lhs->identifier && lhs->identifier->name == name && !ExprMentions(asn->right, name)) {
                decl->value = asn->right;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                --i;
            }
        }
    }
};
