//
// Normalizes negated conditions and folds loop guards back into the loop condition.
//

// Luau lowers `while a and b do` to `while a do if not b then break end ... end` and folds
// `not (not a or not b)` into a temporary plus `not` at the use site. Both shapes are faithful
// but obscure the source. This pass rewrites the shapes back without changing evaluation order.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "FissionAllocator.hpp"
#include "Rewriters/ASTRewriter.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"
#include "SafetyGuard.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

class ConditionFolder : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts) {
            Fission::CheckDecompileDeadline();
            if (!stmt)
                continue;
            NormalizeStatement(stmt);
            FoldOneShotRepeat(stmt);
            FoldLoopGuard(stmt);
        }
        // the fold may expose a `not` that the loop above already passed.
        FoldNegatedTemps(stmts);
        for (auto &stmt : stmts)
            NormalizeStatement(stmt);
    }

  private:
    static std::optional<std::string> IdentifierName(const std::shared_ptr<Expression> &expr) {
        const auto identifier = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr);
        if (!identifier || !identifier->identifier)
            return std::nullopt;
        return identifier->identifier->name;
    }

    // `local X = E; return not X` -> `return not E`. The temp only exists because the lifter needs the
    // negated operand in a register; adjacency keeps evaluation order.
    static void FoldNegatedTemps(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
            const auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            if (!declaration || !declaration->value || declaration->bExported)
                continue;
            const auto name = IdentifierName(declaration->identifier);
            if (!name)
                continue;
            const auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmts[i + 1]);
            if (!ret || ret->returnValues.size() != 1)
                continue;
            const auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(ret->returnValues.front());
            if (!unary || unary->op != "not ")
                continue;
            const auto operand = IdentifierName(unary->operand);
            if (!operand || *operand != *name)
                continue;
            std::unordered_set<std::string> names;
            ScopeAwareRenamer::CollectIdentifierNames(Fission::MakeShared<ExpressionStatementNode>(declaration->value), names);
            if (names.contains(*name))
                continue;
            stmts[i] = Fission::MakeShared<ReturnStatementNode>(
                std::vector<std::shared_ptr<Expression>>{Fission::MakeShared<UnaryExpressionNode>("not ", declaration->value)}
            );
            stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
        }
    }

    static std::shared_ptr<UnaryExpressionNode> AsNot(const std::shared_ptr<Expression> &expr) {
        auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr);
        return unary && unary->op == "not " ? unary : nullptr;
    }

    static int CountNegations(const std::shared_ptr<Expression> &expr) {
        if (!expr)
            return 0;
        if (AsNot(expr))
            return 1 + CountNegations(AsNot(expr)->operand);
        if (expr->nodeKind == ASTNodeKind::BinaryExpression || expr->nodeKind == ASTNodeKind::TableBinaryExpression) {
            const auto binary = std::static_pointer_cast<BinaryExpressionNode>(expr);
            return CountNegations(binary->left) + CountNegations(binary->right);
        }
        if (expr->nodeKind == ASTNodeKind::IfExpression) {
            const auto conditional = std::static_pointer_cast<IfExpressionNode>(expr);
            return CountNegations(conditional->condition) + CountNegations(conditional->thenExpr) + CountNegations(conditional->elseExpr);
        }
        return 0;
    }

    // Builds an expression equivalent to `not expr`, pushing the negation into `and`/`or` and flipping
    // `==`/`~=`. Operands stay in evaluation order, so short-circuiting is preserved.
    // `truthiness` says the result is only tested for truthiness, which makes double-negation removal safe.
    // In a value position `not (not a or not b)` must keep yielding a boolean, so it is left alone.
    static std::shared_ptr<Expression> PushNegation(const std::shared_ptr<Expression> &expr, bool truthiness) {
        if (!expr)
            return nullptr;
        if (const auto unary = AsNot(expr))
            return truthiness ? unary->operand : Fission::MakeShared<UnaryExpressionNode>("not ", expr);
        if (const auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            if (binary->op == "and" || binary->op == "or")
                return Fission::MakeShared<BinaryExpressionNode>(
                    binary->op == "and" ? "or" : "and", PushNegation(binary->left, truthiness), PushNegation(binary->right, truthiness)
                );
            if (binary->op == "==")
                return Fission::MakeShared<BinaryExpressionNode>("~=", binary->left, binary->right);
            if (binary->op == "~=")
                return Fission::MakeShared<BinaryExpressionNode>("==", binary->left, binary->right);
        }
        return Fission::MakeShared<UnaryExpressionNode>("not ", expr);
    }

    // `not X` -> X; comparisons flip; anything else is wrapped in `not (...)`.
    static std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &condition) {
        if (const auto unary = AsNot(condition))
            return unary->operand;
        if (const auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(condition)) {
            if (binary->op == "==")
                return Fission::MakeShared<BinaryExpressionNode>("~=", binary->left, binary->right);
            if (binary->op == "~=")
                return Fission::MakeShared<BinaryExpressionNode>("==", binary->left, binary->right);
        }
        return Fission::MakeShared<UnaryExpressionNode>("not ", condition);
    }

    static void NormalizeChildren(std::shared_ptr<Expression> &expr, bool truthiness) {
        switch (expr->nodeKind) {
        case ASTNodeKind::CallExpression: {
            const auto call = std::static_pointer_cast<CallExpressionNode>(expr);
            NormalizeExpression(call->callee, false);
            for (auto &argument : call->arguments)
                NormalizeExpression(argument, false);
            for (auto &ret : call->rets)
                NormalizeExpression(ret, false);
            break;
        }
        case ASTNodeKind::MethodCallExpression: {
            const auto call = std::static_pointer_cast<NameCallExpressionNode>(expr);
            NormalizeExpression(call->calledOn, false);
            for (auto &argument : call->arguments)
                NormalizeExpression(argument, false);
            for (auto &ret : call->rets)
                NormalizeExpression(ret, false);
            break;
        }
        case ASTNodeKind::BinaryExpression:
        case ASTNodeKind::TableBinaryExpression: {
            const auto binary = std::static_pointer_cast<BinaryExpressionNode>(expr);
            // in a truthiness context the operands are only tested, so they inherit it.
            const bool operandsAreTruthiness = truthiness && (binary->op == "and" || binary->op == "or");
            NormalizeExpression(binary->left, operandsAreTruthiness);
            NormalizeExpression(binary->right, operandsAreTruthiness);
            break;
        }
        case ASTNodeKind::CompoundAssignment: {
            const auto compound = std::static_pointer_cast<CompoundBinaryExpressionNode>(expr);
            NormalizeExpression(compound->left, false);
            NormalizeExpression(compound->right, false);
            break;
        }
        case ASTNodeKind::IndexExpression: {
            const auto index = std::static_pointer_cast<IndexExpressionNode>(expr);
            NormalizeExpression(index->left, false);
            NormalizeExpression(index->right, false);
            break;
        }
        case ASTNodeKind::MemberExpression: {
            const auto member = std::static_pointer_cast<MemberExpressionNode>(expr);
            NormalizeExpression(member->table, false);
            NormalizeExpression(member->key, false);
            break;
        }
        case ASTNodeKind::UnaryExpression: {
            // `not` coerces its operand to a boolean, so the operand is only tested for truthiness.
            const auto unary = std::static_pointer_cast<UnaryExpressionNode>(expr);
            NormalizeExpression(unary->operand, unary->op == "not ");
            break;
        }
        case ASTNodeKind::LiteralValue:
            if (std::static_pointer_cast<LiteralNode>(expr)->bIsTableLiteral) {
                const auto table = std::static_pointer_cast<TableLiteralNode>(expr);
                for (auto &element : table->expressions)
                    NormalizeExpression(element, false);
            }
            break;
        case ASTNodeKind::IfExpression: {
            const auto conditional = std::static_pointer_cast<IfExpressionNode>(expr);
            NormalizeExpression(conditional->condition, true);
            NormalizeExpression(conditional->thenExpr, truthiness);
            NormalizeExpression(conditional->elseExpr, truthiness);
            break;
        }
        default:
            break;
        }
    }

    // `true and X` / `false or X` evaluate to X; `false and X` / `true or X` never evaluate X, so the
    // literal itself is the result. Both directions keep side effects and values identical.
    static std::shared_ptr<Expression> FoldBooleanLiteralOperand(const std::shared_ptr<Expression> &expr) {
        const auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr);
        if (!binary || (binary->op != "and" && binary->op != "or") || !binary->left)
            return nullptr;
        bool truthy = false;
        if (binary->left->nodeKind == ASTNodeKind::LiteralValue) {
            const auto literal = std::static_pointer_cast<LiteralNode>(binary->left);
            if (literal->literalKind == LiteralNodeKind::Boolean)
                truthy = std::static_pointer_cast<BooleanLiteralNode>(binary->left)->value;
            else if (literal->literalKind != LiteralNodeKind::Nil)
                return nullptr;
        } else {
            return nullptr;
        }
        const bool shortCircuits = binary->op == "and" ? !truthy : truthy;
        return shortCircuits ? binary->left : binary->right;
    }

    static void NormalizeExpression(std::shared_ptr<Expression> &expr, bool truthiness) {
        if (!expr)
            return;
        NormalizeChildren(expr, truthiness);
        // Only in conditions: value positions keep the source's short-circuit form (`true and t[""]`).
        if (truthiness) {
            if (auto folded = FoldBooleanLiteralOperand(expr)) {
                expr = std::move(folded);
                NormalizeExpression(expr, truthiness);
                return;
            }
        }
        const auto unary = AsNot(expr);
        if (!unary)
            return;
        // Only push the negation in when it removes negations overall: `not (not a or not b)` -> `a and b`
        // in a condition, but `not (a and b)` stays, since `not a or not b` reads worse.
        auto pushed = PushNegation(unary->operand, truthiness);
        if (!pushed || CountNegations(pushed) >= CountNegations(expr))
            return;
        expr = std::move(pushed);
        NormalizeExpression(expr, truthiness);
    }

    static void NormalizeStatement(std::shared_ptr<Statement> &stmt) {
        switch (stmt->nodeKind) {
        case ASTNodeKind::VariableDeclaration: {
            const auto declaration = std::static_pointer_cast<VariableDeclarationNode>(stmt);
            NormalizeExpression(declaration->value, false);
            break;
        }
        case ASTNodeKind::AssignmentStatement: {
            const auto assignment = std::static_pointer_cast<AssignmentStatementNode>(stmt);
            NormalizeExpression(assignment->left, false);
            NormalizeExpression(assignment->right, false);
            break;
        }
        case ASTNodeKind::ExpressionStatement: {
            const auto statement = std::static_pointer_cast<ExpressionStatementNode>(stmt);
            NormalizeExpression(statement->expression, false);
            break;
        }
        case ASTNodeKind::ReturnExpression: {
            const auto ret = std::static_pointer_cast<ReturnStatementNode>(stmt);
            for (auto &value : ret->returnValues)
                NormalizeExpression(value, false);
            break;
        }
        case ASTNodeKind::IfStatement: {
            const auto branch = std::static_pointer_cast<IfStatementNode>(stmt);
            NormalizeExpression(branch->condition, true);
            break;
        }
        case ASTNodeKind::WhileStatement: {
            const auto loop = std::static_pointer_cast<WhileStatementNode>(stmt);
            NormalizeExpression(loop->condition, true);
            break;
        }
        case ASTNodeKind::RepeatStatement: {
            const auto loop = std::static_pointer_cast<RepeatStatementNode>(stmt);
            NormalizeExpression(loop->condition, true);
            break;
        }
        case ASTNodeKind::ForNumeric: {
            const auto loop = std::static_pointer_cast<ForNumericNode>(stmt);
            NormalizeExpression(loop->startVariable, false);
            NormalizeExpression(loop->maxIncreased, false);
            NormalizeExpression(loop->increaseBy, false);
            break;
        }
        case ASTNodeKind::ForGeneral: {
            const auto loop = std::static_pointer_cast<ForGeneralNode>(stmt);
            NormalizeExpression(loop->generator, false);
            NormalizeExpression(loop->state, false);
            NormalizeExpression(loop->index, false);
            break;
        }
        default:
            break;
        }
    }

    // `if cond then break end` as the first statement of a loop body. Leading comments are kept.
    static std::pair<std::shared_ptr<Expression>, size_t> AsLeadingBreakGuard(const std::shared_ptr<BlockStatementNode> &body) {
        if (!body)
            return {nullptr, 0};
        size_t index = 0;
        while (index < body->body.size() && body->body[index] && body->body[index]->nodeKind == ASTNodeKind::Comment)
            ++index;
        if (index >= body->body.size())
            return {nullptr, index};
        const auto branch = std::dynamic_pointer_cast<IfStatementNode>(body->body[index]);
        if (!branch || branch->elseBranch || !branch->thenBranch || branch->thenBranch->body.size() != 1)
            return {nullptr, index};
        if (!std::dynamic_pointer_cast<BreakStatementNode>(branch->thenBranch->body.front()))
            return {nullptr, index};
        return {branch->condition, index};
    }

    // `break`/`continue` owned by this loop; nested loops keep theirs.
    static bool HasOwnLoopExit(const std::vector<std::shared_ptr<Statement>> &stmts) {
        for (const auto &stmt : stmts) {
            if (std::dynamic_pointer_cast<BreakStatementNode>(stmt) || std::dynamic_pointer_cast<ContinueStatementNode>(stmt))
                return true;
            if (const auto branch = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
                if ((branch->thenBranch && HasOwnLoopExit(branch->thenBranch->body)) || (branch->elseBranch && HasOwnLoopExit(branch->elseBranch->body)))
                    return true;
            } else if (const auto block = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
                if (HasOwnLoopExit(block->body))
                    return true;
            }
        }
        return false;
    }

    // `repeat BODY until true` runs its body once. The lifter uses that shape for a plain conditional, so
    // fold it back: `repeat if g then break end rest until true` -> `if not g then rest end`, and a guard
    // free body -> `do rest end`. A break/continue that targets the repeat would change meaning, so it
    // keeps the loop.
    static void FoldOneShotRepeat(std::shared_ptr<Statement> &stmt) {
        const auto loop = std::dynamic_pointer_cast<RepeatStatementNode>(stmt);
        if (!loop || !loop->body)
            return;
        const auto literal = AsLiteral<BooleanLiteralNode>(loop->condition);
        if (!literal || !literal->value)
            return;

        const auto [guard, index] = AsLeadingBreakGuard(loop->body);
        std::vector<std::shared_ptr<Statement>> body(loop->body->body.begin(), loop->body->body.end());
        if (guard)
            body.erase(body.begin() + static_cast<std::ptrdiff_t>(index));
        if (HasOwnLoopExit(body))
            return;

        if (guard) {
            auto branch = Fission::MakeShared<IfStatementNode>();
            branch->condition = InvertCondition(guard);
            branch->thenBranch = Fission::MakeShared<BlockStatementNode>();
            branch->thenBranch->body = std::move(body);
            stmt = branch;
            NormalizeExpression(branch->condition, true);
            return;
        }
        auto block = Fission::MakeShared<BlockStatementNode>();
        block->body = std::move(body);
        block->bEmitAsDoBlock = true;
        stmt = block;
    }

    // `while a do if b then break end rest end` -> `while a and not b do rest end`
    // Only `while` is folded here: `repeat ... until true` is a one-shot block, handled above.
    static void FoldLoopGuard(const std::shared_ptr<Statement> &stmt) {
        const auto loop = std::dynamic_pointer_cast<WhileStatementNode>(stmt);
        if (!loop)
            return;
        const auto [guard, index] = AsLeadingBreakGuard(loop->body);
        if (!guard)
            return;
        loop->body->body.erase(loop->body->body.begin() + static_cast<std::ptrdiff_t>(index));
        loop->condition = Fission::MakeShared<BinaryExpressionNode>("and", loop->condition, InvertCondition(guard));
        NormalizeExpression(loop->condition, true);
    }
};
