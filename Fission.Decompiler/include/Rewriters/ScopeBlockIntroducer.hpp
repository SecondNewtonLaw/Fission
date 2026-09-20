// Scopes non-crossing register-reuse phases in `do ... end` blocks using a linear bottom-up walk.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ScopeBlockIntroducer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        std::unordered_set<std::string> sink;
        ProcessList(statements, sink);
    }

  private:
    // Scope each child list before splitting this list at local redefinitions.
    void ProcessList(std::vector<std::shared_ptr<Statement>> &stmts, std::unordered_set<std::string> &refsOut) {
        const size_t n = stmts.size();
        if (n == 0)
            return;

        std::vector<std::unordered_set<std::string>> refs(n);
        std::vector<std::string> declName(n);
        bool bailMulti = false;
        for (size_t i = 0; i < n; ++i) {
            bool multi = false;
            declName[i] = DeclaredLocalName(stmts[i], multi);
            if (multi)
                bailMulti = true; // a statement binding >1 local: phase scoping unsupported, leave as-is
            ProcessStatement(stmts[i], refs[i]);
        }
        if (bailMulti) {
            for (auto &r : refs)
                for (const auto &nm : r)
                    refsOut.insert(nm);
            return;
        }

        // Per-lifetime last use, keyed by declaration index. A use at i extends the lifetime of the
        // latest declaration of that name at or before i, so a binding is NOT considered live past a
        // later redefinition of the same name (uses after the redefinition belong to the new binding).
        // A declaration's own bound name is excluded from its ref-set by ProcessStatement, so a
        // redefinition whose RHS does not read the old binding does not block its own cut.
        std::unordered_map<std::string, size_t> currentDecl;
        std::unordered_map<size_t, size_t> lifeLastUse;
        for (size_t i = 0; i < n; ++i) {
            for (const auto &nm : refs[i]) {
                auto it = currentDecl.find(nm);
                if (it != currentDecl.end())
                    lifeLastUse[it->second] = i;
            }
            if (!declName[i].empty())
                currentDecl[declName[i]] = i;
        }

        auto bubble = [&]() {
            for (auto &r : refs)
                for (const auto &nm : r)
                    refsOut.insert(nm);
        };

        // Cut before every redefinition (a `local X` whose name was already declared earlier in this
        // list), pulling the next phase's leading comments along.
        std::vector<size_t> cuts;
        {
            std::unordered_set<std::string> seen;
            size_t segStart = 0;
            for (size_t i = 0; i < n; ++i) {
                if (!declName[i].empty() && seen.count(declName[i]) != 0) {
                    size_t cut = i;
                    while (cut > segStart && std::dynamic_pointer_cast<CommentNode>(stmts[cut - 1]) != nullptr)
                        --cut;
                    if (cut > segStart) {
                        cuts.push_back(cut);
                        segStart = cut;
                    }
                }
                if (!declName[i].empty())
                    seen.insert(declName[i]);
            }
        }
        if (cuts.empty()) {
            bubble();
            return;
        }

        const auto originalCuts = cuts;
        for (size_t s = 0; s < cuts.size(); ++s) {
            const size_t previous = s == 0 ? 0 : originalCuts[s - 1];
            const size_t next = s + 1 < originalCuts.size() ? originalCuts[s + 1] : n;
            for (size_t i = previous; i < originalCuts[s]; ++i) {
                const auto use = lifeLastUse.find(i);
                if (!declName[i].empty() && use != lifeLastUse.end() && use->second >= originalCuts[s] && use->second < next)
                    cuts[s] = (std::min)(cuts[s], i);
            }
        }
        std::sort(cuts.begin(), cuts.end());
        cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());

        std::vector<size_t> bounds;
        bounds.reserve(cuts.size() + 2);
        bounds.push_back(0);
        for (size_t c : cuts)
            bounds.push_back(c);
        bounds.push_back(n);
        const size_t segCount = bounds.size() - 1;

        // segEndAt[i] = end (exclusive) of the segment containing i.
        std::vector<size_t> segEndAt(n, n);
        for (size_t s = 0; s < segCount; ++s)
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i)
                segEndAt[i] = bounds[s + 1];

        // crossing[d]: the lifetime declared at d is used at or after its segment's end (a do-block
        // around that segment would scope it away). A lifetime with no use is dead -> non-crossing.
        std::vector<bool> crossing(n, false);
        for (size_t i = 0; i < n; ++i) {
            if (declName[i].empty())
                continue;
            auto it = lifeLastUse.find(i);
            const size_t lu = (it != lifeLastUse.end()) ? it->second : i;
            crossing[i] = lu >= segEndAt[i];
        }

        // A segment becomes a do-block iff it has >=1 non-crossing local decl (a genuinely scoped
        // temp); wrapping a segment of only escapers would add noise without scoping anything.
        std::vector<bool> wrapped(segCount, false);
        std::vector<bool> mixed(segCount, false);
        bool anyWrap = false;
        for (size_t s = 0; s < segCount; ++s) {
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i)
                if (!declName[i].empty() && !crossing[i]) {
                    wrapped[s] = true;
                    anyWrap = true;
                    break;
                }
        }
        if (!anyWrap) {
            bubble();
            return;
        }

        std::unordered_map<std::string, size_t> firstDeclaration;
        for (size_t i = 0; i < n; ++i)
            if (!declName[i].empty() && !firstDeclaration.contains(declName[i]))
                firstDeclaration[declName[i]] = i;

        for (size_t s = 0; s < segCount; ++s) {
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i) {
                if (declName[i].empty() || !crossing[i] || firstDeclaration[declName[i]] == i || !CanDemoteLocal(stmts[i]))
                    continue;
                DemoteLocal(stmts[i]);
                declName[i].clear();
                crossing[i] = false;
            }
            bool hasLocal = false;
            bool hasCrossing = false;
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i) {
                hasLocal |= !declName[i].empty();
                hasCrossing |= !declName[i].empty() && crossing[i];
            }
            wrapped[s] = hasLocal && !hasCrossing;
            mixed[s] = hasLocal && hasCrossing;
            if (wrapped[s])
                anyWrap = true;
        }

        std::vector<std::vector<std::pair<size_t, size_t>>> partial(segCount);
        for (size_t s = 0; s < segCount; ++s) {
            if (!mixed[s])
                continue;
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i) {
                if (declName[i].empty() || crossing[i])
                    continue;
                const auto use = lifeLastUse.find(i);
                const size_t end = (use == lifeLastUse.end() ? i : use->second) + 1;
                bool containsCrossing = false;
                for (size_t j = i; j < end; ++j)
                    if (!declName[j].empty() && crossing[j]) {
                        containsCrossing = true;
                        break;
                    }
                if (!containsCrossing)
                    partial[s].emplace_back(i, end);
            }
        }

        std::vector<std::shared_ptr<Statement>> rebuilt;
        for (size_t s = 0; s < segCount; ++s) {
            std::vector<std::shared_ptr<Statement>> seg;
            for (size_t i = bounds[s]; i < bounds[s + 1]; ++i)
                seg.push_back(stmts[i]);
            if (seg.empty())
                continue;
            if (wrapped[s]) {
                // keep leading comments (e.g. the function-info block) outside the do-block
                size_t lead = 0;
                while (lead < seg.size() && std::dynamic_pointer_cast<CommentNode>(seg[lead]) != nullptr)
                    ++lead;
                for (size_t k = 0; k < lead; ++k)
                    rebuilt.push_back(seg[k]);
                if (lead < seg.size()) {
                    auto block = std::make_shared<BlockStatementNode>();
                    block->bEmitAsDoBlock = true;
                    block->body.assign(seg.begin() + lead, seg.end());
                    rebuilt.push_back(block);
                }
            } else if (!partial[s].empty()) {
                size_t i = bounds[s];
                size_t range = 0;
                while (i < bounds[s + 1]) {
                    while (range < partial[s].size() && partial[s][range].second <= i)
                        ++range;
                    if (range >= partial[s].size() || partial[s][range].first != i) {
                        rebuilt.push_back(stmts[i++]);
                        continue;
                    }
                    size_t end = partial[s][range].second;
                    while (++range < partial[s].size() && partial[s][range].first < end)
                        end = (std::max)(end, partial[s][range].second);
                    auto block = std::make_shared<BlockStatementNode>();
                    block->bEmitAsDoBlock = true;
                    block->body.assign(stmts.begin() + i, stmts.begin() + end);
                    rebuilt.push_back(block);
                    i = end;
                }
            } else {
                rebuilt.insert(rebuilt.end(), seg.begin(), seg.end());
            }
        }
        stmts = std::move(rebuilt);

        bubble();
    }

    // Single local name a statement binds as a fresh local (the redefinition trigger), or "".
    // The lifter emits `local` from four node kinds: VariableDeclaration, a local FunctionDeclaration,
    // and a Call / NameCall whose result is bound (rets + bIsLocalDeclaration). Sets `multi` when a
    // statement binds more than one local (or via a non-identifier target); multi-name phase scoping
    // is unsupported, so the caller leaves the whole list untouched.
    static std::string DeclaredLocalName(const std::shared_ptr<Statement> &stmt, bool &multi) {
        multi = false;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                return id->identifier->name;
            return "";
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->bIsLocalDeclaration && !fn->bAnonymousInline)
            return fn->functionName;
        // call-result local, e.g. `local v = o:m()`; usually an ExpressionStatement wrapping the call.
        if (auto inner = LocalDeclCall(stmt)) {
            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(inner))
                return SingleRetName(c->rets, multi);
            if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(inner))
                return SingleRetName(nc->rets, multi);
        }
        return "";
    }

    static bool CanDemoteLocal(const std::shared_ptr<Statement> &stmt) {
        if (std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
            return true;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt))
            return fn->bIsLocalDeclaration && !fn->bAnonymousInline;
        return LocalDeclCall(stmt) != nullptr;
    }

    static void DemoteLocal(std::shared_ptr<Statement> &stmt) {
        if (auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            stmt = std::make_shared<AssignmentStatementNode>(declaration->identifier, declaration->value);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            fn->bIsLocalDeclaration = false;
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(LocalDeclCall(stmt)))
            call->bIsLocalDeclaration = false;
        else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(LocalDeclCall(stmt)))
            nameCall->bIsLocalDeclaration = false;
    }

    // The underlying Call/NameCall of a call-result local declaration (`local rets = f()`), whether it
    // sits directly in the list or inside an ExpressionStatement; nullptr if the statement is not one.
    static std::shared_ptr<Expression> LocalDeclCall(const std::shared_ptr<Statement> &stmt) {
        std::shared_ptr<Expression> e;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt))
            e = es->expression;
        else
            e = std::dynamic_pointer_cast<Expression>(stmt);
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e); c && c->bIsLocalDeclaration && !c->rets.empty())
            return c;
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e); nc && nc->bIsLocalDeclaration && !nc->rets.empty())
            return nc;
        return nullptr;
    }

    static std::string SingleRetName(const std::vector<std::shared_ptr<Expression>> &rets, bool &multi) {
        if (rets.size() != 1) {
            multi = true; // `local a, b = f()`; unsupported
            return "";
        }
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(rets[0]);
        if (!id || !id->identifier) {
            multi = true; // non-identifier bind target; bail conservatively
            return "";
        }
        return id->identifier->name;
    }

    // Collect the names a statement REFERENCES (uses/writes), recursing into nested blocks and inline
    // closures (which are scoped in passing). A declaration's own bound name is excluded so it does
    // not count as a use of itself.
    void ProcessStatement(const std::shared_ptr<Statement> &stmt, std::unordered_set<std::string> &refs) {
        if (!stmt)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            ScanExpr(decl->value, refs); // RHS only; the bound LHS name is the declaration, not a use
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            ScanExpr(ifS->condition, refs);
            if (ifS->thenBranch)
                ProcessList(ifS->thenBranch->body, refs);
            if (ifS->elseBranch)
                ProcessList(ifS->elseBranch->body, refs);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (fn->lpFunctionBody)
                ProcessList(fn->lpFunctionBody->body, refs);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            ScanExpr(w->condition, refs);
            if (w->body)
                ProcessList(w->body->body, refs);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            ScanExpr(r->condition, refs);
            if (r->body)
                ProcessList(r->body->body, refs);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            ScanExpr(fnum->startVariable, refs);
            ScanExpr(fnum->increaseBy, refs);
            ScanExpr(fnum->maxIncreased, refs);
            if (fnum->lpLoopBody)
                ProcessList(fnum->lpLoopBody->body, refs);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            ScanExpr(fgen->generator, refs);
            ScanExpr(fgen->state, refs);
            ScanExpr(fgen->index, refs);
            if (fgen->body)
                ProcessList(fgen->body->body, refs);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            ScanExpr(asn->left, refs); // a bare-identifier LHS write to a scoped-away local is unsafe
            ScanExpr(asn->right, refs);
            return;
        }
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            ScanCallOrExpr(es->expression, refs);
            return;
        }
        if (std::dynamic_pointer_cast<CallExpressionNode>(stmt) || std::dynamic_pointer_cast<NameCallExpressionNode>(stmt)) {
            ScanCallOrExpr(std::dynamic_pointer_cast<Expression>(stmt), refs);
            return;
        }
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &v : ret->returnValues)
                ScanExpr(v, refs);
            return;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            ProcessList(blk->body, refs);
            return;
        }
        // BreakStatementNode / ContinueStatementNode / CommentNode reference nothing. Any future
        // statement type lands here referencing nothing; which could permit an unsafe cut, so new
        // statement kinds that can read locals MUST be added above.
    }

    // Scan a call used as a statement: callee/args are uses; rets are uses only when the call is NOT
    // a local declaration (a local decl's rets are the bound names). Falls back to a plain expression
    // scan when `e` is not a call.
    void ScanCallOrExpr(const std::shared_ptr<Expression> &e, std::unordered_set<std::string> &refs) {
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            ScanExpr(c->callee, refs);
            for (const auto &a : c->arguments)
                ScanExpr(a, refs);
            if (!c->bIsLocalDeclaration)
                for (const auto &r : c->rets)
                    ScanExpr(r, refs);
            return;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            ScanExpr(nc->calledOn, refs);
            for (const auto &a : nc->arguments)
                ScanExpr(a, refs);
            if (!nc->bIsLocalDeclaration)
                for (const auto &r : nc->rets)
                    ScanExpr(r, refs);
            return;
        }
        ScanExpr(e, refs);
    }

    void ScanExpr(const std::shared_ptr<Expression> &expr, std::unordered_set<std::string> &refs) {
        if (!expr)
            return;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr)) {
            if (id->identifier)
                refs.insert(id->identifier->name);
            return;
        }
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            ScanExpr(mem->table, refs);
            ScanExpr(mem->key, refs);
            return;
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            ScanExpr(idx->left, refs);
            ScanExpr(idx->right, refs);
            return;
        }
        if (auto cmp = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            // separate type: does NOT derive from BinaryExpressionNode, so must be handled explicitly
            ScanExpr(cmp->left, refs);
            ScanExpr(cmp->right, refs);
            return;
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            // also catches TableBinaryExpressionNode (derives from BinaryExpressionNode)
            ScanExpr(bin->left, refs);
            ScanExpr(bin->right, refs);
            return;
        }
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            ScanExpr(un->operand, refs);
            return;
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            ScanExpr(call->callee, refs);
            for (const auto &a : call->arguments)
                ScanExpr(a, refs);
            for (const auto &rt : call->rets)
                ScanExpr(rt, refs);
            return;
        }
        if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            ScanExpr(nameCall->calledOn, refs);
            for (const auto &a : nameCall->arguments)
                ScanExpr(a, refs);
            for (const auto &rt : nameCall->rets)
                ScanExpr(rt, refs);
            return;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            if (fn->lpFunctionBody)
                ProcessList(fn->lpFunctionBody->body, refs); // scope inside inline closures too
            return;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (const auto &e : tbl->expressions)
                ScanExpr(e, refs);
            return;
        }
        // literals (nil/bool/number/integer/string/vector), VarArgExpression, NoExpression reference
        // nothing. New expression kinds that can read locals MUST be added above.
    }
};
