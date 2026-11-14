//
// Created by Dottik on 2/6/2026.
//

// Collapses short-circuit diamonds only when all duplicated tails render identically.

#pragma once
#include "FissionAllocator.hpp"
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class ShortCircuitFolder : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i < stmts.size();) {
            if (TryFoldSharedTailIf(stmts, i))
                continue; // folded in place; re-check the same index
            if (i + 1 < stmts.size() && TryFoldAt(stmts, i))
                continue; // folded in place; re-check the same index
            ++i;
        }
    }

  private:
    // whether every path through `body` writes `name` before reading it. Folding a guard turns the
    // store before it into a conditional store, so the store must be unobservable on the path that
    // skips it; requiring it first on every path (and into a constant) proves that.
    static bool EveryPathAssignsFirst(const std::vector<std::shared_ptr<Statement>> &body, const std::string &name) {
        for (const auto &stmt : body) {
            if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
                return SimpleIdentName(asn->left).value_or("") == name && AsConstant(asn->right);
            if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt))
                return decl->value && SimpleIdentName(decl->identifier).value_or("") == name && AsConstant(decl->value);
            if (auto nested = std::dynamic_pointer_cast<IfStatementNode>(stmt))
                return nested->thenBranch && nested->elseBranch && EveryPathAssignsFirst(nested->thenBranch->body, name) &&
                       EveryPathAssignsFirst(nested->elseBranch->body, name);
            return false;
        }
        return false;
    }

    // `if C1 then A; if not C2 then T end else T end` (or the complement with `not C1` and the
    // arms swapped) puts the same tail T on both the guard's false path and the outer else path,
    // once per nesting level, so an and/or chain duplicates its tail exponentially. When A only
    // stores a constant that T always overwrites first, collapsing the two tests preserves the
    // store on every path that can observe it: `if C1 and C2 then A else T end`.
    static bool TryFoldSharedTailIf(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        auto outer = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
        if (!outer || !outer->thenBranch || !outer->elseBranch)
            return false;
        if (TryFoldArm(*outer, false))
            return true;
        return TryFoldArm(*outer, true);
    }

    static bool TryFoldArm(IfStatementNode &outer, bool negated) {
        auto actionArm = negated ? outer.elseBranch : outer.thenBranch;
        auto tailArm = negated ? outer.thenBranch : outer.elseBranch;
        if (!actionArm || !tailArm)
            return false;
        auto &body = actionArm->body;
        if (body.size() != 2)
            return false;
        auto stored = std::dynamic_pointer_cast<AssignmentStatementNode>(body[0]);
        auto guard = std::dynamic_pointer_cast<IfStatementNode>(body[1]);
        if (!stored || !guard || guard->elseBranch || !guard->thenBranch)
            return false;
        const auto storedName = SimpleIdentName(stored->left);
        if (!storedName || !AsConstant(stored->right))
            return false;
        auto innerNot = std::dynamic_pointer_cast<UnaryExpressionNode>(guard->condition);
        if (!innerNot || innerNot->op != "not ")
            return false;
        if (!StatementsEqual(tailArm->body, guard->thenBranch->body))
            return false;
        if (!EveryPathAssignsFirst(tailArm->body, *storedName))
            return false;

        std::shared_ptr<Expression> outerCondition = outer.condition;
        if (negated) {
            auto outerNot = std::dynamic_pointer_cast<UnaryExpressionNode>(outerCondition);
            if (!outerNot || outerNot->op != "not ")
                return false;
            outerCondition = outerNot->operand;
        }

        outer.condition = Fission::MakeShared<BinaryExpressionNode>("and", outerCondition, innerNot->operand);
        auto foldedArm = Fission::MakeShared<BlockStatementNode>();
        foldedArm->body = {body[0]};
        outer.thenBranch = foldedArm;
        outer.elseBranch = guard->thenBranch;
        return true;
    }

    static bool AsConstant(const std::shared_ptr<Expression> &expr) {
        return AsLiteral<BooleanLiteralNode>(expr) || AsLiteral<NilLiteralNode>(expr) || AsLiteral<NumberLiteralNode>(expr) ||
               AsLiteral<IntegerLiteralNode>(expr) || AsLiteral<StringLiteralNode>(expr);
    }

    static std::optional<std::string> SimpleIdentName(const std::shared_ptr<Expression> &expr) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
            return id->identifier->name;
        return std::nullopt;
    }

    // value written to `v` by `stmt` (`v = rhs`, or a Call/NameCall whose single ret is v), else null.
    static std::shared_ptr<Expression> AsAssignToVar(const std::shared_ptr<Statement> &stmt, const std::string &v) {
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            if (auto name = SimpleIdentName(asn->left); name && *name == v)
                return asn->right;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression))
                if (nc->rets.size() == 1 && SimpleIdentName(nc->rets[0]).value_or("") == v)
                    return nc;
            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(es->expression))
                if (c->rets.size() == 1 && SimpleIdentName(c->rets[0]).value_or("") == v)
                    return c;
        }
        return nullptr;
    }

    // a Call/NameCall pulled into a sub-expression must render inline (no `local x =` prefix).
    static void InlineifyValue(const std::shared_ptr<Expression> &e) {
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            nc->rets.clear();
            nc->inlineCall = true;
        } else if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            c->rets.clear();
            c->inlineCall = true;
        }
    }

    static std::string RenderStatements(const std::vector<std::shared_ptr<Statement>> &body) {
        SourceGenerator g;
        for (const auto &s : body)
            if (s)
                s->Accept(&g);
        return g.buffer.str();
    }
    static bool StatementsEqual(const std::vector<std::shared_ptr<Statement>> &a, const std::vector<std::shared_ptr<Statement>> &b) {
        return RenderStatements(a) == RenderStatements(b);
    }

    static bool TryFoldAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t i) {
        auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
        std::string vname;
        if (decl && decl->value == nullptr)
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                vname = id->identifier->name;
        if (vname.empty())
            return false;

        auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
        if (!ifS || !ifS->thenBranch || !ifS->elseBranch)
            return false;

        auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition);
        auto &thenB = ifS->thenBranch->body;
        auto &elseB = ifS->elseBranch->body;
        if (!un || un->op != "not " || thenB.empty() || elseB.empty())
            return false;

        auto fallback = AsAssignToVar(thenB[0], vname);
        auto primary = AsAssignToVar(elseB[0], vname);
        if (fallback && primary && thenB.size() == 1 && elseB.size() <= 2) {
            auto selected = std::dynamic_pointer_cast<BinaryExpressionNode>(AsAssignToVar(elseB.back(), vname));
            const auto fallbackName = SimpleIdentName(fallback);
            const bool plainFallback = (fallbackName && *fallbackName != vname) || AsLiteral<NilLiteralNode>(fallback) ||
                                       AsLiteral<BooleanLiteralNode>(fallback) || AsLiteral<NumberLiteralNode>(fallback) ||
                                       AsLiteral<IntegerLiteralNode>(fallback) || AsLiteral<StringLiteralNode>(fallback);
            if (selected && selected->op == "or" && StatementsEqual({selected->right}, {fallback}) &&
                (elseB.size() == 1 || (plainFallback && SimpleIdentName(selected->left).value_or("") == vname))) {
                if (elseB.size() == 1)
                    primary = selected->left;
                InlineifyValue(primary);
                InlineifyValue(fallback);
                auto andExpr = Fission::MakeShared<BinaryExpressionNode>("and", un->operand, primary);
                auto orExpr = Fission::MakeShared<BinaryExpressionNode>("or", andExpr, fallback);
                stmts[i + 1] = Fission::MakeShared<AssignmentStatementNode>(decl->identifier, orExpr);
                return true;
            }
        }
        if (elseB.size() != 2)
            return false;
        auto innerIf = std::dynamic_pointer_cast<IfStatementNode>(elseB[1]);
        const bool innerOk = innerIf && innerIf->thenBranch && !innerIf->elseBranch && SimpleIdentName(innerIf->condition).value_or("") == vname;
        if (!fallback || !primary || !innerOk)
            return false;

        std::vector<std::shared_ptr<Statement>> tailThen(thenB.begin() + 1, thenB.end());
        std::vector<std::shared_ptr<Statement>> tailInner = innerIf->thenBranch->body;
        std::vector<std::shared_ptr<Statement>> tailAfter(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 2, stmts.end());
        if (!StatementsEqual(tailThen, tailAfter) || !StatementsEqual(tailInner, tailAfter))
            return false;

        InlineifyValue(primary);
        InlineifyValue(fallback);
        auto andExpr = Fission::MakeShared<BinaryExpressionNode>("and", un->operand, primary);
        auto orExpr = Fission::MakeShared<BinaryExpressionNode>("or", andExpr, fallback);
        decl->value = orExpr;                                            // local V = C and P or F
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1); // drop the diamond
        return true;
    }
};
