// Folds pure branch assignments to one target into a Luau if-expression.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

class IfExpressionFolder {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) { FoldFunction(statements); }

  private:
    static std::shared_ptr<Expression> Negate(const std::shared_ptr<Expression> &condition) {
        if (const auto inverse = std::dynamic_pointer_cast<UnaryExpressionNode>(condition); inverse && inverse->op == "not ")
            return inverse->operand;
        if (const auto compare = std::dynamic_pointer_cast<BinaryExpressionNode>(condition); compare && (compare->op == "==" || compare->op == "~="))
            return std::make_shared<BinaryExpressionNode>(compare->op == "==" ? "~=" : "==", compare->left, compare->right);
        return std::make_shared<UnaryExpressionNode>("not ", condition);
    }

    static bool EndsInBareReturn(const std::shared_ptr<BlockStatementNode> &arm) {
        const auto ret = arm && !arm->body.empty() ? std::dynamic_pointer_cast<ReturnStatementNode>(arm->body.back()) : nullptr;
        return ret && ret->returnValues.empty();
    }

    // Luau copies the function's closing `return` into the arms of its last if; falling off the end says the same.
    static void DropTailReturns(std::vector<std::shared_ptr<Statement>> &body) {
        const auto branch = body.empty() ? nullptr : std::dynamic_pointer_cast<IfStatementNode>(body.back());
        if (!branch)
            return;
        if (branch->elseBranch && branch->elseBranch->body.size() == 1 && EndsInBareReturn(branch->elseBranch))
            branch->elseBranch.reset();
        if (branch->elseBranch && branch->thenBranch && branch->thenBranch->body.size() == 1 && EndsInBareReturn(branch->thenBranch)) {
            branch->condition = Negate(branch->condition);
            branch->thenBranch = std::move(branch->elseBranch);
        }
        for (const auto &arm : {branch->thenBranch, branch->elseBranch}) {
            if (!arm)
                continue;
            if (arm->body.size() > 1 && EndsInBareReturn(arm))
                arm->body.pop_back();
            DropTailReturns(arm->body);
        }
    }

    // `if a then A; return end; if b then B else C end` closing a function is one chain: `if a then A elseif b ...`
    static void MergeTailGuards(std::vector<std::shared_ptr<Statement>> &body) {
        while (body.size() >= 2) {
            const auto chain = std::dynamic_pointer_cast<IfStatementNode>(body.back());
            const auto guard = std::dynamic_pointer_cast<IfStatementNode>(body[body.size() - 2]);
            if (!chain || !guard || guard->elseBranch || !guard->thenBranch || guard->thenBranch->body.size() < 2 || !EndsInBareReturn(guard->thenBranch))
                return;
            guard->thenBranch->body.pop_back();
            guard->elseBranch = std::make_shared<BlockStatementNode>();
            guard->elseBranch->body.push_back(chain);
            body.pop_back();
        }
    }

    void FoldFunction(std::vector<std::shared_ptr<Statement>> &body) {
        Fold(body);
        if (!body.empty())
            if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(body.back()); ret && ret->returnValues.empty())
                body.pop_back();
        DropTailReturns(body);
        MergeTailGuards(body);
        for (size_t i = body.size(); i > 0; --i) {
            auto guard = std::dynamic_pointer_cast<IfStatementNode>(body[i - 1]);
            if (!guard || guard->elseBranch || !guard->thenBranch || guard->thenBranch->body.size() != 1 || i + 1 != body.size() ||
                !std::dynamic_pointer_cast<ExpressionStatementNode>(body.back()))
                continue;
            auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(guard->thenBranch->body.front());
            if (!ret || !ret->returnValues.empty())
                continue;
            if (auto inverse = std::dynamic_pointer_cast<UnaryExpressionNode>(guard->condition); inverse && inverse->op == "not ")
                guard->condition = inverse->operand;
            else
                guard->condition = std::make_shared<UnaryExpressionNode>("not ", guard->condition);
            guard->thenBranch->body.assign(body.begin() + static_cast<std::ptrdiff_t>(i), body.end());
            body.erase(body.begin() + static_cast<std::ptrdiff_t>(i), body.end());
            break;
        }
    }

    // `<ident> = <expr>` -> capture name + value.
    static bool AsSimpleAssign(const std::shared_ptr<Statement> &s, std::string &name, std::shared_ptr<Expression> &value) {
        auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s);
        if (!asn)
            return false;
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
        if (!id || !id->identifier)
            return false;
        name = id->identifier->name;
        value = asn->right;
        return true;
    }

    static bool Calls(const std::shared_ptr<Expression> &expr) {
        if (!expr)
            return false;
        if (std::dynamic_pointer_cast<CallExpressionNode>(expr) || std::dynamic_pointer_cast<NameCallExpressionNode>(expr))
            return true;
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr))
            return Calls(bin->left) || Calls(bin->right);
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr))
            return Calls(un->operand);
        if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expr))
            return Calls(index->left) || Calls(index->right);
        if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr))
            return Calls(member->table) || Calls(member->key);
        if (auto table = std::dynamic_pointer_cast<TableLiteralNode>(expr))
            return std::ranges::any_of(table->expressions, [](const auto &value) { return Calls(value); });
        if (auto iff = std::dynamic_pointer_cast<IfExpressionNode>(expr))
            return Calls(iff->condition) || Calls(iff->thenExpr) || Calls(iff->elseExpr);
        return false;
    }

    static std::shared_ptr<Statement> OnlyStmt(const std::shared_ptr<BlockStatementNode> &b) {
        if (!b || b->body.size() != 1)
            return nullptr;
        return b->body[0];
    }

    // If `ifStmt` is a pure diamond that assigns a single target in every arm (the else arm may be a
    // nested diamond -> `elseif`), return the folded IfExpression and set `name`; else nullptr.
    std::shared_ptr<IfExpressionNode> AsIfExpr(const std::shared_ptr<IfStatementNode> &ifStmt, std::string &name) {
        if (!ifStmt || !ifStmt->condition || !ifStmt->thenBranch || !ifStmt->elseBranch)
            return nullptr;
        std::string tn;
        std::shared_ptr<Expression> tv;
        auto thenOnly = OnlyStmt(ifStmt->thenBranch);
        if (!thenOnly || !AsSimpleAssign(thenOnly, tn, tv))
            return nullptr;

        std::shared_ptr<Expression> elseExpr;
        auto elseOnly = OnlyStmt(ifStmt->elseBranch);
        std::string en;
        std::shared_ptr<Expression> ev;
        if (elseOnly && AsSimpleAssign(elseOnly, en, ev)) {
            if (en != tn)
                return nullptr;
            elseExpr = ev;
        } else if (auto nestedIf = std::dynamic_pointer_cast<IfStatementNode>(elseOnly)) {
            std::string nn;
            auto nested = AsIfExpr(nestedIf, nn);
            if (!nested || nn != tn)
                return nullptr;
            elseExpr = nested;
        } else {
            return nullptr;
        }

        name = tn;
        if (auto inverse = std::dynamic_pointer_cast<UnaryExpressionNode>(ifStmt->condition);
            inverse && inverse->op == "not " && !std::dynamic_pointer_cast<IfExpressionNode>(elseExpr))
            return std::make_shared<IfExpressionNode>(inverse->operand, elseExpr, tv);
        return std::make_shared<IfExpressionNode>(ifStmt->condition, tv, elseExpr);
    }

    void Fold(std::vector<std::shared_ptr<Statement>> &stmts) {
        // Fold at THIS level first (top-down): AsIfExpr must see the raw nested `if` in an else arm to
        // build the `elseif` chain; recursing first would rewrite it out from under the detection.
        for (size_t i = 0; i < stmts.size(); ++i) {
            auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
            if (!ifStmt)
                continue;
            std::string name;
            auto ifExpr = AsIfExpr(ifStmt, name);
            if (!ifExpr)
                continue;

            // Merge with an immediately-preceding bare `local <name>` -> `local <name> = <ifexpr>`.
            if (i > 0) {
                if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i - 1]); decl && !decl->value) {
                    if (auto did = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
                        did && did->identifier && did->identifier->name == name) {
                        std::unordered_set<std::string> names;
                        ScopeAwareRenamer::CollectIdentifierNames(std::make_shared<ExpressionStatementNode>(ifExpr), names);
                        const bool shadowsEarlierBinding =
                            names.contains(name) && std::any_of(stmts.begin(), stmts.begin() + static_cast<std::ptrdiff_t>(i - 1), [&](const auto &statement) {
                                auto earlier = std::dynamic_pointer_cast<VariableDeclarationNode>(statement);
                                auto id = earlier ? std::dynamic_pointer_cast<IdentifierExpressionNode>(earlier->identifier) : nullptr;
                                return id && id->identifier && id->identifier->name == name;
                            });
                        if (!names.contains(name) || shadowsEarlierBinding) {
                            stmts[i - 1] = std::make_shared<VariableDeclarationNode>(decl->identifier, ifExpr);
                            stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
                            --i;
                            continue;
                        }
                    }
                }
            }
            // Standalone diamond -> `<name> = <ifexpr>` (a reassignment expressed as an if-expression); arms that call
            // stay statements, as a chain assigning an existing local reads.
            if (Calls(ifExpr))
                continue;
            auto lhs = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
            stmts[i] = std::make_shared<AssignmentStatementNode>(lhs, ifExpr);
        }

        // Folded nodes have no remaining block children.
        for (auto &s : stmts)
            FoldChildren(s);
    }

    void FoldChildren(const std::shared_ptr<Statement> &s) {
        if (!s)
            return;
        if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
            FoldExpression(iff->condition);
            if (iff->thenBranch)
                Fold(iff->thenBranch->body);
            if (iff->elseBranch)
                Fold(iff->elseBranch->body);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            FoldExpression(w->condition);
            if (w->body)
                Fold(w->body->body);
        } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            FoldExpression(r->condition);
            if (r->body)
                Fold(r->body->body);
        } else if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
            FoldExpression(fn->startVariable);
            FoldExpression(fn->increaseBy);
            FoldExpression(fn->maxIncreased);
            if (fn->lpLoopBody)
                Fold(fn->lpLoopBody->body);
        } else if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            FoldExpression(fg->generator);
            FoldExpression(fg->state);
            FoldExpression(fg->index);
            if (fg->body)
                Fold(fg->body->body);
        } else if (auto fdn = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            if (fdn->lpFunctionBody)
                FoldFunction(fdn->lpFunctionBody->body);
        } else if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            Fold(blk->body);
        } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            FoldExpression(asn->left);
            FoldExpression(asn->right);
        } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            FoldExpression(decl->value);
        } else if (auto expr = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
            FoldExpression(expr->expression);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (auto &value : ret->returnValues)
                FoldExpression(value);
        }
    }

    void FoldExpression(const std::shared_ptr<Expression> &expr) {
        if (!expr)
            return;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr); fn && fn->lpFunctionBody) {
            FoldFunction(fn->lpFunctionBody->body);
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            FoldExpression(call->callee);
            for (auto &arg : call->arguments)
                FoldExpression(arg);
        } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            FoldExpression(nameCall->calledOn);
            for (auto &arg : nameCall->arguments)
                FoldExpression(arg);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            FoldExpression(bin->left);
            FoldExpression(bin->right);
        } else if (auto bin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            FoldExpression(bin->left);
            FoldExpression(bin->right);
        } else if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            FoldExpression(index->left);
            FoldExpression(index->right);
        } else if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            FoldExpression(member->table);
            FoldExpression(member->key);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            FoldExpression(un->operand);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &value : tbl->expressions)
                FoldExpression(value);
        } else if (auto iff = std::dynamic_pointer_cast<IfExpressionNode>(expr)) {
            FoldExpression(iff->condition);
            FoldExpression(iff->thenExpr);
            FoldExpression(iff->elseExpr);
        }
    }
};
