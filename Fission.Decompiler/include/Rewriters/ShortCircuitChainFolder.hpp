//
// Created by Dottik on 23/9/2026.
//

// Folds the statement shapes short-circuit lowering leaves behind: `x = a; x = x or b`, guarded
// assignments, or-chains feeding one shared use, and `x = x` residue.

#pragma once
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class ShortCircuitChainFolder : public ASTRewriter {
  public:
    // fold `X = expr; X = X <op> expr2` into `X = expr <op> expr2`, in place until fixpoint.
    // also drops trivially-dead empty `if` residue from earlier passes.
    static void FoldShortCircuitChain(std::vector<std::shared_ptr<Statement>> &stmts) {
        std::erase_if(stmts, IsTriviallyDeadIf);

        bool changed = true;
        while (changed) {
            changed = FoldTerminalMixedAndOr(stmts);
            if (changed)
                continue;
            changed = FoldTerminalOrChain(stmts);
            if (changed)
                continue;
            for (size_t i = 0; i + 1 < stmts.size(); ++i) {
                // Plain assignments and declarations both publish the value read by the next statement.
                auto first = AsAssignmentForm(stmts[i]);
                if (!first)
                    continue;

                auto secondForm = AsShortCircuitAssign(stmts[i + 1]);
                if (!secondForm || secondForm->lhsName != first->lhsName)
                    continue;

                auto folded = std::make_shared<BinaryExpressionNode>(secondForm->op, first->rhs, secondForm->rhs);
                if (!first->isDeclaration) {
                    stmts[i] = std::make_shared<AssignmentStatementNode>(first->lhs, folded);
                } else {
                    // keep `local` shape so naming stays correct.
                    stmts[i] = std::make_shared<VariableDeclarationNode>(first->lhs, folded);
                }
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1);
                changed = true;
                break; // restart so the folded stmt can fold with its next neighbor.
            }
        }
    }

    static void EraseSelfAssignments(std::vector<std::shared_ptr<Statement>> &stmts) { std::erase_if(stmts, IsSelfAssign); }

    // rewrite Luau's short-circuit lowering back into one assignment:
    //   if not target then target = expr end   ==>   target = target or expr
    //   if target     then target = expr end   ==>   target = target and expr
    static std::shared_ptr<Statement> TryRewriteShortCircuitAssignment(const std::shared_ptr<IfStatementNode> &ifStmt) {
        if (!ifStmt || !ifStmt->thenBranch || ifStmt->elseBranch)
            return nullptr;

        // strip the CommentNode siblings the MOVE handler injects so the body matches.
        std::shared_ptr<AssignmentStatementNode> assign;
        for (const auto &stmt : ifStmt->thenBranch->body) {
            if (std::dynamic_pointer_cast<CommentNode>(stmt))
                continue;
            if (assign)
                return nullptr; // more than one effective statement: not the fold pattern.
            assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
            if (!assign)
                return nullptr; // non-assignment effective statement: bail.
        }
        if (!assign)
            return nullptr;

        const auto leftName = ExtractIdentifierName(assign->left);
        if (!leftName)
            return nullptr;

        std::string op;
        std::shared_ptr<Expression> conditionRef;
        if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(ifStmt->condition); unary && unary->op == "not ") {
            conditionRef = unary->operand;
            op = "or";
        } else {
            conditionRef = ifStmt->condition;
            op = "and";
        }

        const auto condName = ExtractIdentifierName(conditionRef);
        if (!condName || *condName != *leftName)
            return nullptr;

        auto lhsRead = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(*leftName));
        auto combined = std::make_shared<BinaryExpressionNode>(op, lhsRead, assign->right);
        return std::make_shared<AssignmentStatementNode>(assign->left, combined);
    }

  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        for (auto &stmt : stmts)
            if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt))
                if (auto rewritten = TryRewriteShortCircuitAssignment(ifStmt))
                    stmt = rewritten;
        FoldShortCircuitChain(stmts);
        EraseSelfAssignments(stmts);
    }

  private:
    // Extract the identifier name from an Expression if it is a simple identifier reference.
    static std::optional<std::string> ExtractIdentifierName(const std::shared_ptr<Expression> &expr) {
        if (!expr)
            return std::nullopt;
        if (auto idExpr = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); idExpr && idExpr->identifier && !idExpr->identifier->bIsGlobal)
            return idExpr->identifier->name;
        if (auto id = std::dynamic_pointer_cast<Identifier>(expr); id && !id->bIsGlobal)
            return id->name;
        return std::nullopt;
    }

    static bool IsSelfAssign(const std::shared_ptr<Statement> &stmt) {
        auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!assign)
            return false;
        auto lhsName = ExtractIdentifierName(assign->left);
        if (!lhsName)
            return false;
        auto rhsName = ExtractIdentifierName(assign->right);
        return rhsName.has_value() && *rhsName == *lhsName;
    }

    // `X = X or Y` / `X = X and Y` shape. nullopt if not that.
    struct ShortCircuitForm {
        std::string op;      // "or" or "and"
        std::string lhsName; // target identifier name on the LHS
        std::shared_ptr<Expression> rhs;
        std::shared_ptr<Expression> originalLeft; // preserve the original Expression node for the LHS
    };

    struct AssignmentForm {
        std::string lhsName;
        std::shared_ptr<Expression> lhs;
        std::shared_ptr<Expression> rhs;
        bool isDeclaration = false;
    };

    enum class TerminalUseKind { ReturnValue, CallThenReturn };

    struct TerminalUse {
        TerminalUseKind kind;
        size_t consumed = 0;
        size_t argIndex = 0;
        std::shared_ptr<CallExpressionNode> call;
        std::shared_ptr<NameCallExpressionNode> nameCall;
        // temp is the index of `f(tbl[temp])`, not the bare arg `f(temp)`: substitute the chain into the index.
        bool intoIndexKey = false;
    };

    static std::optional<AssignmentForm> AsAssignmentForm(const std::shared_ptr<Statement> &stmt) {
        if (auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            auto lhsName = ExtractIdentifierName(assign->left);
            if (!lhsName || !assign->right)
                return std::nullopt;
            return AssignmentForm{*lhsName, assign->left, assign->right, false};
        }

        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            auto lhsName = ExtractIdentifierName(decl->identifier);
            if (!lhsName || !decl->value)
                return std::nullopt;
            return AssignmentForm{*lhsName, decl->identifier, decl->value, true};
        }

        return std::nullopt;
    }

    static std::optional<ShortCircuitForm> AsShortCircuitAssign(const std::shared_ptr<Statement> &stmt) {
        auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
        if (!assign)
            return std::nullopt;

        auto leftName = ExtractIdentifierName(assign->left);
        if (!leftName)
            return std::nullopt;

        auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(assign->right);
        if (!binary || (binary->op != "or" && binary->op != "and"))
            return std::nullopt;

        auto binLhsName = ExtractIdentifierName(binary->left);
        if (!binLhsName || *binLhsName != *leftName)
            return std::nullopt;

        return ShortCircuitForm{binary->op, *leftName, binary->right, assign->left};
    }

    // drop empty `if cond then end` whose cond is a side-effect-free read. short-circuit fold residue.
    static bool IsTriviallyDeadIf(const std::shared_ptr<Statement> &stmt) {
        auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt);
        if (!ifStmt)
            return false;
        const bool thenEmpty = !ifStmt->thenBranch || ifStmt->thenBranch->body.empty();
        const bool elseEmpty = !ifStmt->elseBranch || ifStmt->elseBranch->body.empty();
        if (!thenEmpty || !elseEmpty)
            return false;

        // only bare identifier or `not <identifier>`.
        auto cond = ifStmt->condition;
        if (!cond)
            return true;
        if (ExtractIdentifierName(cond).has_value())
            return true;
        if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); unary && unary->op == "not ")
            return ExtractIdentifierName(unary->operand).has_value();
        return false;
    }

    static bool IsEmptyReturn(const std::shared_ptr<Statement> &stmt) {
        auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt);
        return ret && ret->returnValues.empty();
    }

    static bool ExpressionsEquivalent(const std::shared_ptr<Expression> &lhs, const std::shared_ptr<Expression> &rhs) {
        if (!lhs || !rhs)
            return lhs == rhs;

        auto lhsName = ExtractIdentifierName(lhs);
        auto rhsName = ExtractIdentifierName(rhs);
        if (lhsName || rhsName)
            return lhsName && rhsName && *lhsName == *rhsName;

        if (auto l = std::dynamic_pointer_cast<StringLiteralNode>(lhs)) {
            auto r = std::dynamic_pointer_cast<StringLiteralNode>(rhs);
            return r && l->value == r->value;
        }
        if (auto l = std::dynamic_pointer_cast<BooleanLiteralNode>(lhs)) {
            auto r = std::dynamic_pointer_cast<BooleanLiteralNode>(rhs);
            return r && l->value == r->value;
        }
        if (auto l = std::dynamic_pointer_cast<NumberLiteralNode>(lhs)) {
            auto r = std::dynamic_pointer_cast<NumberLiteralNode>(rhs);
            return r && l->value == r->value;
        }
        if (auto l = std::dynamic_pointer_cast<IntegerLiteralNode>(lhs)) {
            auto r = std::dynamic_pointer_cast<IntegerLiteralNode>(rhs);
            return r && l->value == r->value;
        }
        if (std::dynamic_pointer_cast<NilLiteralNode>(lhs) || std::dynamic_pointer_cast<NilLiteralNode>(rhs))
            return std::dynamic_pointer_cast<NilLiteralNode>(lhs) && std::dynamic_pointer_cast<NilLiteralNode>(rhs);
        if (auto l = std::dynamic_pointer_cast<MemberExpressionNode>(lhs)) {
            auto r = std::dynamic_pointer_cast<MemberExpressionNode>(rhs);
            return r && ExpressionsEquivalent(l->table, r->table) && ExpressionsEquivalent(l->key, r->key);
        }

        return false;
    }

    // unique argument carrying `name`, bare (`f(name)`) or as an index (`f(tbl[name])`). {index, intoIndexKey};
    // nullopt if absent or referenced by more than one argument.
    static std::optional<std::pair<size_t, bool>> FindTempArgument(const std::vector<std::shared_ptr<Expression>> &args, const std::string &name) {
        std::optional<std::pair<size_t, bool>> found;
        for (size_t i = 0; i < args.size(); ++i) {
            bool match = false;
            bool intoIndexKey = false;
            if (auto argName = ExtractIdentifierName(args[i]); argName && *argName == name) {
                match = true;
            } else if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(args[i])) {
                // `f(tbl[name])`: the temp is the computed index (right child); the table (left) is preserved.
                if (auto keyName = ExtractIdentifierName(index->right); keyName && *keyName == name) {
                    match = true;
                    intoIndexKey = true;
                }
            }
            if (!match)
                continue;
            if (found)
                return std::nullopt;
            found = std::pair{i, intoIndexKey};
        }
        return found;
    }

    static std::optional<TerminalUse> MatchTerminalUse(const std::vector<std::shared_ptr<Statement>> &stmts, size_t offset, const std::string &tmpName) {
        if (offset >= stmts.size())
            return std::nullopt;

        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmts[offset]); ret && ret->returnValues.size() == 1) {
            auto retName = ExtractIdentifierName(ret->returnValues.front());
            if (retName && *retName == tmpName)
                return TerminalUse{TerminalUseKind::ReturnValue, 1, 0, nullptr, nullptr};
        }

        auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[offset]);
        if (!exprStmt || offset + 1 >= stmts.size() || !IsEmptyReturn(stmts[offset + 1]))
            return std::nullopt;

        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression)) {
            auto arg = FindTempArgument(call->arguments, tmpName);
            if (arg)
                return TerminalUse{TerminalUseKind::CallThenReturn, 2, arg->first, call, nullptr, arg->second};
        }
        if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression)) {
            auto arg = FindTempArgument(nameCall->arguments, tmpName);
            if (arg)
                return TerminalUse{TerminalUseKind::CallThenReturn, 2, arg->first, nullptr, nameCall, arg->second};
        }

        return std::nullopt;
    }

    static bool SameTerminalUse(const TerminalUse &lhs, const TerminalUse &rhs) {
        if (lhs.kind != rhs.kind)
            return false;
        if (lhs.kind == TerminalUseKind::ReturnValue)
            return true;
        if (lhs.argIndex != rhs.argIndex || lhs.intoIndexKey != rhs.intoIndexKey)
            return false;
        if (lhs.call || rhs.call) {
            if (!lhs.call || !rhs.call || lhs.call->arguments.size() != rhs.call->arguments.size())
                return false;
            if (!ExpressionsEquivalent(lhs.call->callee, rhs.call->callee))
                return false;
            if (lhs.intoIndexKey) {
                // index targets must index the same table to be the same consumer.
                auto lm = std::dynamic_pointer_cast<IndexExpressionNode>(lhs.call->arguments[lhs.argIndex]);
                auto rm = std::dynamic_pointer_cast<IndexExpressionNode>(rhs.call->arguments[rhs.argIndex]);
                if (!lm || !rm || !ExpressionsEquivalent(lm->left, rm->left))
                    return false;
            }
            return true;
        }
        if (!lhs.nameCall || !rhs.nameCall || lhs.nameCall->arguments.size() != rhs.nameCall->arguments.size())
            return false;
        if (!ExpressionsEquivalent(lhs.nameCall->calledOn, rhs.nameCall->calledOn) || !ExpressionsEquivalent(lhs.nameCall->callWhat, rhs.nameCall->callWhat))
            return false;
        if (lhs.intoIndexKey) {
            auto lm = std::dynamic_pointer_cast<IndexExpressionNode>(lhs.nameCall->arguments[lhs.argIndex]);
            auto rm = std::dynamic_pointer_cast<IndexExpressionNode>(rhs.nameCall->arguments[rhs.argIndex]);
            if (!lm || !rm || !ExpressionsEquivalent(lm->left, rm->left))
                return false;
        }
        return true;
    }

    static bool SameTerminalTarget(const TerminalUse &shape, const TerminalUse &candidate) {
        if (shape.kind != candidate.kind)
            return false;
        if (shape.kind == TerminalUseKind::ReturnValue)
            return true;
        if (shape.argIndex != candidate.argIndex || shape.intoIndexKey != candidate.intoIndexKey)
            return false;
        if (shape.call || candidate.call) {
            if (!shape.call || !candidate.call || shape.call->arguments.size() != candidate.call->arguments.size())
                return false;
            if (!ExpressionsEquivalent(shape.call->callee, candidate.call->callee))
                return false;
            for (size_t i = 0; i < shape.call->arguments.size(); ++i)
                if (i != shape.argIndex && !ExpressionsEquivalent(shape.call->arguments[i], candidate.call->arguments[i]))
                    return false;
            return true;
        }
        if (!shape.nameCall || !candidate.nameCall || shape.nameCall->arguments.size() != candidate.nameCall->arguments.size())
            return false;
        if (!ExpressionsEquivalent(shape.nameCall->calledOn, candidate.nameCall->calledOn) ||
            !ExpressionsEquivalent(shape.nameCall->callWhat, candidate.nameCall->callWhat))
            return false;
        for (size_t i = 0; i < shape.nameCall->arguments.size(); ++i)
            if (i != shape.argIndex && !ExpressionsEquivalent(shape.nameCall->arguments[i], candidate.nameCall->arguments[i]))
                return false;
        return true;
    }

    static std::optional<std::pair<TerminalUse, std::shared_ptr<Expression>>>
    MatchFinalTerminalValue(const std::vector<std::shared_ptr<Statement>> &stmts, size_t offset, const TerminalUse &shape) {
        if (offset >= stmts.size())
            return std::nullopt;
        if (shape.kind == TerminalUseKind::ReturnValue) {
            auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmts[offset]);
            if (ret && ret->returnValues.size() == 1)
                return std::pair{TerminalUse{TerminalUseKind::ReturnValue, 1, 0, nullptr, nullptr}, ret->returnValues.front()};
            return std::nullopt;
        }

        // doesn't apply to an index target: the "final value" form would capture `tbl[temp]`, not the temp.
        if (shape.intoIndexKey)
            return std::nullopt;

        auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[offset]);
        if (!exprStmt)
            return std::nullopt;

        size_t consumed = (offset + 1 < stmts.size() && IsEmptyReturn(stmts[offset + 1])) ? 2 : 1;
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression)) {
            if (shape.argIndex >= call->arguments.size())
                return std::nullopt;
            TerminalUse candidate{TerminalUseKind::CallThenReturn, consumed, shape.argIndex, call, nullptr};
            if (SameTerminalTarget(shape, candidate))
                return std::pair{candidate, call->arguments[shape.argIndex]};
        }
        if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression)) {
            if (shape.argIndex >= nameCall->arguments.size())
                return std::nullopt;
            TerminalUse candidate{TerminalUseKind::CallThenReturn, consumed, shape.argIndex, nullptr, nameCall};
            if (SameTerminalTarget(shape, candidate))
                return std::pair{candidate, nameCall->arguments[shape.argIndex]};
        }
        return std::nullopt;
    }

    static std::shared_ptr<Expression> MakeOrChain(const std::vector<std::shared_ptr<Expression>> &exprs) {
        std::shared_ptr<Expression> chain = exprs.front();
        for (size_t i = 1; i < exprs.size(); ++i)
            chain = std::make_shared<BinaryExpressionNode>("or", chain, exprs[i]);
        return chain;
    }

    static std::shared_ptr<Statement> BuildTerminalReplacement(TerminalUse terminal, const std::shared_ptr<Expression> &chain) {
        if (terminal.kind == TerminalUseKind::ReturnValue)
            return std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{chain});
        if (terminal.call) {
            if (terminal.intoIndexKey) {
                if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(terminal.call->arguments[terminal.argIndex]))
                    index->right = chain;
            } else {
                terminal.call->arguments[terminal.argIndex] = chain;
            }
            return std::make_shared<ExpressionStatementNode>(terminal.call);
        }
        if (terminal.intoIndexKey) {
            if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(terminal.nameCall->arguments[terminal.argIndex]))
                index->right = chain;
        } else {
            terminal.nameCall->arguments[terminal.argIndex] = chain;
        }
        return std::make_shared<ExpressionStatementNode>(terminal.nameCall);
    }

    static bool FoldTerminalMixedAndOr(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (size_t start = 0; start + 1 < stmts.size(); ++start)
            if (FoldTerminalMixedAndOrAt(stmts, start))
                return true;
        return false;
    }

    // fold the if/else-return shape Luau emits for `return COND and PRIMARY or FALLBACK`.
    static bool FoldTerminalMixedAndOrAt(std::vector<std::shared_ptr<Statement>> &stmts, size_t start) {
        // a phi-hoisted bare `local V` may precede the pair; skip it, erase on success.
        size_t base = start;
        std::optional<std::string> hoistedLocalName;
        if (auto leadDecl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[start]); leadDecl && leadDecl->value == nullptr) {
            if (auto leadName = ExtractIdentifierName(leadDecl->identifier)) {
                hoistedLocalName = *leadName;
                base = start + 1;
            }
        }
        if (stmts.size() < base + 2)
            return false;

        auto outerIf = std::dynamic_pointer_cast<IfStatementNode>(stmts[base]);
        if (!outerIf || !outerIf->thenBranch || !outerIf->elseBranch)
            return false;

        auto retStmt = std::dynamic_pointer_cast<ReturnStatementNode>(stmts[base + 1]);
        if (!retStmt || retStmt->returnValues.size() != 1)
            return false;
        auto retName = ExtractIdentifierName(retStmt->returnValues.front());
        if (!retName)
            return false;
        const auto &V = *retName;

        // skipped local must be the same var we're collapsing.
        if (hoistedLocalName && *hoistedLocalName != V)
            return false;

        auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(outerIf->condition);
        if (!unary || unary->op != "not ")
            return false;
        auto COND = unary->operand;

        auto &thenBody = outerIf->thenBranch->body;
        std::shared_ptr<Expression> fallbackValue;

        if (thenBody.size() == 1) {
            auto thenRet = std::dynamic_pointer_cast<ReturnStatementNode>(thenBody[0]);
            if (!thenRet || thenRet->returnValues.size() != 1)
                return false;
            auto thenRetName = ExtractIdentifierName(thenRet->returnValues.front());
            if (!thenRetName || *thenRetName != V)
                return false;
            fallbackValue = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(V));
        } else if (thenBody.size() == 2) {
            auto thenAsgn = AsAssignmentForm(thenBody[0]);
            if (!thenAsgn || thenAsgn->lhsName != V)
                return false;
            fallbackValue = thenAsgn->rhs;
            auto thenRet = std::dynamic_pointer_cast<ReturnStatementNode>(thenBody[1]);
            if (!thenRet || thenRet->returnValues.size() != 1)
                return false;
            auto thenRetName = ExtractIdentifierName(thenRet->returnValues.front());
            if (!thenRetName || *thenRetName != V)
                return false;
        } else {
            return false;
        }
        if (!fallbackValue)
            return false;

        // anything after the truthy early return would run before the fallback return
        auto &elseBody = outerIf->elseBranch->body;
        if (elseBody.size() != 2)
            return false;

        auto elseAsgn = AsAssignmentForm(elseBody[0]);
        if (!elseAsgn || elseAsgn->lhsName != V)
            return false;
        auto primaryValue = elseAsgn->rhs;

        auto elseIf = std::dynamic_pointer_cast<IfStatementNode>(elseBody[1]);
        if (!elseIf || elseIf->elseBranch || !elseIf->thenBranch)
            return false;
        auto elseIfCondName = ExtractIdentifierName(elseIf->condition);
        if (!elseIfCondName || *elseIfCondName != V)
            return false;
        // guard body size before indexing body[0] (CreateBlock can yield an empty body).
        if (elseIf->thenBranch->body.size() != 1)
            return false;
        auto elseIfRet = std::dynamic_pointer_cast<ReturnStatementNode>(elseIf->thenBranch->body[0]);
        if (!elseIfRet || elseIfRet->returnValues.size() != 1)
            return false;
        auto elseIfRetName = ExtractIdentifierName(elseIfRet->returnValues.front());
        if (!elseIfRetName || *elseIfRetName != V)
            return false;

        auto andExpr = std::make_shared<BinaryExpressionNode>("and", COND, primaryValue);
        auto orExpr = std::make_shared<BinaryExpressionNode>("or", andExpr, fallbackValue);
        stmts[base] = std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{orExpr});
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(base) + 1);
        // drop the redundant hoisted `local V`.
        if (base != start)
            stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(start));
        return true;
    }

    static bool FoldTerminalOrChain(std::vector<std::shared_ptr<Statement>> &stmts) {
        for (size_t i = 0; i + 3 < stmts.size(); ++i) {
            auto first = AsAssignmentForm(stmts[i]);
            if (!first)
                continue;

            std::vector<std::shared_ptr<Expression>> exprs{first->rhs};
            std::optional<TerminalUse> terminal;
            size_t cursor = i + 1;

            while (cursor + 1 < stmts.size()) {
                auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmts[cursor]);
                if (!ifStmt || ifStmt->elseBranch || !ifStmt->thenBranch)
                    break;
                auto condName = ExtractIdentifierName(ifStmt->condition);
                bool negativeArm = false;
                if (!condName || *condName != first->lhsName) {
                    // trailing `and` term: `if not V then USE end`; V is the and-left, the next assign the and-right.
                    auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(ifStmt->condition);
                    if (!unary || unary->op != "not ")
                        break;
                    auto innerName = ExtractIdentifierName(unary->operand);
                    if (!innerName || *innerName != first->lhsName)
                        break;
                    negativeArm = true;
                }
                auto branchTerminal = MatchTerminalUse(ifStmt->thenBranch->body, 0, first->lhsName);
                if (!branchTerminal || branchTerminal->consumed != ifStmt->thenBranch->body.size())
                    break;
                if (terminal && !SameTerminalUse(*terminal, *branchTerminal))
                    break;
                terminal = *branchTerminal;
                ++cursor;

                auto next = AsAssignmentForm(stmts[cursor]);
                if (!next || next->lhsName != first->lhsName)
                    return false;
                if (negativeArm) {
                    // fold the last collected term with `next` into one `and` group: `... or (left and right)`.
                    if (exprs.empty())
                        break;
                    auto andLeft = exprs.back();
                    exprs.pop_back();
                    exprs.push_back(std::make_shared<BinaryExpressionNode>("and", andLeft, next->rhs));
                    ++cursor;
                    break; // the `and` group closes the chain; the shared consumer follows at `cursor`.
                }
                exprs.push_back(next->rhs);
                ++cursor;
            }

            if (!terminal || exprs.size() < 2)
                continue;
            auto tailTerminal = MatchTerminalUse(stmts, cursor, first->lhsName);
            if (tailTerminal && SameTerminalUse(*terminal, *tailTerminal)) {
                stmts[i] = BuildTerminalReplacement(*tailTerminal, MakeOrChain(exprs));
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, stmts.begin() + static_cast<std::ptrdiff_t>(cursor + tailTerminal->consumed));
                return true;
            }

            auto finalTerminal = MatchFinalTerminalValue(stmts, cursor, *terminal);
            if (!finalTerminal)
                continue;
            exprs.push_back(finalTerminal->second);
            stmts[i] = BuildTerminalReplacement(finalTerminal->first, MakeOrChain(exprs));
            stmts.erase(
                stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, stmts.begin() + static_cast<std::ptrdiff_t>(cursor + finalTerminal->first.consumed)
            );
            return true;
        }

        return false;
    }
};
