//
// Created by Dottik on 10/12/2025.
//

#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

#include <algorithm>

#if defined(__clang__)
#pragma clang optimize off
#endif

constexpr uint32_t InvalidBlockId = static_cast<uint32_t>(-1);

// Forward declaration for the chain-fold pass used by CreateBlock below.
static void FoldShortCircuitChain(std::vector<std::shared_ptr<Statement>> &stmts);
static bool IsSelfAssign(const std::shared_ptr<Statement> &stmt);

static std::shared_ptr<BlockStatementNode> CreateBlock(const std::vector<std::shared_ptr<Statement>> &stmts) {
    auto block = std::make_shared<BlockStatementNode>();
    block->body = stmts;
    FoldShortCircuitChain(block->body);
    for (auto it = block->body.begin(); it != block->body.end();) {
        if (IsSelfAssign(*it))
            it = block->body.erase(it);
        else
            ++it;
    }
    return block;
}

// Extract the identifier name from an Expression if it is a simple identifier reference.
static std::optional<std::string> ExtractIdentifierName(const std::shared_ptr<Expression> &expr) {
    if (!expr)
        return std::nullopt;
    if (auto idExpr = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); idExpr && idExpr->identifier)
        return idExpr->identifier->name;
    if (auto id = std::dynamic_pointer_cast<Identifier>(expr); id)
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

static std::optional<size_t> FindIdentifierArgument(const std::vector<std::shared_ptr<Expression>> &args, const std::string &name) {
    std::optional<size_t> found;
    for (size_t i = 0; i < args.size(); ++i) {
        auto argName = ExtractIdentifierName(args[i]);
        if (!argName || *argName != name)
            continue;
        if (found)
            return std::nullopt;
        found = i;
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
        auto argIndex = FindIdentifierArgument(call->arguments, tmpName);
        if (argIndex)
            return TerminalUse{TerminalUseKind::CallThenReturn, 2, *argIndex, call, nullptr};
    }
    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression)) {
        auto argIndex = FindIdentifierArgument(nameCall->arguments, tmpName);
        if (argIndex)
            return TerminalUse{TerminalUseKind::CallThenReturn, 2, *argIndex, nullptr, nameCall};
    }

    return std::nullopt;
}

static bool SameTerminalUse(const TerminalUse &lhs, const TerminalUse &rhs) {
    if (lhs.kind != rhs.kind)
        return false;
    if (lhs.kind == TerminalUseKind::ReturnValue)
        return true;
    if (lhs.argIndex != rhs.argIndex)
        return false;
    if (lhs.call || rhs.call) {
        if (!lhs.call || !rhs.call || lhs.call->arguments.size() != rhs.call->arguments.size())
            return false;
        return ExpressionsEquivalent(lhs.call->callee, rhs.call->callee);
    }
    if (!lhs.nameCall || !rhs.nameCall || lhs.nameCall->arguments.size() != rhs.nameCall->arguments.size())
        return false;
    return ExpressionsEquivalent(lhs.nameCall->calledOn, rhs.nameCall->calledOn) && ExpressionsEquivalent(lhs.nameCall->callWhat, rhs.nameCall->callWhat);
}

static bool SameTerminalTarget(const TerminalUse &shape, const TerminalUse &candidate) {
    if (shape.kind != candidate.kind)
        return false;
    if (shape.kind == TerminalUseKind::ReturnValue)
        return true;
    if (shape.argIndex != candidate.argIndex)
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
        terminal.call->arguments[terminal.argIndex] = chain;
        return std::make_shared<ExpressionStatementNode>(terminal.call);
    }
    terminal.nameCall->arguments[terminal.argIndex] = chain;
    return std::make_shared<ExpressionStatementNode>(terminal.nameCall);
}

// fold the if/else-return shape Luau emits for `return COND and PRIMARY or FALLBACK`.
static bool FoldTerminalMixedAndOr(std::vector<std::shared_ptr<Statement>> &stmts) {
    if (stmts.size() < 2)
        return false;

    // a phi-hoisted bare `local V` may precede the pair; skip it, erase on success.
    size_t base = 0;
    std::optional<std::string> hoistedLocalName;
    if (auto leadDecl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[0]); leadDecl && leadDecl->value == nullptr) {
        if (auto leadName = ExtractIdentifierName(leadDecl->identifier)) {
            hoistedLocalName = *leadName;
            base = 1;
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

    auto &elseBody = outerIf->elseBranch->body;
    if (elseBody.size() < 2)
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
    auto elseIfRet = std::dynamic_pointer_cast<ReturnStatementNode>(elseIf->thenBranch->body[0]);
    if (!elseIfRet || elseIfRet->returnValues.size() != 1)
        return false;
    auto elseIfRetName = ExtractIdentifierName(elseIfRet->returnValues.front());
    if (!elseIfRetName || *elseIfRetName != V)
        return false;
    if (elseIf->thenBranch->body.size() != 1)
        return false;

    auto andExpr = std::make_shared<BinaryExpressionNode>("and", COND, primaryValue);
    auto orExpr = std::make_shared<BinaryExpressionNode>("or", andExpr, fallbackValue);
    stmts[base] = std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{orExpr});
    stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(base) + 1);
    // drop the redundant hoisted `local V`.
    if (base == 1)
        stmts.erase(stmts.begin());
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
            if (!condName || *condName != first->lhsName)
                break;
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
        stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i) + 1, stmts.begin() + static_cast<std::ptrdiff_t>(cursor + finalTerminal->first.consumed));
        return true;
    }

    return false;
}

// fold `X = expr; X = X <op> expr2` into `X = expr <op> expr2`, in place until fixpoint.
// also drops trivially-dead empty `if` residue from earlier passes.
static void FoldShortCircuitChain(std::vector<std::shared_ptr<Statement>> &stmts) {
    // pre-pass: erase dead empty `if`.
    for (auto it = stmts.begin(); it != stmts.end();) {
        if (IsTriviallyDeadIf(*it))
            it = stmts.erase(it);
        else
            ++it;
    }

    bool changed = true;
    while (changed) {
        changed = FoldTerminalMixedAndOr(stmts);
        if (changed)
            continue;
        changed = FoldTerminalOrChain(stmts);
        if (changed)
            continue;
        for (size_t i = 0; i + 1 < stmts.size(); ++i) {
            // first half: plain assign or `local x = expr` — both publish x for the next read-back.
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

// rewrite Luau's short-circuit lowering back into one assignment:
//   if not target then target = expr end   ==>   target = target or expr
//   if target     then target = expr end   ==>   target = target and expr
// only innermost level per pass; nested chains fold later.
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
        // `if not x then x = expr end`  ==>  `x = x or expr`
        conditionRef = unary->operand;
        op = "or";
    } else {
        // `if x then x = expr end`  ==>  `x = x and expr`
        conditionRef = ifStmt->condition;
        op = "and";
    }

    const auto condName = ExtractIdentifierName(conditionRef);
    if (!condName || *condName != *leftName)
        return nullptr;

    // rhs = target read `or`/`and` the original assigned expr.
    auto lhsRead = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(*leftName));
    auto combined = std::make_shared<BinaryExpressionNode>(op, lhsRead, assign->right);
    return std::make_shared<AssignmentStatementNode>(assign->left, combined);
}

// single-use value whose one user is a call-family op consuming it as a non-callee arg.
// gates inlining a closure into the call site (`foo(function() ... end)`).
static bool IsSingleUseCallArgument(AnalyzedFunction *func, int32_t reg, int32_t ssaVersion) {
    SSARef ref{static_cast<uint8_t>(reg), ssaVersion};
    auto it = func->users.find(ref);
    if (it == func->users.end() || it->second.size() != 1)
        return false;
    auto *user = it->second.front();
    if (!user || user->operands.empty())
        return false;
    switch (user->operation) {
    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
    case LiftedOperation::NAMECALL:
        break;
    default:
        return false;
    }
    const int32_t calleeReg = user->operands[0].value.reg;
    // closure being the callee itself is an IIFE, not a passed-arg.
    if (reg == calleeReg)
        return false;
    // NAMECALL's auto-bound `self` at calleeReg+1 isn't a user-visible arg.
    if (user->operation == LiftedOperation::NAMECALL && reg == calleeReg + 1)
        return false;
    return true;
}

// valid Luau ident (alnum + _, no leading digit). Roblox instance names may have spaces — reject those.
static bool IsValidLuauIdent(const std::string &s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
        return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

// single-use closure whose use is a table-field store value (`t.field = <closure>`).
// inline into the store instead of leaking a global like `anon_43_0 = function()...`.
static bool IsSingleUseTableFieldValue(AnalyzedFunction *func, int32_t reg, int32_t ssaVersion) {
    SSARef ref{static_cast<uint8_t>(reg), ssaVersion};
    auto it = func->users.find(ref);
    if (it == func->users.end() || it->second.size() != 1)
        return false;
    auto *user = it->second.front();
    if (!user || user->operands.size() < 2)
        return false;
    switch (user->operation) {
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEN:
        break;
    default:
        return false;
    }
    // must be the stored value (operand[0]), not the table (operand[1]).
    return user->operands[0].value.reg == reg && user->operands[1].value.reg != reg;
}

std::shared_ptr<Expression> ASTLifter::InvertCondition(const std::shared_ptr<Expression> &cond) {
    if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); unary && unary->op == "not ") {
        return unary->operand;
    }

    if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
        if (binary->op == "==") {
            binary->op = "~=";
            return binary;
        } else if (binary->op == "~=") {
            binary->op = "==";
            return binary;
        } else if (binary->op == "<") {
            binary->op = ">=";
            return binary;
        } else if (binary->op == ">") {
            binary->op = "<=";
            return binary;
        } else if (binary->op == "<=") {
            binary->op = ">";
            return binary;
        } else if (binary->op == ">=") {
            binary->op = "<";
            return binary;
        }
    }

    return std::make_shared<UnaryExpressionNode>("not ", cond);
}

ASTLifter::ASTLifter() {}

ASTFunction ASTLifter::Lift(AnalyzedFunction &analyzedFunction) {
    this->m_currentFunction = &analyzedFunction;
    this->m_definedRegisters.clear();
    this->m_pinnedRegisters.clear();
    this->m_processedInstructions.clear();
    this->m_phiConsumers.clear();

    for (const auto &block : analyzedFunction.basicBlocks) {
        for (const auto &phi : block.phiNodes) {
            for (size_t i = 1; i < phi.operands.size(); ++i) {
                const auto &op = phi.operands[i];
                if (op.type == LiftedOperandType::Register) {
                    m_phiConsumers.insert({op.value.reg, op.ssaVersion});
                }
            }
        }
    }

    ASTFunction ast;
    ast.backingFunction = &analyzedFunction;
    analyzedFunction.PopulateNames();

    for (int32_t arg = 0; arg < analyzedFunction.lpLiftedFunction->lpDeserialized->numparams; ++arg)
        m_definedRegisters.insert(arg);

    if (!analyzedFunction.basicBlocks.empty()) {
        // pre-scan: mark CALLs consumed by generic FOR loops processed so they don't double-emit.
        for (auto &b : analyzedFunction.basicBlocks) {
            if (b.bType != BlockType::LoopHeader)
                continue;
            auto *tail = b.lpTail;
            if (!tail)
                continue;
            LiftedOperation forOp = tail->operation;

            // FORNPREP: trace start/limit/step phis to pre-header LOADs and suppress them
            // (inlined into the for header; ShouldInline otherwise refuses phi-consumed defs).
            if (forOp == LiftedOperation::FORNPREP) {
                int32_t baseReg = tail->operands[0].value.reg;
                int32_t limitVer = -1, stepVer = -1, startVer = -1;
                if (analyzedFunction.implicitUses.contains(tail)) {
                    const auto &impl = analyzedFunction.implicitUses.at(tail);
                    if (impl.size() >= 3) {
                        limitVer = impl[0];
                        stepVer = impl[1];
                        startVer = impl[2];
                    }
                }
                auto markLoopValue = [&](int32_t r, int32_t v) {
                    if (v < 0)
                        return;
                    auto lookupDef = [&](int32_t rr, int32_t vv) -> LiftedInstruction * {
                        SSARef ref{rr, vv};
                        if (!analyzedFunction.definitionMap.contains(ref))
                            return nullptr;
                        return analyzedFunction.definitionMap.at(ref);
                    };
                    auto *def = lookupDef(r, v);
                    if (!def)
                        return;
                    if (def->operation == LiftedOperation::PHI) {
                        for (size_t pi = 0; pi < b.predecessors.size() && pi + 1 < def->operands.size(); ++pi) {
                            if (b.loopLatch.has_value() && b.predecessors[pi] == b.loopLatch.value())
                                continue;
                            def = lookupDef(r, def->operands[pi + 1].ssaVersion);
                            break;
                        }
                    }
                    if (def && def->operation == LiftedOperation::LOAD)
                        m_processedInstructions.insert(def->instructionIndex);
                };
                markLoopValue(baseReg, limitVer);
                markLoopValue(baseReg + 1, stepVer);
                markLoopValue(baseReg + 2, startVer);
                continue;
            }

            if (forOp != LiftedOperation::FORGPREP && forOp != LiftedOperation::FORGPREP_INEXT && forOp != LiftedOperation::FORGPREP_NEXT)
                continue;

            int32_t baseReg = tail->operands[0].value.reg;
            int32_t genVer = -1, stateVer = -1, indexVer = -1;
            if (analyzedFunction.implicitUses.contains(tail)) {
                const auto &impl = analyzedFunction.implicitUses.at(tail);
                if (impl.size() >= 3) {
                    genVer = impl[0];
                    stateVer = impl[1];
                    indexVer = impl[2];
                }
            }

            auto findDef = [&](int32_t reg, int32_t ver) -> LiftedInstruction * {
                if (ver < 0)
                    return nullptr;
                SSARef ref{reg, ver};
                if (!analyzedFunction.definitionMap.contains(ref))
                    return nullptr;
                return analyzedFunction.definitionMap.at(ref);
            };

            auto *genDef = findDef(baseReg, genVer);
            auto *stateDef = findDef(baseReg + 1, stateVer);
            auto *indexDef = findDef(baseReg + 2, indexVer);

            if (genDef && stateDef && indexDef && genDef == stateDef && stateDef == indexDef &&
                (genDef->operation == LiftedOperation::CALL || genDef->operation == LiftedOperation::CALLFB ||
                 genDef->operation == LiftedOperation::NAMECALL)) {
                m_processedInstructions.insert(genDef->instructionIndex);
                int32_t callInfoIdx = (genDef->operation == LiftedOperation::NAMECALL) ? genDef->instructionIndex + 2 : genDef->instructionIndex;
                if (callInfoIdx < static_cast<int32_t>(analyzedFunction.lpLiftedFunction->instructions.size()))
                    m_processedInstructions.insert(callInfoIdx);
            }
        }

        std::set<uint32_t> visited;
        ast.statements = LiftControlFlow(0, InvalidBlockId, visited);
        std::string ttinfo = "Unavailable";

        if (analyzedFunction.lpLiftedFunction->lpDeserialized->typeinfo.size() != 0) {
            ttinfo = "Available";
        }

        auto s = std::format(
            R"(
    Fission ~~ Function Information:
        ~ Upvalue Count: {}
        ~ Argument Count: {}
        ~ Debug Name: {}
        ~ Bytecode ID: {}
        ~ Registers Used: R0-R{}
        ~ Type Information: {}
)",
            analyzedFunction.lpLiftedFunction->lpDeserialized->nups, analyzedFunction.lpLiftedFunction->lpDeserialized->numparams,
            analyzedFunction.lpLiftedFunction->lpDeserialized->debugName.value_or("anon/no name"),
            analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId, analyzedFunction.lpLiftedFunction->lpDeserialized->maxstacksize - 1, ttinfo
        );

        if (analyzedFunction.lpLiftedFunction->lpDeserialized->bIsMain) {
            s = std::format(
                "\n    Decompiled with the Fission decompiler for RbxCli\n    Bytecode Version: '{}'\n    Type Version: {}",
                analyzedFunction.lpLiftedFunction->lpDeserialized->uBytecodeVersion, analyzedFunction.lpLiftedFunction->lpDeserialized->uTypeVersion
            );
        }

        // note every own-register name suffixed to dodge an upvalue collision.
        for (const auto &renamed : analyzedFunction.disambiguatedNames)
            ast.statements.insert(
                ast.statements.begin(),
                std::make_shared<CommentNode>(
                    std::format("Fission: INFO: local '{}' has been suffixed to avoid shadowing an existing, upper scope variable.", renamed.first), true, true
                )
            );

        // The main banner stays; the per-function info block is informational.
        ast.statements.insert(
            ast.statements.begin(), std::make_shared<CommentNode>(s, true, !analyzedFunction.lpLiftedFunction->lpDeserialized->bIsMain)
        );
    }

    for (auto &subFunc : analyzedFunction.innerFunctions) {
        ASTLifter subLifter;
        ast.subFunctions.push_back(subLifter.Lift(subFunc));
    }

    return ast;
}

std::shared_ptr<Expression> ASTLifter::LiftCondition(const LiftedInstruction *inst) {
    if (!inst)
        return std::make_shared<BooleanLiteralNode>(false);

    switch (inst->operation) {
    case LiftedOperation::JUMPIFNOTEQ:
        return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFEQ:
        return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFLT:
        return std::make_shared<BinaryExpressionNode>("<", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFNOTLT:
        return std::make_shared<BinaryExpressionNode>(">=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFLE:
        return std::make_shared<BinaryExpressionNode>("<=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFNOTLE:
        return std::make_shared<BinaryExpressionNode>(">", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIF:
        return LiftExpression(inst->operands[0]);
    case LiftedOperation::JUMPIFNOT:
        return std::make_shared<UnaryExpressionNode>("not ", LiftExpression(inst->operands[0]));
    case LiftedOperation::JUMPXEQK: {
        if (inst->operands[2].type == LiftedOperandType::ImmediateConstant) {
            auto kIdx = inst->operands[2].value.imm.k;
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs;
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[kIdx];
            switch (k.kType) {
            case LUA_TNIL:
                rhs = std::make_shared<NilLiteralNode>();
                break;
            case LUA_TBOOLEAN:
                rhs = std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
                break;
            case LUA_TNUMBER:
                rhs = std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
                break;
            case LUA_TINTEGER:
                rhs = std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
                break;
            case LUA_TSTRING:
                rhs = std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
                break;
            default:
                rhs = std::make_shared<NilLiteralNode>();
                break;
            }

            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }
        if (inst->operands[2].type == LiftedOperandType::ImmediateBool) {
            auto bValue = inst->operands[2].value.imm.b;
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs = std::make_shared<BooleanLiteralNode>(bValue);
            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }

        if (inst->operands[2].type == LiftedOperandType::ImmediateNil) {
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs = std::make_shared<NilLiteralNode>();
            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }
        return std::make_shared<BooleanLiteralNode>(false);
    }
    default:
        return std::make_shared<BooleanLiteralNode>(false);
    }
}

std::optional<uint32_t> ASTLifter::DetectInfiniteWhileLatch(uint32_t headerId, uint32_t innerLatchId) {
    if (headerId >= m_currentFunction->basicBlocks.size())
        return std::nullopt;
    const auto &header = m_currentFunction->basicBlocks[headerId];
    for (uint32_t predId : header.predecessors) {
        if (predId == headerId || predId == innerLatchId || predId >= m_currentFunction->basicBlocks.size())
            continue;
        const auto &pred = m_currentFunction->basicBlocks[predId];
        // a predecessor edge already proves pred jumps to the header; an unconditional
        // LoopLatch with a plain JUMP is the testless back-edge of an outer `while true`.
        if (pred.bType != BlockType::LoopLatch || pred.bTerminator != BlockTerminator::Unconditional)
            continue;
        if (!pred.lpTail || pred.lpTail->operation != LiftedOperation::JUMP)
            continue;
        return predId;
    }
    return std::nullopt;
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited) {
    std::vector<std::shared_ptr<Statement>> nodes;

    // iterative tail-traversal: recursing the linear `after` continuation overflows the stack on long
    // `if .. return end; ...` chains. branch/loop bodies still recurse (bounded by nesting depth).
    while (true) {
        if (currentBlockId == InvalidBlockId || currentBlockId >= m_currentFunction->basicBlocks.size())
            break;

        // body code reaching the innermost loop exit directly == `break` (normal exit goes via latch).
        // don't mark visited — exit is still lifted once after the loop.
        if (!m_loopExitStack.empty() && currentBlockId == m_loopExitStack.back()) {
            nodes.push_back(std::make_shared<BreakStatementNode>());
            break;
        }

        // return blocks are allowed to be duplicated, as they have no successors.
        // compilers may inline the return for a break, which is annoying as fuck, and will break our lifting.
        // fuck you luauc.
        if (this->m_currentFunction->basicBlocks.at(currentBlockId).bType != BlockType::Return) {
            if (currentBlockId == stopBlockId || visited.contains(currentBlockId))
                break;
        }

        if (!visited.contains(currentBlockId))
            visited.insert(currentBlockId); // prevent double insertion product of block above.

        const auto &block = m_currentFunction->basicBlocks[currentBlockId];

        auto stmts = LiftBlockInstructions(block);

        // Linear continuation for the next iteration; -1 terminates the loop.
        uint32_t nextBlockId = InvalidBlockId;

        switch (block.bType) {
    case BlockType::IfHeader: {
        nodes.insert(nodes.end(), stmts.begin(), stmts.end()); // Body before conditional statement.

        // `x = a ~= b` materialised as a LOADB diamond is not an `if`; collapse to one assign.
        if (auto mat = DetectBooleanMaterialization(currentBlockId)) {
            nodes.push_back(mat->assignment);
            nextBlockId = mat->continueBlock;
            break;
        }

        if (block.ifStatementTrue.has_value() && block.ifStatementFalse.has_value()) {
            uint32_t trueIdx = block.ifStatementTrue.value();
            uint32_t falseIdx = block.ifStatementFalse.value();
            std::shared_ptr<Expression> trueCond;

            // coalesce `if a or b or c then BODY else ELSE` before merge analysis: as nested ifs,
            // FindMergeBlock mistakes the shared BODY for the merge and clobbers sibling branches.
            if (auto orChain = DetectOrChain(currentBlockId)) {
                trueCond = orChain->condition;
                trueIdx = orChain->bodyIdx;
                falseIdx = orChain->elseIdx;
                for (uint32_t cb : orChain->chainBlocks)
                    visited.insert(cb);
            } else {
                trueCond = LiftCondition(block.lpTail);
            }

            uint32_t mergeIdx = FindMergeBlock(trueIdx, falseIdx);

            if (mergeIdx == InvalidBlockId) {
                bool trueIsReturn = (m_currentFunction->basicBlocks[trueIdx].bType == BlockType::Return);
                bool falseIsReturn = (m_currentFunction->basicBlocks[falseIdx].bType == BlockType::Return);

                if (trueIsReturn && !falseIsReturn) {
                    mergeIdx = falseIdx;
                } else if (!trueIsReturn && falseIsReturn) {
                    mergeIdx = trueIdx;
                }
            }

            auto ifStmt = std::make_shared<IfStatementNode>();
            auto visitedCopy = visited;
            if (mergeIdx != InvalidBlockId)
                visitedCopy.insert(mergeIdx);

            // snapshot regs declared BEFORE branches: HoistPhiLocals tests "already in outer scope?"
            // against this, not the post-branch set (branch assigns would wrongly suppress the hoist).
            const auto definedBeforeBranches = m_definedRegisters;

            if (mergeIdx == falseIdx) {
                ifStmt->condition = (trueCond);
                ifStmt->thenBranch = CreateBlock(LiftControlFlow(trueIdx, mergeIdx, visitedCopy));
            } else if (mergeIdx == trueIdx) {
                ifStmt->condition = InvertCondition(trueCond);

                // iterative build for deep `if-elseif-...-else` where every link shares one merge target
                // (compiler dispatch trees). probe the chain depth without mutating state; only take the
                // iterative path past the recursion-safe threshold, keeping short cases on the recursive path.
                constexpr uint32_t kChainProbeThreshold = 64;
                auto probeChainDepth = [&](uint32_t startId, uint32_t stopId) -> uint32_t {
                    uint32_t depth = 0;
                    uint32_t cur = startId;
                    std::set<uint32_t> seen;
                    while (cur != InvalidBlockId && cur < m_currentFunction->basicBlocks.size() && !seen.contains(cur)) {
                        seen.insert(cur);
                        const auto &b = m_currentFunction->basicBlocks[cur];
                        if (b.bType != BlockType::IfHeader)
                            break;
                        if (!b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value())
                            break;
                        uint32_t bt = b.ifStatementTrue.value();
                        uint32_t bf = b.ifStatementFalse.value();
                        uint32_t bm = FindMergeBlock(bt, bf);
                        if (bm == InvalidBlockId) {
                            bool tR = (m_currentFunction->basicBlocks[bt].bType == BlockType::Return);
                            bool fR = (m_currentFunction->basicBlocks[bf].bType == BlockType::Return);
                            if (tR && !fR)
                                bm = bf;
                            else if (!tR && fR)
                                bm = bt;
                        }
                        // Require same merge target as outer mergeIdx — this is the deep dispatch
                        // pattern, not a generic if-then-elseif tree.
                        if (bm != stopId || bm != bt)
                            break;
                        ++depth;
                        cur = bf;
                    }
                    return depth;
                };

                if (probeChainDepth(falseIdx, mergeIdx) >= kChainProbeThreshold) {
                    IfStatementNode *parentIfRaw = ifStmt.get();
                    uint32_t chainBlockId = falseIdx;
                    uint32_t chainStopId = mergeIdx;
                    std::set<uint32_t> chainVisited = visitedCopy;
                    while (true) {
                        if (chainBlockId == InvalidBlockId || chainBlockId >= m_currentFunction->basicBlocks.size())
                            break;
                        if (!m_loopExitStack.empty() && chainBlockId == m_loopExitStack.back())
                            break;
                        const auto &chainBlock = m_currentFunction->basicBlocks[chainBlockId];
                        if (chainBlock.bType != BlockType::Return) {
                            if (chainBlockId == chainStopId || chainVisited.contains(chainBlockId))
                                break;
                        }
                        if (chainBlock.bType != BlockType::IfHeader)
                            break;
                        if (!chainBlock.ifStatementTrue.has_value() || !chainBlock.ifStatementFalse.has_value())
                            break;
                        if (DetectBooleanMaterialization(chainBlockId).has_value())
                            break;
                        if (DetectOrChain(chainBlockId).has_value())
                            break;
                        uint32_t cTrueIdx = chainBlock.ifStatementTrue.value();
                        uint32_t cFalseIdx = chainBlock.ifStatementFalse.value();
                        uint32_t cMergeIdx = FindMergeBlock(cTrueIdx, cFalseIdx);
                        if (cMergeIdx == InvalidBlockId) {
                            bool t = (m_currentFunction->basicBlocks[cTrueIdx].bType == BlockType::Return);
                            bool f = (m_currentFunction->basicBlocks[cFalseIdx].bType == BlockType::Return);
                            if (t && !f)
                                cMergeIdx = cFalseIdx;
                            else if (!t && f)
                                cMergeIdx = cTrueIdx;
                        }
                        if (cMergeIdx != chainStopId || cMergeIdx != cTrueIdx)
                            break;
                        chainVisited.insert(chainBlockId);
                        auto chainStmts = LiftBlockInstructions(chainBlock);
                        auto newIf = std::make_shared<IfStatementNode>();
                        newIf->condition = InvertCondition(LiftCondition(chainBlock.lpTail));
                        std::vector<std::shared_ptr<Statement>> blockBody;
                        blockBody.insert(blockBody.end(), chainStmts.begin(), chainStmts.end());
                        blockBody.push_back(newIf);
                        parentIfRaw->thenBranch = CreateBlock(blockBody);
                        parentIfRaw = newIf.get();
                        chainBlockId = cFalseIdx;
                    }
                    parentIfRaw->thenBranch = CreateBlock(LiftControlFlow(chainBlockId, chainStopId, chainVisited));
                } else {
                    ifStmt->thenBranch = CreateBlock(LiftControlFlow(falseIdx, mergeIdx, visitedCopy));
                }
            } else {
                ifStmt->condition = (trueCond);
                ifStmt->thenBranch = CreateBlock(LiftControlFlow(trueIdx, mergeIdx, visitedCopy));

                auto elseStmts = LiftControlFlow(falseIdx, mergeIdx, visitedCopy);
                if (!elseStmts.empty()) {
                    ifStmt->elseBranch = CreateBlock(elseStmts);
                }

                // only swap when there's an else to swap in; else-less empty-then would
                // leave thenBranch null and feed a malformed node to the fold passes.
                if (ifStmt->thenBranch->body.empty() && ifStmt->elseBranch) {
                    std::swap(ifStmt->thenBranch, ifStmt->elseBranch);
                    ifStmt->condition = InvertCondition(trueCond);
                }
            }

            if (auto rewritten = TryRewriteShortCircuitAssignment(ifStmt))
                nodes.push_back(rewritten);
            else {
                HoistPhiLocals(static_cast<int32_t>(mergeIdx), ifStmt, nodes, definedBeforeBranches);
                nodes.push_back(ifStmt);
            }

            // don't fall through into the loop exit here (would inline post-loop code / emit a stray
            // break); it's the break target and is lifted once after the loop.
            const bool mergeIsLoopExit = !m_loopExitStack.empty() && mergeIdx == m_loopExitStack.back();
            if (mergeIdx != InvalidBlockId && !mergeIsLoopExit)
                nextBlockId = mergeIdx;

        } else {
            nodes.push_back(std::make_shared<CommentNode>("Fission: Warning - Malformed IfHeader", true));
        }
        break;
    }
    case BlockType::LoopHeader: {
        // `while <const> do <inner loop> end`: the testless outer shares this header with the inner
        // loop (only a back-edge). detect it so the inner loop wraps in `while true`, else it's dropped.
        std::optional<uint32_t> infiniteWhileLatch;
        if (block.loopLatch.has_value())
            infiniteWhileLatch = DetectInfiniteWhileLatch(currentBlockId, *block.loopLatch);
        const size_t loopNodesStart = nodes.size();

        if (block.loopLatch.has_value()) {
            uint32_t latchIdx = block.loopLatch.value();
            uint32_t exitIdx = block.loopLatch.value();

            std::set<uint32_t> loopVisited = visited;

            bool isRepeatUntil = (block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop;

            if (isRepeatUntil) {
                // use recorded exit only if real; analyzer sometimes records the latch as exit.
                // else fall back to the non-latch header successor, or post-loop stmts leak into the body.
                if (block.loopExit.has_value() && block.loopExit.value() != latchIdx)
                    exitIdx = block.loopExit.value();
                else {
                    for (auto s : block.successors)
                        if (s != latchIdx) {
                            exitIdx = s;
                            break;
                        }
                }

                auto repeatNode = std::make_shared<RepeatStatementNode>();
                auto &latch = m_currentFunction->basicBlocks[latchIdx];

                // two repeat-until shapes: cond in latch (JUMPIF) vs cond in header (latch is plain JUMP).
                bool condInLatch = latch.lpTail && latch.lpTail->operation != LiftedOperation::JUMP;

                if (condInLatch) {
                    // Pattern 1: condition at end of latch block
                    repeatNode->condition = InvertCondition(LiftCondition(latch.lpTail));
                    if (latchIdx != currentBlockId) {
                        auto latchStmts = LiftBlockInstructions(latch);
                        stmts.insert(stmts.end(), latchStmts.begin(), latchStmts.end());
                    }
                } else {
                    // Pattern 2: latch is plain JUMP back to header. exit cond is in the header
                    // or decomposed into trailing if-return blocks (`until a or b`); lift body then scan.
                    uint32_t bodyStart = InvalidBlockId;
                    for (auto s : block.successors) {
                        if (s != exitIdx) {
                            bodyStart = s;
                            break;
                        }
                    }

                    std::set<uint32_t> bodyVisited = loopVisited;
                    bodyVisited.insert(latchIdx);
                    bodyVisited.insert(exitIdx);

                    std::vector<std::shared_ptr<Statement>> bodyStmts;
                    if (bodyStart != InvalidBlockId) {
                        bodyStmts = LiftControlFlow(bodyStart, latchIdx, bodyVisited);
                    }

                    // prepend header: in repeat-until it's also the first body block, so its
                    // mutating defs must survive even when used by the trailing cond/return.
                    auto headerStmts = LiftBlockInstructions(block, true);
                    bodyStmts.insert(bodyStmts.begin(), headerStmts.begin(), headerStmts.end());

                    // Append latch instructions if latch is not the current block.
                    if (latchIdx != currentBlockId) {
                        auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                        bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                    }

                    // scan trailing if-return blocks: Luau decomposes `until a or b` into separate exit checks.
                    std::vector<std::shared_ptr<Expression>> exitConds;
                    while (!bodyStmts.empty()) {
                        auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(bodyStmts.back());
                        if (!ifStmt || ifStmt->elseBranch || !ifStmt->thenBranch)
                            break;
                        if (ifStmt->thenBranch->body.size() != 1)
                            break;
                        auto retStmt = std::dynamic_pointer_cast<ReturnStatementNode>(ifStmt->thenBranch->body[0]);
                        if (!retStmt || retStmt->returnValues.empty())
                            break;
                        exitConds.push_back(ifStmt->condition);
                        bodyStmts.pop_back();
                    }

                    if (!exitConds.empty()) {
                        auto combined = exitConds[0];
                        for (size_t i = 1; i < exitConds.size(); ++i) {
                            combined = std::make_shared<BinaryExpressionNode>("or", combined, exitConds[i]);
                        }
                        repeatNode->condition = combined;
                    } else {
                        repeatNode->condition = LiftCondition(block.lpTail);
                    }

                    repeatNode->body = CreateBlock(bodyStmts);
                }
                nodes.push_back(repeatNode);
            } else if ((block.dwBlockFlags & LoopBlockFlags::ForNumericLoop) == LoopBlockFlags::ForNumericLoop) {
                uint32_t bodyIdx = InvalidBlockId;
                for (auto succ : block.successors) {
                    if (succ != block.loopLatch.value_or(InvalidBlockId)) {
                        bodyIdx = succ;
                        break;
                    }
                }
                if (block.loopExit.has_value() && block.loopExit.value() != bodyIdx && block.loopExit.value() != block.loopLatch.value_or(InvalidBlockId))
                    exitIdx = block.loopExit.value();
                else if (block.loopLatch.has_value()) {
                    for (auto succ : m_currentFunction->basicBlocks[*block.loopLatch].successors) {
                        if (succ != block.dwBlockId) {
                            exitIdx = succ;
                            break;
                        }
                    }
                }

                auto forNode = std::make_shared<ForNumericNode>();
                auto forPrepInst = block.lpTail;
                int32_t limitVer = -1, stepVer = -1, startVer = -1;

                if (this->m_currentFunction->implicitUses.contains(forPrepInst)) {
                    const auto &impl = this->m_currentFunction->implicitUses.at(forPrepInst);
                    if (impl.size() >= 3) {
                        limitVer = impl[0];
                        stepVer = impl[1];
                        startVer = impl[2];
                    }
                }

                int baseReg = block.lpTail->operands[0].value.reg;
                // name the loop var once up-front so body/header/SSA map agree. LiftExpression would
                // resolve it to the inlined LOADN constant → broken `for 1 = 1, ..., 1 do` (InfiniteYield).
                const std::string loopVarName = std::format("i_{}", baseReg + 2);
                {
                    LiftedOperand op;
                    {
                        PinnedRegisterScope pin(this, baseReg + 2);

                        this->m_currentFunction->SetVariableName(baseReg + 2, startVer, loopVarName);
                        // expose this loop's exit so body branches to it become `break` (real exit only).
                        const bool hasBreakTarget =
                            (exitIdx != InvalidBlockId && exitIdx != latchIdx && exitIdx != bodyIdx);
                        if (hasBreakTarget)
                            m_loopExitStack.push_back(exitIdx);
                        forNode->lpLoopBody = CreateBlock(LiftControlFlow(
                            bodyIdx, *block.loopLatch,
                            loopVisited /* this likely we have to replace with visited, as we will else be accidentally traversing again */
                        ));
                        if (hasBreakTarget)
                            m_loopExitStack.pop_back();

                        // use the body's string, not LiftExpression (may inline a LOAD const). route via
                        // ResolveVariableName so its m_definedRegisters bookkeeping still runs.
                        {
                            LiftedOperand idOp{};
                            idOp.type = LiftedOperandType::Register;
                            idOp.value.reg = baseReg + 2;
                            idOp.ssaVersion = startVer;
                            const auto resolved = ResolveVariableName(idOp);
                            forNode->loopVariable =
                                std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(resolved.empty() ? loopVarName : resolved));
                        }

                        this->m_currentFunction->ClearVersionName(
                            baseReg, block.lpTail->operands[0].ssaVersion
                        ); // clear v name or else it will not do anything good.
                    }
                    // start/limit/step may be phi outputs (FORNLOOP writes R(A+2) in the latch). trace
                    // the phi to the pre-header LOAD to inline the const (ShouldInline refuses phi-consumed).
                    auto liftLoopValue = [&](int reg, int32_t ver) -> std::shared_ptr<Expression> {
                        auto makeKey = [](int r, int32_t v) {
                            LiftedOperand k{};
                            k.type = LiftedOperandType::Register;
                            k.value.reg = static_cast<uint8_t>(r);
                            k.ssaVersion = static_cast<uint8_t>(v);
                            return k;
                        };
                        int32_t effectiveVer = ver;
                        auto *def = m_currentFunction->GetDefinition(makeKey(reg, ver));
                        if (def && def->operation == LiftedOperation::PHI && def->operands.size() >= 2) {
                            for (size_t i = 0; i < block.predecessors.size() && i + 1 < def->operands.size(); ++i) {
                                if (block.loopLatch.has_value() && block.predecessors[i] == block.loopLatch.value())
                                    continue;
                                effectiveVer = def->operands[i + 1].ssaVersion;
                                break;
                            }
                        }
                        auto *effDef = m_currentFunction->GetDefinition(makeKey(reg, effectiveVer));
                        if (effDef && effDef->operation == LiftedOperation::LOAD) {
                            m_processedInstructions.insert(effDef->instructionIndex);
                            auto &valOp = effDef->operands[1];
                            if (valOp.type == LiftedOperandType::ImmediateInteger)
                                return std::make_shared<NumberLiteralNode>(valOp.value.imm.n);
                            if (valOp.type == LiftedOperandType::ImmediateNil)
                                return std::make_shared<NilLiteralNode>();
                            if (valOp.type == LiftedOperandType::ImmediateBool)
                                return std::make_shared<BooleanLiteralNode>(valOp.value.imm.b);
                            if (valOp.type == LiftedOperandType::ImmediateConstant) {
                                const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[valOp.value.imm.k];
                                switch (k.kType) {
                                case LUA_TNIL:
                                    return std::make_shared<NilLiteralNode>();
                                case LUA_TBOOLEAN:
                                    return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
                                case LUA_TNUMBER:
                                    return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
                                case LUA_TINTEGER:
                                    return std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
                                case LUA_TSTRING:
                                    return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
                                case LUA_TVECTOR: {
                                    const auto &[x, y, z, w] = std::get<LuauVector>(k.constantData);
                                    return std::make_shared<VectorNode>(x, y, z, w);
                                }
                                default:
                                    return std::make_shared<NilLiteralNode>();
                                }
                            }
                        }
                        LiftedOperand lop;
                        lop.type = LiftedOperandType::Register;
                        lop.value.reg = reg;
                        lop.ssaVersion = effectiveVer;
                        return LiftExpression(lop, true);
                    };

                    forNode->startVariable = liftLoopValue(baseReg + 2, startVer);
                    forNode->maxIncreased = liftLoopValue(baseReg, limitVer);
                    forNode->increaseBy = liftLoopValue(baseReg + 1, stepVer);
                }
                nodes.push_back(forNode);
            } else if ((block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop) {
                auto whileNode = std::make_shared<WhileStatementNode>();

                if (block.loopExit.has_value())
                    exitIdx = block.loopExit.value();

                uint32_t bodyStart = InvalidBlockId;
                for (auto s : block.successors)
                    if (s != exitIdx)
                        bodyStart = s;

                // header with stmts before its exit test: folding the test into the cond would reorder
                // it ahead of them. keep `while true` and emit the test as a `break` at its real spot.
                const bool headerHasBody = !stmts.empty();
                // a header with no conditional exit (unconditional back-edge, or fallthrough into
                // the body) has no test == infinite `while true`; LiftCondition on its
                // non-comparison tail would yield `false`, rendering `while not false`.
                const bool infiniteWhile = block.bTerminator != BlockTerminator::Conditional;
                if (headerHasBody || infiniteWhile)
                    whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                else
                    whileNode->condition = InvertCondition(LiftCondition(block.lpTail));

                if (bodyStart != InvalidBlockId) {
                    std::set<uint32_t> cloopVisited = visited;
                    cloopVisited.insert(latchIdx);
                    cloopVisited.insert(exitIdx);
                    const bool hasBreakTarget = (exitIdx != InvalidBlockId && exitIdx != latchIdx && exitIdx != bodyStart);
                    if (hasBreakTarget)
                        m_loopExitStack.push_back(exitIdx);
                    auto bodyStmts = LiftControlFlow(bodyStart, latchIdx, cloopVisited);
                    if (hasBreakTarget)
                        m_loopExitStack.pop_back();

                    if (latchIdx != currentBlockId) {
                        auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                        bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                    }

                    if (headerHasBody) {
                        // header body → `if <exit cond> then break end` → rest of body.
                        auto breakIf = std::make_shared<IfStatementNode>();
                        breakIf->condition = LiftCondition(block.lpTail);
                        breakIf->thenBranch = CreateBlock({std::make_shared<BreakStatementNode>()});
                        bodyStmts.insert(bodyStmts.begin(), breakIf);
                    }
                    bodyStmts.insert(bodyStmts.begin(), stmts.begin(), stmts.end());
                    whileNode->body = CreateBlock(bodyStmts);
                } else {
                    whileNode->body = CreateBlock(stmts);
                }
                nodes.push_back(whileNode);
            } else if (
                (block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop) == LoopBlockFlags::ForGeneralLoop ||
                (block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop_Pairs) == LoopBlockFlags::ForGeneralLoop_Pairs ||
                (block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop_Indexed) == LoopBlockFlags::ForGeneralLoop_Indexed
            ) {
                uint32_t bodyIdx = InvalidBlockId;
                for (auto succ : block.successors) {
                    if (succ != block.loopLatch.value_or(InvalidBlockId)) {
                        bodyIdx = succ;
                        break;
                    }
                }
                if (block.loopExit.has_value() && block.loopExit.value() != bodyIdx && block.loopExit.value() != block.loopLatch.value_or(InvalidBlockId))
                    exitIdx = block.loopExit.value();
                else if (block.loopLatch.has_value()) {
                    for (auto succ : m_currentFunction->basicBlocks[*block.loopLatch].successors) {
                        if (succ != block.dwBlockId) {
                            exitIdx = succ;
                            break;
                        }
                    }
                }

                auto forNode = std::make_shared<ForGeneralNode>();
                int baseReg = block.lpTail->operands[0].value.reg;
                int numVars = m_currentFunction->basicBlocks[*block.loopLatch].lpTail->operands[2].value.imm.n & 0xFF;

                {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;

                    int32_t genVer = -1, stateVer = -1, indexVer = -1;
                    if (m_currentFunction->implicitUses.contains(block.lpTail)) {
                        const auto &impl = m_currentFunction->implicitUses.at(block.lpTail);
                        if (impl.size() >= 3) {
                            genVer = impl[0];
                            stateVer = impl[1];
                            indexVer = impl[2];
                        }
                    }

                    const bool hasBreakTarget =
                        (exitIdx != InvalidBlockId && exitIdx != block.loopLatch.value_or(InvalidBlockId) && exitIdx != bodyIdx);
                    if (hasBreakTarget)
                        m_loopExitStack.push_back(exitIdx);
                    forNode->body = CreateBlock(LiftControlFlow(bodyIdx, *block.loopLatch, loopVisited));
                    if (hasBreakTarget)
                        m_loopExitStack.pop_back();

                    for (int i = 0; i < numVars; ++i) {
                        LiftedOperand varOp;
                        varOp.type = LiftedOperandType::Register;
                        varOp.value.reg = baseReg + 3 + i;
                        forNode->loopVariables.push_back(LiftExpression(varOp));
                    }

                    // Check if all 3 implicit uses come from the same CALL (e.g. pairs(t))
                    LiftedOperand genCheck{op};
                    genCheck.value.reg = baseReg;
                    genCheck.ssaVersion = genVer;
                    LiftedOperand stateCheck{op};
                    stateCheck.value.reg = baseReg + 1;
                    stateCheck.ssaVersion = stateVer;
                    LiftedOperand indexCheck{op};
                    indexCheck.value.reg = baseReg + 2;
                    indexCheck.ssaVersion = indexVer;

                    auto *genDef = genVer >= 0 ? m_currentFunction->GetDefinition(genCheck) : nullptr;
                    auto *stateDef = stateVer >= 0 ? m_currentFunction->GetDefinition(stateCheck) : nullptr;
                    auto *indexDef = indexVer >= 0 ? m_currentFunction->GetDefinition(indexCheck) : nullptr;

                    bool allFromSameCall =
                        (genDef && stateDef && indexDef && genDef == stateDef && stateDef == indexDef &&
                         (genDef->operation == LiftedOperation::CALL || genDef->operation == LiftedOperation::CALLFB ||
                          genDef->operation == LiftedOperation::NAMECALL));

                    if (allFromSameCall) {
                        // e.g. for k,v in pairs(t) do — the call returns 3 values
                        forNode->generator = LiftCall(*genDef, genDef->instructionIndex, true);
                        forNode->state = nullptr;
                        forNode->index = nullptr;
                    } else {
                        op.value.reg = baseReg;
                        op.ssaVersion = genVer;
                        forNode->generator = LiftExpression(op, false);

                        op.value.reg = baseReg + 1;
                        op.ssaVersion = stateVer;
                        forNode->state = LiftExpression(op, false);

                        op.value.reg = baseReg + 2;
                        op.ssaVersion = indexVer;
                        forNode->index = LiftExpression(op, false);

                        // `for k,v in t do` lowers to [t, nil, nil] (Luau pads to 3). the nil state/control
                        // aren't idiomatic/portable, so collapse to the single-generator form.
                        if (std::dynamic_pointer_cast<NilLiteralNode>(forNode->state) &&
                            std::dynamic_pointer_cast<NilLiteralNode>(forNode->index)) {
                            forNode->state = nullptr;
                            forNode->index = nullptr;
                        }
                    }
                }
                nodes.push_back(forNode);
            }

            nextBlockId = exitIdx;
        }

        if (infiniteWhileLatch.has_value() && nodes.size() > loopNodesStart) {
            std::vector<std::shared_ptr<Statement>> loopBody(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
            nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
            auto whileNode = std::make_shared<WhileStatementNode>();
            whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
            whileNode->body = CreateBlock(loopBody);
            nodes.push_back(whileNode);
            nextBlockId = InvalidBlockId; // infinite loop: nothing after the back-edge is reachable.
        }
        break;
    }
    case BlockType::Break:
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        nodes.push_back(std::make_shared<BreakStatementNode>());
        break;
    case BlockType::Return:
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        break;
    case BlockType::LoopLatch:
        if ((block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop && block.loopHeader == block.dwBlockId &&
            block.loopExit.has_value()) {
            auto repeatNode = std::make_shared<RepeatStatementNode>();
            repeatNode->condition = InvertCondition(LiftCondition(block.lpTail));
            repeatNode->body = CreateBlock(stmts);
            nodes.push_back(repeatNode);
            nextBlockId = block.loopExit.value();
        } else {
            nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        }
        break;
    default: {
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        if (!block.successors.empty())
            nextBlockId = block.successors[0];
        break;
    }
    }

        if (nextBlockId == InvalidBlockId)
            break;
        currentBlockId = nextBlockId;
    }

    // Fold consecutive `X = expr; X = X or expr2` pairs at this level so chains
    // (`a or b or c`) collapse end-to-end as control flow is unwound bottom-up.
    FoldShortCircuitChain(nodes);

    return nodes;
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftBlockInstructions(const BasicBlock &block, bool forceDefinitions) {
    std::vector<std::shared_ptr<Statement>> statements;
    if (!block.lpHead)
        return statements;

    for (int i = block.lpHead->instructionIndex; i <= block.lpTail->instructionIndex; ++i) {
        if (m_processedInstructions.contains(i))
            continue;

        const auto &inst = m_currentFunction->lpLiftedFunction->instructions[i];

        if (!forceDefinitions && ShouldInline(&inst))
            continue;

        switch (inst.operation) {
        case LiftedOperation::GETVARARGS: {
            break; // not handled here.
        }
        case LiftedOperation::SETGLOBAL: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[1].value.imm.k];
            auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::get<std::string>(k.constantData)));
            statements.push_back(std::make_shared<AssignmentStatementNode>(left, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLE: {
            auto idxExpr = std::make_shared<IndexExpressionNode>(LiftExpression(inst.operands[1]), LiftExpression(inst.operands[2]));
            statements.push_back(std::make_shared<AssignmentStatementNode>(idxExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLEKS: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[2].value.imm.k];
            auto memExpr = std::make_shared<MemberExpressionNode>(LiftExpression(inst.operands[1]), std::get<std::string>(k.constantData));
            statements.push_back(std::make_shared<AssignmentStatementNode>(memExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::RETURN: {
            std::vector<std::shared_ptr<Expression>> rets;
            if (m_currentFunction->implicitUses.contains(&inst)) {
                for (int32_t ver : m_currentFunction->implicitUses.at(&inst)) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = inst.operands[0].value.reg + (int)rets.size();
                    op.ssaVersion = ver;

                    auto def = m_currentFunction->GetDefinition(op);
                    if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::NAMECALL)) {
                        int32_t callIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                        if (m_currentFunction->lpLiftedFunction->instructions[callIdx].operands[2].value.imm.n == 0) {
                            rets.push_back(LiftCall(*def, def->instructionIndex, true));
                            break;
                        }
                    }
                    rets.push_back(LiftExpression(op));
                }
            }

            if (i == (int32_t)this->m_currentFunction->lpLiftedFunction->instructions.size() - 1 && inst.operation == LiftedOperation::RETURN && rets.empty()) {
                for (const auto &pred : block.predecessors)
                    if (this->m_currentFunction->basicBlocks.at(pred).bType == BlockType::IfHeader ||
                        this->m_currentFunction->basicBlocks.at(pred).bType == BlockType::LoopHeader ||
                        this->m_currentFunction->basicBlocks.at(pred).bType ==
                            BlockType::LoopLatch) { // the compiler may inline the break as a RETURN instruction instead.
                        statements.push_back(std::make_shared<ReturnStatementNode>(rets));
                        break;
                    }
                continue; // ignore last return if and only if there's no returns.
            }

            statements.push_back(std::make_shared<ReturnStatementNode>(rets));
            break;
        }
        case LiftedOperation::CALL:
        case LiftedOperation::CALLFB:
        case LiftedOperation::NAMECALL: {
            auto callExpr = LiftCall(inst, i, false);

            std::vector<std::shared_ptr<Expression>> lhs;
            std::vector<SSARef> defs;
            for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
                if (defInst == &inst)
                    defs.push_back(ref);
            }
            std::ranges::sort(defs, [](auto &a, auto &b) { return a.regIndex < b.regIndex; });

            for (const auto &ref : defs) {
                if (m_currentFunction->useCounts[ref] > 0) {
                    if (auto nameCallNode = std::dynamic_pointer_cast<NameCallExpressionNode>(callExpr); nameCallNode) {
                        auto *identExpr = std::dynamic_pointer_cast<IdentifierExpressionNode>(nameCallNode->callWhat).get();
                        if (!identExpr)
                            continue;
                        const auto &methodName = identExpr->identifier->name;
                        // `:FindFirstChild`/`:GetService`/`:WaitForChild` name their result after the child.
                        if (methodName == "FindFirstChild" || methodName == "GetService" || methodName == "WaitForChild") {
                            if (!m_currentFunction->implicitUses.contains(&inst))
                                continue;
                            const auto &argVersions = m_currentFunction->implicitUses.at(&inst);
                            if (argVersions.size() == 2) {
                                // we can name the register appropiately.
                                LiftedOperand op;
                                op.type = LiftedOperandType::Register;
                                op.value.reg = inst.operands[0].value.reg + 1 + 1 /* arg1 */;
                                op.ssaVersion = argVersions[1];
                                auto expr = LiftExpression(op, true);
                                if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(expr); str && IsValidLuauIdent(str->value))
                                    this->m_currentFunction->SetVariableName(ref.regIndex, ref.version, str->value);
                            }
                        }
                    } else if (auto callNode = std::dynamic_pointer_cast<CallExpressionNode>(callExpr)) {
                        // `require(path:WaitForChild("Module"))` names the result after the child.
                        auto callee = std::dynamic_pointer_cast<IdentifierExpressionNode>(callNode->callee);
                        if (callee && callee->identifier && callee->identifier->name == "require" && callNode->arguments.size() == 1) {
                            if (auto argCall = std::dynamic_pointer_cast<NameCallExpressionNode>(callNode->arguments[0])) {
                                auto m = std::dynamic_pointer_cast<IdentifierExpressionNode>(argCall->callWhat);
                                const bool childLookup =
                                    m && m->identifier && (m->identifier->name == "WaitForChild" || m->identifier->name == "FindFirstChild");
                                if (childLookup && argCall->arguments.size() == 1)
                                    if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(argCall->arguments[0]); str && IsValidLuauIdent(str->value))
                                        this->m_currentFunction->SetVariableName(ref.regIndex, ref.version, str->value);
                            }
                        }
                    }

                    lhs.push_back(
                        std::make_shared<IdentifierExpressionNode>(
                            std::make_shared<Identifier>(ResolveVariableName({LiftedOperandType::Register, {ref.regIndex}, ref.version}))
                        )
                    );
                }
            }

            if (!lhs.empty()) {
                if (auto callNode = std::dynamic_pointer_cast<CallExpressionNode>(callExpr)) {
                    callNode->rets = lhs;
                } else if (auto nameCallNode = std::dynamic_pointer_cast<NameCallExpressionNode>(callExpr)) {
                    nameCallNode->rets = lhs;
                }
            }

            statements.push_back(std::make_shared<ExpressionStatementNode>(callExpr));

            if (inst.operation == LiftedOperation::NAMECALL) {
                m_processedInstructions.insert(i + 1);
                m_processedInstructions.insert(i + 2);
            }
            break;
        }

        case LiftedOperation::DUPCLOSURE: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[1].value.imm.k];
            const auto duplicatedFunction = std::get<LuauProto>(k.constantData);

            AnalyzedFunction *targetFunc = nullptr;
            for (auto &inner : m_currentFunction->innerFunctions) {
                if (inner.lpLiftedFunction->lpDeserialized->bytecodeId == duplicatedFunction->bytecodeId) {
                    targetFunc = &inner;
                    targetFunc->PopulateNames(); // populate to prevent bad usages.
                    // disambiguate this nested fn's own vN/argN from the upvalues it captures.
                    targetFunc->nameSuffix = std::format("_{}", duplicatedFunction->bytecodeId);
                    break;
                }
            }

            // walk trailing CAPTUREs; "propagate" = rename the source reg to the upvalue's debug name
            // instead of emitting `local up = source`. needs: debug name + VAL/REF capture + register source.
            struct CaptureAction {
                LiftedInstruction *capInst;
                std::string upName;
                bool hasDebugName;
                bool willPropagate;
                bool shouldEmit;
            };
            std::vector<CaptureAction> captureActions;

            size_t capIdx = 0;
            while (i + 1 + capIdx < m_currentFunction->lpLiftedFunction->instructions.size()) {
                auto &cap = m_currentFunction->lpLiftedFunction->instructions[i + 1 + capIdx];
                if (cap.operation != LiftedOperation::CAPTURE)
                    break;

                CaptureAction action{};
                action.capInst = &cap;
                action.hasDebugName = duplicatedFunction->upvalueNames.size() > capIdx;
                action.upName = targetFunc->GetUpvalueName(capIdx);

                const int captureMode = cap.operands[0].value.imm.n;
                const bool srcIsRegister = cap.operands.size() >= 2 && cap.operands[1].type == LiftedOperandType::Register;
                // VAL/REF capture's upvalue IS its source local (VAL = stable snapshot, REF = shared cell).
                // alias to the source's name, never emit `local uv_N = source`: those collide across
                // sibling closures (all number from 0) and, for REF, desync later writes.
                if ((captureMode == 0 || captureMode == 1) && !action.hasDebugName && srcIsRegister)
                    action.upName = m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) → alias above; LCT_UPVAL (2) → parent's
                // upvalue at that index. override survives the sub-lift's PopulateNames().
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister && targetFunc)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), action.upName);
                else if (captureMode == 2 && targetFunc)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), m_currentFunction->GetUpvalueName(cap.operands[1].value.reg));

                captureActions.push_back(action);
                m_processedInstructions.insert(i + 1 + capIdx);
                ++capIdx;
            }

            // apply renames before lifting sub-exprs so neighbours/inner-lift agree. write ssaOverrides
            // directly (beats globalRegNames' default `argN`); SetVariableName is lowest-priority and
            // would be shadowed for arg registers.
            for (const auto &act : captureActions) {
                if (!act.willPropagate)
                    continue;
                const auto &srcOp = act.capInst->operands[1];
                m_currentFunction->ssaOverrides[SSARef{static_cast<uint8_t>(srcOp.value.reg), srcOp.ssaVersion}] = act.upName;
                statements.push_back(std::make_shared<CommentNode>(std::format("Fission: INFO: Name '{}' propagated from upvalue names.", act.upName), true, true));
            }

            // emit `local up = expr` only for non-propagated captures; skip marker comments if none.
            bool anyEmit = false;
            for (const auto &act : captureActions)
                if (act.shouldEmit) {
                    anyEmit = true;
                    break;
                }

            if (anyEmit) {
                statements.push_back(
                    std::make_shared<CommentNode>(
                        std::format("Fission: Beginning captures for function with name '{}'", this->GetFunctionName(duplicatedFunction)), true, true
                    )
                );
                for (const auto &act : captureActions) {
                    if (!act.shouldEmit)
                        continue;
                    statements.push_back(
                        std::make_shared<CommentNode>(act.hasDebugName ? "Fission: name from debug information." : "Fission: autogenerated name.", true, true)
                    );
                    statements.push_back(
                        std::make_shared<VariableDeclarationNode>(
                            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(act.upName)), this->LiftExpression(act.capInst->operands[1])
                        )
                    );
                }
                statements.push_back(
                    std::make_shared<CommentNode>(
                        std::format("Fission: Ending captures for function with name '{}'", this->GetFunctionName(duplicatedFunction)), true, true
                    )
                );
            }

            ASTLifter subLifter;
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = this->GetFunctionName(duplicatedFunction);
            // Pin the closure name to *this* SSA version only. Using
            // SetGlobalName here would cause every later reuse of the same
            // register (e.g. the return slot of a `pcall(closure)`) to keep
            // calling that value by the closure's name.
            m_currentFunction->SetVariableName(inst.operands[0].value.reg, inst.operands[0].ssaVersion, funcName);

            std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> argNames;
            for (int j = 0; j < duplicatedFunction->numparams; ++j) {
                std::string argName = targetFunc->GetVarName(j, 0);
                if (argName.empty() || argName == std::format("v{}", j))
                    argName = std::format("a{}", j);

                auto identifier = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(argName));
                if (auto n = Deserializer::TryGetTypeName(duplicatedFunction, j)) {
                    argNames[j] =
                        std::make_shared<FunctionArgumentExpression>(identifier, std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(*n)));
                } else {
                    argNames[j] = std::make_shared<FunctionArgumentExpression>(identifier, std::nullopt);
                }
            }

            if (targetFunc->lpLiftedFunction->lpDeserialized->isvararg) /* marker indicates vararg is required at the end of the function's arguments. */
                argNames[duplicatedFunction->numparams] = std::make_shared<FunctionArgumentExpression>(
                    std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("...")), std::nullopt
                ); // insert vararg.

            auto bodyBlock = std::make_shared<BlockStatementNode>();
            bodyBlock->body = subAst.statements;
            // literally almost the same handler as NEWCLOSURE.

            auto fnDecl =
                std::make_shared<FunctionDeclarationNode>(funcName, duplicatedFunction->numparams, argNames, duplicatedFunction->isvararg, bodyBlock, true);

            auto &saveWhere = inst.operands[0];

            // If the closure has a single use that is a call argument, park
            // it in the inline-substitution map and skip emitting a top-level
            // `local function name(...) ... end` declaration entirely.
            if (IsSingleUseCallArgument(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion) ||
                IsSingleUseTableFieldValue(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                break;
            }

            if (m_definedRegisters.contains(saveWhere.value.reg)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                statements.push_back(
                    std::make_shared<AssignmentStatementNode>(
                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(saveWhere))), fnDecl
                    )
                );
                break;
            }

            statements.push_back(fnDecl);

            if (this->m_currentFunction->users.contains({saveWhere.value.reg, saveWhere.ssaVersion})) {
                auto users = this->m_currentFunction->users[{saveWhere.value.reg, saveWhere.ssaVersion}];
                for (const auto &user : users) {
                    if (user->operation == LiftedOperation::SETGLOBAL) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back()); fDec->bIsLocalDeclaration) {
                m_currentFunction->SetVariableName(inst.operands[0].value.reg, inst.operands[0].ssaVersion, funcName);
            }

            break;
        }
        case LiftedOperation::NEWCLOSURE: {
            int protoIdx = inst.operands[1].value.imm.k;
            const auto proto = m_currentFunction->lpLiftedFunction->lpDeserialized->subfunctions[protoIdx];

            AnalyzedFunction *targetFunc = nullptr;
            for (auto &inner : m_currentFunction->innerFunctions) {
                if (inner.lpLiftedFunction->lpDeserialized->bytecodeId == proto->bytecodeId) {
                    targetFunc = &inner;
                    targetFunc->PopulateNames(); // populate to prevent bad usages.
                    // Disambiguate this nested function's own vN/argN names from the
                    // upvalues it captures (which read as the enclosing scope's names).
                    targetFunc->nameSuffix = std::format("_{}", proto->bytecodeId);
                    break;
                }
            }

            // See DUPCLOSURE for the rationale of the propagate-or-emit split.
            struct CaptureAction {
                LiftedInstruction *capInst;
                std::string upName;
                bool hasDebugName;
                bool willPropagate;
                bool shouldEmit;
            };
            std::vector<CaptureAction> captureActions;

            size_t capIdx = 0;
            while (i + 1 + capIdx < m_currentFunction->lpLiftedFunction->instructions.size()) {
                auto &cap = m_currentFunction->lpLiftedFunction->instructions[i + 1 + capIdx];
                if (cap.operation != LiftedOperation::CAPTURE)
                    break;

                CaptureAction action{};
                action.capInst = &cap;
                action.hasDebugName = proto->upvalueNames.size() > capIdx;
                action.upName = targetFunc->GetUpvalueName(capIdx);

                const int captureMode = cap.operands[0].value.imm.n;
                const bool srcIsRegister = cap.operands.size() >= 2 && cap.operands[1].type == LiftedOperandType::Register;
                // VAL/REF capture's upvalue IS its source local (VAL = stable snapshot, REF = shared cell).
                // alias to the source's name, never emit `local uv_N = source`: those collide across
                // sibling closures (all number from 0) and, for REF, desync later writes.
                if ((captureMode == 0 || captureMode == 1) && !action.hasDebugName && srcIsRegister)
                    action.upName = m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) → alias above; LCT_UPVAL (2) → parent's
                // upvalue at that index. override survives the sub-lift's PopulateNames().
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister && targetFunc)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), action.upName);
                else if (captureMode == 2 && targetFunc)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), m_currentFunction->GetUpvalueName(cap.operands[1].value.reg));

                captureActions.push_back(action);
                m_processedInstructions.insert(i + 1 + capIdx);
                ++capIdx;
            }

            // See DUPCLOSURE for why ssaOverrides (and not SetVariableName).
            for (const auto &act : captureActions) {
                if (!act.willPropagate)
                    continue;
                const auto &srcOp = act.capInst->operands[1];
                m_currentFunction->ssaOverrides[SSARef{static_cast<uint8_t>(srcOp.value.reg), srcOp.ssaVersion}] = act.upName;
                statements.push_back(std::make_shared<CommentNode>(std::format("Fission: INFO: Name '{}' propagated from upvalue names.", act.upName), true, true));
            }

            bool anyEmit = false;
            for (const auto &act : captureActions)
                if (act.shouldEmit) {
                    anyEmit = true;
                    break;
                }

            if (anyEmit) {
                statements.push_back(
                    std::make_shared<CommentNode>(std::format("Fission: Beginning captures for function with name '{}'", this->GetFunctionName(proto)), true, true)
                );
                for (const auto &act : captureActions) {
                    if (!act.shouldEmit)
                        continue;
                    statements.push_back(
                        std::make_shared<CommentNode>(act.hasDebugName ? "Fission: name from debug information." : "Fission: autogenerated name.", true, true)
                    );
                    statements.push_back(
                        std::make_shared<VariableDeclarationNode>(
                            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(act.upName)), this->LiftExpression(act.capInst->operands[1])
                        )
                    );
                }
                statements.push_back(
                    std::make_shared<CommentNode>(std::format("Fission: Ending captures for function with name '{}'", this->GetFunctionName(proto)), true, true)
                );
            }

            ASTLifter subLifter;
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = this->GetFunctionName(proto);

            std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> argNames;
            for (int j = 0; j < proto->numparams; ++j) {
                std::string argName = targetFunc->GetVarName(j, 0);
                if (argName.empty() || argName == std::format("v{}", j))
                    argName = std::format("a{}", j);

                auto identifier = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(argName));
                if (auto n = Deserializer::TryGetTypeName(proto, j)) {
                    argNames[j] =
                        std::make_shared<FunctionArgumentExpression>(identifier, std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(*n)));
                } else {
                    argNames[j] = std::make_shared<FunctionArgumentExpression>(identifier, std::nullopt);
                }
            }

            if (targetFunc->lpLiftedFunction->lpDeserialized->isvararg) /* marker indicates vararg is required at the end of the function's arguments. */
                argNames[proto->numparams] = std::make_shared<FunctionArgumentExpression>(
                    std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("...")), std::nullopt
                ); // insert vararg.

            auto bodyBlock = std::make_shared<BlockStatementNode>();
            bodyBlock->body = subAst.statements;

            auto fnDecl = std::make_shared<FunctionDeclarationNode>(funcName, proto->numparams, argNames, proto->isvararg, bodyBlock, true);

            auto &saveWhere = inst.operands[0];

            // See DUPCLOSURE: single-use call-arg / table-field closures collapse
            // to an inline `function(...) ... end` substituted at the use site.
            if (IsSingleUseCallArgument(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion) ||
                IsSingleUseTableFieldValue(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                break;
            }

            if (m_definedRegisters.contains(saveWhere.value.reg)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                statements.push_back(
                    std::make_shared<AssignmentStatementNode>(
                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(saveWhere))), fnDecl
                    )
                );
                break;
            }

            statements.push_back(fnDecl);

            if (this->m_currentFunction->users.contains({saveWhere.value.reg, saveWhere.ssaVersion})) {
                auto users = this->m_currentFunction->users[{saveWhere.value.reg, saveWhere.ssaVersion}];
                for (const auto &user : users) {
                    if (user->operation == LiftedOperation::SETGLOBAL) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back()); fDec->bIsLocalDeclaration) {
                // Pin the closure name to this SSA version only - see DUPCLOSURE comment.
                m_currentFunction->SetVariableName(inst.operands[0].value.reg, inst.operands[0].ssaVersion, funcName);
            }

            break;
        }

        case LiftedOperation::MOVE: {
            bool isNewDef = !m_definedRegisters.contains(inst.operands[0].value.reg);
            if (isNewDef)
                m_definedRegisters.insert(inst.operands[0].value.reg);

            auto target = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(inst.operands[0])));
            auto val = LiftExpression(inst.operands[1], false);

            if (auto valId = std::dynamic_pointer_cast<IdentifierExpressionNode>(val); valId && valId->identifier) {
                if (target->identifier->name == valId->identifier->name)
                    break;
            }

            if (isNewDef)
                statements.push_back(std::make_shared<VariableDeclarationNode>(target, val));
            else
                statements.push_back(std::make_shared<AssignmentStatementNode>(target, val));
            break;
        }

        case LiftedOperation::SETUPVAL: {
            auto upname = m_currentFunction->GetUpvalueName(inst.operands[1].value.imm.n);
            auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(upname));
            statements.push_back(std::make_shared<AssignmentStatementNode>(left, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            break;

        default: {
            if (inst.operands.empty())
                break;
            if (inst.operands[0].type != LiftedOperandType::Register)
                break;

            if (forceDefinitions && block.lpTail && inst.operation == LiftedOperation::LOAD) {
                SSARef defRef{inst.operands[0].value.reg, inst.operands[0].ssaVersion};
                if (m_currentFunction->users.contains(defRef)) {
                    const auto &users = m_currentFunction->users.at(defRef);
                    bool onlyFeedsTerminator = !users.empty();
                    for (const auto *user : users) {
                        if (user != block.lpTail) {
                            onlyFeedsTerminator = false;
                            break;
                        }
                    }
                    if (onlyFeedsTerminator)
                        break;
                }
            }

            const auto *def = m_currentFunction->GetDefinition(inst.operands[0]);
            if (def == &inst) {
                auto isDefined = m_definedRegisters.contains(inst.operands[0].value.reg);
                auto target = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(inst.operands[0])));
                auto val = LiftExpression(inst.operands[0], true);

                const bool isParameterWrite = inst.operands[0].value.reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams;
                if ((inst.operands[0].ssaVersion <= 1 && !isParameterWrite) || !isDefined)
                    statements.push_back(std::make_shared<VariableDeclarationNode>(target, val));
                else {
                    if (val->nodeKind == ASTNodeKind::BinaryExpression) {
                        // binary expressions may be compounded under specific conditions.
                        if (auto lpBinExpr = std::dynamic_pointer_cast<BinaryExpressionNode>(val); lpBinExpr != nullptr)
                            if (auto identifier = std::dynamic_pointer_cast<IdentifierExpressionNode>(lpBinExpr->left); identifier != nullptr) {
                                // expression is compound.
                                if (identifier->identifier->name == target->identifier->name) {
                                    statements.push_back(std::make_shared<CompoundBinaryExpressionNode>(lpBinExpr->op, lpBinExpr->left, lpBinExpr->right));
                                    break;
                                }
                            }
                    }

                    statements.push_back(std::make_shared<AssignmentStatementNode>(target, val));
                }
                if (forceDefinitions)
                    m_processedInstructions.insert(inst.instructionIndex);
            }
            break;
        }
        }
    }
    return statements;
}

bool ASTLifter::CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const std::set<uint32_t> &visitedScopes) {
    if (start == target)
        return true;

    std::queue<uint32_t> q;
    std::set<uint32_t> visited;

    q.push(start);
    visited.insert(start);

    while (!q.empty()) {
        uint32_t curr = q.front();
        q.pop();

        if (curr == target)
            return true;
        if (curr == stopBlock)
            continue;

        if (visited.size() > 5000)
            return false;

        const auto &block = m_currentFunction->basicBlocks[curr];

        for (uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (visitedScopes.contains(succ))
                continue;

            if (!visited.contains(succ)) {
                visited.insert(succ);
                q.push(succ);
            }
        }
    }
    return false;
}

std::shared_ptr<Expression> ASTLifter::LiftExpression(const LiftedOperand &__operand, bool forceExpression) {
    // Walk MOVE chains iteratively. Chains like `MOVE r1<-r0; MOVE r2<-r1; ...` would
    // otherwise tail-recurse one frame per hop, blowing the stack on deep copy fans.
    LiftedOperand operand = __operand;
    while (true) {
        if (operand.type != LiftedOperandType::Register)
            break;
        if (m_pinnedRegisters.contains(operand.value.reg))
            return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
        SSARef closureRef{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
        if (m_inlineableClosures.contains(closureRef))
            break;
        const auto *moveDef = m_currentFunction->GetDefinition(operand);
        if (!moveDef || moveDef->operation != LiftedOperation::MOVE)
            break;
        if (m_processedInstructions.contains(moveDef->instructionIndex))
            break;
        if (!forceExpression && !ShouldInline(moveDef))
            break;
        operand = moveDef->operands[1];
    }

    if (operand.type == LiftedOperandType::Register && m_pinnedRegisters.contains(operand.value.reg)) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }

    // Inline anonymous-closure substitution: the closure was parked here in
    // place of a top-level declaration because it has a single use that is
    // this argument slot. Erase so a misclassified second use cannot duplicate
    // the function literal (would also be semantically wrong).
    if (operand.type == LiftedOperandType::Register) {
        SSARef ref{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
        auto it = m_inlineableClosures.find(ref);
        if (it != m_inlineableClosures.end()) {
            auto closureNode = it->second;
            m_inlineableClosures.erase(it);
            return closureNode;
        }
    }

    if (operand.type == LiftedOperandType::ImmediateNil)
        return std::make_shared<NilLiteralNode>();
    if (operand.type == LiftedOperandType::ImmediateBool)
        return std::make_shared<BooleanLiteralNode>(operand.value.imm.b);
    if (operand.type == LiftedOperandType::ImmediateInteger)
        return std::make_shared<NumberLiteralNode>(operand.value.imm.n);
    if (operand.type == LiftedOperandType::ImmediateConstant) {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[operand.value.imm.k];
        switch (k.kType) {
        case LUA_TNIL:
            return std::make_shared<NilLiteralNode>();
        case LUA_TBOOLEAN:
            return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
        case LUA_TNUMBER:
            return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
        case LUA_TINTEGER:
            return std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
        case LUA_TSTRING:
            return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
        case LUA_TVECTOR: {
            const auto &[x, y, z, w] = std::get<LuauVector>(k.constantData);
            return std::make_shared<VectorNode>(x, y, z, w);
        }
        default:
            return std::make_shared<NilLiteralNode>();
        }
    }

    const auto *def = m_currentFunction->GetDefinition(operand);

    if (def && def->operation == LiftedOperation::GETVARARGS)
        return std::make_shared<VarArgExpression>();

    if (def && m_processedInstructions.contains(def->instructionIndex))
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));

    if (!def || (!forceExpression && !ShouldInline(def))) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }

    if (def->operation == LiftedOperation::LOAD) {
        if (def->operands[1].type == LiftedOperandType::ImmediateNil)
            return std::make_shared<NilLiteralNode>();
        if (def->operands[1].type == LiftedOperandType::ImmediateBool)
            return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
        if (def->operands[1].type == LiftedOperandType::ImmediateInteger)
            return std::make_shared<NumberLiteralNode>(def->operands[1].value.imm.n);
        if (def->operands[1].type == LiftedOperandType::ImmediateConstant) {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[1].value.imm.k];
            switch (k.kType) {
            case LUA_TNIL:
                return std::make_shared<NilLiteralNode>();
            case LUA_TBOOLEAN:
                return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
            case LUA_TNUMBER:
                return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
            case LUA_TINTEGER:
                return std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
            case LUA_TSTRING:
                return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
            case LUA_TVECTOR: {
                const auto &[x, y, z, w] = std::get<LuauVector>(k.constantData);
                return std::make_shared<VectorNode>(x, y, z, w);
            }
            default:
                return std::make_shared<NilLiteralNode>();
            }
        }
    }

    // Chained binops `a+b+c+d+...` are left-leaning ADD(ADD(ADD(...),c),d). Recursing left
    // gave one stack frame per link → blew the stack on obfuscated arithmetic. Walk the spine
    // iteratively, collecting (op, rightExpr) pairs, then fold up.
    auto opSymbol = [](LiftedOperation o) -> const char * {
        switch (o) {
        case LiftedOperation::ADD: case LiftedOperation::ADDK: return "+";
        case LiftedOperation::SUB: case LiftedOperation::SUBK: return "-";
        case LiftedOperation::MUL: case LiftedOperation::MULK: return "*";
        case LiftedOperation::DIV: case LiftedOperation::DIVK: return "/";
        case LiftedOperation::MOD: case LiftedOperation::MODK: return "%";
        case LiftedOperation::POW: case LiftedOperation::POWK: return "^";
        case LiftedOperation::AND: case LiftedOperation::ANDK: return "and";
        case LiftedOperation::OR:  case LiftedOperation::ORK:  return "or";
        default: return nullptr;
        }
    };
    auto isBinaryK = [](LiftedOperation o) {
        return o == LiftedOperation::ADDK || o == LiftedOperation::SUBK || o == LiftedOperation::MULK ||
               o == LiftedOperation::DIVK || o == LiftedOperation::MODK || o == LiftedOperation::POWK ||
               o == LiftedOperation::ANDK || o == LiftedOperation::ORK;
    };
    auto resolveKConstant = [&](int32_t kIdx) -> std::shared_ptr<Expression> {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[kIdx];
        switch (k.kType) {
        case LUA_TNIL: return std::make_shared<NilLiteralNode>();
        case LUA_TBOOLEAN: return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
        case LUA_TNUMBER: return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
        case LUA_TINTEGER: return std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
        case LUA_TSTRING: return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
        case LUA_TVECTOR: {
            const auto &[x, y, z, w] = std::get<LuauVector>(k.constantData);
            return std::make_shared<VectorNode>(x, y, z, w);
        }
        default: return std::make_shared<NilLiteralNode>();
        }
    };

    if (opSymbol(def->operation) != nullptr) {
        std::vector<std::pair<const char *, std::shared_ptr<Expression>>> rights;
        const LiftedInstruction *curDef = def;
        LiftedOperand leftLeafOperand{};
        bool leftLeafSet = false;
        while (true) {
            const char *sym = opSymbol(curDef->operation);
            std::shared_ptr<Expression> rightExpr;
            if (isBinaryK(curDef->operation))
                rightExpr = resolveKConstant(curDef->operands[2].value.imm.k);
            else
                rightExpr = LiftExpression(curDef->operands[2]);
            rights.emplace_back(sym, rightExpr);

            const auto &leftOp = curDef->operands[1];
            if (leftOp.type != LiftedOperandType::Register) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            if (m_pinnedRegisters.contains(leftOp.value.reg)) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            SSARef leftRef{static_cast<uint8_t>(leftOp.value.reg), leftOp.ssaVersion};
            if (m_inlineableClosures.contains(leftRef)) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            const auto *leftDef = m_currentFunction->GetDefinition(leftOp);
            if (!leftDef || m_processedInstructions.contains(leftDef->instructionIndex) || !ShouldInline(leftDef) ||
                opSymbol(leftDef->operation) == nullptr) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            curDef = leftDef;
        }
        std::shared_ptr<Expression> expr = leftLeafSet ? LiftExpression(leftLeafOperand) : std::make_shared<NilLiteralNode>();
        for (auto it = rights.rbegin(); it != rights.rend(); ++it)
            expr = std::make_shared<BinaryExpressionNode>(it->first, expr, it->second);
        return expr;
    }

    switch (def->operation) {

    case LiftedOperation::NOT:
        return std::make_shared<UnaryExpressionNode>("not " /* not is extra space. */, LiftExpression(def->operands[1]));
    case LiftedOperation::MINUS:
        return std::make_shared<UnaryExpressionNode>("-", LiftExpression(def->operands[1]));
    case LiftedOperation::LENGTH:
        return std::make_shared<UnaryExpressionNode>("#", LiftExpression(def->operands[1]));

    case LiftedOperation::MOVE:
        return LiftExpression(def->operands[1], forceExpression);
    case LiftedOperation::GETVARARGS:
        return std::make_shared<VarArgExpression>();

    case LiftedOperation::CONCAT: {
        int startReg = def->operands[1].value.reg;
        int endReg = def->operands[2].value.reg;
        std::vector<LiftedOperand> operands;

        bool implicitCoversAll = false;
        if (m_currentFunction->implicitUses.contains(def)) {
            const auto &vers = m_currentFunction->implicitUses.at(def);
            if (vers.size() == (size_t)(endReg - startReg + 1)) {
                implicitCoversAll = true;
                for (size_t i = 0; i < vers.size(); ++i) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = startReg + i;
                    op.ssaVersion = vers[i];
                    operands.push_back(op);
                }
            }
        }

        if (!implicitCoversAll) {
            operands.push_back(def->operands[1]);

            if (m_currentFunction->implicitUses.contains(def)) {
                const auto &vers = m_currentFunction->implicitUses.at(def);
                for (size_t i = 0; i < vers.size(); ++i) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = startReg + 1 + i;
                    op.ssaVersion = vers[i];
                    operands.push_back(op);
                }
            } else {
                for (int r = startReg + 1; r < endReg; ++r) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = r;
                    op.ssaVersion = -1;
                    operands.push_back(op);
                }
            }

            if (endReg > startReg) {
                operands.push_back(def->operands[2]);
            }
        }

        std::shared_ptr<Expression> expr = nullptr;
        for (const auto &op : operands) {
            auto part = LiftExpression(op);
            expr = expr ? std::make_shared<BinaryExpressionNode>("..", expr, part) : part;
        }
        return expr ? expr : std::make_shared<StringLiteralNode>("");
    }

    case LiftedOperation::DUPTABLE:
    case LiftedOperation::NEWTABLE:
        return LiftTableLiteral(*def);

    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
    case LiftedOperation::NAMECALL:
        return LiftCall(*def, def->instructionIndex, true);

    case LiftedOperation::GETGLOBAL: {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[1].value.imm.k];
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::get<std::string>(k.constantData)));
    }
    case LiftedOperation::GETUPVAL: {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->m_currentFunction->GetUpvalueName(def->operands[1].value.imm.n)));
    }
    case LiftedOperation::GETIMPORT: {
        uint32_t importData = def->operands[2].value.imm.u;
        int count = importData >> 30;
        int id0 = int(importData >> 20) & 1023;
        int id1 = int(importData >> 10) & 1023;
        int id2 = int(importData) & 1023;

        std::vector<std::string> parts;
        auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants;
        if (count >= 1)
            parts.push_back(std::get<std::string>(constants.at(id0).constantData));
        if (count >= 2)
            parts.push_back(std::get<std::string>(constants.at(id1).constantData));
        if (count >= 3)
            parts.push_back(std::get<std::string>(constants.at(id2).constantData));

        if (parts.empty())
            return std::make_shared<NilLiteralNode>();

        std::shared_ptr<Expression> curr = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(parts[0]));
        for (size_t i = 1; i < parts.size(); ++i)
            curr = std::make_shared<MemberExpressionNode>(curr, parts[i]);
        return curr;
    }
    case LiftedOperation::GETTABLE:
    case LiftedOperation::GETTABLEKS: {
        // Walk member chains `a.b.c.d.e.f` iteratively. Each GETTABLE(KS) takes operands[1]
        // as the base; recursing left stacks one frame per dotted hop.
        struct Hop {
            bool isKeyed;
            std::shared_ptr<Expression> indexExpr;
            std::string memberName;
        };
        std::vector<Hop> hops;
        const LiftedInstruction *curDef = def;
        LiftedOperand leftLeafOp{};
        bool leftLeafSet = false;
        while (true) {
            Hop hop{};
            if (curDef->operation == LiftedOperation::GETTABLEKS) {
                hop.isKeyed = false;
                const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[curDef->operands[2].value.imm.k];
                hop.memberName = std::get<std::string>(k.constantData);
            } else {
                hop.isKeyed = true;
                hop.indexExpr = LiftExpression(curDef->operands[2]);
            }
            hops.push_back(std::move(hop));

            const auto &leftOp = curDef->operands[1];
            if (leftOp.type != LiftedOperandType::Register) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            if (m_pinnedRegisters.contains(leftOp.value.reg)) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            SSARef lr{static_cast<uint8_t>(leftOp.value.reg), leftOp.ssaVersion};
            if (m_inlineableClosures.contains(lr)) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            const auto *leftDef = m_currentFunction->GetDefinition(leftOp);
            if (!leftDef || m_processedInstructions.contains(leftDef->instructionIndex) || !ShouldInline(leftDef) ||
                (leftDef->operation != LiftedOperation::GETTABLE && leftDef->operation != LiftedOperation::GETTABLEKS)) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            curDef = leftDef;
        }
        std::shared_ptr<Expression> expr = leftLeafSet ? LiftExpression(leftLeafOp) : std::make_shared<NilLiteralNode>();
        for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
            if (it->isKeyed)
                expr = std::make_shared<IndexExpressionNode>(expr, it->indexExpr);
            else
                expr = std::make_shared<MemberExpressionNode>(expr, it->memberName);
        }
        return expr;
    }

    default:
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }
}

std::shared_ptr<Expression> ASTLifter::LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested) {
    // Iterative tail-fold: a NAMECALL emitted right before a CALL on the same base reg
    // is the real call site. Walk back through any stacked pairs without recursing.
    const LiftedInstruction *curInst = &inst;
    int32_t curIdx = instructionIndex;
    while ((curInst->operation == LiftedOperation::CALL || curInst->operation == LiftedOperation::CALLFB) && curIdx >= 2) {
        const auto &prev = m_currentFunction->lpLiftedFunction->instructions[curIdx - 2];
        if (prev.operation != LiftedOperation::NAMECALL || prev.operands[0].value.reg != curInst->operands[0].value.reg)
            break;
        curInst = &prev;
        curIdx -= 2;
    }
    const LiftedInstruction &resolvedInst = *curInst;
    const int32_t resolvedIdx = curIdx;

    bool isNameCall = (resolvedInst.operation == LiftedOperation::NAMECALL);
    int32_t callInfoIndex = isNameCall ? resolvedIdx + 2 : resolvedIdx;

    if (static_cast<size_t>(callInfoIndex) >= m_currentFunction->lpLiftedFunction->instructions.size())
        return std::make_shared<NilLiteralNode>();

    const auto &callInfoInst = m_currentFunction->lpLiftedFunction->instructions[callInfoIndex];
    int regFunc = callInfoInst.operands[0].value.reg;

    std::vector<std::shared_ptr<Expression>> args;
    std::shared_ptr<Expression> callee;
    bool isVararg = false;

    if (isNameCall)
        callee = LiftExpression(resolvedInst.operands[1], false);
    else
        callee = LiftExpression(resolvedInst.operands[0], false);

    if (callee->nodeKind == ASTNodeKind::LiteralValue) {
        auto literal = std::dynamic_pointer_cast<LiteralNode>(callee);
        if (literal != nullptr)
            literal->bUseParenthesis = true;
    }

    if (m_currentFunction->implicitUses.contains(&callInfoInst)) {
        const auto &argVersions = m_currentFunction->implicitUses.at(&callInfoInst);
        int startOffset = isNameCall ? 1 : 0;
        for (size_t k = startOffset; k < argVersions.size(); ++k) {
            LiftedOperand op;
            op.type = LiftedOperandType::Register;
            op.value.reg = regFunc + 1 + k;
            op.ssaVersion = argVersions[k];

            auto def = m_currentFunction->GetDefinition(op);
            if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::NAMECALL)) {
                int32_t actualCallIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                const auto &defCallInfo = m_currentFunction->lpLiftedFunction->instructions[actualCallIdx];
                if (defCallInfo.operands[2].value.imm.n == 0) {
                    m_processedInstructions.insert(def->instructionIndex);
                    if (def->operation == LiftedOperation::NAMECALL) {
                        m_processedInstructions.insert(def->instructionIndex + 1);
                        m_processedInstructions.insert(def->instructionIndex + 2);
                    }
                    args.push_back(LiftCall(*def, def->instructionIndex, true));
                    break;
                }
            }
            if (def && def->operation == LiftedOperation::GETVARARGS) {
                isVararg = true;
                args.push_back(std::make_shared<VarArgExpression>()); // var arg may be present in the middle of arguments, unfunny.
                continue;
            }
            args.push_back(LiftExpression(op, false));
        }
    }

    int32_t prevIdx = resolvedIdx - 1;
    if (prevIdx >= 0) {
        const auto &prevInst = m_currentFunction->lpLiftedFunction->instructions[prevIdx];
        if (prevInst.operation == LiftedOperation::CALL && prevInst.operands[2].value.imm.n == 0) {
            if (!m_processedInstructions.contains(prevIdx)) {
                m_processedInstructions.insert(prevIdx);
                args.push_back(LiftCall(prevInst, prevIdx, true));
            }
        }
    }

    std::vector<std::shared_ptr<Expression>> rets;
    if (isNameCall) {
        auto kIdx = resolvedInst.operands[2].value.imm.k;
        std::string method = std::get<std::string>(m_currentFunction->lpLiftedFunction->lpDeserialized->constants.at(kIdx).constantData);

        return std::make_shared<NameCallExpressionNode>(
            callee, std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(method)), args, rets, isVararg, isNested
        );
    } else {
        return std::make_shared<CallExpressionNode>(callee, args, rets, isVararg, isNested);
    }
}

static bool IsLegalLuauIdentifier(const std::string &str) {
    if (str.empty() || std::isdigit(static_cast<unsigned char>(str.front())))
        return false;
    return std::ranges::all_of(str, [](char c) {
        auto uc = static_cast<unsigned char>(c);
        return std::isalnum(uc) || uc == '_';
    });
}

static std::shared_ptr<Expression> MakeTableKey(const std::string &keyStr) {
    if (IsLegalLuauIdentifier(keyStr))
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(keyStr));
    return std::make_shared<StringLiteralNode>(keyStr);
}

std::shared_ptr<TableLiteralNode> ASTLifter::LiftTableLiteral(const LiftedInstruction &inst) {
    std::vector<std::shared_ptr<Expression>> elements;
    std::vector<int32_t> candidatesIndexes;
    int32_t tableReg = inst.operands[0].value.reg;
    // The SSA version of *this* table instance. A later store that writes the same
    // register but a different version belongs to a different table (the register
    // was reused) and must not be folded into this constructor.
    int32_t tableVersion = inst.operands[0].ssaVersion;
    size_t scanLimit = 100;
    size_t maxIdx = m_currentFunction->lpLiftedFunction->instructions.size();
    bool bFoundSetList = false;

    if (inst.operation == LiftedOperation::DUPTABLE) {
        int constantIdx = inst.operands[1].value.imm.k;
        const auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants;
        if (constantIdx < 0 || static_cast<size_t>(constantIdx) >= constants.size())
            return std::make_shared<TableLiteralNode>();

        const auto &constant = constants[constantIdx];
        if (constant.kType == LUA_TTABLE) {
            const auto &tableData = constant.GetValue<LuauTable>();
            for (size_t i = 0; i < tableData.keys.size() && i < tableData.valueConstantIndices.size(); i++) {
                auto keyExpr = MakeTableKey(tableData.keys[i]);
                std::shared_ptr<Expression> valExpr = nullptr;
                const auto valIdx = tableData.valueConstantIndices[i];
                if (valIdx < 0 || static_cast<size_t>(valIdx) >= constants.size()) {
                    elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, std::make_shared<NilLiteralNode>()));
                    continue;
                }

                const auto &valConst = constants[valIdx];
                switch (valConst.kType) {
                case LUA_TNIL:
                    valExpr = std::make_shared<NilLiteralNode>();
                    break;
                case LUA_TBOOLEAN:
                    valExpr = std::make_shared<BooleanLiteralNode>(std::get<bool>(valConst.constantData));
                    break;
                case LUA_TNUMBER:
                    valExpr = std::make_shared<NumberLiteralNode>(std::get<double>(valConst.constantData));
                    break;
                case LUA_TINTEGER:
                    valExpr = std::make_shared<IntegerLiteralNode>(std::get<int64_t>(valConst.constantData));
                    break;
                case LUA_TSTRING:
                    valExpr = std::make_shared<StringLiteralNode>(std::get<std::string>(valConst.constantData));
                    break;
                case LUA_TVECTOR: {
                    const auto &[x, y, z, w] = std::get<LuauVector>(valConst.constantData);
                    valExpr = std::make_shared<VectorNode>(x, y, z, w);
                    break;
                }
                default:
                    valExpr = std::make_shared<NilLiteralNode>();
                    break;
                }
                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
            }
        }
        return std::make_shared<TableLiteralNode>(elements);
    }

    // A field value can only live inside the `{ ... }` constructor if it is an
    // expression we can inline at the field site. Values that are emitted as their
    // own statement (closures, non-trivial calls, anything `ShouldInline` rejects)
    // would otherwise be referenced by a bare, often-reused register name (e.g.
    // `getLocalPlayer = v5`) which is both wrong and not sound. When we hit one,
    // abort coalescing entirely: the table stays `{}` and every assignment is
    // emitted as a separate, locally-sound `t.field = value` statement.
    auto valueFoldable = [&](const LiftedOperand &valOp) -> bool {
        if (valOp.type != LiftedOperandType::Register)
            return true; // immediates / constants inline trivially
        const auto *def = m_currentFunction->GetDefinition(valOp);
        if (!def)
            return true; // parameter / plain identifier reference
        return ShouldInline(def);
    };
    bool bAbortCoalesce = false;

    for (size_t i = inst.instructionIndex + 1; i < inst.instructionIndex + scanLimit && i < maxIdx; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];

        if (candidate.operation == LiftedOperation::JUMP || candidate.operation == LiftedOperation::JUMPIF ||
            candidate.operation == LiftedOperation::JUMPIFNOT || candidate.operation == LiftedOperation::RETURN ||
            candidate.operation == LiftedOperation::BREAK || candidate.operation == LiftedOperation::FORNPREP ||
            candidate.operation == LiftedOperation::FORNLOOP || candidate.operation == LiftedOperation::FORGLOOP ||
            candidate.operation == LiftedOperation::FORGPREP || candidate.operation == LiftedOperation::FORGPREP_INEXT ||
            candidate.operation == LiftedOperation::FORGPREP_NEXT)
            break;

        if (candidate.operation == LiftedOperation::SETLIST) {
            if (candidate.operands[0].value.reg == tableReg && candidate.operands[0].ssaVersion != tableVersion)
                break; // register reused for a different table — stop here.
            if (candidate.operands[0].value.reg == tableReg) {
                if (m_currentFunction->implicitUses.contains(&candidate)) {
                    const auto &versions = m_currentFunction->implicitUses.at(&candidate);
                    int startReg = candidate.operands[1].value.reg;

                    for (size_t k = 0; k < versions.size(); k++) {
                        LiftedOperand itemOp;
                        itemOp.type = LiftedOperandType::Register;
                        itemOp.value.reg = startReg + k;
                        itemOp.ssaVersion = versions[k];
                        std::shared_ptr<Expression> expr = nullptr;
                        if (auto lpDef = m_currentFunction->GetDefinition(itemOp)) {
                            if (lpDef->operation == LiftedOperation::LOAD) {
                                expr = LiftExpression(itemOp, true);
                                this->m_processedInstructions.insert(lpDef->instructionIndex); // mark as processed.
                            } else if (lpDef->operation == LiftedOperation::MOVE) {
                                expr = LiftExpression(lpDef->operands[1], false);
                                this->m_processedInstructions.insert(lpDef->instructionIndex);
                            } else {
                                expr = LiftExpression(itemOp, false);
                            }
                        }

                        elements.push_back(expr);
                    }
                }
                bFoundSetList = true;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEKS) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (!valueFoldable(candidate.operands[0])) {
                    bAbortCoalesce = true;
                    break;
                }
                auto kIdx = candidate.operands[2].value.imm.k;
                const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[kIdx];
                std::string keyStr = std::get<std::string>(k.constantData);

                auto keyExpr = MakeTableKey(keyStr);
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEN) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (!valueFoldable(candidate.operands[0])) {
                    bAbortCoalesce = true;
                    break;
                }
                int idx = candidate.operands[2].value.imm.n + 1;

                auto keyExpr = std::make_shared<MemberExpressionNode>(std::make_shared<NumberLiteralNode>(idx));
                auto valExpr = LiftExpression(candidate.operands[0]);

                // SETTABLEN opcodes are emitted sometimes where
                // local v = 2
                // t[v] = n
                // this emits as
                // SETTABLEN TABLEREG, VALUEREG, INDEX (i dont care for the ordering, cope reader).
                // because of this, we have to imply that the keyExpr will be a literal ['']

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLE) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (!valueFoldable(candidate.operands[0])) {
                    bAbortCoalesce = true;
                    break;
                }
                auto keyExpr = LiftExpression(candidate.operands[2], true);
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<TableBinaryExpressionNode>("=", keyExpr, valExpr));
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        }
    }

    // A non-inlinable field value was found: discard the partial constructor and
    // emit nothing-consumed so the assignments lift as their own statements.
    if (bAbortCoalesce)
        return std::make_shared<TableLiteralNode>();

    if (bFoundSetList || !elements.empty())
        // the instructions are processed, since they're inlined.
        for (const auto &ins : candidatesIndexes)
            m_processedInstructions.insert(ins);

    if (!elements.empty())
        return std::make_shared<TableLiteralNode>(elements);
    else
        return std::make_shared<TableLiteralNode>(); // the list is likely instantiated and then added to. This can cause some malformations in some cases,
                                                     // reject inlining.
}

bool ASTLifter::ShouldInline(const LiftedInstruction *inst) {
    if (!inst || inst->operands.size() < 1)
        return false;

    // A register captured by a closure (VAL or REF) must remain a real local: the
    // closure's upvalue is aliased to this variable's name (no `local uv = source`
    // copy is emitted), so inlining its def into a single use would erase the
    // declaration the upvalue binds to — and for REF would desync writes. (LCT_UPVAL
    // captures a parent upvalue, not a local def here, so it is unaffected.)
    if (inst->operands[0].type == LiftedOperandType::Register) {
        const SSARef defRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        if (auto it = m_currentFunction->users.find(defRef); it != m_currentFunction->users.end())
            for (const auto *user : it->second)
                if (user && user->operation == LiftedOperation::CAPTURE && user->operands.size() >= 1 && user->operands[0].value.imm.n <= 1)
                    return false;
    }

    // these statements have side-effects which cannot be skipped.
    // the values they produce aren't inlineable in any way, doing so would break them anyway!
    switch (inst->operation) {
    case LiftedOperation::RETURN:
    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
        return false;
    default:
        break;
    }

    if (inst->operands.size() > 0 && inst->operands[0].type == LiftedOperandType::Register &&
        (inst->operation != LiftedOperation::MOVE && m_currentFunction->IsSingleUse(inst->operands[0]))) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_phiConsumers.contains(defRef))
            return false;
    }

    if (inst->operation == LiftedOperation::NEWTABLE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_currentFunction->useCounts.contains(defRef)) {
            const auto &users = m_currentFunction->users[defRef];
            int realUses = 0;
            for (const auto *user : users) {
                if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                     user->operation == LiftedOperation::SETTABLEN) &&
                    user->operands[1].value.reg == inst->operands[0].value.reg)
                    return false;

                if (user->operation == LiftedOperation::SETLIST && user->operands[0].value.reg == inst->operands[0].value.reg)
                    continue;

                if (user->operation == LiftedOperation::MOVE)
                    // MOVE instructions points to the table being reused. It cannot be inlined because of this.
                    return false;
                realUses++;
            }
            // there is exactly one real user of the register, the rest are product of the syntactic sugar, inline it.
            return realUses == 1;
        }
    }

    if (inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB || inst->operation == LiftedOperation::NAMECALL) {
        if (inst->operation == LiftedOperation::CALL && inst->operands[2].value.imm.n == 0)
            return true;

        if (inst->operation == LiftedOperation::NAMECALL) {
            int regA = inst->operands[0].value.reg;

            if ((inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands[2].value.imm.n == 0)
                return true;

            for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
                if (defInst == inst && ref.regIndex == regA) {
                    auto users = m_currentFunction->users[{static_cast<uint8_t>(regA), ref.version}];
                    if (users.size() == 1) {
                        auto op = users[0]->operation;
                        // allow inlining returns, other Calls, and arith ops.
                        if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::NAMECALL || op == LiftedOperation::ADD ||
                            op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV || op == LiftedOperation::MOD ||
                            op == LiftedOperation::POW || op == LiftedOperation::CONCAT || op == LiftedOperation::MINUS || op == LiftedOperation::NOT ||
                            op == LiftedOperation::LENGTH) {
                            return true;
                        }
                    }
                    return false;
                }
            }

            return false;
        }

        int usedDefs = 0;
        SSARef usedRef;

        for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
            if (defInst == inst) {
                if (m_currentFunction->useCounts[ref] > 0) {
                    usedDefs++;
                    usedRef = ref;
                }
            }
        }

        if (usedDefs > 1)
            return false;
        if (usedDefs == 0)
            return false;

        auto users = m_currentFunction->users[usedRef];
        if (users.size() == 1) {
            auto op = users[0]->operation;
            if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::NAMECALL || op == LiftedOperation::ADD ||
                op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV || op == LiftedOperation::MOD ||
                op == LiftedOperation::POW || op == LiftedOperation::CONCAT || op == LiftedOperation::MINUS || op == LiftedOperation::NOT ||
                op == LiftedOperation::LENGTH || op == LiftedOperation::JUMPIFEQ || op == LiftedOperation::JUMPIFNOTEQ || op == LiftedOperation::JUMPIFLT ||
                op == LiftedOperation::JUMPIFNOTLT || op == LiftedOperation::JUMPIFLE || op == LiftedOperation::JUMPIFNOTLE || op == LiftedOperation::JUMPIF ||
                op == LiftedOperation::JUMPIFNOT || op == LiftedOperation::JUMPXEQK) {
                return true;
            }
        }
        return false;
    }

    if (inst->operation == LiftedOperation::MOVE)
        return m_currentFunction->IsSingleUse(inst->operands[0]) && !m_currentFunction->IsConsumedByPhi(inst->operands[0]);

    if (m_currentFunction->IsSimpleOrConstant(inst->operands[0]) && !m_currentFunction->IsConsumedByPhi(inst->operands[0]) &&
        inst->operands[0].ssaVersion != 1 /* first version cannot be inlined. It is a declaration */)
        return true;

    // A pure constant `LOAD` with no explicit users is a call argument (those are
    // tracked via implicitUses, not useCounts) or simply dead. Either way it is
    // safe to inline at its single use site: the value has no side effects and
    // there is no explicit reader whose declaration we would be removing. This
    // lets `local v = "X"; f(v)` collapse to `f("X")` even at the first version.
    // A pure constant `LOAD` is safe to inline (and duplicate) at every use: it has
    // no side effects. The only hazard is removing the *declaration* of a register
    // that is later reassigned, so restrict this to registers with a single SSA
    // version (never redefined). This collapses `local v = "X"; f(v)` to `f("X")`
    // even when the constant is a call argument (those are tracked via implicitUses
    // and so escape the regular use-count, leaving the value stuck as a local).
    if (inst->operation == LiftedOperation::LOAD && inst->operands.size() >= 2 && inst->operands[0].type == LiftedOperandType::Register &&
        !m_currentFunction->IsConsumedByPhi(inst->operands[0])) {
        const auto vt = inst->operands[1].type;
        const bool isPureConst = vt == LiftedOperandType::ImmediateConstant || vt == LiftedOperandType::ImmediateInteger ||
                                 vt == LiftedOperandType::ImmediateBool || vt == LiftedOperandType::ImmediateNil;
        if (isPureConst) {
            const uint8_t reg = inst->operands[0].value.reg;
            const SSARef ref{reg, inst->operands[0].ssaVersion};
            // Unsafe only if a reader of this value also writes the same register
            // (a self-reassignment such as `x = x + 1`): inlining would delete the
            // declaration the reassignment depends on. Otherwise the constant is
            // free to inline at each use.
            bool selfReassigned = false;
            if (auto it = m_currentFunction->users.find(ref); it != m_currentFunction->users.end()) {
                for (const auto *user : it->second) {
                    if (!user->operands.empty() && user->operands[0].type == LiftedOperandType::Register && user->operands[0].value.reg == reg) {
                        selfReassigned = true;
                        break;
                    }
                }
            }
            if (!selfReassigned)
                return true;
        }
    }

    if (m_currentFunction->IsSingleUse(inst->operands[0])) {
        if (m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;
        if (inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE)
            return false; // do not omit NEWCLOSURE and DUPCLOSURE.
        return true;
    }

    return false;
}

std::string ASTLifter::ResolveVariableName(const LiftedOperand &op) {
    if (op.type != LiftedOperandType::Register)
        return "err_not_reg";

    m_definedRegisters.insert(op.value.reg);
    std::string name = m_currentFunction->GetVarName(op.value.reg, op.ssaVersion);
    if (name.empty())
        return std::format("v{}", op.value.reg);

    return name;
}

std::optional<ASTLifter::BoolMaterialization> ASTLifter::DetectBooleanMaterialization(uint32_t headerId) {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (headerId >= blocks.size())
        return std::nullopt;
    const auto &H = blocks[headerId];
    if (!H.ifStatementTrue.has_value() || !H.ifStatementFalse.has_value() || !H.lpTail)
        return std::nullopt;

    const uint32_t tIdx = *H.ifStatementTrue;  // jump-taken target
    const uint32_t fIdx = *H.ifStatementFalse; // fall-through
    if (tIdx >= blocks.size() || fIdx >= blocks.size())
        return std::nullopt;

    // First non-NOP instruction of a block, and how many non-NOP instructions it has.
    auto firstReal = [&](const BasicBlock &blk) -> const LiftedInstruction * {
        if (!blk.lpHead || !blk.lpTail)
            return nullptr;
        for (int i = blk.lpHead->instructionIndex; i <= blk.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            if (ins.operation != LiftedOperation::NOP)
                return &ins;
        }
        return nullptr;
    };
    auto countReal = [&](const BasicBlock &blk) -> int {
        if (!blk.lpHead || !blk.lpTail)
            return 0;
        int c = 0;
        for (int i = blk.lpHead->instructionIndex; i <= blk.lpTail->instructionIndex; ++i)
            if (m_currentFunction->lpLiftedFunction->instructions[i].operation != LiftedOperation::NOP)
                ++c;
        return c;
    };
    auto isBoolLoad = [](const LiftedInstruction *ins) {
        return ins && ins->operands.size() >= 2 && ins->operands[0].type == LiftedOperandType::Register &&
               ins->operands[1].type == LiftedOperandType::ImmediateBool;
    };

    // Fall-through block F: exactly one real instruction, a `LOADB Rd,bF` that
    // jumps straight into T, and reached only from the header.
    const auto &F = blocks[fIdx];
    if (countReal(F) != 1)
        return std::nullopt;
    const LiftedInstruction *fLoad = firstReal(F);
    if (!fLoad || fLoad->operation != LiftedOperation::LOADNJUMP || !isBoolLoad(fLoad))
        return std::nullopt;
    if (F.successors.size() != 1 || F.successors[0] != tIdx)
        return std::nullopt;
    if (F.predecessors.size() != 1 || F.predecessors[0] != headerId)
        return std::nullopt;

    // Jump target T: starts with `LOADB Rd,bT` for the same register.
    const auto &T = blocks[tIdx];
    const LiftedInstruction *tLoad = firstReal(T);
    if (!tLoad || tLoad->operation != LiftedOperation::LOAD || !isBoolLoad(tLoad))
        return std::nullopt;

    const uint8_t reg = fLoad->operands[0].value.reg;
    if (tLoad->operands[0].value.reg != reg)
        return std::nullopt;
    const bool bF = fLoad->operands[1].value.imm.b;
    const bool bT = tLoad->operands[1].value.imm.b;
    if (bF == bT)
        return std::nullopt; // not a true/false split — leave it alone.

    // The merge must be entered only from the header and the false-load block, so
    // that `reg` is provably the diamond's boolean and nothing else.
    if (std::set<uint32_t>(T.predecessors.begin(), T.predecessors.end()) != std::set<uint32_t>{headerId, fIdx})
        return std::nullopt;

    // Only collapse genuine condition jumps; LiftCondition yields BooleanLiteral
    // for opcodes it cannot turn into a comparison/truth test.
    auto cond = LiftCondition(H.lpTail);
    if (!cond || std::dynamic_pointer_cast<BooleanLiteralNode>(cond))
        return std::nullopt;

    // Control reaches T directly when the jump is taken (cond true → bT) and via
    // F when it is not (cond false → bF). So reg == cond when bT is true, else !cond.
    std::shared_ptr<Expression> value = (bT && !bF) ? cond : InvertCondition(cond);

    const LiftedOperand target = tLoad->operands[0];
    const bool isDefined = m_definedRegisters.contains(reg);
    const bool isParameter = reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams;
    auto ident = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(target)));

    std::shared_ptr<Statement> assignment;
    if ((target.ssaVersion <= 1 && !isParameter) || !isDefined)
        assignment = std::make_shared<VariableDeclarationNode>(ident, value);
    else
        assignment = std::make_shared<AssignmentStatementNode>(ident, value);
    m_definedRegisters.insert(reg);

    // Consume both boolean loads so block lifting does not re-emit them.
    m_processedInstructions.insert(fLoad->instructionIndex);
    m_processedInstructions.insert(tLoad->instructionIndex);

    return BoolMaterialization{assignment, tIdx};
}

void ASTLifter::HoistPhiLocals(int32_t mergeIdx, const std::shared_ptr<IfStatementNode> &ifStmt,
                               std::vector<std::shared_ptr<Statement>> &nodes, const std::unordered_set<int32_t> &definedBeforeBranches) {
    if (mergeIdx < 0 || mergeIdx >= static_cast<int32_t>(m_currentFunction->basicBlocks.size()))
        return;

    const auto &mergeBlock = m_currentFunction->basicBlocks[mergeIdx];
    if (mergeBlock.phiNodes.empty())
        return;

    // Top-level statements of a branch body; null-safe.
    auto branchBody = [](const std::shared_ptr<BlockStatementNode> &branch) -> std::vector<std::shared_ptr<Statement>> * {
        return branch ? &branch->body : nullptr;
    };

    std::vector<std::vector<std::shared_ptr<Statement>> *> branches;
    if (auto *b = branchBody(ifStmt->thenBranch))
        branches.push_back(b);
    if (auto *b = branchBody(ifStmt->elseBranch))
        branches.push_back(b);

    for (const auto &phi : mergeBlock.phiNodes) {
        if (phi.operands.empty() || phi.operands[0].type != LiftedOperandType::Register)
            continue;

        int32_t reg = phi.operands[0].value.reg;
        std::string name = m_currentFunction->GetVarName(reg, phi.operands[0].ssaVersion);
        if (name.empty())
            name = std::format("v{}", reg);

        // Find every initialized `local <name> = ...` at branch top-level. If a
        // branch carries one, the value escapes the branch and must be hoisted.
        bool needsHoist = false;
        for (auto *body : branches) {
            for (auto &stmt : *body) {
                // Case 1: `local <name> = <expr>` (VariableDeclarationNode). Convert to a
                // bare assignment so the hoisted pre-declaration is the sole `local`.
                if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt); decl && decl->value != nullptr) {
                    auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
                    if (!ident || !ident->identifier || ident->identifier->name != name)
                        continue;

                    stmt = std::make_shared<AssignmentStatementNode>(decl->identifier, decl->value);
                    needsHoist = true;
                    continue;
                }

                // Case 2: `local <name> = <call>` — a Call/NameCall whose single return target
                // is the merged register. These keep their node (the call must stay in place);
                // we only suppress the `local` keyword so the hoisted declaration owns it.
                if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
                    auto retMatches = [&](const std::vector<std::shared_ptr<Expression>> &rets) -> bool {
                        if (rets.size() != 1)
                            return false;
                        auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(rets[0]);
                        return ident && ident->identifier && ident->identifier->name == name;
                    };
                    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression);
                        nameCall && retMatches(nameCall->rets)) {
                        nameCall->bIsLocalDeclaration = false;
                        needsHoist = true;
                    } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression);
                               call && retMatches(call->rets)) {
                        call->bIsLocalDeclaration = false;
                        needsHoist = true;
                    }
                }
            }
        }

        // Only declare `local <name>` when the register had no declaration BEFORE
        // this `if`. If it was already defined in an enclosing scope (e.g.
        // `local v = X; if v then <reassign v> end`), a fresh `local` would shadow
        // it and the post-merge read would see the stale outer value. We test the
        // pre-branch snapshot, not m_definedRegisters, because lifting the branch
        // assignments already inserted `reg`.
        if (needsHoist && !definedBeforeBranches.contains(reg)) {
            nodes.push_back(std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
            m_definedRegisters.insert(reg);
        }
    }
}

int32_t ASTLifter::FindMergeBlock(uint32_t branchA, uint32_t branchB) {
    if (branchA == branchB)
        return static_cast<int32_t>(branchA);

    if (this->m_currentFunction->basicBlocks.at(branchA).bType == BlockType::Return ||
        this->m_currentFunction->basicBlocks.at(branchB).bType == BlockType::Return)
        return -1; // no merge block when one of them is a return.

    std::set<uint32_t> reachableFromA;
    std::queue<uint32_t> q;

    q.push(branchA);
    reachableFromA.insert(branchA);
    while (!q.empty()) {
        uint32_t curr = q.front();
        q.pop();

        if (reachableFromA.size() > 2000u)
            break;

        const auto &block = m_currentFunction->basicBlocks[curr];
        for (uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (!reachableFromA.contains(succ)) {
                reachableFromA.insert(succ);
                q.push(succ);
            }
        }
    }

    if (reachableFromA.contains(branchB))
        return static_cast<int32_t>(branchB);

    std::queue<uint32_t> q2;
    q2.push(branchB);
    std::set<uint32_t> visitedB;
    visitedB.insert(branchB);

    while (!q2.empty()) {
        const uint32_t curr = q2.front();
        q2.pop();

        for (const auto &block = m_currentFunction->basicBlocks[curr]; uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (reachableFromA.contains(succ))
                return static_cast<int32_t>(succ);

            if (!visitedB.contains(succ)) {
                visitedB.insert(succ);
                q2.push(succ);
            }
        }
    }

    return -1;
}

// Only comparison conditionals (`==`, `~=`, `<`, `<=`, JUMPXEQK) anchor an
// OR-dispatch. Raw truthiness jumps (JUMPIF/JUMPIFNOT) are how Luau lowers
// value short-circuits (`x = a and b or c`); those are folded back into a single
// expression by TryRewriteShortCircuitAssignment / the LOADB-diamond handler, so
// coalescing them here would corrupt the expression.
static bool IsComparisonConditional(LiftedOperation op) {
    switch (op) {
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPXEQK:
        return true;
    default:
        return false;
    }
}

std::optional<ASTLifter::OrChainInfo> ASTLifter::DetectOrChain(uint32_t headerId) {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (headerId >= blocks.size())
        return std::nullopt;
    const auto &head = blocks[headerId];
    if (head.bType != BlockType::IfHeader || !head.ifStatementTrue.has_value() || !head.ifStatementFalse.has_value())
        return std::nullopt;

    // The shared OR target is the head's jump-to-true edge. Every link of the
    // chain must reach this same block.
    const uint32_t body = head.ifStatementTrue.value();

    const auto isLink = [&](uint32_t id) {
        if (id >= blocks.size())
            return false;
        const auto &b = blocks[id];
        return b.bType == BlockType::IfHeader && b.ifStatementTrue.has_value() && b.ifStatementFalse.has_value() &&
               (b.ifStatementTrue.value() == body || b.ifStatementFalse.value() == body);
    };

    // Structural walk first (no condition lifting): follow the run while each link
    // shares `body`. A link whose TRUE edge hits body continues through its FALSE
    // edge; the run must terminate on an inverted final link whose FALSE edge hits
    // body (Luau's lowering of the last OR term). Terminating any other way is the
    // AND-chain shape (uniform true-edge sharing) — reject it.
    struct Link {
        uint32_t blockId;
        bool invert;
    };
    std::vector<Link> links;
    std::set<uint32_t> guard;
    uint32_t cur = headerId;
    uint32_t elseIdx = InvalidBlockId;
    bool invertedFinal = false;

    while (cur < blocks.size() && !guard.contains(cur)) {
        const auto &b = blocks[cur];
        if (b.bType != BlockType::IfHeader || !b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value())
            break;
        if (!b.lpTail || !IsComparisonConditional(b.lpTail->operation))
            break; // truthiness link → leave to the short-circuit expression folders

        const uint32_t bt = b.ifStatementTrue.value();
        const uint32_t bf = b.ifStatementFalse.value();

        if (bt == body) {
            links.push_back({cur, false});
            guard.insert(cur);
            if (isLink(bf) && !guard.contains(bf)) {
                cur = bf; // fall-through is the next link
                continue;
            }
            elseIdx = bf; // ran out without an inverted terminator → not an OR-chain
            invertedFinal = false;
            break;
        }
        if (bf == body) {
            links.push_back({cur, true}); // inverted final term: false edge reaches body
            guard.insert(cur);
            elseIdx = bt;
            invertedFinal = true;
            break;
        }
        break; // does not share the body → end of (non-)chain
    }

    if (!invertedFinal || links.size() < 2 || elseIdx == InvalidBlockId)
        return std::nullopt;

    // Structure confirmed. Lift each link's condition (the condition under which it
    // reaches `body`) and OR-fold left to right.
    std::shared_ptr<Expression> condition;
    for (const auto &lk : links) {
        auto c = LiftCondition(blocks[lk.blockId].lpTail);
        if (lk.invert)
            c = InvertCondition(c);
        condition = condition ? std::static_pointer_cast<Expression>(std::make_shared<BinaryExpressionNode>("or", condition, c)) : c;
    }

    OrChainInfo info;
    info.condition = condition;
    info.bodyIdx = body;
    info.elseIdx = elseIdx;
    info.chainBlocks.reserve(links.size());
    for (const auto &lk : links)
        info.chainBlocks.push_back(lk.blockId);
    return info;
}
