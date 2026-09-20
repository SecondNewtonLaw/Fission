// Places generated register declarations in the lowest scope dominating every access.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

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
            m_scopes.clear();
            m_access.clear();
            m_declaredAnywhere.clear();
            m_bareAssigned.clear();
            m_declCount.clear();
            m_declScope.clear();
            m_nestedFunctions.clear();
            m_excludedNames = functions[i].capturedNames;

            const int root = NewScope(/*parent*/ -1, /*isLoopBody*/ false, functions[i].body);
            WalkBlock(*functions[i].body, root);

            InsertMissingDeclarations();
            DemoteDeepDeclarations();
            InsertMultiDeclarationFallbacks();
            for (auto &scope : m_scopes)
                CoalesceAdjacentDeclarations(*scope.body);
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
    // name -> the set of scope ids in which it is read or written
    std::unordered_map<std::string, std::unordered_set<int>> m_access;
    std::unordered_set<std::string> m_declaredAnywhere;
    // names that appear as the lhs of a bare `name = expr` assignment. Only these are safe to insert
    // an orphan `local name` for: their presence proves the lifter is naming the register `vN`
    // consistently (a debug-named local whose uses merely read as `vN` has no such bare assignment,
    // and inserting for it would spuriously double-declare).
    std::unordered_set<std::string> m_bareAssigned;
    // name -> number of `local name` declarations seen, and the scope id of the (single) one. M3
    // demotion only touches names with exactly one decl (multiple decls == deliberate shadowing).
    std::unordered_map<std::string, int> m_declCount;
    std::unordered_map<std::string, int> m_declScope;
    std::vector<FunctionWork> m_nestedFunctions;
    std::unordered_set<std::string> m_excludedNames;

    void RecordDecl(const std::string &name, int scopeId) {
        m_declaredAnywhere.insert(name);
        if (IsOwnedRegisterName(name)) {
            m_declCount[name]++;
            m_declScope[name] = scopeId;
        }
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

    void RecordAccess(const std::string &name, int scopeId) {
        if (IsOwnedRegisterName(name))
            m_access[name].insert(scopeId);
    }

    // collect every identifier read inside an expression as an access in `scopeId`.
    void CollectExpr(const std::shared_ptr<Expression> &e, int scopeId) {
        if (!e)
            return;
        switch (e->nodeKind) {
        case ASTNodeKind::IdentifierExpression:
            if (auto id = std::static_pointer_cast<IdentifierExpressionNode>(e); id->identifier)
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
            if (fn->lpFunctionBody)
                m_nestedFunctions.push_back({&fn->lpFunctionBody->body, fn->capturedNames});
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

        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier) {
                RecordDecl(id->identifier->name, scopeId);
                RecordAccess(id->identifier->name, scopeId);
            }
            CollectExpr(decl->value, scopeId);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); id && id->identifier && IsOwnedRegisterName(id->identifier->name))
                m_bareAssigned.insert(id->identifier->name);
            CollectExpr(asn->left, scopeId);
            CollectExpr(asn->right, scopeId);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
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
                    m_declaredAnywhere.insert(lv->identifier->name); // loop var is a decl in the body
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
                        m_declaredAnywhere.insert(id->identifier->name);
                WalkBlock(fg->body->body, c);
            }
            return;
        }
        if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            // `local function vN()` declares vN in this scope (it is a FunctionDeclarationNode, not a
            // VariableDeclarationNode); record it so we never insert a duplicate decl for it.
            if (fdn->bIsLocalDeclaration && IsRegisterName(fdn->functionName)) {
                RecordDecl(fdn->functionName, scopeId);
                RecordAccess(fdn->functionName, scopeId);
            }
            if (fdn->lpFunctionBody)
                m_nestedFunctions.push_back({&fdn->lpFunctionBody->body, fdn->capturedNames});
            return;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            // transparent container at hoister time (ScopeBlockIntroducer runs later): same scope
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

    // for a name with no declaration anywhere, insert `local vN` at the lowest scope dominating all
    // its accesses, hoisted out of any loop body (a loop-carried value must be declared outside the
    // loop or it re-initialises each iteration).
    void InsertMissingDeclarations() {
        for (const auto &[name, scopeSet] : m_access) {
            if (m_declaredAnywhere.count(name))
                continue; // has a decl somewhere -> M3's job (demotion), not M2
            if (scopeSet.empty())
                continue;

            int target = *scopeSet.begin();
            for (int s : scopeSet)
                target = LCA(target, s);
            if (target < 0)
                continue;

            // hoist above every loop body: a decl inside a loop re-runs per iteration
            while (target >= 0 && m_scopes[target].isLoopBody && m_scopes[target].parent >= 0)
                target = m_scopes[target].parent;
            if (target < 0)
                continue;

            // only act on names the lifter bare-assigns (a genuine leak with consistent `vN` naming);
            // a name that only ever appears in reads here is the debug-name-mismatch case; skip it.
            if (!m_bareAssigned.count(name))
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
                body->insert(body->begin(), std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
        }
    }

    // is `anc` an ancestor of (or equal to) scope `s` in the scope tree?
    bool IsAncestorOrSelf(int anc, int s) const {
        while (s >= 0) {
            if (s == anc)
                return true;
            s = m_scopes[s].parent;
        }
        return false;
    }

    // M3: a name with exactly one `local name = expr` declaration whose scope does NOT dominate all of
    // its accesses is placed too deep (a use sits above or beside the decl and reads a global/nil).
    // Hoist it: declare `local name` once at the access LCA and turn the original into a plain
    // assignment. Conservative gates keep this off deliberate shadowing and the interim-name-mismatch
    // case: single decl only, never a bare-assigned name (those went through M2), and the decl must
    // carry a value (a bare `local name` has nothing to demote).
    void DemoteDeepDeclarations() {
        for (const auto &[name, count] : m_declCount) {
            if (count != 1)
                continue; // multiple decls -> intentional shadowing, leave it
            const auto scopeIt = m_declScope.find(name);
            const auto accIt = m_access.find(name);
            if (scopeIt == m_declScope.end() || accIt == m_access.end())
                continue;
            const int declScope = scopeIt->second;

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
            // find the single `local name = expr` in the decl scope's top-level statements
            int declIdx = -1;
            for (int i = 0; i < static_cast<int>(declBody->size()); ++i)
                if (auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>((*declBody)[i]); vd && DeclName(vd) == name && vd->value) {
                    declIdx = i;
                    break;
                }
            if (declIdx < 0)
                continue; // no simple valued decl found (bare local, or nested) -> skip

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
            auto vd = std::static_pointer_cast<VariableDeclarationNode>((*declBody)[declIdx]);
            (*declBody)[declIdx] = std::make_shared<AssignmentStatementNode>(vd->identifier, vd->value);
            m_scopes[target].body->insert(m_scopes[target].body->begin(), std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
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
            if (m_declCount[name] <= 1 || rootDeclarations.contains(name))
                continue;
            root->insert(root->begin(), std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
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
    bool StatementMentions(const std::shared_ptr<Statement> &s, const std::string &name) {
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
                    if (StatementMentions(c, name))
                        return true;
            if (iff->elseBranch)
                for (const auto &c : iff->elseBranch->body)
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            if (ExprMentions(w->condition, name))
                return true;
            if (w->body)
                for (const auto &c : w->body->body)
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            if (r->body)
                for (const auto &c : r->body->body)
                    if (StatementMentions(c, name))
                        return true;
            return ExprMentions(r->condition, name);
        }
        if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
            if (ExprMentions(fn->startVariable, name) || ExprMentions(fn->increaseBy, name) || ExprMentions(fn->maxIncreased, name))
                return true;
            if (fn->lpLoopBody)
                for (const auto &c : fn->lpLoopBody->body)
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            if (ExprMentions(fg->generator, name) || ExprMentions(fg->state, name) || ExprMentions(fg->index, name))
                return true;
            if (fg->body)
                for (const auto &c : fg->body->body)
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            if (fdn->lpFunctionBody)
                for (const auto &c : fdn->lpFunctionBody->body)
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            for (const auto &c : blk->body)
                if (StatementMentions(c, name))
                    return true;
            return false;
        }
        return false;
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
                    if (StatementMentions(c, name))
                        return true;
            return false;
        }
        default:
            return true;
        }
    }

    void CoalesceAdjacentDeclarations(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[i + 1]);
            const auto name = decl ? DeclName(decl) : std::string{};
            auto lhs = asn ? std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left) : nullptr;
            if (decl && !decl->value && IsRegisterName(name) && lhs && lhs->identifier && lhs->identifier->name == name && !ExprMentions(asn->right, name)) {
                decl->value = asn->right;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                --i;
            }
        }
    }
};
