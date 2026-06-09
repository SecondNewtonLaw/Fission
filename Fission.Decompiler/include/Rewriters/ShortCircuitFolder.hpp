//
// Created by Dottik on 2/6/2026.
//
// Collapses the diamond Luau lowers `local V = C and P or F` into back to a single
// expression. The three duplicated `tail` copies must be identical (checked by
// rendering) for the fold to be sound.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

class ShortCircuitFolder : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (size_t i = 0; i + 1 < stmts.size();) {
            if (TryFoldAt(stmts, i))
                continue; // folded in place; re-check the same index
            ++i;
        }
    }

  private:
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
        if (!un || un->op != "not " || thenB.empty() || elseB.size() != 2)
            return false;

        auto fallback = AsAssignToVar(thenB[0], vname);
        auto primary = AsAssignToVar(elseB[0], vname);
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
        auto andExpr = std::make_shared<BinaryExpressionNode>("and", un->operand, primary);
        auto orExpr = std::make_shared<BinaryExpressionNode>("or", andExpr, fallback);
        decl->value = orExpr;                                                  // local V = C and P or F
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1);       // drop the diamond
        return true;
    }
};
