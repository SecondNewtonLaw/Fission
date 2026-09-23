//
// Created by Dottik on 10/12/2025.
//

#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "SSABuilder.hpp"
#include "SafetyGuard.hpp"

// Luau's authoritative builtin metadata (Compiler/src/Builtins.h, an internal header we don't want to
// drag in). We need only the declared result count of a fast builtin to tell a multi-return builtin from
// a single-return one without hardcoding an allowlist. The struct's data layout mirrors Luau's exactly;
// the symbol resolves against the linked Luau.Compiler library (see IsMultretCall).
namespace Luau::Compile {
    struct BuiltinInfo {
        int params;
        int results;
        unsigned int flags;
    };
    BuiltinInfo getBuiltinInfo(int bfid);
} // namespace Luau::Compile

const LuauConstant &ASTLifter::ConstantAt(long idx) const {
    const auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants;
    if (idx < 0 || static_cast<size_t>(idx) >= constants.size())
        throw Fission::DecompilerError("malformed bytecode: constant index outside the constant pool");
    return constants[idx];
}

#include <algorithm>
#include <coroutine>
#include <exception>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Debug-only: keep this file unoptimized so breakpoints/stepping work on the lifter. In release
// (NDEBUG) it must stay optimized; leaving it off there roughly halves decompile throughput on large
// inputs for byte-identical output.
#ifndef NDEBUG
#if defined(__clang__)
#pragma clang optimize off
#endif
#endif

constexpr uint32_t InvalidBlockId = static_cast<uint32_t>(-1);

// Forward declaration for the chain-fold pass used by CreateBlock below.
static void FoldShortCircuitChain(std::vector<std::shared_ptr<Statement>> &stmts);
static bool IsSelfAssign(const std::shared_ptr<Statement> &stmt);

static std::shared_ptr<Identifier> GlobalIdentifier(std::string name) {
    auto identifier = std::make_shared<Identifier>(std::move(name));
    identifier->bIsGlobal = true;
    return identifier;
}

static std::shared_ptr<VectorNode> LiftVectorConstant(const LuauConstant &constant) {
    const auto &vector = std::get<LuauVectorConstant>(constant.constantData);
    return std::visit([](const auto &components) { return std::make_shared<VectorNode>(components); }, vector.components);
}

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

class ControlFlowTask {
  public:
    using Result = std::vector<std::shared_ptr<Statement>>;

    struct promise_type;
    using Handle = std::coroutine_handle<promise_type>;

    struct FinalAwaiter;

    struct promise_type {
        Handle continuation{};
        Handle *next = nullptr;
        std::optional<Result> result;
        std::exception_ptr error;

        ControlFlowTask get_return_object() noexcept { return ControlFlowTask{Handle::from_promise(*this)}; }
        std::suspend_always initial_suspend() const noexcept { return {}; }
        FinalAwaiter final_suspend() const noexcept;
        void return_value(Result value) { result = std::move(value); }
        void unhandled_exception() noexcept { error = std::current_exception(); }
    };

    struct FinalAwaiter {
        bool await_ready() const noexcept { return false; }
        void await_suspend(Handle handle) const noexcept {
            *handle.promise().next = handle.promise().continuation;
        }
        void await_resume() const noexcept {}
    };

    explicit ControlFlowTask(Handle handle) : m_handle(handle) {}
    ControlFlowTask(const ControlFlowTask &) = delete;
    ControlFlowTask &operator=(const ControlFlowTask &) = delete;
    ControlFlowTask(ControlFlowTask &&other) noexcept : m_handle(std::exchange(other.m_handle, {})) {}
    ControlFlowTask &operator=(ControlFlowTask &&other) noexcept {
        if (this != &other) {
            if (m_handle)
                m_handle.destroy();
            m_handle = std::exchange(other.m_handle, {});
        }
        return *this;
    }
    ~ControlFlowTask() {
        if (m_handle)
            m_handle.destroy();
    }

    Result Run() && {
        auto handle = std::exchange(m_handle, {});
        auto next = handle;
        handle.promise().next = &next;
        while (next)
            next.resume();
        if (handle.promise().error) {
            auto error = handle.promise().error;
            handle.destroy();
            std::rethrow_exception(error);
        }
        auto result = std::move(*handle.promise().result);
        handle.destroy();
        return result;
    }

    struct Awaiter {
        Handle handle;

        ~Awaiter() {
            if (handle)
                handle.destroy();
        }

        bool await_ready() const noexcept { return !handle || handle.done(); }
        void await_suspend(Handle continuation) noexcept {
            handle.promise().continuation = continuation;
            handle.promise().next = continuation.promise().next;
            *handle.promise().next = handle;
        }
        Result await_resume() {
            if (handle.promise().error)
                std::rethrow_exception(handle.promise().error);
            return std::move(*handle.promise().result);
        }
    };

    Awaiter operator co_await() && noexcept { return {std::exchange(m_handle, {})}; }

  private:
    Handle m_handle;
};

inline ControlFlowTask::FinalAwaiter ControlFlowTask::promise_type::final_suspend() const noexcept { return {}; }

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

// single-use value whose one user is a NEWCLASSMEMBER consuming it as the member value (operand[1]).
// gates inlining a class method's closure straight into the class body (`function m(self) ... end`).
static bool IsSingleUseClassMemberValue(AnalyzedFunction *func, int32_t reg, int32_t ssaVersion) {
    SSARef ref{static_cast<uint8_t>(reg), ssaVersion};
    auto it = func->users.find(ref);
    if (it == func->users.end() || it->second.size() != 1)
        return false;
    auto *user = it->second.front();
    return user && user->operation == LiftedOperation::NEWCLASSMEMBER && user->operands.size() >= 2 && user->operands[1].value.reg == reg;
}

// valid Luau ident (alnum + _, no leading digit). Roblox instance names may have spaces; reject those.
static bool IsValidLuauIdent(const std::string &s) {
    if (s.empty() || std::isdigit(static_cast<unsigned char>(s[0])))
        return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

std::shared_ptr<Expression> ASTLifter::InvertCondition(const std::shared_ptr<Expression> &cond) {
    if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); unary && unary->op == "not ") {
        return unary->operand;
    }

    if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
        // `==` / `~=` are exact negations of each other with the same operands, so flipping is sound.
        if (binary->op == "==") {
            binary->op = "~=";
            return binary;
        } else if (binary->op == "~=") {
            binary->op = "==";
            return binary;
        }
        // Relational operators are NOT algebraically invertible here: `a < b` and `a >= b` differ on
        // NaN (`not (nan < x)` is true, `nan >= x` is false) and, when the compare raises on mismatched
        // types, `a >= b` fires a different error than `not (a < b)`; the operator and operand order in
        // the "attempt to compare" message change. `a > b` also lowers to `b < a` in bytecode, so
        // flipping to `<=` swaps which operand is evaluated first. The only faithful inversion keeps the
        // exact comparison and negates it. (`not (a < b)` recompiles to the same LT with an inverted
        // branch, preserving operand order and the raised error.)
    }

    return std::make_shared<UnaryExpressionNode>("not ", cond);
}

ASTLifter::ASTLifter() {}

ASTFunction ASTLifter::Lift(AnalyzedFunction &analyzedFunction) {
    this->m_currentFunction = &analyzedFunction;
    if (m_debugNotes && m_debugNotes->Enabled())
        m_debugFunction = std::format("F{} ({})", analyzedFunction.lpLiftedFunction->lpDeserialized ? static_cast<int>(analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId) : -1,
                                      analyzedFunction.lpLiftedFunction->name);
    Explain("function {}: lifting {} CFG blocks", m_debugFunction, analyzedFunction.basicBlocks.size());
    this->m_definedRegisters.clear();
    this->m_pinnedRegisters.clear();
    this->m_capturedRegisters.clear();
    this->m_processedInstructions.clear();
    this->m_inlineConsumedDefs.clear();
    this->m_foldConsumedDefs.clear();
    this->m_pendingClasses.clear();
    this->m_phiConsumers.clear();
    this->m_deferToConditionInline.clear();
    this->m_shouldInlineMemo.clear(); // keyed by this function's instructions
    this->m_shouldInlineActive.clear();
    this->m_mergeCache.clear();       // keyed by this function's block ids
    this->m_valueArmDuplications = 0;

    // Registers that hold a control-flow-materialised boolean (a comparison lowered to a LOADB
    // diamond: `LOAD Rd,bT` in one arm, `LOADNJUMP Rd,bF` in the other). The diamond is collapsed to
    // `Rd = <cond>` only later (DetectBooleanMaterialization), so at table-constructor fold time Rd's
    // def still looks like a plain bool LOAD. Folding it would bake the raw `true`/`false` into the
    // `{ ... }` literal and drop the comparison. Recording the LOADNJUMP-bool targets here lets the
    // fold decline and fall back to sound sequential `t.field = <cond>` stores.
    this->m_diamondBoolRegs.clear();
    for (const auto &i : analyzedFunction.lpLiftedFunction->instructions)
        if (i.operation == LiftedOperation::LOADNJUMP && i.operands.size() >= 2 && i.operands[0].type == LiftedOperandType::Register &&
            i.operands[1].type == LiftedOperandType::ImmediateBool)
            this->m_diamondBoolRegs.insert(i.operands[0].value.reg);

    // reverse-index definitionMap once: instruction -> its SSARefs. avoids per-instruction full scans (O(N^2)).
    this->m_defsByInstruction.clear();
    for (const auto &[ref, defInst] : analyzedFunction.definitionMap)
        this->m_defsByInstruction[defInst].push_back(ref);

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

    // pre-pass: a loop condition re-evaluates every iteration. A side-effectful def (CALL family)
    // defined OUTSIDE the condition's block must not inline into it; that would move the call
    // into the loop. Marked before lifting so the def's own statement still emits at its site.
    this->m_forcedMaterialization.clear();
    for (const auto &[definition, refs] : m_defsByInstruction) {
        if (!definition || (definition->operation != LiftedOperation::CALL && definition->operation != LiftedOperation::CALLFB &&
                            definition->operation != LiftedOperation::NAMECALL))
            continue;
        const int definitionBlock = analyzedFunction.GetBlockId(definition);
        const LiftedInstruction *crossBlockUser = nullptr;
        for (const auto &ref : refs) {
            if (const auto users = analyzedFunction.users.find(ref); users != analyzedFunction.users.end())
                for (const auto *user : users->second)
                    if (analyzedFunction.GetBlockId(user) != definitionBlock) {
                        crossBlockUser = user;
                        break;
                    }
            if (crossBlockUser)
                break;
        }
        if (crossBlockUser) {
            m_forcedMaterialization.insert(definition);
            ExplainKeep(definition, "call result is consumed in another CFG block", crossBlockUser);
        }
    }
    for (const auto &block : analyzedFunction.basicBlocks) {
        if ((!block.loopHeader.has_value() && !block.loopLatch.has_value()) || !block.lpTail)
            continue;
        const auto tailOp = block.lpTail->operation;
        const bool conditional = tailOp == LiftedOperation::JUMPIF || tailOp == LiftedOperation::JUMPIFNOT || tailOp == LiftedOperation::JUMPIFEQ ||
                                 tailOp == LiftedOperation::JUMPIFNOTEQ || tailOp == LiftedOperation::JUMPIFLE || tailOp == LiftedOperation::JUMPIFNOTLE ||
                                 tailOp == LiftedOperation::JUMPIFLT || tailOp == LiftedOperation::JUMPIFNOTLT || tailOp == LiftedOperation::JUMPXEQK;
        if (!conditional)
            continue;
        for (const auto &o : block.lpTail->operands) {
            if (o.type != LiftedOperandType::Register)
                continue;
            const auto *def = analyzedFunction.GetDefinition(o);
            if (!def)
                continue;
            const auto defOp = def->operation;
            const bool sideEffectful = defOp == LiftedOperation::CALL || defOp == LiftedOperation::CALLFB || defOp == LiftedOperation::NAMECALL ||
                                       defOp == LiftedOperation::FASTCALL || defOp == LiftedOperation::FASTCALL1 || defOp == LiftedOperation::FASTCALL2 ||
                                       defOp == LiftedOperation::FASTCALL2K || defOp == LiftedOperation::FASTCALL3;
            if (sideEffectful && analyzedFunction.GetBlockId(def) != static_cast<int>(block.dwBlockId)) {
                m_forcedMaterialization.insert(def);
                ExplainKeep(def, "inlining would move an outside effect into the loop condition", block.lpTail);
            }
        }
    }

    for (const auto &block : analyzedFunction.basicBlocks) {
        if (!block.lpHead || !block.lpTail)
            continue;
        for (const auto *table = block.lpHead; table <= block.lpTail; ++table) {
            if ((table->operation != LiftedOperation::NEWTABLE && table->operation != LiftedOperation::DUPTABLE) || table->operands.empty())
                continue;
            for (const auto *setList = table + 1; setList <= block.lpTail; ++setList) {
                if (setList->operation != LiftedOperation::SETLIST || setList->operands.size() < 2 ||
                    setList->operands[0].value.reg != table->operands[0].value.reg ||
                    setList->operands[0].ssaVersion != table->operands[0].ssaVersion || !analyzedFunction.implicitUses.contains(setList))
                    continue;
                const auto &versions = analyzedFunction.implicitUses.at(setList);
                const int32_t startReg = setList->operands[1].value.reg;
                boost::unordered_flat_set<SSARef, std::hash<SSARef>> seen;
                std::function<bool(const LiftedOperand &)> dependsOnLaterClosure = [&](const LiftedOperand &operand) -> bool {
                    if (operand.type != LiftedOperandType::Register)
                        return false;
                    const SSARef ref{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
                    if (!seen.insert(ref).second)
                        return false;
                    const auto *definition = analyzedFunction.GetDefinition(operand);
                    if (!definition)
                        return false;
                    if ((definition->operation == LiftedOperation::NEWCLOSURE || definition->operation == LiftedOperation::DUPCLOSURE) &&
                        definition->instructionIndex > table->instructionIndex)
                        return true;
                    for (size_t i = 1; i < definition->operands.size(); ++i)
                        if (dependsOnLaterClosure(definition->operands[i]))
                            return true;
                    return false;
                };
                for (size_t i = 0; i < versions.size(); ++i) {
                    LiftedOperand element{};
                    element.type = LiftedOperandType::Register;
                    element.value.reg = startReg + static_cast<int32_t>(i);
                    element.ssaVersion = versions[i];
                    seen.clear();
                    if (dependsOnLaterClosure(element)) {
                        m_forcedMaterialization.insert(table);
                        ExplainKeep(table, "constructor element depends on a later closure", setList);
                        break;
                    }
                }
                break;
            }
        }
    }

    Explain("function {}: forced {} definitions to remain statements because moving them could change effects or closure order",
            m_debugFunction, m_forcedMaterialization.size());

    ASTFunction ast;
    ast.backingFunction = &analyzedFunction;
    analyzedFunction.PopulateNames();

    for (int32_t arg = 0; arg < analyzedFunction.lpLiftedFunction->lpDeserialized->numparams; ++arg)
        m_definedRegisters.insert(arg);

    // pre-pass: a VAL/REF closure capture aliases an upvalue to its source local's name. Set that
    // override BEFORE lifting any statement, so the source local's declaration AND every use are emitted
    // under the upvalue's debug name; consistent with the closure's reference. The closure handler sets
    // the same override, but only when it runs; a source defined earlier would by then already have been
    // emitted under its default name and be dead-stripped (the closure's upvalue read no longer matching
    // it). SSA-keyed, so register reuse cannot cross-rename an unrelated local.
    {
        const auto &instrs = analyzedFunction.lpLiftedFunction->instructions;
        const auto &constants = analyzedFunction.lpLiftedFunction->lpDeserialized->constants;
        const auto &subs = analyzedFunction.lpLiftedFunction->lpDeserialized->subfunctions;
        // Lifted locals lose their `do` scope, so a debug name shared with a global would capture it.
        std::unordered_set<std::string> globalNames;
        const std::function<void(const LiftedFunction &)> collectGlobals = [&](const LiftedFunction &function) {
            const auto &functionConstants = function.lpDeserialized->constants;
            auto add = [&](int32_t index) {
                if (index >= 0 && static_cast<size_t>(index) < functionConstants.size() && functionConstants[index].kType == LUA_TSTRING)
                    globalNames.insert(std::get<std::string>(functionConstants[index].constantData));
            };
            for (const auto &global : function.instructions)
                if ((global.operation == LiftedOperation::GETGLOBAL || global.operation == LiftedOperation::SETGLOBAL) && global.operands.size() >= 2)
                    add(global.operands[1].value.imm.k);
                else if (global.operation == LiftedOperation::GETIMPORT && global.operands.size() >= 3)
                    add(static_cast<int32_t>(global.operands[2].value.imm.u >> 20) & 1023);
            for (const auto &child : function.subfunctions)
                collectGlobals(child);
        };
        collectGlobals(*analyzedFunction.lpLiftedFunction);
        const auto localName = [&](const std::string &name, uint8_t reg) {
            if (!globalNames.contains(name))
                return name;
            std::string renamed = std::format("{}_{}", name, reg);
            for (int index = 2; globalNames.contains(renamed); ++index)
                renamed = std::format("{}_{}_{}", name, reg, index);
            return renamed;
        };
        for (size_t i = 0; i < instrs.size(); ++i) {
            const auto &inst = instrs[i];
            LuauProto proto = nullptr;
            if (inst.operation == LiftedOperation::DUPCLOSURE) {
                const int32_t kIdx = inst.operands[1].value.imm.k;
                if (kIdx >= 0 && static_cast<size_t>(kIdx) < constants.size() && std::holds_alternative<LuauProto>(constants[kIdx].constantData))
                    proto = std::get<LuauProto>(constants[kIdx].constantData);
            } else if (inst.operation == LiftedOperation::NEWCLOSURE) {
                const int32_t protoIdx = inst.operands[1].value.imm.k;
                if (protoIdx >= 0 && static_cast<size_t>(protoIdx) < subs.size())
                    proto = subs[protoIdx];
            } else {
                continue;
            }
            if (!proto)
                continue;
            for (size_t capIdx = 0; i + 1 + capIdx < instrs.size() && instrs[i + 1 + capIdx].operation == LiftedOperation::CAPTURE; ++capIdx) {
                const auto &cap = instrs[i + 1 + capIdx];
                const int mode = cap.operands[0].value.imm.n;
                const bool srcIsRegister = cap.operands.size() >= 2 && cap.operands[1].type == LiftedOperandType::Register;
                if ((mode == 0 || mode == 1) && srcIsRegister) {
                    m_capturedRegisters.insert(cap.operands[1].value.reg);
                    // An auto-shaped debug name (`v1`, from recompiled output) would alias another register's default name.
                    if (proto->upvalueNames.size() > capIdx && !AnalyzedFunction::IsAutoNameShaped(proto->upvalueNames[capIdx])) {
                        const std::string name = localName(proto->upvalueNames[capIdx], cap.operands[1].value.reg);
                        analyzedFunction.ssaOverrides[SSARef{static_cast<uint8_t>(cap.operands[1].value.reg), cap.operands[1].ssaVersion}] = name;
                        // a parameter is one local for the whole body; reads before the capture must agree
                        if (cap.operands[1].value.reg < analyzedFunction.lpLiftedFunction->lpDeserialized->numparams && IsValidLuauIdent(name))
                            analyzedFunction.SetGlobalName(cap.operands[1].value.reg, name);
                        if (mode == 1)
                            for (const auto &local : analyzedFunction.lpLiftedFunction->lpDeserialized->locvars)
                                if (local.reg == cap.operands[1].value.reg && local.startpc <= cap.instructionIndex && cap.instructionIndex < local.endpc)
                                    for (const auto &[ref, definition] : analyzedFunction.definitionMap)
                                        if (ref.regIndex == local.reg && definition && local.startpc <= definition->instructionIndex &&
                                            definition->instructionIndex < local.endpc)
                                            analyzedFunction.ssaOverrides[ref] = name;
                    }
                }
            }
        }
    }

    // Captured phi inputs must use the same local name as the merged upvalue.
    std::vector<SSARef> capturedNames;
    for (const auto &[ref, name] : analyzedFunction.ssaOverrides)
        capturedNames.push_back(ref);
    for (size_t i = 0; i < capturedNames.size(); ++i) {
        const auto ref = capturedNames[i];
        const auto name = analyzedFunction.ssaOverrides.at(ref);
        if (const auto users = analyzedFunction.users.find(ref); users != analyzedFunction.users.end())
            for (const auto *user : users->second)
                if (user->operation == LiftedOperation::PHI && user->operands[0].value.reg == ref.regIndex) {
                    const auto &output = user->operands[0];
                    const SSARef outputRef{static_cast<uint8_t>(output.value.reg), output.ssaVersion};
                    if (analyzedFunction.ssaOverrides.emplace(outputRef, name).second)
                        capturedNames.push_back(outputRef);
                }
        const auto def = analyzedFunction.definitionMap.find(ref);
        if (def == analyzedFunction.definitionMap.end() || def->second->operation != LiftedOperation::PHI)
            continue;
        for (size_t j = 1; j < def->second->operands.size(); ++j) {
            const auto &input = def->second->operands[j];
            if (input.type != LiftedOperandType::Register || input.value.reg != ref.regIndex)
                continue;
            const SSARef inputRef{static_cast<uint8_t>(input.value.reg), input.ssaVersion};
            if (analyzedFunction.ssaOverrides.emplace(inputRef, name).second)
                capturedNames.push_back(inputRef);
        }
    }

    std::unordered_map<int32_t, std::vector<std::pair<SSARef, LiftedInstruction *>>> definitionsByRegister;
    for (const auto &[ref, definition] : analyzedFunction.definitionMap)
        if (definition && definition->operation != LiftedOperation::PHI)
            definitionsByRegister[ref.regIndex].emplace_back(ref, definition);
    for (auto &[reg, definitions] : definitionsByRegister) {
        if (definitions.size() < 2 || analyzedFunction.globalRegNames.contains(reg))
            continue;
        std::ranges::sort(definitions, [](const auto &left, const auto &right) { return left.second->instructionIndex < right.second->instructionIndex; });
        for (size_t i = 0; i + 1 < definitions.size(); ++i) {
            const auto [ref, definition] = definitions[i];
            if (definition->operation == LiftedOperation::NAMECALL || definition->operation == LiftedOperation::NAMECALLUDATA)
                continue;
            if (analyzedFunction.ssaOverrides.contains(ref) || analyzedFunction.variableNames.contains(ref))
                continue;
            auto current = ref;
            int32_t lastUse = definition->instructionIndex;
            std::unordered_set<SSARef> seen;
            while (seen.insert(current).second) {
                const auto users = analyzedFunction.users.find(current);
                if (users == analyzedFunction.users.end() || users->second.size() != 1)
                    break;
                auto *user = users->second.front();
                lastUse = std::max(lastUse, user->instructionIndex);
                const auto outputs = m_defsByInstruction.find(user);
                if (!ShouldInline(user) || outputs == m_defsByInstruction.end() || outputs->second.size() != 1)
                    break;
                current = outputs->second.front();
            }
            const auto block = analyzedFunction.GetBlockId(definition);
            if (block < 0)
                continue;
            bool overlaps = false;
            for (size_t j = i + 1; j < definitions.size() && definitions[j].second->instructionIndex < lastUse; ++j)
                if (analyzedFunction.GetBlockId(definitions[j].second) == block) {
                    overlaps = true;
                    break;
                }
            if (overlaps)
                analyzedFunction.ssaOverrides[ref] = std::format("v{}_{}", reg, ref.version);
        }
    }

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
                    const bool headerPhi = std::ranges::any_of(b.phiNodes, [&](const LiftedInstruction &phi) { return &phi == def; });
                    if (headerPhi) {
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

        boost::unordered_flat_set<uint32_t> visited;
        ast.statements = LiftControlFlow(0, InvalidBlockId, visited).Run();
        // nested blocks fold via CreateBlock; the top-level body comes straight from LiftControlFlow, so fold it too.
        FoldShortCircuitChain(ast.statements);
        Explain("function {}: control-flow lift produced {} top-level statements", m_debugFunction, ast.statements.size());

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

        for (const auto &renamed : analyzedFunction.disambiguatedNames)
            ast.statements.insert(
                ast.statements.begin(),
                std::make_shared<CommentNode>(
                    std::format("Fission: INFO: binding '{}' has been suffixed to avoid shadowing an existing, upper scope variable.", renamed.first), true,
                    true
                )
            );

        for (const auto &renamed : analyzedFunction.prefixedLocalRenames)
            ast.statements.insert(
                ast.statements.begin(),
                std::make_shared<CommentNode>(
                    std::format(
                        "Fission: INFO: local '{}' was prefixed (from '{}') to avoid overwriting a global of the same name.", renamed.first, renamed.second
                    ),
                    true, true
                )
            );

        // The main banner stays; the per-function info block is informational.
        ast.statements.insert(ast.statements.begin(), std::make_shared<CommentNode>(s, true, !analyzedFunction.lpLiftedFunction->lpDeserialized->bIsMain));
    }

    return ast;
}

void ASTLifter::ExplainKeep(const LiftedInstruction *definition, std::string_view reason, const LiftedInstruction *consumer,
                           const LiftedInstruction *barrier) const {
    if (!m_debugNotes || !m_debugNotes->Enabled() || !definition)
        return;
    const int blockId = m_currentFunction->GetBlockId(definition);
    if (blockId < 0 || static_cast<size_t>(blockId) >= m_currentFunction->basicBlocks.size())
        return;
    auto message = std::format("keep _{} {}", definition->instructionIndex, OperationToString(definition->operation));
    if (const auto defs = m_defsByInstruction.find(definition); defs != m_defsByInstruction.end() && !defs->second.empty()) {
        const auto first = std::min_element(defs->second.begin(), defs->second.end(), [](const SSARef &a, const SSARef &b) {
            return a.regIndex < b.regIndex || (a.regIndex == b.regIndex && a.version < b.version);
        });
        message += std::format(" R{}#{}", first->regIndex, first->version);
        if (defs->second.size() > 1)
            message += std::format(" (+{} definitions)", defs->second.size() - 1);
    }
    if (consumer && consumer->operation == LiftedOperation::PHI)
        message += std::format(" before phi in B{}", m_currentFunction->GetBlockId(consumer));
    else if (consumer)
        message += std::format(" before _{} in B{}", consumer->instructionIndex, m_currentFunction->GetBlockId(consumer));
    if (barrier)
        message += std::format("; barrier _{} {}", barrier->instructionIndex, OperationToString(barrier->operation));
    message += std::format(": {}", reason);
    Explain(m_currentFunction->basicBlocks[blockId], "{}", message);
}

std::shared_ptr<Expression> ASTLifter::LiftCondition(const LiftedInstruction *inst) {
    if (!inst)
        return std::make_shared<BooleanLiteralNode>(false);

    // The compiler canonicalizes `a > b` to LT(b, a), losing source operand order. When both
    // operands inline effectful defs, emitting them re-swapped changes evaluation (and first-error)
    // order at recompile. If the defs' instruction order says operand[0] was evaluated AFTER
    // operand[2], restore source order by emitting the mirrored operator with swapped operands.
    const auto liftComparison = [&](const char *op, const char *mirrored) -> std::shared_ptr<Expression> {
        const auto &a = inst->operands[0], &b = inst->operands[2];
        if (a.type == LiftedOperandType::Register && b.type == LiftedOperandType::Register) {
            const auto *da = m_currentFunction->GetDefinition(a);
            const auto *db = m_currentFunction->GetDefinition(b);
            if (da && db && da->instructionIndex > db->instructionIndex && ShouldInline(da) && ShouldInline(db)) {
                auto left = LiftExpression(b);
                auto right = LiftExpression(a);
                return std::make_shared<BinaryExpressionNode>(mirrored, left, right);
            }
        }
        auto left = LiftExpression(a);
        auto right = LiftExpression(b);
        return std::make_shared<BinaryExpressionNode>(op, left, right);
    };

    // negated relational branches (JUMPIFNOTLT/LE) mean "jump if NOT (a </<= b)". Their faithful
    // condition is `not (a < b)`, not the algebraically-flipped `a >= b`: the two agree on ordinary
    // numbers but differ on NaN and, when the compare raises on mismatched types, `a >= b` fires a
    // different "attempt to compare" error (operator + operand order change). Keep the exact compare
    // and negate it. (`==`/`~=` are exact negations, so those flip directly.)
    const auto notWrap = [](std::shared_ptr<Expression> e) -> std::shared_ptr<Expression> {
        return std::make_shared<UnaryExpressionNode>("not ", std::move(e));
    };

    switch (inst->operation) {
    case LiftedOperation::JUMPIFNOTEQ:
        return liftComparison("~=", "~=");
    case LiftedOperation::JUMPIFEQ:
        return liftComparison("==", "==");
    case LiftedOperation::JUMPIFLT:
        return liftComparison("<", ">");
    case LiftedOperation::JUMPIFNOTLT:
        return notWrap(liftComparison("<", ">"));
    case LiftedOperation::JUMPIFLE:
        return liftComparison("<=", ">=");
    case LiftedOperation::JUMPIFNOTLE:
        return notWrap(liftComparison("<=", ">="));
    case LiftedOperation::JUMPIF:
        return LiftExpression(inst->operands[0]);
    case LiftedOperation::JUMPIFNOT:
        return std::make_shared<UnaryExpressionNode>("not ", LiftExpression(inst->operands[0]));
    case LiftedOperation::JUMPXEQK: {
        if (inst->operands[2].type == LiftedOperandType::ImmediateConstant) {
            auto kIdx = inst->operands[2].value.imm.k;
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs;
            const auto &k = ConstantAt(kIdx);
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

// FORxLOOP exits through its forward successor; the other edge returns to the loop header or body.
static uint32_t ResolveLoopExitFromLatch(const std::vector<BasicBlock> &blocks, uint32_t latchId, uint32_t headerId) {
    if (latchId >= blocks.size())
        return InvalidBlockId;
    const BasicBlock &latch = blocks[latchId];
    const int latchEnd = latch.lpTail ? latch.lpTail->instructionIndex : -1;
    uint32_t firstNonHeader = InvalidBlockId;
    for (uint32_t succ : latch.successors) {
        if (succ == headerId || succ >= blocks.size())
            continue;
        if (firstNonHeader == InvalidBlockId)
            firstNonHeader = succ;
        const BasicBlock &sb = blocks[succ];
        if (sb.lpHead && sb.lpHead->instructionIndex > latchEnd)
            return succ; // forward fall-through == loop exit
    }
    return firstNonHeader;
}

ControlFlowTask ASTLifter::LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, boost::unordered_flat_set<uint32_t> &visited) {
    std::vector<std::shared_ptr<Statement>> nodes;

    // true only for the first block of this call (the branch/region entry). A visited block met here is
    // a shared branch arm; met later in the linear walk it is a genuine convergence that must stop.
    bool atEntryBlock = true;
    // blocks this walk has lifted; a visited block outside it was lifted by another arm
    boost::unordered_flat_set<uint32_t> walked;
    const auto blockHasEffect = [&](uint32_t id) {
        const auto &candidate = m_currentFunction->basicBlocks[id];
        for (const LiftedInstruction *instruction = candidate.lpHead; instruction && instruction <= candidate.lpTail; ++instruction)
            if (CanOperationRaise(instruction->operation) || instruction->operation == LiftedOperation::CALL ||
                instruction->operation == LiftedOperation::CALLFB || StaysAsStatement(instruction))
                return true;
        return false;
    };
    const auto isReturnOnly = [&](uint32_t id) {
        const auto &candidate = m_currentFunction->basicBlocks[id];
        return candidate.bType == BlockType::Return && std::all_of(candidate.lpHead, candidate.lpTail + 1, [](const LiftedInstruction &instruction) {
                   return instruction.operation == LiftedOperation::RETURN || instruction.operation == LiftedOperation::NOP;
               });
    };
    const auto reachesContinuationOrReturns = [&](uint32_t start, uint32_t continuation) {
        const auto &blocks = m_currentFunction->basicBlocks;
        boost::unordered_flat_set<uint32_t> seen;
        std::vector<uint32_t> pending{start};
        bool reachesContinuation = false;
        bool reachesReturn = false;
        bool hasEffect = false;
        while (!pending.empty()) {
            const uint32_t id = pending.back();
            pending.pop_back();
            if (id == continuation) {
                reachesContinuation = true;
                continue;
            }
            if (!m_loopExitStack.empty() && id == m_loopExitStack.back()) {
                reachesReturn = true;
                hasEffect = true;
                continue;
            }
            if (id >= blocks.size())
                return false;
            if (!seen.insert(id).second)
                continue;
            const auto &candidate = blocks[id];
            if (candidate.successors.empty()) {
                if (candidate.bType != BlockType::Return || candidate.bTerminator != BlockTerminator::Return)
                    return false;
                reachesReturn = true;
                continue;
            }
            if (candidate.lpHead && candidate.lpTail)
                for (auto *instruction = candidate.lpHead; instruction <= candidate.lpTail; ++instruction)
                    switch (instruction->operation) {
                    case LiftedOperation::CALL:
                    case LiftedOperation::CALLFB:
                    case LiftedOperation::NAMECALL:
                    case LiftedOperation::NAMECALLUDATA:
                    case LiftedOperation::SETGLOBAL:
                    case LiftedOperation::SETUPVAL:
                    case LiftedOperation::SETTABLE:
                    case LiftedOperation::SETTABLEKS:
                    case LiftedOperation::SETTABLEN:
                    case LiftedOperation::SETUDATAKS:
                        hasEffect = true;
                        break;
                    default:
                        break;
                    }
            for (const uint32_t successor : candidate.successors) {
                if (successor >= blocks.size())
                    return false;
                if (successor <= id)
                    return false;
                pending.push_back(successor);
            }
        }
        return reachesContinuation && reachesReturn && hasEffect;
    };

    // the innermost loop's latch is the one exiting to the innermost loop exit
    const auto isInnermostLatch = [&](uint32_t id) {
        if (m_loopExitStack.empty() || id >= m_currentFunction->basicBlocks.size())
            return false;
        const auto &latch = m_currentFunction->basicBlocks[id];
        return latch.bType == BlockType::LoopLatch && std::ranges::find(latch.successors, m_loopExitStack.back()) != latch.successors.end();
    };

    // iterative tail-traversal: recursing the linear `after` continuation overflows the stack on long
    // `if .. return end; ...` chains. branch/loop bodies still recurse (bounded by nesting depth).
    while (true) {
        if (currentBlockId == InvalidBlockId || currentBlockId >= m_currentFunction->basicBlocks.size())
            break;

        // body code reaching the innermost loop exit directly == `break` (normal exit goes via latch).
        // don't mark visited; exit is still lifted once after the loop.
        if (!m_loopExitStack.empty() && currentBlockId == m_loopExitStack.back()) {
            nodes.push_back(std::make_shared<BreakStatementNode>());
            break;
        }
        // reaching the latch before this region ends skips the rest of the iteration
        if (currentBlockId != stopBlockId && stopBlockId != InvalidBlockId && isInnermostLatch(currentBlockId)) {
            // `continue` skips what the source places before the loop test, so the latch's own statements run here too
            const auto &latchBlock = m_currentFunction->basicBlocks[currentBlockId];
            const auto definedBefore = m_definedRegisters;
            const auto processedBefore = m_processedInstructions;
            const auto inlineConsumedBefore = m_inlineConsumedDefs;
            const auto foldConsumedBefore = m_foldConsumedDefs;
            auto latchStmts = LiftBlockInstructions(latchBlock);
            m_definedRegisters = definedBefore;
            m_processedInstructions = processedBefore;
            m_inlineConsumedDefs = inlineConsumedBefore;
            m_foldConsumedDefs = foldConsumedBefore;
            nodes.insert(nodes.end(), latchStmts.begin(), latchStmts.end());
            nodes.push_back(std::make_shared<ContinueStatementNode>());
            break;
        }

        // Duplicate only return-only merges; every other merge may carry an effect.
        if (currentBlockId == stopBlockId && !isReturnOnly(currentBlockId))
            break;
        // return blocks are allowed to be duplicated, as they have no successors.
        // compilers may inline the return for a break, which is annoying as fuck, and will break our lifting.
        // fuck you luauc.
        if (this->m_currentFunction->basicBlocks.at(currentBlockId).bType != BlockType::Return) {
            if (visited.contains(currentBlockId)) {
                // a shared short-circuit value arm reached via a second branch edge: re-lift its value
                // into this branch instead of dropping it. only at the branch entry, and only for
                // side-effect-free value blocks; a visited block met as a linear continuation is a real
                // convergence and still stops.
                constexpr uint32_t kMaxValueArmDuplications = 8192;
                const bool canDup = atEntryBlock && m_valueArmDuplications < kMaxValueArmDuplications;
                // a sibling arm lifted this forward-entered block: it is a tail shared by exclusive paths, not a convergence
                const auto &visitedBlock = m_currentFunction->basicBlocks[currentBlockId];
                const bool siblingTail = !atEntryBlock && !walked.contains(currentBlockId) && m_valueArmDuplications < kMaxValueArmDuplications &&
                                         std::ranges::all_of(visitedBlock.predecessors, [&](uint32_t p) { return p < currentBlockId; });
                if (canDup && IsDuplicableValueArm(currentBlockId, stopBlockId)) {
                    // single pure value block whose successor IS the merge: fall through and re-lift inline.
                    ++m_valueArmDuplications;
                } else if (canDup && IsDuplicablePureRegion(currentBlockId, stopBlockId)) {
                    // deeper shape (`a and (b or c) and d or e`): the shared value block is followed by
                    // further truthiness tests before the merge, so its successor is not the merge and the
                    // single-block check above rejects it. re-lift the whole pure reconverging sub-region
                    // into this branch with a FRESH visited set, so the inner tests are not short-circuited
                    // away. every block in the region is a load/move or truthiness branch, so re-lifting
                    // duplicates no store, call, or raising op; semantically a no-op. bounded by the
                    // region-size cap in IsDuplicablePureRegion plus the global duplication cap.
                    ++m_valueArmDuplications;
                    boost::unordered_flat_set<uint32_t> regionVisited;
                    auto regionNodes = co_await LiftControlFlow(currentBlockId, stopBlockId, regionVisited);
                    nodes.insert(nodes.end(), regionNodes.begin(), regionNodes.end());
                    break;
                } else if (auto region = canDup || siblingTail ? SharedTailRegion(currentBlockId, stopBlockId) : std::nullopt;
                           // a pure tail reached through pure tests folds back into one short-circuit value
                           region && (canDup || std::ranges::any_of(*region, blockHasEffect) || std::ranges::any_of(walked, blockHasEffect))) {
                    // a small effectful tail shared by two exclusive branch edges: each path runs it once,
                    // so emitting it in both branches keeps every effect single. clear its processed marks
                    // (as for shared return blocks) so the second copy is complete.
                    ++m_valueArmDuplications;
                    // blocks outside the region stay visited so flow leaving it (a loop's exit) is not lifted again
                    auto regionVisited = visited;
                    for (const uint32_t id : *region) {
                        regionVisited.erase(id);
                        const auto &regionBlock = m_currentFunction->basicBlocks[id];
                        for (const LiftedInstruction *instruction = regionBlock.lpHead; instruction && instruction <= regionBlock.lpTail; ++instruction)
                            m_processedInstructions.erase(instruction->instructionIndex);
                    }
                    auto regionNodes = co_await LiftControlFlow(currentBlockId, stopBlockId, regionVisited);
                    nodes.insert(nodes.end(), regionNodes.begin(), regionNodes.end());
                    break;
                } else {
                    break;
                }
            }
        }

        if (!visited.contains(currentBlockId))
            visited.insert(currentBlockId); // prevent double insertion product of block above.
        walked.insert(currentBlockId);
        struct LiftingScope {
            std::vector<uint32_t> &stack;
            ~LiftingScope() { stack.pop_back(); }
        };
        m_liftingBlocks.push_back(currentBlockId);        const LiftingScope liftingScope{m_liftingBlocks};

        auto &block = m_currentFunction->basicBlocks[currentBlockId];

        if (block.bType == BlockType::Return && block.predecessors.size() > 1 && currentBlockId != stopBlockId)
            for (auto *instruction = block.lpHead; instruction && instruction <= block.lpTail; ++instruction)
                if (instruction->operation == LiftedOperation::CALL || instruction->operation == LiftedOperation::CALLFB ||
                    instruction->operation == LiftedOperation::NAMECALL || instruction->operation == LiftedOperation::NAMECALLUDATA)
                    m_processedInstructions.erase(instruction->instructionIndex);

        // repeat-until headers are lifted inside the repeat path itself; pre-lifting here is
        // discarded there but still poisons m_definedRegisters, turning the re-lifted body decls
        // into bare (global) assignments.
        const bool isRepeatHeader = block.bType == BlockType::LoopHeader && block.loopLatch.has_value() &&
                                    (block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop;
        const bool deferredWhileHeader = block.bType == BlockType::LoopHeader && block.loopLatch &&
                                         (block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop && block.successors.size() == 2 &&
                                         std::all_of(block.successors.begin(), block.successors.end(), [&](uint32_t successor) {
                                             return CanReach(successor, *block.loopLatch, currentBlockId, {currentBlockId});
                                         });
        auto stmts = isRepeatHeader || deferredWhileHeader ? std::vector<std::shared_ptr<Statement>>{} : LiftBlockInstructions(block);

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
                const bool mergeFromGraph = mergeIdx != InvalidBlockId;

                // An arm that is the enclosing region's end is empty; the join is that end. Return arms have no
                // merge of their own, and a merge found past the stop would pull the enclosing join into this if.
                if (stopBlockId != InvalidBlockId && !isReturnOnly(stopBlockId) && (trueIdx == stopBlockId || falseIdx == stopBlockId))
                    mergeIdx = stopBlockId;

                if (mergeIdx == InvalidBlockId) {
                    const auto terminalReturn = [&](uint32_t id) {
                        const auto &candidate = m_currentFunction->basicBlocks[id];
                        return candidate.bType == BlockType::Return && candidate.bTerminator == BlockTerminator::Return && candidate.successors.empty();
                    };
                    const bool trueIsReturn = terminalReturn(trueIdx);
                    const bool falseIsReturn = terminalReturn(falseIdx);

                    if (reachesContinuationOrReturns(trueIdx, falseIdx)) {
                        mergeIdx = falseIdx;
                    } else if (reachesContinuationOrReturns(falseIdx, trueIdx)) {
                        mergeIdx = trueIdx;
                    } else if (trueIsReturn && !falseIsReturn) {
                        mergeIdx = falseIdx;
                    } else if (!trueIsReturn && falseIsReturn) {
                        mergeIdx = trueIdx;
                    }
                }

                // Loop exits are break targets, not convergences. A direct arm is likewise not a merge when
                // the other arm reaches it only after returning through this header on a later iteration.
                const bool mergeTargetsLoopExit = mergeIdx != InvalidBlockId && !m_loopExitStack.empty() &&
                                                 std::find(m_loopExitStack.begin(), m_loopExitStack.end(), mergeIdx) != m_loopExitStack.end();
                const bool mergeTargetsActiveLoopExit = !m_loopExitStack.empty() && mergeIdx == m_loopExitStack.back();
                if (mergeTargetsActiveLoopExit) {
                    if (mergeIdx == trueIdx)
                        mergeIdx = falseIdx;
                    else if (mergeIdx == falseIdx)
                        mergeIdx = trueIdx;
                    else if (reachesContinuationOrReturns(trueIdx, falseIdx))
                        mergeIdx = falseIdx;
                    else if (reachesContinuationOrReturns(falseIdx, trueIdx))
                        mergeIdx = trueIdx;
                    else
                        mergeIdx = InvalidBlockId;
                }

                const bool mergeIsDirectArm = mergeIdx == trueIdx || mergeIdx == falseIdx;
                const uint32_t otherArm = mergeIdx == trueIdx ? falseIdx : trueIdx;
                const bool mergeIsNextIterationArm = !mergeTargetsLoopExit && mergeFromGraph && mergeIsDirectArm && mergeIdx != stopBlockId &&
                                                      !CanReach(otherArm, mergeIdx, currentBlockId, {currentBlockId});
                if ((mergeTargetsLoopExit && !mergeTargetsActiveLoopExit) || mergeIsNextIterationArm)
                    mergeIdx = InvalidBlockId;
                // one arm ends this region while the other jumps into the latch: join at the region end and
                // let the latch edge lift as `continue`
                if (mergeIdx != stopBlockId && stopBlockId != InvalidBlockId && (mergeIdx == InvalidBlockId || isInnermostLatch(mergeIdx)) &&
                    (isInnermostLatch(trueIdx) || isInnermostLatch(falseIdx)) && (trueIdx == stopBlockId || falseIdx == stopBlockId))
                    mergeIdx = stopBlockId;

                auto ifStmt = std::make_shared<IfStatementNode>();
                auto visitedCopy = visited;
                if (mergeIdx != InvalidBlockId)
                    visitedCopy.insert(mergeIdx);

                // snapshot regs declared BEFORE branches: HoistPhiLocals tests "already in outer scope?"
                // against this, not the post-branch set (branch assigns would wrongly suppress the hoist).
                const auto definedBeforeBranches = m_definedRegisters;

                if (mergeIdx == falseIdx) {
                    ifStmt->condition = (trueCond);
                    ifStmt->thenBranch = CreateBlock(co_await LiftControlFlow(trueIdx, mergeIdx, visitedCopy));
                } else if (mergeIdx == trueIdx) {
                    ifStmt->condition = InvertCondition(trueCond);

                    // iterative build for deep `if-elseif-...-else` where every link shares one merge target
                    // (compiler dispatch trees). probe the chain depth without mutating state; only take the
                    // iterative path past the recursion-safe threshold, keeping short cases on the recursive path.
                    constexpr uint32_t kChainProbeThreshold = 24;
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
                            // Require same merge target as outer mergeIdx; this is the deep dispatch
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
                        boost::unordered_flat_set<uint32_t> chainVisited = visitedCopy;
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
                        parentIfRaw->thenBranch = CreateBlock(co_await LiftControlFlow(chainBlockId, chainStopId, chainVisited));
                    } else {
                        ifStmt->thenBranch = CreateBlock(co_await LiftControlFlow(falseIdx, mergeIdx, visitedCopy));
                    }
                } else {
                    const auto dispatchLink = [&](uint32_t id) {
                        if (id >= m_currentFunction->basicBlocks.size())
                            return false;
                        const auto &candidate = m_currentFunction->basicBlocks[id];
                        if (candidate.bType != BlockType::IfHeader || !candidate.ifStatementTrue || !candidate.ifStatementFalse ||
                            !candidate.phiNodes.empty() ||
                            FindMergeBlock(*candidate.ifStatementTrue, *candidate.ifStatementFalse) != static_cast<int32_t>(mergeIdx))
                            return false;
                        uint32_t arm = *candidate.ifStatementFalse;
                        while (arm != mergeIdx) {
                            if (arm >= m_currentFunction->basicBlocks.size())
                                return false;
                            const auto &part = m_currentFunction->basicBlocks[arm];
                            if ((part.bType != BlockType::Standard && part.bType != BlockType::Continue) || part.successors.size() != 1 ||
                                part.successors.front() <= arm)
                                return false;
                            arm = part.successors.front();
                        }
                        return *candidate.ifStatementTrue > id && *candidate.ifStatementTrue != mergeIdx;
                    };
                    uint32_t chainLength = 0, probe = currentBlockId;
                    while (chainLength < 24 && dispatchLink(probe)) {
                        ++chainLength;
                        probe = *m_currentFunction->basicBlocks[probe].ifStatementTrue;
                    }
                    std::vector<std::shared_ptr<Statement>> elseStmts;
                    if (chainLength == 24) {
                        ifStmt->condition = InvertCondition(trueCond);
                        ifStmt->thenBranch = CreateBlock(co_await LiftControlFlow(falseIdx, mergeIdx, visitedCopy));
                        auto parent = ifStmt;
                        uint32_t link = trueIdx;
                        while (dispatchLink(link)) {
                            visitedCopy.insert(link);
                            const auto &part = m_currentFunction->basicBlocks[link];
                            auto prefix = LiftBlockInstructions(part);
                            auto child = std::make_shared<IfStatementNode>();
                            child->condition = InvertCondition(LiftCondition(part.lpTail));
                            child->thenBranch = CreateBlock(co_await LiftControlFlow(*part.ifStatementFalse, mergeIdx, visitedCopy));
                            prefix.push_back(child);
                            parent->elseBranch = CreateBlock(prefix);
                            parent = child;
                            link = *part.ifStatementTrue;
                        }
                        auto tail = co_await LiftControlFlow(link, mergeIdx, visitedCopy);
                        if (!tail.empty())
                            parent->elseBranch = CreateBlock(tail);
                    } else {
                        ifStmt->condition = (trueCond);
                        ifStmt->thenBranch = CreateBlock(co_await LiftControlFlow(trueIdx, mergeIdx, visitedCopy));
                        m_definedRegisters = definedBeforeBranches;
                        elseStmts = co_await LiftControlFlow(falseIdx, mergeIdx, visitedCopy);
                    }
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
                    HoistPhiLocals(static_cast<int32_t>(mergeIdx), stopBlockId, ifStmt, nodes, definedBeforeBranches);
                    nodes.push_back(ifStmt);
                }

                for (const uint32_t branchBlockId : visitedCopy)
                    if (branchBlockId != mergeIdx)
                        visited.insert(branchBlockId);

                // Restore scope: a register written only inside the branches that does not survive to the
                // merge (it has no phi there) was a branch-local temporary. Drop it from m_definedRegisters
                // so a later instruction reusing that slot emits a fresh `local` instead of a bare assignment
                // -- otherwise the reused slot leaks to a global (a merge-block `NEWTABLE` into a slot that a
                // branch used as a call argument rendered `v2 = {}` with no `local`). Everything defined
                // before the branches and every register live across the merge (phi outputs, including those
                // HoistPhiLocals declared) is kept, so a value that must stay in an enclosing scope is
                // never re-declared.
                // Only outside a loop: inside one, a register written in a branch can be live across the
                // back-edge (read in a later iteration) without a phi at this inner merge, so "no phi here"
                // does not prove it is dead -- pruning it would re-`local` a value that must persist across
                // iterations. Straight-line code has no back-edge, so the merge genuinely ends the temp's scope.
                if (mergeIdx != InvalidBlockId && mergeIdx < m_currentFunction->basicBlocks.size() && m_loopExitStack.empty()) {
                    // Erase in place: only registers added inside the branches (not in the pre-branch
                    // snapshot) that have no phi at the merge are dropped. Copying the whole defined-set
                    // here would be O(|defined|) allocation per `if` and explodes on flattened dispatch
                    // blocks (thousands of ifs sharing a large live-set), so build only the small phi
                    // survivor set and a small erase list.
                    const auto &mergePhis = m_currentFunction->basicBlocks[mergeIdx].phiNodes;
                    boost::unordered_flat_set<int32_t> survivesMerge;
                    survivesMerge.reserve(mergePhis.size());
                    for (const auto &phi : mergePhis)
                        if (!phi.operands.empty() && phi.operands[0].type == LiftedOperandType::Register)
                            survivesMerge.insert(phi.operands[0].value.reg);
                    std::vector<int32_t> branchTemps;
                    for (const auto reg : m_definedRegisters)
                        if (!definedBeforeBranches.contains(reg) && !survivesMerge.contains(reg) && !m_capturedRegisters.contains(reg))
                            branchTemps.push_back(reg);
                    for (const auto reg : branchTemps)
                        m_definedRegisters.erase(reg);
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
            std::optional<uint32_t> sharedOuterRepeatCondition;
            std::optional<uint32_t> sharedOuterRepeatLatch;
            std::optional<uint32_t> sharedOuterRepeatExit;
            if (isRepeatHeader && block.loopLatch && block.loopExit) {
                for (uint32_t predId : block.predecessors) {
                    if (predId == *block.loopLatch || predId >= m_currentFunction->basicBlocks.size())
                        continue;
                    const auto &bridge = m_currentFunction->basicBlocks[predId];
                    if (bridge.bType != BlockType::LoopLatch || bridge.bTerminator != BlockTerminator::Unconditional || bridge.loopHeader != currentBlockId ||
                        bridge.predecessors.size() != 1)
                        continue;
                    const uint32_t conditionId = bridge.predecessors.front();
                    if (conditionId >= m_currentFunction->basicBlocks.size())
                        continue;
                    const auto &conditionBlock = m_currentFunction->basicBlocks[conditionId];
                    if (conditionBlock.bTerminator != BlockTerminator::Conditional || conditionBlock.successors.size() != 2)
                        continue;
                    const auto bridgeSuccessor = std::find(conditionBlock.successors.begin(), conditionBlock.successors.end(), predId);
                    if (bridgeSuccessor == conditionBlock.successors.end() || !CanReach(*block.loopExit, conditionId, currentBlockId, {currentBlockId}))
                        continue;
                    const uint32_t exit = conditionBlock.successors.front() == predId ? conditionBlock.successors.back() : conditionBlock.successors.front();
                    if (exit == currentBlockId || exit == *block.loopLatch)
                        continue;
                    sharedOuterRepeatCondition = conditionId;
                    sharedOuterRepeatLatch = predId;
                    sharedOuterRepeatExit = exit;
                    Explain(block, "reconstruct outer repeat: B{} tests exit B{} and reaches this shared header through latch B{}", conditionId, exit, predId);
                    break;
                }
            }

            // `while <const> do <inner loop> end`: the testless outer shares this header with the inner
            // loop (only a back-edge). detect it so the inner loop wraps in `while true`, else it's dropped.
            std::optional<uint32_t> infiniteWhileLatch;
            if (block.loopLatch.has_value() && !sharedOuterRepeatLatch)
                infiniteWhileLatch = DetectInfiniteWhileLatch(currentBlockId, *block.loopLatch);
            // around a repeat, only a back-edge the code after `until` flows into is an enclosing loop; others are inner loops
            if (infiniteWhileLatch && isRepeatHeader &&
                (!block.loopExit || !CanReach(*block.loopExit, *infiniteWhileLatch, currentBlockId, {currentBlockId})))
                infiniteWhileLatch.reset();
            if (infiniteWhileLatch)
                Explain(block, "wrap inner loop in while true because enclosing latch B{} returns to this shared header", *infiniteWhileLatch);
            const size_t loopNodesStart = nodes.size();

            if (block.loopLatch.has_value()) {
                uint32_t latchIdx = block.loopLatch.value();
                uint32_t exitIdx = block.loopLatch.value();

                boost::unordered_flat_set<uint32_t> loopVisited = visited;

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
                        std::optional<uint32_t> sharedWhileLatch;
                        std::optional<uint32_t> sharedWhileExit;
                        for (uint32_t predId : block.predecessors) {
                            if (predId == latchIdx || predId >= m_currentFunction->basicBlocks.size())
                                continue;
                            const auto &pred = m_currentFunction->basicBlocks[predId];
                            if (pred.bType != BlockType::LoopLatch || pred.bTerminator != BlockTerminator::Unconditional ||
                                (pred.dwBlockFlags & LoopBlockFlags::WhileLoop) != LoopBlockFlags::WhileLoop || pred.loopHeader != currentBlockId)
                                continue;

                            uint32_t testId = InvalidBlockId;
                            for (const auto &candidate : m_currentFunction->basicBlocks) {
                                if (candidate.bTerminator != BlockTerminator::Conditional || candidate.successors.size() != 2 ||
                                    !candidate.lpTail || candidate.lpTail->instructionIndex >= pred.lpTail->instructionIndex)
                                    continue;
                                const bool firstLoops = CanReach(candidate.successors[0], predId, currentBlockId, {currentBlockId});
                                const bool secondLoops = CanReach(candidate.successors[1], predId, currentBlockId, {currentBlockId});
                                if (firstLoops == secondLoops)
                                    continue;
                                const uint32_t other = firstLoops ? candidate.successors[1] : candidate.successors[0];
                                if (other != latchIdx && !CanReach(other, latchIdx, currentBlockId, {currentBlockId, predId}))
                                    continue;
                                if (testId == InvalidBlockId || candidate.lpTail->instructionIndex >
                                                                    m_currentFunction->basicBlocks[testId].lpTail->instructionIndex) {
                                    testId = candidate.dwBlockId;
                                    sharedWhileExit = other;
                                }
                            }
                            if (testId != InvalidBlockId) {
                                sharedWhileLatch = predId;
                                Explain(block, "preserve enclosing while through shared repeat header; latch B{} exits through B{}", predId,
                                        *sharedWhileExit);
                                break;
                            }
                        }

                        m_deferToConditionInline.clear();
                        std::vector<std::pair<LiftedOperand, const LiftedInstruction *>> pendingConditionDefs;
                        for (const auto &conditionOperand : latch.lpTail->operands)
                            pendingConditionDefs.emplace_back(conditionOperand, latch.lpTail);

                        while (!pendingConditionDefs.empty()) {
                            const auto [conditionOperand, expectedUser] = pendingConditionDefs.back();
                            pendingConditionDefs.pop_back();
                            if (conditionOperand.type != LiftedOperandType::Register)
                                continue;

                            const auto *conditionDef = m_currentFunction->GetDefinition(conditionOperand);
                            if (!conditionDef || m_currentFunction->GetBlockId(conditionDef) != static_cast<int>(latch.dwBlockId))
                                continue;

                            const SSARef conditionRef{static_cast<uint8_t>(conditionOperand.value.reg), conditionOperand.ssaVersion};
                            const auto users = m_currentFunction->users.find(conditionRef);
                            if (users == m_currentFunction->users.end() || users->second.size() != 1 || users->second.front() != expectedUser)
                                continue;
                            if (InliningReordersEffect(conditionDef, expectedUser))
                                continue;
                            if (!m_deferToConditionInline.insert(conditionDef).second)
                                continue;

                            for (size_t operandIndex = 1; operandIndex < conditionDef->operands.size(); ++operandIndex)
                                pendingConditionDefs.emplace_back(conditionDef->operands[operandIndex], conditionDef);

                            if ((conditionDef->operation == LiftedOperation::CALL || conditionDef->operation == LiftedOperation::CALLFB) &&
                                !conditionDef->operands.empty() && m_currentFunction->implicitUses.contains(conditionDef)) {
                                const auto &versions = m_currentFunction->implicitUses.at(conditionDef);
                                const int32_t baseRegister = conditionDef->operands[0].value.reg + 1;
                                for (size_t argumentIndex = 0; argumentIndex < versions.size(); ++argumentIndex) {
                                    LiftedOperand argument{};
                                    argument.type = LiftedOperandType::Register;
                                    argument.value.reg = baseRegister + static_cast<int32_t>(argumentIndex);
                                    argument.ssaVersion = versions[argumentIndex];
                                    pendingConditionDefs.emplace_back(argument, conditionDef);
                                }

                                const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
                                const auto instructionIndex = conditionDef->instructionIndex;
                                if (instructionIndex >= 2 && instructions[instructionIndex - 2].operation == LiftedOperation::NAMECALL &&
                                    instructions[instructionIndex - 2].operands.size() > 1 &&
                                    instructions[instructionIndex - 2].operands[0].value.reg == conditionDef->operands[0].value.reg &&
                                    m_currentFunction->GetBlockId(&instructions[instructionIndex - 2]) == static_cast<int>(latch.dwBlockId))
                                    m_deferToConditionInline.insert(&instructions[instructionIndex - 2]);
                            }
                        }

                        std::vector<std::shared_ptr<Statement>> bodyStmts;
                        if (sharedWhileLatch && sharedWhileExit) {
                            const bool hasRepeatExit = exitIdx != InvalidBlockId && exitIdx != latchIdx;
                            if (hasRepeatExit)
                                m_loopExitStack.push_back(exitIdx);
                            auto &headerBlock = m_currentFunction->basicBlocks[currentBlockId];
                            const uint32_t savedFlags = headerBlock.dwBlockFlags;
                            const auto savedLatch = headerBlock.loopLatch;
                            const auto savedExit = headerBlock.loopExit;
                            headerBlock.dwBlockFlags = static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                            headerBlock.loopLatch = *sharedWhileLatch;
                            headerBlock.loopExit = *sharedWhileExit;

                            boost::unordered_flat_set<uint32_t> bodyVisited = loopVisited;
                            bodyVisited.erase(currentBlockId);
                            bodyVisited.insert(latchIdx);
                            bodyVisited.insert(exitIdx);
                            bodyStmts = co_await LiftControlFlow(currentBlockId, *sharedWhileExit, bodyVisited);

                            headerBlock.dwBlockFlags = savedFlags;
                            headerBlock.loopLatch = savedLatch;
                            headerBlock.loopExit = savedExit;
                            if (*sharedWhileExit != latchIdx) {
                                boost::unordered_flat_set<uint32_t> tailVisited = loopVisited;
                                tailVisited.insert(latchIdx);
                                auto tailStmts = co_await LiftControlFlow(*sharedWhileExit, latchIdx, tailVisited);
                                bodyStmts.insert(bodyStmts.end(), tailStmts.begin(), tailStmts.end());
                            }
                            if (hasRepeatExit)
                                m_loopExitStack.pop_back();
                        } else if (block.bTerminator == BlockTerminator::Conditional && latchIdx != currentBlockId) {
                            auto &headerBlock = m_currentFunction->basicBlocks[currentBlockId];
                            constexpr uint32_t kAllLoopFlags = static_cast<uint32_t>(LoopBlockFlags::WhileLoop) |
                                                               static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) |
                                                               static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
                                                               static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                                               static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed) |
                                                               static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                            const uint32_t savedFlags = headerBlock.dwBlockFlags;
                            const auto savedLatch = headerBlock.loopLatch;
                            const auto savedHeader = headerBlock.loopHeader;
                            const BlockType savedType = headerBlock.bType;
                            headerBlock.dwBlockFlags &= ~kAllLoopFlags;
                            headerBlock.loopLatch.reset();
                            headerBlock.loopHeader.reset();
                            headerBlock.bType = BlockType::IfHeader;

                            boost::unordered_flat_set<uint32_t> bodyVisited = loopVisited;
                            bodyVisited.erase(currentBlockId);
                            bodyVisited.insert(latchIdx);
                            bodyVisited.insert(exitIdx);
                            m_loopExitStack.push_back(exitIdx);
                            bodyStmts = co_await LiftControlFlow(currentBlockId, latchIdx, bodyVisited);
                            m_loopExitStack.pop_back();

                            headerBlock.dwBlockFlags = savedFlags;
                            headerBlock.loopLatch = savedLatch;
                            headerBlock.loopHeader = savedHeader;
                            headerBlock.bType = savedType;
                        } else {
                            stmts = LiftBlockInstructions(block);
                            uint32_t bodyStart = InvalidBlockId;
                            for (auto s : block.successors)
                                if (s != latchIdx && s != exitIdx) {
                                    bodyStart = s;
                                    break;
                                }
                            boost::unordered_flat_set<uint32_t> bodyVisited = loopVisited;
                            bodyVisited.insert(latchIdx);
                            bodyVisited.insert(exitIdx);
                            if (bodyStart != InvalidBlockId) {
                                m_loopExitStack.push_back(exitIdx);
                                bodyStmts = co_await LiftControlFlow(bodyStart, latchIdx, bodyVisited);
                                m_loopExitStack.pop_back();
                            }
                            bodyStmts.insert(bodyStmts.begin(), stmts.begin(), stmts.end());
                        }
                        if (latchIdx != currentBlockId) {
                            auto latchStmts = LiftBlockInstructions(latch);
                            bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                        }
                        auto condition = LiftCondition(latch.lpTail);
                        m_deferToConditionInline.clear();
                        if (!latch.ifStatementTrue.has_value() || latch.ifStatementTrue.value() != exitIdx)
                            condition = InvertCondition(condition);
                        repeatNode->condition = condition;
                        repeatNode->body = CreateBlock(bodyStmts);
                    } else {
                        // Pattern 2: latch is plain JUMP back to header. exit cond is in the header
                        // or decomposed into trailing if-return blocks (`until a or b`); lift body then scan.
                        bool infiniteHandled = false;

                        // Special case; INFINITE repeat (`until <false const>`, e.g. `until nil`): the compiler
                        // folds it to an unconditional back-edge with no real exit. If the header carries a
                        // truthiness branch, that is an INNER `if`, not the until; the generic code below would
                        // take the header's if AS the until-condition and DROP the body (losing its effects and
                        // throws). Detect it; the computed "exit" still reaches the latch, i.e. it is inside the
                        // loop; and reconstruct `repeat <header if + body> until false`, rendering the inner if.
                        // Restricted to the 1-word JUMPIF/JUMPIFNOT headers where `lpTail + 1` reliably locates
                        // the fall-through body (2-word comparison headers carry an auxiliary word).
                        if (block.lpTail && (block.lpTail->operation == LiftedOperation::JUMPIF || block.lpTail->operation == LiftedOperation::JUMPIFNOT) &&
                            exitIdx != InvalidBlockId && exitIdx < m_currentFunction->basicBlocks.size() &&
                            CanReach(exitIdx, latchIdx, currentBlockId, boost::unordered_flat_set<uint32_t>{currentBlockId})) {
                            const LiftedInstruction *fallThrough = block.lpTail + 1;
                            uint32_t bodyBlk = InvalidBlockId, skipBlk = InvalidBlockId;
                            for (auto s : block.successors) {
                                if (s < m_currentFunction->basicBlocks.size() && m_currentFunction->basicBlocks[s].lpHead == fallThrough)
                                    bodyBlk = s;
                                else
                                    skipBlk = s;
                            }
                            if (bodyBlk != InvalidBlockId) {
                                // header pre-branch statements (the condition's operands inline, not emitted here)
                                std::vector<std::shared_ptr<Statement>> bodyStmts = LiftBlockInstructions(block);
                                auto innerIf = std::make_shared<IfStatementNode>();
                                innerIf->condition = InvertCondition(LiftCondition(block.lpTail)); // body runs on the not-jump path
                                const uint32_t thenStop = (skipBlk != InvalidBlockId) ? skipBlk : latchIdx;
                                boost::unordered_flat_set<uint32_t> ifVisited = loopVisited;
                                ifVisited.insert(latchIdx);
                                ifVisited.insert(thenStop);
                                innerIf->thenBranch = CreateBlock(co_await LiftControlFlow(bodyBlk, thenStop, ifVisited));
                                bodyStmts.push_back(innerIf);
                                // continuation after the inner if (the merge = skip target) up to the latch
                                if (skipBlk != InvalidBlockId && skipBlk != latchIdx) {
                                    boost::unordered_flat_set<uint32_t> tailVisited = loopVisited;
                                    tailVisited.insert(latchIdx);
                                    auto tailStmts = co_await LiftControlFlow(skipBlk, latchIdx, tailVisited);
                                    bodyStmts.insert(bodyStmts.end(), tailStmts.begin(), tailStmts.end());
                                }
                                repeatNode->condition = std::make_shared<BooleanLiteralNode>(false); // `until false`; infinite
                                repeatNode->body = CreateBlock(bodyStmts);
                                Explain(block, "emit repeat-until-false because both header branches remain inside loop and B{} is its latch", latchIdx);
                                infiniteHandled = true;
                            }
                        }

                        if (!infiniteHandled) {
                            uint32_t bodyStart = InvalidBlockId;
                            for (auto s : block.successors) {
                                if (s != exitIdx) {
                                    bodyStart = s;
                                    break;
                                }
                            }

                            boost::unordered_flat_set<uint32_t> bodyVisited = loopVisited;
                            bodyVisited.insert(latchIdx);
                            bodyVisited.insert(exitIdx);

                            std::vector<std::shared_ptr<Statement>> bodyStmts;
                            if (bodyStart != InvalidBlockId) {
                                bodyStmts = co_await LiftControlFlow(bodyStart, latchIdx, bodyVisited);
                            }

                            // A header instruction whose ONLY consumer is this block's terminator (the
                            // until-condition, lifted separately below) must not be force-emitted as a body
                            // statement: it would ALSO be re-lifted into the condition, so an effectful def
                            // (`repeat until f()`) runs twice. Defer it so the header skips emitting it and the
                            // condition inlines it once. Restricted to sole-terminator use; header values that
                            // also flow out of the loop still get force-materialized (why `forceDefinitions`
                            // exists). NOT the same as marking it processed: that would make the condition
                            // reference it by name (a register alias, dropping the call), not inline it.
                            m_deferToConditionInline.clear();
                            if (block.lpTail) {
                                std::vector<std::pair<LiftedOperand, const LiftedInstruction *>> pendingConditionDefs;
                                for (const auto &conditionOperand : block.lpTail->operands)
                                    pendingConditionDefs.emplace_back(conditionOperand, block.lpTail);

                                while (!pendingConditionDefs.empty()) {
                                    const auto [conditionOperand, expectedUser] = pendingConditionDefs.back();
                                    pendingConditionDefs.pop_back();
                                    if (conditionOperand.type != LiftedOperandType::Register)
                                        continue;
                                    const auto *conditionDef = m_currentFunction->GetDefinition(conditionOperand);
                                    if (!conditionDef || m_currentFunction->GetBlockId(conditionDef) != static_cast<int>(block.dwBlockId))
                                        continue;
                                    // A loop-carried register must stay materialized: `x = x - 1 until x <= 0`
                                    // reads x next iteration, so dropping the store breaks the loop. This single-
                                    // block repeat has no phi nodes and the back-edge read is not in `users`, so
                                    // neither is a reliable signal. The robust test is live-in: if this register
                                    // is READ before it is written within the header, its value flows in from a
                                    // previous iteration and must be stored. `repeat until f()` writes its result
                                    // register fresh (GETIMPORT/CALL) before any read, so it is dead on entry and
                                    // safe to inline once into the condition.
                                    const uint8_t reg = static_cast<uint8_t>(conditionOperand.value.reg);
                                    bool liveInToHeader = false;
                                    for (const LiftedInstruction *hp = block.lpHead; hp && hp <= block.lpTail && !liveInToHeader; ++hp) {
                                        for (size_t oi = 0; oi < hp->operands.size(); ++oi) {
                                            if (hp->operands[oi].type != LiftedOperandType::Register || hp->operands[oi].value.reg != reg)
                                                continue;
                                            const AccessType acc = SSABuilder::GetRegisterAccess(*hp, oi);
                                            if (acc == AccessType::Read || acc == AccessType::ReadWrite)
                                                liveInToHeader = true; // read before any write -> flows in from prior iteration
                                            break;                     // first access to `reg` in this instruction decides
                                        }
                                        if (hp->operands.size() && hp->operands[0].type == LiftedOperandType::Register && hp->operands[0].value.reg == reg &&
                                            SSABuilder::GetRegisterAccess(*hp, 0) == AccessType::Write)
                                            break; // first write to `reg` reached without a prior read -> not live-in
                                    }
                                    if (liveInToHeader)
                                        continue;
                                    const SSARef conditionRef{reg, conditionOperand.ssaVersion};
                                    const auto users = m_currentFunction->users.find(conditionRef);
                                    if (users == m_currentFunction->users.end() || users->second.size() != 1 || users->second.front() != expectedUser)
                                        continue;
                                    if (InliningReordersEffect(conditionDef, expectedUser))
                                        continue;
                                    if (!m_deferToConditionInline.insert(conditionDef).second)
                                        continue;

                                    for (size_t operandIndex = 1; operandIndex < conditionDef->operands.size(); ++operandIndex)
                                        pendingConditionDefs.emplace_back(conditionDef->operands[operandIndex], conditionDef);

                                    if ((conditionDef->operation == LiftedOperation::CALL || conditionDef->operation == LiftedOperation::CALLFB) &&
                                        !conditionDef->operands.empty() && m_currentFunction->implicitUses.contains(conditionDef)) {
                                        const auto &versions = m_currentFunction->implicitUses.at(conditionDef);
                                        const int32_t baseRegister = conditionDef->operands[0].value.reg + 1;
                                        for (size_t argumentIndex = 0; argumentIndex < versions.size(); ++argumentIndex) {
                                            LiftedOperand argument{};
                                            argument.type = LiftedOperandType::Register;
                                            argument.value.reg = baseRegister + static_cast<int32_t>(argumentIndex);
                                            argument.ssaVersion = versions[argumentIndex];
                                            pendingConditionDefs.emplace_back(argument, conditionDef);
                                        }

                                        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
                                        const auto instructionIndex = conditionDef->instructionIndex;
                                        if (instructionIndex >= 2 && instructions[instructionIndex - 2].operation == LiftedOperation::NAMECALL &&
                                            instructions[instructionIndex - 2].operands.size() > 1 &&
                                            instructions[instructionIndex - 2].operands[0].value.reg == conditionDef->operands[0].value.reg &&
                                            m_currentFunction->GetBlockId(&instructions[instructionIndex - 2]) == static_cast<int>(block.dwBlockId) &&
                                            !InliningReordersEffect(&instructions[instructionIndex - 2], conditionDef))
                                            m_deferToConditionInline.insert(&instructions[instructionIndex - 2]);
                                    }
                                }
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
                            m_deferToConditionInline.clear();

                            repeatNode->body = CreateBlock(bodyStmts);
                        }
                    }
                    nodes.push_back(repeatNode);
                    // Preserve repeat-body definitions until declaration hoisting handles escaping values.
                } else if ((block.dwBlockFlags & LoopBlockFlags::ForNumericLoop) == LoopBlockFlags::ForNumericLoop) {
                    // a numeric-for's body is the FORNPREP fall-through; the other successor (the prep's
                    // jump target) is the skip/exit taken when the range is empty. picking "any successor
                    // != latch" breaks an EMPTY-bodied for: the fall-through IS the FORNLOOP latch, so that
                    // heuristic grabs the skip-target instead and lifts whatever follows the loop as its
                    // body (mis-nesting + a missing `end`). use the fall-through; if it is the latch the
                    // body is correctly empty.
                    uint32_t bodyIdx = InvalidBlockId;
                    const LiftedInstruction *fallThrough = block.lpTail + 1;
                    for (auto succ : block.successors) {
                        if (succ < m_currentFunction->basicBlocks.size() && m_currentFunction->basicBlocks[succ].lpHead == fallThrough) {
                            bodyIdx = succ;
                            break;
                        }
                    }
                    if (bodyIdx == InvalidBlockId) // defensive: malformed header without a fall-through successor
                        for (auto succ : block.successors) {
                            if (succ != block.loopLatch.value_or(InvalidBlockId)) {
                                bodyIdx = succ;
                                break;
                            }
                        }
                    if (block.loopExit.has_value() && block.loopExit.value() != bodyIdx && block.loopExit.value() != block.loopLatch.value_or(InvalidBlockId))
                        exitIdx = block.loopExit.value();
                    else if (block.loopLatch.has_value()) {
                        uint32_t resolved = ResolveLoopExitFromLatch(m_currentFunction->basicBlocks, *block.loopLatch, block.dwBlockId);
                        boost::unordered_flat_set<uint32_t> exitChain;
                        while (resolved < m_currentFunction->basicBlocks.size()) {
                            if (!exitChain.insert(resolved).second) {
                                resolved = InvalidBlockId;
                                break;
                            }
                            const auto &candidate = m_currentFunction->basicBlocks[resolved];
                            if (candidate.bTerminator != BlockTerminator::Unconditional || !candidate.lpTail || candidate.lpHead != candidate.lpTail ||
                                candidate.lpTail->operation != LiftedOperation::JUMP || candidate.successors.size() != 1)
                                break;
                            resolved = candidate.successors.front();
                        }
                        if (resolved != InvalidBlockId)
                            exitIdx = resolved;
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
                    const int32_t loopVariableReg = baseReg + 2;
                    const bool loopVariableWasDefined = m_definedRegisters.contains(loopVariableReg);
                    const std::string startValueName = m_currentFunction->GetVarName(loopVariableReg, startVer);
                    // name the loop var once up-front so body/header/SSA map agree. LiftExpression would
                    // resolve it to the inlined LOADN constant -> broken `for 1 = 1, ..., 1 do` (InfiniteYield).
                    const std::string loopVarName = std::format("i_{}", baseReg + 2);
                    {
                        LiftedOperand op;
                        {
                            PinnedRegisterScope pin(this, {baseReg + 2, startVer});

                            this->m_currentFunction->SetVariableName(baseReg + 2, startVer, loopVarName);
                            // expose this loop's exit so body branches to it become `break` (real exit only).
                            const bool hasBreakTarget = (exitIdx != InvalidBlockId && exitIdx != latchIdx && exitIdx != bodyIdx);
                            if (hasBreakTarget)
                                m_loopExitStack.push_back(exitIdx);
                            const auto definedBeforeLoopBody = m_definedRegisters;
                            forNode->lpLoopBody = CreateBlock(
                                co_await LiftControlFlow(
                                    bodyIdx, *block.loopLatch,
                                    visited
                                )
                            );
                            m_definedRegisters = definedBeforeLoopBody;
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
                            m_currentFunction->SetVariableName(loopVariableReg, startVer, startValueName);

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
                                k.ssaVersion = v;
                                return k;
                            };
                            int32_t effectiveVer = ver;
                            auto *def = m_currentFunction->GetDefinition(makeKey(reg, ver));
                            const bool headerPhi = std::ranges::any_of(block.phiNodes, [&](const LiftedInstruction &phi) { return &phi == def; });
                            if (headerPhi && def->operands.size() >= 2) {
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
                                    const auto &k = ConstantAt(valOp.value.imm.k);
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
                                    case LUA_TVECTOR:
                                        return LiftVectorConstant(k);
                                    default:
                                        return std::make_shared<NilLiteralNode>();
                                    }
                                }
                            }
                            LiftedOperand lop;
                            lop.type = LiftedOperandType::Register;
                            lop.value.reg = reg;
                            lop.ssaVersion = effectiveVer;
                            // The start/limit/step expression is materialised into the `for i = a, b, c`
                            // header here. If its def is an effectful CALL (`for i = 1, f(), 1`), that call
                            // is ALSO in the pre-header block and would be emitted a second time as its own
                            // statement; running its side effects twice. Mark it processed so the block
                            // lifter skips it. Restricted to single-use calls: a pure read is idempotent
                            // (no double-effect to suppress) and marking one processed drops a declaration a
                            // later use needs (regressing decl-placement / forward references).
                            // The bound may be the call directly, or a `MOVE Rbound, Rcall` copy of it (the
                            // compiler stages start/limit/step into consecutive registers). Follow the MOVE
                            // chain to the underlying effectful call so it is suppressed at its real site.
                            const LiftedInstruction *rootDef = effDef;
                            for (int g = 0; g < 8 && rootDef && rootDef->operation == LiftedOperation::MOVE && rootDef->operands.size() >= 2 &&
                                            rootDef->operands[1].type == LiftedOperandType::Register;
                                 ++g) {
                                const auto *nxt = m_currentFunction->GetDefinition(rootDef->operands[1]);
                                if (!nxt || nxt == rootDef)
                                    break;
                                rootDef = nxt;
                            }
                            // The bound may be a call, or a `MOVE Rbound, Rcall` copy of one, inlined into the
                            // `for i = a, b, c` header. If that call is also emitted as its own statement it runs
                            // twice, doubling its side effects. Suppress the statement, but ONLY for effectful
                            // calls whose result feeds nothing but the for-header (FOR*PREP/FOR*LOOP re-read the
                            // bound each iteration, plus a staging MOVE); a use elsewhere needs the local, and
                            // suppressing plain reads/other defs regresses declaration placement.
                            const bool effectfulCall =
                                rootDef && (rootDef->operation == LiftedOperation::CALL || rootDef->operation == LiftedOperation::CALLFB ||
                                            rootDef->operation == LiftedOperation::NAMECALL || rootDef->operation == LiftedOperation::NAMECALLUDATA);
                            if (effectfulCall && !rootDef->operands.empty() && rootDef->operands[0].type == LiftedOperandType::Register) {
                                bool safe = m_currentFunction->IsSingleUse(rootDef->operands[0]);
                                if (!safe) {
                                    safe = true;
                                    if (auto it = m_currentFunction->users.find(
                                            SSARef{static_cast<uint8_t>(rootDef->operands[0].value.reg), rootDef->operands[0].ssaVersion}
                                        );
                                        it != m_currentFunction->users.end())
                                        for (const auto *u : it->second) {
                                            const auto uo = u->operation;
                                            if (uo != LiftedOperation::MOVE && uo != LiftedOperation::FORNPREP && uo != LiftedOperation::FORNLOOP &&
                                                uo != LiftedOperation::FORGPREP && uo != LiftedOperation::FORGPREP_NEXT &&
                                                uo != LiftedOperation::FORGPREP_INEXT && uo != LiftedOperation::FORGLOOP) {
                                                safe = false;
                                                break;
                                            }
                                        }
                                }
                                if (safe)
                                    m_processedInstructions.insert(rootDef->instructionIndex);
                            }
                            // Same hazard for a NON-call computed bound (index/arith/`not <read>`): when its def
                            // lives in an already-lifted pre-header block it is emitted THERE as `local vN = <expr>`,
                            // and force-inlining it into the `for i = <expr>, ...` header would render a second copy
                            // whose index/arith metamethod re-runs (SEM_DIVERGE for-bound cluster). Mark it processed
                            // so the header references that local by name (this runs AFTER the pre-header emit, so the
                            // decl survives; unlike the FORNPREP pre-pass, which would suppress it and dangle). Gated
                            // on: raising read / `not`, emitted as a statement (!ShouldInline), sitting OUTSIDE the
                            // header block, and feeding nothing but loop machinery (a staging MOVE, prep/loop reads,
                            // or this loop's own variable phi). A single-block bound (`for i = 1, #t`) stays inlined.
                            if (!effectfulCall && rootDef && (CanOperationRaise(rootDef->operation) || rootDef->operation == LiftedOperation::NOT) &&
                                !rootDef->operands.empty() && rootDef->operands[0].type == LiftedOperandType::Register && block.lpHead && block.lpTail &&
                                (rootDef->instructionIndex < block.lpHead->instructionIndex || rootDef->instructionIndex > block.lpTail->instructionIndex) &&
                                !ShouldInline(rootDef)) {
                                bool safe = true;
                                if (auto it = m_currentFunction->users.find(
                                        SSARef{static_cast<uint8_t>(rootDef->operands[0].value.reg), rootDef->operands[0].ssaVersion}
                                    );
                                    it != m_currentFunction->users.end())
                                    for (const auto *u : it->second) {
                                        const auto uo = u->operation;
                                        if (uo != LiftedOperation::MOVE && uo != LiftedOperation::PHI && uo != LiftedOperation::FORNPREP &&
                                            uo != LiftedOperation::FORNLOOP && uo != LiftedOperation::FORGPREP && uo != LiftedOperation::FORGPREP_NEXT &&
                                            uo != LiftedOperation::FORGPREP_INEXT && uo != LiftedOperation::FORGLOOP) {
                                            safe = false;
                                            break;
                                        }
                                    }
                                else
                                    safe = false;
                                if (safe)
                                    m_processedInstructions.insert(rootDef->instructionIndex);
                            }
                            return LiftExpression(lop);
                        };

                        forNode->startVariable = liftLoopValue(baseReg + 2, startVer);
                        forNode->maxIncreased = liftLoopValue(baseReg, limitVer);
                        forNode->increaseBy = liftLoopValue(baseReg + 1, stepVer);
                    }
                    nodes.insert(nodes.end(), stmts.begin(), stmts.end());
                    nodes.push_back(forNode);
                    if (!loopVariableWasDefined)
                        m_definedRegisters.erase(loopVariableReg);
                } else if ((block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop) {
                    auto whileNode = std::make_shared<WhileStatementNode>();
                    const bool infiniteWhile = block.bTerminator != BlockTerminator::Conditional;

                    if (block.loopExit.has_value())
                        exitIdx = block.loopExit.value();
                    if (infiniteWhile)
                        exitIdx = latchIdx;

                    // A header diamond can precede the actual exit test, including across a nested for-loop.
                    bool compoundHandled = false;
                    const auto &bb2 = m_currentFunction->basicBlocks;
                    // follow single-successor (pass-through) blocks from an arm to the first branching block.
                    auto followToBranch = [&](uint32_t start) -> uint32_t {
                        uint32_t cur = start;
                        for (int guard = 0; guard < 8; ++guard) {
                            if (cur >= bb2.size() || cur >= latchIdx || cur <= currentBlockId)
                                return InvalidBlockId;
                            const auto &nested = bb2[cur];
                            if (nested.loopLatch && nested.lpTail &&
                                (nested.lpTail->operation == LiftedOperation::FORNPREP || nested.lpTail->operation == LiftedOperation::FORGPREP ||
                                 nested.lpTail->operation == LiftedOperation::FORGPREP_NEXT || nested.lpTail->operation == LiftedOperation::FORGPREP_INEXT)) {
                                cur = ResolveLoopExitFromLatch(bb2, *nested.loopLatch, cur);
                                continue;
                            }
                            if (bb2[cur].successors.size() != 1)
                                return cur;
                            cur = bb2[cur].successors[0];
                        }
                        return InvalidBlockId;
                    };
                    auto reachesLatch = [&](uint32_t start) -> bool {
                        std::set<uint32_t> seen;
                        std::queue<uint32_t> q;
                        q.push(start);
                        while (!q.empty()) {
                            const uint32_t n = q.front();
                            q.pop();
                            if (n == latchIdx)
                                return true;
                            if (n == currentBlockId || n >= bb2.size() || seen.count(n))
                                continue;
                            seen.insert(n);
                            for (uint32_t s : bb2[n].successors)
                                q.push(s);
                        }
                        return false;
                    };
                    // a header that does not exit itself (`while a or b`) lifts as `while true` whose exit edges break
                    const bool deferredExit = deferredWhileHeader && block.loopExit && *block.loopExit != latchIdx && *block.loopExit != currentBlockId;
                    if (deferredWhileHeader && (!block.loopExit || deferredExit)) {
                        whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                        auto &headerBlock = m_currentFunction->basicBlocks[currentBlockId];
                        constexpr uint32_t kAllLoopFlags = static_cast<uint32_t>(LoopBlockFlags::WhileLoop) |
                                                           static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) |
                                                           static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
                                                           static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                                           static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed) |
                                                           static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        const uint32_t savedFlags = headerBlock.dwBlockFlags;
                        const auto savedLatch = headerBlock.loopLatch;
                        const auto savedHeader = headerBlock.loopHeader;
                        const BlockType savedType = headerBlock.bType;
                        headerBlock.dwBlockFlags &= ~kAllLoopFlags;
                        headerBlock.loopLatch.reset();
                        headerBlock.loopHeader.reset();
                        headerBlock.bType = BlockType::IfHeader;

                        boost::unordered_flat_set<uint32_t> bodyVisited = visited;
                        bodyVisited.erase(currentBlockId);
                        bodyVisited.insert(latchIdx);
                        if (deferredExit) {
                            bodyVisited.insert(*block.loopExit);
                            m_loopExitStack.push_back(*block.loopExit);
                        }
                        auto bodyStmts = co_await LiftControlFlow(currentBlockId, latchIdx, bodyVisited);
                        if (deferredExit) {
                            m_loopExitStack.pop_back();
                            exitIdx = *block.loopExit;
                        }
                        if (latchIdx != currentBlockId) {
                            auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                            bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                        }

                        headerBlock.dwBlockFlags = savedFlags;
                        headerBlock.loopLatch = savedLatch;
                        headerBlock.loopHeader = savedHeader;
                        headerBlock.bType = savedType;
                        whileNode->body = CreateBlock(bodyStmts);
                        nodes.push_back(whileNode);
                        compoundHandled = true;
                    } else if (block.bTerminator == BlockTerminator::Conditional && block.successors.size() == 2) {
                        const uint32_t armA = block.successors[0];
                        const uint32_t armB = block.successors[1];
                        const uint32_t common = FindMergeBlock(armA, armB);
                        uint32_t mergeM = common != InvalidBlockId && common > currentBlockId && common < latchIdx ? followToBranch(common) : InvalidBlockId;
                        if (mergeM != InvalidBlockId) {
                            if (mergeM < bb2.size() && bb2[mergeM].bTerminator == BlockTerminator::Conditional && bb2[mergeM].successors.size() == 2) {
                                uint32_t realExit = InvalidBlockId;
                                for (uint32_t s : bb2[mergeM].successors)
                                    if (!reachesLatch(s)) {
                                        realExit = s;
                                        break;
                                    }
                                const bool exitIsHeaderSucc = realExit != InvalidBlockId &&
                                                              std::find(block.successors.begin(), block.successors.end(), realExit) != block.successors.end();
                                if (realExit != InvalidBlockId && !exitIsHeaderSucc) {
                                    whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                                    auto &hdr = m_currentFunction->basicBlocks[currentBlockId];
                                    if (deferredWhileHeader)
                                        stmts = LiftBlockInstructions(hdr);
                                    constexpr uint32_t kAllLoopFlags =
                                        static_cast<uint32_t>(LoopBlockFlags::WhileLoop) | static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) |
                                        static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                        static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed) | static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                                    const uint32_t savedFlags = hdr.dwBlockFlags;
                                    const auto savedLatch = hdr.loopLatch;
                                    const auto savedHeader = hdr.loopHeader;
                                    const BlockType savedType = hdr.bType;
                                    auto *savedHead = hdr.lpHead;
                                    hdr.dwBlockFlags &= ~kAllLoopFlags;
                                    hdr.loopLatch.reset();
                                    hdr.loopHeader.reset();
                                    hdr.bType = BlockType::IfHeader;
                                    hdr.lpHead = hdr.lpTail;

                                    boost::unordered_flat_set<uint32_t> cflow = visited;
                                    cflow.erase(currentBlockId);
                                    cflow.insert(latchIdx);
                                    cflow.insert(realExit);
                                    m_loopExitStack.push_back(realExit);
                                    auto bodyStmts = co_await LiftControlFlow(currentBlockId, latchIdx, cflow);
                                    m_loopExitStack.pop_back();
                                    bodyStmts.insert(bodyStmts.begin(), stmts.begin(), stmts.end());
                                    if (latchIdx != currentBlockId) {
                                        auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                                        bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                                    }
                                    hdr.dwBlockFlags = savedFlags;
                                    hdr.loopLatch = savedLatch;
                                    hdr.loopHeader = savedHeader;
                                    hdr.bType = savedType;
                                    hdr.lpHead = savedHead;

                                    whileNode->body = CreateBlock(bodyStmts);
                                    nodes.push_back(whileNode);
                                    exitIdx = realExit; // common loop-tail below sets nextBlockId from exitIdx
                                    compoundHandled = true;
                                }
                            }
                        }
                    }

                    if (!compoundHandled) {
                        if (deferredWhileHeader)
                            stmts = LiftBlockInstructions(block);
                        uint32_t bodyStart = InvalidBlockId;
                        for (auto s : block.successors)
                            if (infiniteWhile || s != exitIdx)
                                bodyStart = s;

                        std::shared_ptr<Expression> whileOrCondition;
                        if (stmts.empty() && bodyStart != InvalidBlockId && exitIdx < bb2.size() && block.ifStatementTrue.has_value() &&
                            block.ifStatementFalse.has_value()) {
                            std::vector<uint32_t> rhsBlocks;
                            std::set<uint32_t> guard;
                            uint32_t rhsIdx = exitIdx;
                            while (rhsIdx < bb2.size() && guard.insert(rhsIdx).second) {
                                const auto &rhs = bb2[rhsIdx];
                                const bool link = rhs.bType == BlockType::IfHeader && rhs.bTerminator == BlockTerminator::Conditional && rhs.lpTail &&
                                                  rhs.ifStatementTrue.has_value() && rhs.ifStatementFalse.has_value() &&
                                                  (rhs.ifStatementTrue.value() == bodyStart || rhs.ifStatementFalse.value() == bodyStart);
                                bool clean = link;
                                for (auto *inst = rhs.lpHead; clean && inst && inst < rhs.lpTail; ++inst)
                                    clean = inst->operation == LiftedOperation::NOP || inst->operation == LiftedOperation::PHI || ShouldInline(inst);
                                if (!clean) {
                                    if (link)
                                        rhsBlocks.clear();
                                    break;
                                }

                                rhsBlocks.push_back(rhsIdx);
                                rhsIdx = rhs.ifStatementTrue.value() == bodyStart ? rhs.ifStatementFalse.value() : rhs.ifStatementTrue.value();
                            }
                            if (!rhsBlocks.empty() && rhsIdx < bb2.size() && std::find(rhsBlocks.begin(), rhsBlocks.end(), rhsIdx) == rhsBlocks.end()) {
                                auto condition =
                                    block.ifStatementTrue.value() == bodyStart ? LiftCondition(block.lpTail) : InvertCondition(LiftCondition(block.lpTail));
                                for (uint32_t id : rhsBlocks) {
                                    const auto &rhs = bb2[id];
                                    auto term =
                                        rhs.ifStatementTrue.value() == bodyStart ? LiftCondition(rhs.lpTail) : InvertCondition(LiftCondition(rhs.lpTail));
                                    condition = std::make_shared<BinaryExpressionNode>("or", condition, term);
                                }
                                whileOrCondition = condition;
                                visited.insert(rhsBlocks.begin(), rhsBlocks.end());
                                exitIdx = rhsIdx;
                            }
                        }

                        // header with stmts before its exit test: folding the test into the cond would reorder
                        // it ahead of them. keep `while true` and emit the test as a `break` at its real spot.
                        const bool headerHasBody = !stmts.empty();
                        // a header with no conditional exit (unconditional back-edge, or fallthrough into
                        // the body) has no test == infinite `while true`; LiftCondition on its
                        // non-comparison tail would yield `false`, rendering `while not false`.
                        if (whileOrCondition)
                            whileNode->condition = whileOrCondition;
                        else if (headerHasBody || infiniteWhile)
                            whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                        else if (block.ifStatementTrue.has_value() && block.ifStatementTrue.value() == bodyStart)
                            whileNode->condition = LiftCondition(block.lpTail);
                        else
                            whileNode->condition = InvertCondition(LiftCondition(block.lpTail));

                        if (bodyStart != InvalidBlockId) {
                            boost::unordered_flat_set<uint32_t> cloopVisited = visited;
                            cloopVisited.insert(latchIdx);
                            cloopVisited.insert(exitIdx);
                            const bool hasBreakTarget = (exitIdx != InvalidBlockId && exitIdx != latchIdx && exitIdx != bodyStart);
                            if (hasBreakTarget)
                                m_loopExitStack.push_back(exitIdx);
                            auto bodyStmts = co_await LiftControlFlow(bodyStart, latchIdx, cloopVisited);
                            if (hasBreakTarget)
                                m_loopExitStack.pop_back();

                            if (latchIdx != currentBlockId) {
                                auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                                bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                            }

                            if (headerHasBody) {
                                // header body -> `if <exit cond> then break end` -> rest of body.
                                auto breakIf = std::make_shared<IfStatementNode>();
                                const bool exitOnFalse = block.ifStatementFalse.has_value() && block.ifStatementFalse.value() == exitIdx;
                                breakIf->condition = exitOnFalse ? InvertCondition(LiftCondition(block.lpTail)) : LiftCondition(block.lpTail);
                                breakIf->thenBranch = CreateBlock({std::make_shared<BreakStatementNode>()});
                                bodyStmts.insert(bodyStmts.begin(), breakIf);
                            }
                            bodyStmts.insert(bodyStmts.begin(), stmts.begin(), stmts.end());
                            whileNode->body = CreateBlock(bodyStmts);
                        } else {
                            whileNode->body = CreateBlock(stmts);
                        }
                        nodes.push_back(whileNode);
                    }
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
                        const uint32_t resolved = ResolveLoopExitFromLatch(m_currentFunction->basicBlocks, *block.loopLatch, block.dwBlockId);
                        if (resolved != InvalidBlockId)
                            exitIdx = resolved;
                    }

                    auto forNode = std::make_shared<ForGeneralNode>();
                    // a well-formed generic-for has a FORGPREP* header tail (>=1 operand: the base register)
                    // and a FORGLOOP latch tail (>=3 operands: base, count, numVars). A malformed CFG can
                    // pair this header with a latch whose tail is neither; reading those operands would be
                    // out of bounds, so drop the sample.
                    const LiftedInstruction *latchTail = (block.loopLatch.has_value() && *block.loopLatch < m_currentFunction->basicBlocks.size())
                                                             ? m_currentFunction->basicBlocks[*block.loopLatch].lpTail
                                                             : nullptr;
                    if (!block.lpTail || block.lpTail->operands.empty() || !latchTail || latchTail->operands.size() < 3)
                        throw Fission::DecompilerError("malformed bytecode: generic-for without a well-formed FORGPREP/FORGLOOP pair");
                    int baseReg = block.lpTail->operands[0].value.reg;
                    int numVars = latchTail->operands[2].value.imm.n & 0xFF;
                    boost::unordered_flat_set<int32_t> definedLoopVariables;
                    for (int i = 0; i < numVars; ++i)
                        if (m_definedRegisters.contains(baseReg + 3 + i))
                            definedLoopVariables.insert(baseReg + 3 + i);

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

                        const bool hasBreakTarget = (exitIdx != InvalidBlockId && exitIdx != block.loopLatch.value_or(InvalidBlockId) && exitIdx != bodyIdx);
                        if (hasBreakTarget)
                            m_loopExitStack.push_back(exitIdx);
                        const auto definedBeforeLoopBody = m_definedRegisters;
                        // the loop variables are bound by the `for`; a write in the body assigns them
                        for (int i = 0; i < numVars; ++i)
                            m_definedRegisters.insert(baseReg + 3 + i);
                        forNode->body = CreateBlock(co_await LiftControlFlow(bodyIdx, *block.loopLatch, visited));
                        m_definedRegisters = definedBeforeLoopBody;
                        if (hasBreakTarget)
                            m_loopExitStack.pop_back();

                        for (int i = 0; i < numVars; ++i) {
                            LiftedOperand varOp;
                            varOp.type = LiftedOperandType::Register;
                            varOp.value.reg = baseReg + 3 + i;
                            if (const auto defs = m_defsByInstruction.find(latchTail); defs != m_defsByInstruction.end())
                                for (const auto &ref : defs->second)
                                    if (ref.regIndex == varOp.value.reg) {
                                        varOp.ssaVersion = ref.version;
                                        break;
                                    }
                            // a loop variable is a fresh per-iteration binding; always its own name, never
                            // LiftExpression (which inlines a reused register's value -> `for <expr> in ...`,
                            // a syntax error). Mirrors the numeric-for loop-variable handling above.
                            forNode->loopVariables.push_back(
                                std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(varOp)))
                            );
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
                            // e.g. for k,v in pairs(t) do; the call returns 3 values
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
                            if (std::dynamic_pointer_cast<NilLiteralNode>(forNode->state) && std::dynamic_pointer_cast<NilLiteralNode>(forNode->index)) {
                                forNode->state = nullptr;
                                forNode->index = nullptr;
                            }
                        }
                    }
                    nodes.insert(nodes.end(), stmts.begin(), stmts.end());
                    nodes.push_back(forNode);
                    for (int i = 0; i < numVars; ++i)
                        if (!definedLoopVariables.contains(baseReg + 3 + i))
                            m_definedRegisters.erase(baseReg + 3 + i);
                }

            // A chosen "exit" with no path beyond the loop is actually inside the loop body.
            bool exitInBody = false;
            if (exitIdx != InvalidBlockId && exitIdx < m_currentFunction->basicBlocks.size() && block.loopLatch.has_value()) {
                bool hasLoopBack = false;
                bool hasPostLoopPath = false;
                for (uint32_t s : m_currentFunction->basicBlocks[exitIdx].successors)
                    if (s == block.loopLatch.value() || s == currentBlockId)
                        hasLoopBack = true;
                    else
                        hasPostLoopPath = true;
                exitInBody = hasLoopBack && !hasPostLoopPath;
            }
                nextBlockId = exitInBody ? InvalidBlockId : exitIdx;
            }

            if (sharedOuterRepeatCondition && sharedOuterRepeatLatch && sharedOuterRepeatExit && nodes.size() > loopNodesStart) {
                std::vector<std::shared_ptr<Statement>> loopBody(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
                if (nextBlockId != InvalidBlockId && nextBlockId != *sharedOuterRepeatCondition) {
                    auto tailVisited = visited;
                    tailVisited.insert(*sharedOuterRepeatLatch);
                    tailVisited.insert(*sharedOuterRepeatExit);
                    m_loopExitStack.push_back(*sharedOuterRepeatExit);
                    auto tail = co_await LiftControlFlow(nextBlockId, *sharedOuterRepeatCondition, tailVisited);
                    m_loopExitStack.pop_back();
                    loopBody.insert(loopBody.end(), tail.begin(), tail.end());
                }

                auto &conditionBlock = m_currentFunction->basicBlocks[*sharedOuterRepeatCondition];
                auto conditionStatements = LiftBlockInstructions(conditionBlock);
                loopBody.insert(loopBody.end(), conditionStatements.begin(), conditionStatements.end());
                auto condition = LiftCondition(conditionBlock.lpTail);
                if (!conditionBlock.ifStatementTrue || *conditionBlock.ifStatementTrue != *sharedOuterRepeatExit)
                    condition = InvertCondition(condition);

                nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
                auto repeatNode = std::make_shared<RepeatStatementNode>();
                repeatNode->body = CreateBlock(loopBody);
                repeatNode->condition = condition;
                nodes.push_back(repeatNode);
                visited.insert(*sharedOuterRepeatCondition);
                visited.insert(*sharedOuterRepeatLatch);
                nextBlockId = *sharedOuterRepeatExit;
            }

            if (infiniteWhileLatch.has_value() && nodes.size() > loopNodesStart) {
                // include any post-inner-loop tail that flows back to the outer back-edge: that's the
                // until-check of a run-once `repeat ... until <truthy>` wrapping the inner loop, lifted
                // as a trailing if-return or fall-through to the latch. without this the wrap looks
                // genuinely infinite and the until-check/return is silently dropped.
                std::vector<std::shared_ptr<Statement>> tail;
                // a tail branch that can no longer reach the back-edge leaves the wrap (a `repeat` test lifted as this shape)
                uint32_t wrapExit = InvalidBlockId;
                if (nextBlockId != InvalidBlockId && nextBlockId < m_currentFunction->basicBlocks.size() && nextBlockId != *infiniteWhileLatch) {
                    boost::unordered_flat_set<uint32_t> seen{nextBlockId};
                    std::vector<uint32_t> pending{nextBlockId};
                    while (!pending.empty() && wrapExit == InvalidBlockId && seen.size() < 256) {
                        const uint32_t id = pending.back();
                        pending.pop_back();
                        for (const uint32_t succ : m_currentFunction->basicBlocks[id].successors) {
                            if (succ == *infiniteWhileLatch || succ == currentBlockId || succ >= m_currentFunction->basicBlocks.size() || !seen.insert(succ).second)
                                continue;
                            if (m_currentFunction->basicBlocks[succ].bType != BlockType::Return &&
                                !CanReach(succ, *infiniteWhileLatch, currentBlockId, {currentBlockId})) {
                                wrapExit = succ;
                                break;
                            }
                            pending.push_back(succ);
                        }
                    }
                    if (wrapExit != InvalidBlockId)
                        m_loopExitStack.push_back(wrapExit);
                    auto tailVisited = visited;
                    tail = co_await LiftControlFlow(nextBlockId, *infiniteWhileLatch, tailVisited);
                    if (wrapExit != InvalidBlockId)
                        m_loopExitStack.pop_back();
                }
                std::vector<std::shared_ptr<Statement>> loopBody(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
                loopBody.insert(loopBody.end(), tail.begin(), tail.end());
                nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(loopNodesStart), nodes.end());
                auto whileNode = std::make_shared<WhileStatementNode>();
                whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                whileNode->body = CreateBlock(loopBody);
                nodes.push_back(whileNode);
                nextBlockId = wrapExit;
            }
            break;
        }
        case BlockType::Break:
            Explain(block, "emit break for edge to B{}", block.successors.empty() ? InvalidBlockId : block.successors.front());
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
        atEntryBlock = false; // past the entry: any further visited block is a real convergence.
        currentBlockId = nextBlockId;
    }

    // Fold consecutive `X = expr; X = X or expr2` pairs at this level so chains
    // (`a or b or c`) collapse end-to-end as control flow is unwound bottom-up.
    FoldShortCircuitChain(nodes);

    co_return nodes;
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftBlockInstructions(const BasicBlock &block, bool forceDefinitions) {
    std::vector<std::shared_ptr<Statement>> statements;
    if (!block.lpHead)
        return statements;

    const auto activeLocalName = [&](const LiftedInstruction &instruction, const LiftedOperand &target) -> std::optional<std::string> {
        for (const auto &local : m_currentFunction->lpLiftedFunction->lpDeserialized->locvars)
            if (local.reg == target.value.reg && local.startpc <= instruction.instructionIndex && instruction.instructionIndex < local.endpc &&
                !local.varname.empty())
                return local.varname;
        return std::nullopt;
    };

    for (int i = block.lpHead->instructionIndex; i <= block.lpTail->instructionIndex; ++i) {
        if (m_processedInstructions.contains(i))
            continue;

        const auto &inst = m_currentFunction->lpLiftedFunction->instructions[i];

        if (inst.operation == LiftedOperation::NEWCLASS && inst.operands.size() >= 4) {
            const auto &shapeConst = ConstantAt(inst.operands[3].value.imm.k);
            if (!shapeConst.IsClassShape())
                throw Fission::DecompilerError("malformed NEWCLASS: AUX is not a class-shape constant");
            const auto &shape = std::get<LuauClassShape>(shapeConst.constantData);
            auto classNode =
                std::make_shared<ClassDeclarationNode>(shape.className, shape.propertyNames, std::vector<std::shared_ptr<FunctionDeclarationNode>>{});
            classNode->bOpen = inst.operands[2].type == LiftedOperandType::ImmediateBool && inst.operands[2].value.imm.b;
            if (inst.operands[1].type == LiftedOperandType::Register)
                classNode->superclass = LiftExpression(inst.operands[1]);

            const SSARef classRef{static_cast<uint8_t>(inst.operands[0].value.reg), inst.operands[0].ssaVersion};
            m_pendingClasses[classRef] = classNode;
            if (!shape.className.empty())
                m_currentFunction->SetVariableName(classRef.regIndex, classRef.version, shape.className);
            statements.push_back(classNode);
            m_processedInstructions.insert(i);
            continue;
        }

        // V10 class declaration: a LOAD of a class-shape constant is the class value. Park an initially
        // method-less `class Name ... end` (name + property names from the shape); each NEWCLASSMEMBER on
        // this register then appends its method (see the NEWCLASSMEMBER case). Bind the class register to
        // its name so later reads (e.g. `return Name`) resolve to it.
        if (inst.operation == LiftedOperation::LOAD && inst.operands.size() >= 2 && inst.operands[1].type == LiftedOperandType::ImmediateConstant) {
            const auto &shapeConst = ConstantAt(inst.operands[1].value.imm.k);
            if (shapeConst.IsClassShape()) {
                const auto &shape = std::get<LuauClassShape>(shapeConst.constantData);
                const SSARef classRef{static_cast<uint8_t>(inst.operands[0].value.reg), inst.operands[0].ssaVersion};

                auto classNode =
                    std::make_shared<ClassDeclarationNode>(shape.className, shape.propertyNames, std::vector<std::shared_ptr<FunctionDeclarationNode>>{});
                m_pendingClasses[classRef] = classNode;
                if (!shape.className.empty())
                    m_currentFunction->SetVariableName(classRef.regIndex, classRef.version, shape.className);

                statements.push_back(classNode);
                m_processedInstructions.insert(i);
                continue;
            }
        }

        if (m_deferToConditionInline.contains(&inst) && inst.operation != LiftedOperation::NEWCLOSURE && inst.operation != LiftedOperation::DUPCLOSURE)
            continue;

        // Normally an inlinable def is skipped (materialized at its use). Under forceDefinitions it is
        // emitted so header defs survive; except a def deferred to its terminator condition, which the
        // condition inlines once (see the repeat-until Pattern 2 lift); force-emitting it too would run
        // an effectful `repeat until f()` twice.
        const bool openCall = (inst.operation == LiftedOperation::CALL || inst.operation == LiftedOperation::CALLFB) && inst.operands.size() > 2 &&
                              inst.operands[2].value.imm.n == 0;
        if (ShouldInline(&inst) && (!forceDefinitions || m_deferToConditionInline.contains(&inst) || openCall || inst.operation == LiftedOperation::GETVARARGS))
            continue;
        if (forceDefinitions && m_foldConsumedDefs.contains(inst.instructionIndex))
            continue; // already rendered inside a folded table constructor

        switch (inst.operation) {
        case LiftedOperation::GETVARARGS: {
            auto defs = m_defsByInstruction[&inst];
            std::ranges::sort(defs, {}, &SSARef::regIndex);
            const int32_t baseReg = inst.operands[0].value.reg;
            for (const auto &ref : defs) {
                LiftedOperand output{};
                output.type = LiftedOperandType::Register;
                output.value.reg = ref.regIndex;
                output.ssaVersion = ref.version;

                const bool isDefined = m_definedRegisters.contains(ref.regIndex);
                auto target = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(output)));
                std::shared_ptr<Expression> value = std::make_shared<VarArgExpression>();
                if (ref.regIndex != baseReg) {
                    auto values = std::make_shared<TableLiteralNode>(std::vector<std::shared_ptr<Expression>>{value});
                    value = std::make_shared<IndexExpressionNode>(values, std::make_shared<NumberLiteralNode>(ref.regIndex - baseReg + 1));
                }

                if (isDefined)
                    statements.push_back(std::make_shared<AssignmentStatementNode>(target, value));
                else
                    statements.push_back(std::make_shared<VariableDeclarationNode>(target, value));
            }
            m_processedInstructions.insert(inst.instructionIndex);
            break;
        }
        case LiftedOperation::SETGLOBAL: {
            const auto &k = ConstantAt(inst.operands[1].value.imm.k);
            auto left = std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(std::get<std::string>(k.constantData)));
            statements.push_back(std::make_shared<AssignmentStatementNode>(left, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLE: {
            auto idxExpr = std::make_shared<IndexExpressionNode>(LiftExpression(inst.operands[1]), LiftExpression(inst.operands[2]));
            statements.push_back(std::make_shared<AssignmentStatementNode>(idxExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLEKS: {
            const auto &k = ConstantAt(inst.operands[2].value.imm.k);
            auto memExpr = std::make_shared<MemberExpressionNode>(LiftExpression(inst.operands[1]), std::get<std::string>(k.constantData));
            statements.push_back(std::make_shared<AssignmentStatementNode>(memExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::NEWCLASSMEMBER: {
            // V10 class-member registration. operand[0] = class register, operand[1] = the value register
            // (the compiled method closure), operand[2] = member-name constant.
            const auto &k = ConstantAt(inst.operands[2].value.imm.k);
            if (k.kType != LUA_TSTRING)
                break; // non-string member name: nothing sensible to emit.
            const auto &memberName = std::get<std::string>(k.constantData);

            // If the class register belongs to a reconstructed `class ... end` (its LOAD carried a class
            // shape), fold this method into the class body as `function name(self, ...) ... end`.
            const SSARef classRef{static_cast<uint8_t>(inst.operands[0].value.reg), inst.operands[0].ssaVersion};
            if (auto pc = m_pendingClasses.find(classRef); pc != m_pendingClasses.end()) {
                if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(LiftExpression(inst.operands[1]))) {
                    fn->functionName = memberName;
                    fn->bAnonymousInline = false;
                    fn->bIsLocalDeclaration = false;
                    // the receiver (register 0) already renders as `self` in both the param list and the
                    // body: SetReceiverName was applied before this closure's body was sub-lifted.
                    pc->second->methods.push_back(std::move(fn));
                }
                break;
            }

            // Otherwise a bare member registration on a plain table: `class.member = <value>`.
            auto memExpr = std::make_shared<MemberExpressionNode>(LiftExpression(inst.operands[0]), memberName);
            statements.push_back(std::make_shared<AssignmentStatementNode>(memExpr, LiftExpression(inst.operands[1])));
            break;
        }
        case LiftedOperation::SETTABLEN: {
            // `t[n] = value` numeric-index store that was not folded into a table constructor (e.g. it
            // reads the table being built). operand[2] is the 0-based index immediate, so source index = n + 1.
            auto idxExpr = std::make_shared<IndexExpressionNode>(
                LiftExpression(inst.operands[1]), std::make_shared<NumberLiteralNode>(static_cast<double>(inst.operands[2].value.imm.n) + 1.0)
            );
            statements.push_back(std::make_shared<AssignmentStatementNode>(idxExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETLIST: {
            // A SETLIST that LiftTableLiteral declined to fold into a `{ ... }` constructor (an element
            // would forward-reference a value declared after the table). Emit each element as its own
            // `t[index] = elem` assignment; by now every contributing local is declared. operands[3] is
            // the aux start index (1-based; SETLIST writes table[aux + k] = R(base + k)).
            // Only a SETLIST on a *local* table reaches here meaningfully: such a table is declared at
            // its NEWTABLE (earlier in this block), so the assignment target exists. A SETLIST on an
            // inlinable table is folded into a `{...}` literal at the table's use site (a later RETURN /
            // call), which the block loop has not reached yet; skip it so we do not also emit bogus
            // `({...})[i] = ...` statements against a throwaway literal.
            if (const auto *tableDef = m_currentFunction->GetDefinition(inst.operands[0]); tableDef && ShouldInline(tableDef))
                break;
            if (!m_currentFunction->implicitUses.contains(&inst))
                break;
            const auto &versions = m_currentFunction->implicitUses.at(&inst);
            const int32_t aux = (inst.operands.size() > 3 && inst.operands[3].value.imm.n >= 1) ? inst.operands[3].value.imm.n : 1;
            for (size_t k = 0; k < versions.size(); ++k) {
                if (k == 0) {
                    LiftedOperand first{};
                    first.type = LiftedOperandType::Register;
                    first.value.reg = inst.operands[1].value.reg;
                    first.ssaVersion = versions.front();
                    if (const auto *firstDef = m_currentFunction->GetDefinition(first); firstDef && m_inlineConsumedDefs.contains(firstDef->instructionIndex))
                        continue;
                }
                auto elem = LiftSetListElement(inst, k, false);
                auto idxExpr = std::make_shared<IndexExpressionNode>(
                    LiftExpression(inst.operands[0]), std::make_shared<NumberLiteralNode>(static_cast<double>(aux + static_cast<int32_t>(k)))
                );
                auto assignment = std::make_shared<AssignmentStatementNode>(idxExpr, elem);
                statements.push_back(assignment);
            }
            break;
        }
        case LiftedOperation::RETURN: {
            std::vector<std::shared_ptr<Expression>> rets;
            // True when the final return value came from the multiret tail-spread path below (a `return
            // f()` that spreads all of f's results). When false, a call as the last value was truncated
            // to a fixed count by the bytecode, so it needs `(f())` parens to not tail-spread on recompile.
            bool lastIsMultretSpread = false;
            const LiftedInstruction *lastRetDef = nullptr; // def of the last return value, for the IsMultretCall gate
            if (m_currentFunction->implicitUses.contains(&inst)) {
                for (int32_t ver : m_currentFunction->implicitUses.at(&inst)) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = inst.operands[0].value.reg + (int)rets.size();
                    op.ssaVersion = ver;

                    auto def = m_currentFunction->GetDefinition(op);
                    if (def &&
                        (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB || def->operation == LiftedOperation::NAMECALL)) {
                        int32_t callIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                        // the NAMECALL path skips ahead to the paired CALL (+2). On malformed bytecode that
                        // slot may be out of range or carry a different op with fewer operands, so guard the
                        // index and the operands[2] read; fall back to a normal value lift if it doesn't hold.
                        const auto &liftedInsts = m_currentFunction->lpLiftedFunction->instructions;
                        if (callIdx >= 0 && static_cast<size_t>(callIdx) < liftedInsts.size() && liftedInsts[callIdx].operands.size() > 2 &&
                            liftedInsts[callIdx].operands[2].value.imm.n == 0) {
                            rets.push_back(LiftCall(*def, def->instructionIndex, true));
                            lastIsMultretSpread = true;
                            break;
                        }
                    }
                    rets.push_back(LiftExpression(op));
                    lastIsMultretSpread = false;
                    lastRetDef = def;
                }
            }

            // `return (f())` / `return a, (f())`: a multiret call inlined as the LAST return value was
            // truncated to one value (its bytecode retcount was fixed, else it would have taken the
            // spread path above). A bare `return f()` tail-spreads, changing the returned arity, so mark
            // the call to render parenthesized. IsMultretCall skips single-return fast builtins so
            // `return buffer.readu8(p)` stays bare. Earlier values sit in comma slots that already truncate.
            if (!rets.empty() && !lastIsMultretSpread && lastRetDef && IsMultretCall(*lastRetDef, lastRetDef->instructionIndex)) {
                if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(rets.back()))
                    c->bAdjustToOne = true;
                else if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(rets.back()))
                    n->bAdjustToOne = true;
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
            const auto &resultInst = inst.operation == LiftedOperation::NAMECALL ? m_currentFunction->lpLiftedFunction->instructions[i + 2] : inst;

            std::vector<std::shared_ptr<Expression>> lhs;
            std::vector<SSARef> defs;
            if (const auto it = m_defsByInstruction.find(&resultInst); it != m_defsByInstruction.end())
                defs = it->second;
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
                            if (!m_currentFunction->implicitUses.contains(&resultInst))
                                continue;
                            const auto &argVersions = m_currentFunction->implicitUses.at(&resultInst);
                            if (argVersions.size() == 2) {
                                LiftedOperand op;
                                op.type = LiftedOperandType::Register;
                                op.value.reg = resultInst.operands[0].value.reg + 1 + 1 /* arg1 */;
                                op.ssaVersion = argVersions[1];
                                auto expr = LiftExpression(op, true);
                                if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(expr);
                                    str && IsValidLuauIdent(str->value) && !m_phiConsumers.contains(ref))
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
                                    if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(argCall->arguments[0]);
                                        str && IsValidLuauIdent(str->value) && !m_phiConsumers.contains(ref))
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

            m_processedInstructions.insert(resultInst.instructionIndex);

            if (inst.operation == LiftedOperation::NAMECALL) {
                m_processedInstructions.insert(i);
                m_processedInstructions.insert(i + 1);
                m_processedInstructions.insert(i + 2);
            }
            break;
        }

        case LiftedOperation::DUPCLOSURE: {
            const auto &k = ConstantAt(inst.operands[1].value.imm.k);
            const auto duplicatedFunction = std::get<LuauProto>(k.constantData);

            AnalyzedFunction *targetFunc = nullptr;
            for (auto &inner : m_currentFunction->innerFunctions) {
                if (inner.lpLiftedFunction->lpDeserialized->bytecodeId == duplicatedFunction->bytecodeId) {
                    targetFunc = &inner;
                    targetFunc->PopulateNames(); // populate to prevent bad usages.
                    SeedEnclosingNames(*targetFunc);
                    // disambiguate this nested fn's own vN/argN from the upvalues it captures.
                    targetFunc->nameSuffix = std::format("_{}", duplicatedFunction->bytecodeId);
                    break;
                }
            }
            // as NEWCLOSURE: a malformed closure constant can name a proto with no matching lifted child;
            // the code below dereferences targetFunc, so drop the sample rather than null-deref.
            if (!targetFunc)
                throw Fission::DecompilerError("malformed bytecode: DUPCLOSURE proto is not a lifted child function");

            if (duplicatedFunction->numparams >= 1 &&
                IsSingleUseClassMemberValue(m_currentFunction, inst.operands[0].value.reg, inst.operands[0].ssaVersion))
                targetFunc->SetReceiverName(0, "self");

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
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister)
                    action.upName = m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) -> alias above; LCT_UPVAL (2) -> parent's
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
                if (srcOp.value.reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams && IsValidLuauIdent(act.upName))
                    m_currentFunction->SetGlobalName(srcOp.value.reg, act.upName);
                statements.push_back(
                    std::make_shared<CommentNode>(std::format("Fission: INFO: Name '{}' propagated from upvalue names.", act.upName), true, true)
                );
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
            subLifter.SetDebugNotes(m_debugNotes);
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = this->GetFunctionName(duplicatedFunction);
            if (const auto name = m_currentFunction->ssaOverrides.find({inst.operands[0].value.reg, inst.operands[0].ssaVersion});
                name != m_currentFunction->ssaOverrides.end())
                funcName = name->second;
            // Pin the closure name to *this* SSA version only. Using
            // SetGlobalName here would cause every later reuse of the same
            // register (e.g. the return slot of a `pcall(closure)`) to keep
            // calling that value by the closure's name.
            // BUT NOT when this version is a phi input (a branch value of `local v = if c then <closure>
            // else e`): its value merges into the named local, so it must inherit that name. Pinning the
            // anon name here makes the closure's own assignment resolve to `anon_N = function`; a bare
            // global that drops the merge target, leaving the local nil.
            if (!m_currentFunction->IsConsumedByPhi(inst.operands[0]))
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
            for (const auto &capture : captureActions)
                fnDecl->capturedNames.insert(capture.upName);

            auto &saveWhere = inst.operands[0];
            if (m_deferToConditionInline.contains(&inst)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                m_processedInstructions.insert(i);
                break;
            }

            // If the closure has a single use that is a call argument, park
            // it in the inline-substitution map and skip emitting a top-level
            // `local function name(...) ... end` declaration entirely.
            // Table-field uses (SETTABLE*) are intentionally excluded: the
            // structurer can duplicate merge blocks into multiple predecessors,
            // so a "single" SETTABLEKS in IR may emit multiple times. The
            // inline map is consumed-and-erased on first lookup, so later
            // emissions resolve to an unbound register name. Emitting as a
            // proper named declaration sidesteps that.
            if (IsSingleUseCallArgument(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion) ||
                IsSingleUseClassMemberValue(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                break;
            }

            // Preserve phi targets and assignments to active debug locals.
            const auto localName = activeLocalName(inst, saveWhere);
            if (m_currentFunction->IsConsumedByPhi(saveWhere) || (m_definedRegisters.contains(saveWhere.value.reg) && localName)) {
                if (localName) {
                    m_currentFunction->SetVariableName(saveWhere.value.reg, saveWhere.ssaVersion, *localName);
                    if (const auto users = m_currentFunction->users.find({saveWhere.value.reg, saveWhere.ssaVersion}); users != m_currentFunction->users.end())
                        for (const auto *user : users->second)
                            if (user->operation == LiftedOperation::PHI && !user->operands.empty())
                                m_currentFunction->SetVariableName(user->operands[0].value.reg, user->operands[0].ssaVersion, *localName);
                }
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
                    if (user->operation == LiftedOperation::SETGLOBAL && std::all_of(users.begin(), users.end(), [&](const auto *other) {
                            return other == user;
                        })) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // Name the declaration after the global it is assigned to. `foo = function() end`
                        // has an ANONYMOUS closure (funcName == anon_...); emitting `function anon_N()`
                        // binds the wrong global and drops the `foo` assignment entirely (foo stays nil).
                        // `function <global>()` binds the intended global. (A named closure already
                        // carries this name via its debug name, so this is a no-op there.)
                        if (fDec && user->operands.size() >= 2 && user->operands[1].type == LiftedOperandType::ImmediateConstant) {
                            const auto &gk = ConstantAt(user->operands[1].value.imm.k);
                            if (gk.kType == LUA_TSTRING)
                                fDec->functionName = std::get<std::string>(gk.constantData);
                        }

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back()); fDec->bIsLocalDeclaration) {
                m_definedRegisters.insert(inst.operands[0].value.reg);
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
                    SeedEnclosingNames(*targetFunc);
                    // Disambiguate this nested function's own vN/argN names from the
                    // upvalues it captures (which read as the enclosing scope's names).
                    targetFunc->nameSuffix = std::format("_{}", proto->bytecodeId);
                    break;
                }
            }
            // valid bytecode references a NEWCLOSURE proto that is a direct child (lifted into
            // innerFunctions, matched by bytecodeId). A malformed subfunction reference can point at an
            // unrelated proto with no matching inner function; the code below dereferences targetFunc, so
            // drop the sample rather than null-deref.
            if (!targetFunc)
                throw Fission::DecompilerError("malformed bytecode: NEWCLOSURE proto is not a lifted child function");

            // A class method's closure (its sole use is a NEWCLASSMEMBER value) has an implicit receiver in
            // register 0. Force it to `self` BEFORE the body is sub-lifted so the parameter declaration and
            // every body reference resolve identically (the override survives PopulateNames()).
            if (proto->numparams >= 1 && IsSingleUseClassMemberValue(m_currentFunction, inst.operands[0].value.reg, inst.operands[0].ssaVersion))
                targetFunc->SetReceiverName(0, "self");

            // See DUPCLOSURE: only inline single-use call arguments. Table-field uses
            // emit as a proper named declaration to survive merge-block duplication.
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
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister)
                    action.upName = m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) -> alias above; LCT_UPVAL (2) -> parent's
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
                if (srcOp.value.reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams && IsValidLuauIdent(act.upName))
                    m_currentFunction->SetGlobalName(srcOp.value.reg, act.upName);
                statements.push_back(
                    std::make_shared<CommentNode>(std::format("Fission: INFO: Name '{}' propagated from upvalue names.", act.upName), true, true)
                );
            }

            bool anyEmit = false;
            for (const auto &act : captureActions)
                if (act.shouldEmit) {
                    anyEmit = true;
                    break;
                }

            if (anyEmit) {
                statements.push_back(
                    std::make_shared<CommentNode>(
                        std::format("Fission: Beginning captures for function with name '{}'", this->GetFunctionName(proto)), true, true
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
                    std::make_shared<CommentNode>(std::format("Fission: Ending captures for function with name '{}'", this->GetFunctionName(proto)), true, true)
                );
            }

            ASTLifter subLifter;
            subLifter.SetDebugNotes(m_debugNotes);
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = this->GetFunctionName(proto);
            if (const auto name = m_currentFunction->ssaOverrides.find({inst.operands[0].value.reg, inst.operands[0].ssaVersion});
                name != m_currentFunction->ssaOverrides.end())
                funcName = name->second;

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
            for (const auto &capture : captureActions)
                fnDecl->capturedNames.insert(capture.upName);

            auto &saveWhere = inst.operands[0];
            if (m_deferToConditionInline.contains(&inst)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                m_processedInstructions.insert(i);
                break;
            }

            // See DUPCLOSURE: single-use call-arg closures collapse to an inline
            // `function(...) ... end` substituted at the use site. Table-field
            // uses are excluded; merge-block duplication can cause multiple
            // emissions, and the inline map is consumed-and-erased on first
            // lookup, so later emissions would resolve to an unbound name.
            // ...also a class method's closure (single use is a NEWCLASSMEMBER value): park it so the
            // class reconstruction can pull the literal into the class body as `function m(self) ... end`.
            if (IsSingleUseCallArgument(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion) ||
                IsSingleUseClassMemberValue(m_currentFunction, saveWhere.value.reg, saveWhere.ssaVersion)) {
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                m_inlineableClosures[SSARef{static_cast<uint8_t>(saveWhere.value.reg), saveWhere.ssaVersion}] = fnDecl;
                break;
            }

            // Preserve phi targets and assignments to active debug locals.
            const auto localName = activeLocalName(inst, saveWhere);
            if (m_currentFunction->IsConsumedByPhi(saveWhere) || (m_definedRegisters.contains(saveWhere.value.reg) && localName)) {
                if (localName) {
                    m_currentFunction->SetVariableName(saveWhere.value.reg, saveWhere.ssaVersion, *localName);
                    if (const auto users = m_currentFunction->users.find({saveWhere.value.reg, saveWhere.ssaVersion}); users != m_currentFunction->users.end())
                        for (const auto *user : users->second)
                            if (user->operation == LiftedOperation::PHI && !user->operands.empty())
                                m_currentFunction->SetVariableName(user->operands[0].value.reg, user->operands[0].ssaVersion, *localName);
                }
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
                    if (user->operation == LiftedOperation::SETGLOBAL && std::all_of(users.begin(), users.end(), [&](const auto *other) {
                            return other == user;
                        })) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // Name the declaration after the global it is assigned to. `foo = function() end`
                        // has an ANONYMOUS closure (funcName == anon_...); emitting `function anon_N()`
                        // binds the wrong global and drops the `foo` assignment entirely (foo stays nil).
                        // `function <global>()` binds the intended global. (A named closure already
                        // carries this name via its debug name, so this is a no-op there.)
                        if (fDec && user->operands.size() >= 2 && user->operands[1].type == LiftedOperandType::ImmediateConstant) {
                            const auto &gk = ConstantAt(user->operands[1].value.imm.k);
                            if (gk.kType == LUA_TSTRING)
                                fDec->functionName = std::get<std::string>(gk.constantData);
                        }

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                fDec->bIsLocalDeclaration && !m_currentFunction->IsConsumedByPhi(inst.operands[0])) {
                m_definedRegisters.insert(inst.operands[0].value.reg);
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
                if (forceDefinitions || inst.operation == LiftedOperation::NEWTABLE || inst.operation == LiftedOperation::DUPTABLE)
                    m_processedInstructions.insert(inst.instructionIndex);
            }
            break;
        }
        }
    }
    return statements;
}

bool ASTLifter::CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const boost::unordered_flat_set<uint32_t> &visitedScopes) {
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
    // bound recursion depth: operands resolve their defining instructions recursively, so a
    // pathological def graph from hostile bytecode would overflow the stack. real expressions nest
    // shallowly. RAII restores the counter on every exit path.
    constexpr int kMaxExpressionDepth = 64;
    if (m_expressionDepth >= kMaxExpressionDepth)
        throw Fission::DecompilerError("malformed bytecode: expression nesting too deep");
    struct DepthGuard {
        int &d;
        ~DepthGuard() { --d; }
    } depthGuard{++m_expressionDepth};

    // Walk MOVE chains iteratively. Chains like `MOVE r1<-r0; MOVE r2<-r1; ...` would
    // otherwise tail-recurse one frame per hop, blowing the stack on deep copy fans.
    LiftedOperand operand = __operand;
    while (true) {
        if (operand.type != LiftedOperandType::Register)
            break;
        if (m_pinnedRegisters.contains({operand.value.reg, operand.ssaVersion}))
            return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
        SSARef closureRef{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
        if (m_inlineableClosures.contains(closureRef))
            break;
        const auto *moveDef = m_currentFunction->GetDefinition(operand);
        if (!moveDef || moveDef->operation != LiftedOperation::MOVE)
            break;
        if (m_processedInstructions.contains(moveDef->instructionIndex))
            break;
        if (!forceExpression && !ShouldInline(moveDef) && !m_deferToConditionInline.contains(moveDef))
            break;
        operand = moveDef->operands[1];
    }

    if (operand.type == LiftedOperandType::Register && m_pinnedRegisters.contains({operand.value.reg, operand.ssaVersion})) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
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
        const auto &k = ConstantAt(operand.value.imm.k);
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
        case LUA_TVECTOR:
            return LiftVectorConstant(k);
        default:
            return std::make_shared<NilLiteralNode>();
        }
    }

    const auto *def = m_currentFunction->GetDefinition(operand);

    if (def && m_processedInstructions.contains(def->instructionIndex)) {
        // "processed" covers two distinct fates: emitted as a declaration (has a name; reference it)
        // or consumed by inlining (no declaration exists; naming it reads an undefined register).
        // When a tail-duplicated region lifts the same instruction twice, the second lift must
        // re-inline defs the first lift consumed. Only pure, constant-derived defs (LOAD/GETIMPORT)
        // are safe to materialize twice (pure reads; only one duplicated path executes at runtime).
        const bool consumedByInline = m_inlineConsumedDefs.contains(def->instructionIndex) &&
                                      (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::GETIMPORT ||
                                       def->operation == LiftedOperation::GETGLOBAL || def->operation == LiftedOperation::GETUPVAL);
        if (!consumedByInline)
            return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }

    if (def && def->operation == LiftedOperation::GETVARARGS) {
        auto vararg = std::make_shared<VarArgExpression>();
        vararg->bAdjustToOne = def->operands.size() > 1 && def->operands[1].value.imm.n == 2;
        return vararg;
    }

    if (!def || (!forceExpression && !ShouldInline(def) && !m_deferToConditionInline.contains(def))) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }

    if (def->operation == LiftedOperation::LOADNJUMP && def->operands.size() >= 2 && def->operands[1].type == LiftedOperandType::ImmediateBool)
        return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
    if (def->operation == LiftedOperation::LOAD) {
        if (def->operands[1].type == LiftedOperandType::ImmediateNil)
            return std::make_shared<NilLiteralNode>();
        if (def->operands[1].type == LiftedOperandType::ImmediateBool)
            return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
        if (def->operands[1].type == LiftedOperandType::ImmediateInteger)
            return std::make_shared<NumberLiteralNode>(def->operands[1].value.imm.n);
        if (def->operands[1].type == LiftedOperandType::ImmediateConstant) {
            const auto &k = ConstantAt(def->operands[1].value.imm.k);
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
            case LUA_TVECTOR:
                return LiftVectorConstant(k);
            default:
                return std::make_shared<NilLiteralNode>();
            }
        }
    }

    // Chained binops `a+b+c+d+...` are left-leaning ADD(ADD(ADD(...),c),d). Recursing left
    // gave one stack frame per link -> blew the stack on obfuscated arithmetic. Walk the spine
    // iteratively, collecting (op, rightExpr) pairs, then fold up.
    auto opSymbol = [](LiftedOperation o) -> const char * {
        switch (o) {
        case LiftedOperation::ADD:
        case LiftedOperation::ADDK:
            return "+";
        case LiftedOperation::SUB:
        case LiftedOperation::SUBK:
            return "-";
        case LiftedOperation::MUL:
        case LiftedOperation::MULK:
            return "*";
        case LiftedOperation::DIV:
        case LiftedOperation::DIVK:
            return "/";
        case LiftedOperation::IDIV:
        case LiftedOperation::IDIVK:
            return "//";
        case LiftedOperation::MOD:
        case LiftedOperation::MODK:
            return "%";
        case LiftedOperation::POW:
        case LiftedOperation::POWK:
            return "^";
        case LiftedOperation::AND:
        case LiftedOperation::ANDK:
            return "and";
        case LiftedOperation::OR:
        case LiftedOperation::ORK:
            return "or";
        default:
            return nullptr;
        }
    };
    auto isBinaryK = [](LiftedOperation o) {
        return o == LiftedOperation::ADDK || o == LiftedOperation::SUBK || o == LiftedOperation::MULK || o == LiftedOperation::DIVK ||
               o == LiftedOperation::IDIVK || o == LiftedOperation::MODK || o == LiftedOperation::POWK || o == LiftedOperation::ANDK ||
               o == LiftedOperation::ORK;
    };
    auto resolveKConstant = [&](int32_t kIdx) -> std::shared_ptr<Expression> {
        const auto &k = ConstantAt(kIdx);
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
        case LUA_TVECTOR:
            return LiftVectorConstant(k);
        default:
            return std::make_shared<NilLiteralNode>();
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
            if (m_pinnedRegisters.contains({leftOp.value.reg, leftOp.ssaVersion})) {
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
            if (!leftDef || m_processedInstructions.contains(leftDef->instructionIndex) || !ShouldInline(leftDef) || opSymbol(leftDef->operation) == nullptr) {
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

    case LiftedOperation::SUBRK:
    case LiftedOperation::DIVRK: {
        // constant on the LEFT, register on the right (`k - r`, `k / r`). Luau has dedicated opcodes
        // because subtraction/division aren't commutative; the left-leaning spine fold above only
        // carries registers on the left, so these are lifted here directly. imm is a union, so the
        // constant index stored in `.n` reads back through `.k`.
        auto left = resolveKConstant(def->operands[1].value.imm.k);
        auto right = LiftExpression(def->operands[2]);
        return std::make_shared<BinaryExpressionNode>(def->operation == LiftedOperation::SUBRK ? "-" : "/", left, right);
    }
    case LiftedOperation::GETTABLEN: {
        // `t[n]`: numeric-index read. operand[2] is the 0-based index immediate (Luau encodes index-1),
        // so the source index is n + 1. Use a plain number literal (`t[1]`, not the `1i` integer suffix)
        // so the recompiler re-emits GETTABLEN rather than LOADK + GETTABLE.
        auto base = LiftExpression(def->operands[1]);
        auto index = std::make_shared<NumberLiteralNode>(static_cast<double>(def->operands[2].value.imm.n) + 1.0);
        return std::make_shared<IndexExpressionNode>(base, index);
    }

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

        // one CONCAT over a range groups like the source it came from: `a .. (b .. c)`, `..` being right-associative
        std::vector<std::shared_ptr<Expression>> parts;
        parts.reserve(operands.size());
        for (const auto &op : operands)
            parts.push_back(LiftExpression(op));
        std::shared_ptr<Expression> expr = nullptr;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part)
            expr = expr ? std::make_shared<BinaryExpressionNode>("..", *part, expr) : *part;
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
        const auto &k = ConstantAt(def->operands[1].value.imm.k);
        return std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(std::get<std::string>(k.constantData)));
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

        std::shared_ptr<Expression> curr = std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(parts[0]));
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
                const auto &k = ConstantAt(curDef->operands[2].value.imm.k);
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
            if (m_pinnedRegisters.contains({leftOp.value.reg, leftOp.ssaVersion})) {
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
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }
}

bool ASTLifter::IsMultretCall(const LiftedInstruction &callDef, int32_t callDefIndex) const {
    if (callDef.operation == LiftedOperation::NAMECALL)
        return true; // a method call is never a fast builtin -> always multret-capable
    if (callDef.operation != LiftedOperation::CALL && callDef.operation != LiftedOperation::CALLFB)
        return true; // not a call node; be conservative (should not reach here)

    // A fast builtin's guarding FASTCALL* sits a few instructions before its CALL (only the callee
    // GETIMPORT and <=3 arg moves lie between), so bound the walk; a general call with a long arg list,
    // or a huge flattened block, must not make this O(N) per call. Stop early at an earlier
    // CALL/NAMECALL: reaching one first means THIS call has no guarding FASTCALL and is a general call.
    const auto &instrs = m_currentFunction->lpLiftedFunction->instructions;
    constexpr int32_t kScanWindow = 32;
    const int32_t lo = std::max(0, callDefIndex - kScanWindow);
    for (int32_t j = callDefIndex - 1; j >= lo; --j) {
        switch (instrs[j].operation) {
        case LiftedOperation::FASTPCALL:
            continue; // A selects pcall/xpcall, not a Luau builtin metadata id.
        case LiftedOperation::FASTCALL:
        case LiftedOperation::FASTCALL1:
        case LiftedOperation::FASTCALL2:
        case LiftedOperation::FASTCALL2K:
        case LiftedOperation::FASTCALL3:
            if (instrs[j].operands.empty())
                return true;
            // Ask Luau's own builtin table (no hardcoded allowlist): a builtin is multret-capable iff its
            // declared result count is not exactly 1. results==1 -> single value (buffer.readu8, math.floor,
            // bit32.band, ...) so truncating parens are redundant; results==2 (math.modf/frexp) or results<0
            // (variadic: select, table.unpack, string.byte, ...) can spread, so `(f())` is required.
            return Luau::Compile::getBuiltinInfo(instrs[j].operands[0].value.imm.n).results != 1;
        case LiftedOperation::CALL:
        case LiftedOperation::CALLFB:
        case LiftedOperation::NAMECALL:
            return true; // an earlier call sits between us and any FASTCALL -> we are a general call
        default:
            break;
        }
    }
    return true; // no guarding FASTCALL in range -> general (multret-capable) call
}

std::shared_ptr<Expression> ASTLifter::LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested) {
    // Iterative tail-fold: a NAMECALL emitted right before a CALL on the same base reg
    // is the real call site. Walk back through any stacked pairs without recursing.
    const LiftedInstruction *curInst = &inst;
    int32_t curIdx = instructionIndex;
    while ((curInst->operation == LiftedOperation::CALL || curInst->operation == LiftedOperation::CALLFB) && curIdx >= 2) {
        if (curInst->operands.empty())
            break; // malformed/hostile bytecode: a CALL with no operands
        const auto &prev = m_currentFunction->lpLiftedFunction->instructions[curIdx - 2];
        if (prev.operation != LiftedOperation::NAMECALL || prev.operands.empty() || prev.operands[0].value.reg != curInst->operands[0].value.reg)
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
    // hostile/malformed bytecode: the resolved call (and its paired NAMECALL) may carry too few
    // operands. Every access below assumes operands[0] (and [1] for a namecall callee), so bail to
    // nil rather than dereferencing past the operand list.
    if (callInfoInst.operands.empty() || resolvedInst.operands.size() < (isNameCall ? 2u : 1u))
        return std::make_shared<NilLiteralNode>();
    int regFunc = callInfoInst.operands[0].value.reg;

    std::vector<std::shared_ptr<Expression>> args;
    std::shared_ptr<Expression> callee;
    bool isVararg = false;
    // Def of the last argument when it is an inlined call truncated to one value (not a multiret spread).
    // Such a call in the final slot renders bare and would tail-spread on recompile, so it may need
    // `(f())` parens; resolved by the IsMultretCall gate after the arg loop. Reset whenever the trailing
    // arg is a spread (multiret call / vararg), which already renders correctly.
    const LiftedInstruction *adjustArgCallDef = nullptr;

    if (isNameCall)
        callee = LiftExpression(resolvedInst.operands[1], false);
    else
        callee = LiftExpression(resolvedInst.operands[0], false);

    // A literal callee (`("x"):method()`, `(1).foo`) needs wrapping parens, but
    // the SourceGenerator already adds exactly one layer via EmitPrefix (a
    // literal is not prefix-safe). Setting bUseParenthesis here too produced a
    // redundant second layer -> `(("x")):method()`. Leave it to the emitter.

    if (m_currentFunction->implicitUses.contains(&callInfoInst)) {
        const auto &argVersions = m_currentFunction->implicitUses.at(&callInfoInst);
        int startOffset = isNameCall ? 1 : 0;
        for (size_t k = startOffset; k < argVersions.size(); ++k) {
            LiftedOperand op;
            op.type = LiftedOperandType::Register;
            op.value.reg = regFunc + 1 + k;
            op.ssaVersion = argVersions[k];

            auto def = m_currentFunction->GetDefinition(op);
            if (def && !m_processedInstructions.contains(def->instructionIndex) &&
                (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB || def->operation == LiftedOperation::NAMECALL)) {
                int32_t actualCallIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                const auto &defInstrs = m_currentFunction->lpLiftedFunction->instructions;
                // hostile/malformed bytecode: actualCallIdx may run past the stream, and the paired CALL
                // may carry fewer than 3 operands -- guard both before reading operands[2].
                if (actualCallIdx >= 0 && static_cast<size_t>(actualCallIdx) < defInstrs.size() && defInstrs[actualCallIdx].operands.size() > 2 &&
                    defInstrs[actualCallIdx].operands[2].value.imm.n == 0) {
                    m_processedInstructions.insert(def->instructionIndex);
                    if (def->operation == LiftedOperation::NAMECALL) {
                        m_processedInstructions.insert(def->instructionIndex + 1);
                        m_processedInstructions.insert(def->instructionIndex + 2);
                    }
                    args.push_back(LiftCall(*def, def->instructionIndex, true));
                    adjustArgCallDef = nullptr; // spread tail: renders as `f()` and correctly spreads
                    break;
                }
            }
            if (def && def->operation == LiftedOperation::GETVARARGS && !m_processedInstructions.contains(def->instructionIndex)) {
                isVararg = true;
                auto vararg = std::make_shared<VarArgExpression>();
                vararg->bAdjustToOne = def->operands.size() > 1 && def->operands[1].value.imm.n == 2 && k + 1 == argVersions.size();
                args.push_back(vararg);
                adjustArgCallDef = nullptr;
                continue;
            }
            args.push_back(LiftExpression(op, false));
            if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB || def->operation == LiftedOperation::NAMECALL)) {
                adjustArgCallDef = def;
            } else {
                adjustArgCallDef = nullptr;
            }
        }
    }

    int32_t prevIdx = resolvedIdx - 1;
    if (prevIdx >= 0) {
        const auto &prevInst = m_currentFunction->lpLiftedFunction->instructions[prevIdx];
        if ((prevInst.operation == LiftedOperation::CALL || prevInst.operation == LiftedOperation::CALLFB) && prevInst.operands.size() > 2 &&
            prevInst.operands[2].value.imm.n == 0) {
            if (!m_processedInstructions.contains(prevIdx)) {
                m_processedInstructions.insert(prevIdx);
                args.push_back(LiftCall(prevInst, prevIdx, true));
                adjustArgCallDef = nullptr; // spread tail appended after the arg loop
            }
        }
    }

    // A genuinely-multret call inlined as the LAST argument was truncated to one value by the bytecode
    // (it did not take the spread path above). Bare `f(a, g())` re-spreads g's results, changing arity :
    // mark it to render `f(a, (g()))`. Single-return fast builtins never spread, so IsMultretCall skips
    // them (no redundant parens); a non-inlined multi-use call is a bare name here, so the cast no-ops.
    if (adjustArgCallDef && !args.empty()) {
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(args.back()))
            c->bAdjustToOne = true;
        else if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(args.back()))
            n->bAdjustToOne = true;
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
    if (!std::ranges::all_of(str, [](char c) {
            auto uc = static_cast<unsigned char>(c);
            return std::isalnum(uc) || uc == '_';
        }))
        return false;
    // a reserved keyword is not a legal bare identifier: `{ end = 1 }` and `t.end` are syntax errors,
    // so the caller must bracket/quote the key (`["end"] = 1`) instead.
    static const std::unordered_set<std::string> kReserved = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",   "in",
                                                              "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while"};
    return !kReserved.contains(str);
}

static std::shared_ptr<Expression> MakeTableKey(const std::string &keyStr) {
    if (IsLegalLuauIdentifier(keyStr))
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(keyStr));
    return std::make_shared<StringLiteralNode>(keyStr);
}

std::shared_ptr<Expression> ASTLifter::LiftSetListElement(const LiftedInstruction &setList, size_t k, bool forceComputed) {
    if (!m_currentFunction->implicitUses.contains(&setList))
        return std::make_shared<NilLiteralNode>();
    const auto &versions = m_currentFunction->implicitUses.at(&setList);
    if (k >= versions.size())
        return std::make_shared<NilLiteralNode>();

    const int startReg = setList.operands[1].value.reg;
    LiftedOperand itemOp{};
    itemOp.type = LiftedOperandType::Register;
    itemOp.value.reg = startReg + static_cast<int>(k);
    itemOp.ssaVersion = versions[k];

    std::shared_ptr<Expression> expr = nullptr;
    if (auto lpDef = m_currentFunction->GetDefinition(itemOp)) {
        if (lpDef->operation == LiftedOperation::LOAD) {
            expr = LiftExpression(itemOp, true);
            this->m_processedInstructions.insert(lpDef->instructionIndex);
            this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
        } else if (lpDef->operation == LiftedOperation::MOVE) {
            expr = LiftExpression(lpDef->operands[1], false);
            this->m_processedInstructions.insert(lpDef->instructionIndex);
            this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
        } else {
            // A computed element (e.g. GETTABLEN `t[1]`). SETLIST references its base
            // register twice (operand[1] + the implicit element use), inflating that
            // element's useCount by one. When the element is otherwise unused, force-inline
            // and consume it so it does not leak as a `local vN` declared *after* the
            // constructor; a forward reference that reads nil. Multi-use elements stay
            // names (avoids the GAMECORE-class exponential force-inline).
            const SSARef itemRef{static_cast<uint8_t>(itemOp.value.reg), itemOp.ssaVersion};
            const int selfUses = (itemOp.value.reg == startReg) ? 2 : 1;
            auto ucIt = m_currentFunction->useCounts.find(itemRef);
            const bool singleUse = ucIt == m_currentFunction->useCounts.end() || ucIt->second <= selfUses;
            // a nested table constructor element (NEWTABLE/DUPTABLE) is part of *this*
            // table's literal. its useCount is inflated (a SETLIST counts its element
            // both as an operand read and an implicit use, plus the inner table's own
            // population), so the plain singleUse heuristic misses it and it leaks as a
            // forward-referenced `local vN`. ShouldInline dedups those, so trust it here.
            const bool inlineTable = (lpDef->operation == LiftedOperation::NEWTABLE || lpDef->operation == LiftedOperation::DUPTABLE) && ShouldInline(lpDef);
            const bool canForce = forceComputed && !m_definedRegisters.contains(itemOp.value.reg);
            if (singleUse && forceComputed && !canForce && !ShouldInline(lpDef))
                ExplainKeep(lpDef, "constructor must reuse the materialized local", &setList);
            if ((singleUse && (canForce || ShouldInline(lpDef))) || inlineTable) {
                expr = LiftExpression(itemOp, true);
                this->m_processedInstructions.insert(lpDef->instructionIndex);
                this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
                ConsumeInlinedInputs(*lpDef);
            } else {
                expr = LiftExpression(itemOp, false);
            }
        }
    }

    // a SETLIST element can read a register that is never written (legal Luau: an
    // unwritten register is nil). LiftExpression yields null for that; never put a
    // null into the constructor (the source generator would dereference it); emit nil.
    return expr ? expr : std::static_pointer_cast<Expression>(std::make_shared<NilLiteralNode>());
}

std::shared_ptr<TableLiteralNode> ASTLifter::LiftTableLiteral(const LiftedInstruction &inst) {
    std::vector<std::shared_ptr<Expression>> elements;
    std::vector<int32_t> candidatesIndexes;
    int32_t tableReg = inst.operands[0].value.reg;
    // The SSA version of *this* table instance. A later store that writes the same
    // register but a different version belongs to a different table (the register
    // was reused) and must not be folded into this constructor.
    int32_t tableVersion = inst.operands[0].ssaVersion;
    constexpr size_t scanLimit = 100;
    size_t maxIdx = m_currentFunction->lpLiftedFunction->instructions.size();
    const int tableBlock = m_currentFunction->GetBlockId(&inst);
    size_t blockEnd = inst.instructionIndex + 1;
    if (tableBlock >= 0 && m_currentFunction->basicBlocks[tableBlock].lpTail)
        blockEnd = (std::min)(maxIdx, static_cast<size_t>(m_currentFunction->basicBlocks[tableBlock].lpTail->instructionIndex) + 1);
    size_t scanEnd = (std::min)(maxIdx, inst.instructionIndex + scanLimit);
    bool bFoundSetList = false;
    const LiftedInstruction *deferredSetList = nullptr;
    for (size_t i = inst.instructionIndex + 1; i < scanEnd; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
        if (candidate.operation == LiftedOperation::SETLIST && candidate.operands.size() > 1 &&
            candidate.operands[0].value.reg == tableReg && candidate.operands[0].ssaVersion == tableVersion) {
            deferredSetList = &candidate;
            break;
        }
    }
    if (!deferredSetList && scanEnd < blockEnd) {
        const LiftedInstruction *distantSetList = nullptr;
        bool hasFieldStore = false;
        for (size_t i = inst.instructionIndex + 1; i < blockEnd; ++i) {
            const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
            if (candidate.operation == LiftedOperation::SETLIST && candidate.operands.size() > 2 && candidate.operands[0].value.reg == tableReg &&
                candidate.operands[0].ssaVersion == tableVersion) {
                distantSetList = &candidate;
                break;
            }
            if ((candidate.operation == LiftedOperation::SETTABLE || candidate.operation == LiftedOperation::SETTABLEKS ||
                 candidate.operation == LiftedOperation::SETTABLEN) &&
                candidate.operands.size() > 1 && candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion == tableVersion)
                hasFieldStore = true;
        }

        const auto uses = distantSetList ? m_currentFunction->implicitUses.find(distantSetList) : m_currentFunction->implicitUses.end();
        bool callElements = uses != m_currentFunction->implicitUses.end() && !uses->second.empty();
        for (size_t i = 0; callElements && i < uses->second.size(); ++i) {
            LiftedOperand element{};
            element.type = LiftedOperandType::Register;
            element.value.reg = distantSetList->operands[1].value.reg + static_cast<int32_t>(i);
            element.ssaVersion = uses->second[i];
            const auto *definition = m_currentFunction->GetDefinition(element);
            callElements = definition && (definition->operation == LiftedOperation::CALL || definition->operation == LiftedOperation::CALLFB ||
                                           definition->operation == LiftedOperation::NAMECALL);
        }
        if (!hasFieldStore && callElements && distantSetList->operands[2].value.imm.n == 0) {
            deferredSetList = distantSetList;
            scanEnd = static_cast<size_t>(distantSetList->instructionIndex) + 1;
        }
    }

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
                case LUA_TVECTOR:
                    valExpr = LiftVectorConstant(valConst);
                    break;
                default:
                    valExpr = std::make_shared<NilLiteralNode>();
                    break;
                }
                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
            }
        }
        if (!elements.empty())
            return std::make_shared<TableLiteralNode>(elements);
    }

    // A field value can only live inside the `{ ... }` constructor if it is an
    // expression we can inline at the field site. Values that are emitted as their
    // own statement (closures, non-trivial calls, anything `ShouldInline` rejects)
    // would otherwise be referenced by a bare, often-reused register name (e.g.
    // `getLocalPlayer = v5`) which is both wrong and not sound. When we hit one,
    // abort coalescing entirely: the table stays `{}` and every assignment is
    // emitted as a separate, locally-sound `t.field = value` statement.
    // Foldable means the ENTIRE value subexpression inlines at the field site. It is not enough for the
    // value's own def to be inlinable: if it transitively reads a register whose def is NOT inlinable, that
    // register surfaces as a bare name inside the constructor. When that name's def is emitted as its own
    // (later) statement, the constructor reads it before it is declared; a forward reference that reads nil
    // (or drops the value entirely). So require every register the value reads to be foldable too. Memoized
    // to stay linear on deep/hostile expressions.
    std::unordered_map<SSARef, bool, std::hash<SSARef>> foldableMemo;
    std::function<bool(const LiftedOperand &)> valueFoldable = [&](const LiftedOperand &valOp) -> bool {
        if (valOp.type != LiftedOperandType::Register)
            return true; // immediates / constants inline trivially
        const auto *def = m_currentFunction->GetDefinition(valOp);
        if (!def)
            return true; // parameter / plain identifier reference
        const SSARef key{static_cast<uint8_t>(valOp.value.reg), valOp.ssaVersion};
        if (auto it = foldableMemo.find(key); it != foldableMemo.end())
            return it->second;
        foldableMemo.emplace(key, false); // seed false so a self-referential cycle terminates conservatively
        // a control-flow-materialised boolean (LOADB diamond) is not a plain literal: its def still
        // looks like `LOAD Rd,bool` here, but the real value is the comparison collapsed in later.
        // Folding would bake the raw bool and drop the comparison, so keep it a sequential store.
        if (m_diamondBoolRegs.contains(valOp.value.reg) && (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::LOADNJUMP) &&
            def->operands.size() >= 2 && def->operands[1].type == LiftedOperandType::ImmediateBool) {
            foldableMemo[key] = false;
            return false;
        }
        bool ok = ShouldInline(def); // a non-inlinable def becomes a bare register name -> not foldable
        if (ok)
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && !valueFoldable(def->operands[oi])) {
                    ok = false;
                    break;
                }
        foldableMemo[key] = ok;
        return ok;
    };

    std::unordered_set<SSARef, std::hash<SSARef>> closureVisited;
    std::function<bool(const LiftedOperand &)> dependsOnClosure = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (!closureVisited.insert(key).second)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false;
        if (def->operation == LiftedOperation::NEWCLOSURE || def->operation == LiftedOperation::DUPCLOSURE)
            return true;
        const size_t firstOperand = (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) ? 0 : 1;
        for (size_t i = firstOperand; i < def->operands.size(); ++i)
            if (dependsOnClosure(def->operands[i]))
                return true;
        return false;
    };

    std::unordered_set<SSARef, std::hash<SSARef>> diamondVisited;
    std::function<bool(const LiftedOperand &)> dependsOnMaterializedDiamond = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register || !diamondVisited.insert({static_cast<uint8_t>(op.value.reg), op.ssaVersion}).second)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false;
        if (m_diamondBoolRegs.contains(op.value.reg) && (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::LOADNJUMP) &&
            def->operands.size() >= 2 && def->operands[1].type == LiftedOperandType::ImmediateBool)
            return true;
        for (size_t oi = 1; oi < def->operands.size(); ++oi)
            if (dependsOnMaterializedDiamond(def->operands[oi]))
                return true;
        return false;
    };

    // True if a stored value reads the table register currently being constructed. Folding such a value
    // into the `{ ... }` literal would reference the table before its own declaration completes; a
    // forward reference that silently reads nil (e.g. `t[3] = t[1] + t[2]`). Walks inlinable defs only
    // (non-inlinable ones stay separate statements anyway); memoized to stay linear on deep/hostile exprs.
    std::unordered_map<SSARef, bool, std::hash<SSARef>> readsTableMemo;
    std::function<bool(const LiftedOperand &)> readsTableReg = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        if (op.value.reg == tableReg)
            return true; // direct read of the table being built
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (auto it = readsTableMemo.find(key); it != readsTableMemo.end())
            return it->second;
        readsTableMemo.emplace(key, false); // seed false first so a self-referential cycle terminates
        const auto *def = m_currentFunction->GetDefinition(op);
        bool result = false;
        if (def && (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE)) {
            if (const auto users = m_currentFunction->users.find(key); users != m_currentFunction->users.end()) {
                for (const auto *user : users->second) {
                    if (!user)
                        continue;
                    if (user->operation == LiftedOperation::SETLIST && user->operands.size() > 1 && user->operands[0].value.reg == key.regIndex &&
                        user->operands[0].ssaVersion == key.version && m_currentFunction->implicitUses.contains(user)) {
                        const auto &versions = m_currentFunction->implicitUses.at(user);
                        const int startReg = user->operands[1].value.reg;
                        for (size_t i = 0; i < versions.size(); ++i) {
                            LiftedOperand element{};
                            element.type = LiftedOperandType::Register;
                            element.value.reg = startReg + static_cast<int32_t>(i);
                            element.ssaVersion = versions[i];
                            if (readsTableReg(element)) {
                                result = true;
                                break;
                            }
                        }
                    } else if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                                user->operation == LiftedOperation::SETTABLEN) &&
                               user->operands.size() > 1 && user->operands[1].value.reg == key.regIndex &&
                               user->operands[1].ssaVersion == key.version) {
                        result = readsTableReg(user->operands[0]) || (user->operands.size() > 2 && readsTableReg(user->operands[2]));
                    }
                    if (result)
                        break;
                }
            }
        }
        // recurse through ALL defining instructions, not only inlinable ones: a value that reads the
        // table through a separate (non-inlined) local still cannot precede the table's construction,
        // so folding `t[k] = f(t[1]).field` into the literal would forward-reference the table.
        if (def)
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (readsTableReg(def->operands[oi])) {
                    result = true;
                    break;
                }
        // calls carry their self/arguments as implicit register uses (regFunc+1 .. ), not direct
        // operands; a method call `t[1]:m(t[2])` reads the table through these, so walk them too.
        if (!result && def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) &&
            m_currentFunction->implicitUses.contains(def)) {
            if (readsTableReg(def->operands[0])) {
                readsTableMemo[key] = true;
                return true;
            }
            const auto &vers = m_currentFunction->implicitUses.at(def);
            const int32_t base = def->operands[0].value.reg + 1;
            for (size_t k = 0; k < vers.size(); ++k) {
                LiftedOperand arg{};
                arg.type = LiftedOperandType::Register;
                arg.value.reg = base + static_cast<int32_t>(k);
                arg.ssaVersion = vers[k];
                if (readsTableReg(arg)) {
                    result = true;
                    break;
                }
            }
        }
        readsTableMemo[key] = result;
        return result;
    };

    // Forward-reference detection for SETLIST elements. The hazard: a folded element names a
    // value whose `local` declaration lands AFTER this constructor; e.g. `local t = {a,
    // (inner)[k]}` where `inner` is a populated table built between this NEWTABLE and the SETLIST.
    // Its `local inner` is emitted after `local t`, so the folded `inner[k]` reads nil. This only
    // bites when the table itself is a real local (emitted at the NEWTABLE position); an inlined
    // table is rendered at its single, last use, by when every contributing local already exists.
    const int32_t tableIndex = inst.instructionIndex;
    const bool tableIsLocal = !ShouldInline(&inst);

    // A register whose def is emitted as its own statement that lands after this table.
    auto isLaterName = [&](const LiftedInstruction *def) -> bool {
        return m_currentFunction->GetBlockId(def) == tableBlock && def->instructionIndex > tableIndex;
    };

    // readsLaterName(op): when `op` is rendered by LiftExpression (inlined when ShouldInline,
    // otherwise emitted as a bare name), does it ultimately reference a non-inlinable register
    // declared after this table? Inlinable sub-expressions are followed into their reads; a
    // non-inlinable one is a leaf whose declaration position decides it. Memoized; the optimistic
    // seed terminates self-referential cycles as safe.
    std::unordered_map<SSARef, bool, std::hash<SSARef>> laterMemo;
    std::function<bool(const LiftedOperand &)> readsLaterName = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false; // parameter / entry value
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (auto it = laterMemo.find(key); it != laterMemo.end())
            return it->second;
        laterMemo.emplace(key, false);
        bool res;
        if (ShouldInline(def)) {
            res = false;
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && readsLaterName(def->operands[oi])) {
                    res = true;
                    break;
                }
            if (!res && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) &&
                m_currentFunction->implicitUses.contains(def)) {
                const auto &vers = m_currentFunction->implicitUses.at(def);
                const int32_t base = def->operands[0].value.reg + 1;
                for (size_t a = 0; a < vers.size(); ++a) {
                    LiftedOperand arg{};
                    arg.type = LiftedOperandType::Register;
                    arg.value.reg = base + static_cast<int32_t>(a);
                    arg.ssaVersion = vers[a];
                    if (readsLaterName(arg)) {
                        res = true;
                        break;
                    }
                }
            }
        } else {
            res = isLaterName(def);
        }
        laterMemo[key] = res;
        return res;
    };

    // True if SETLIST element `k` (source register startReg+k @ version), rendered exactly as
    // LiftSetListElement will render it, forward-references a value. Mirror that force-inline
    // decision: LOAD inlines a constant (safe); MOVE inlines its source; a single-use computed
    // element (selfUses accounts for SETLIST counting its first element as both an operand and an
    // implicit use) or a nested table constructor is force-inlined; check its transitive reads;
    // anything else is emitted as a bare name whose declaration position decides it.
    auto elementForwardRefs = [&](int startReg, int reg, int32_t version) -> bool {
        LiftedOperand itemOp{};
        itemOp.type = LiftedOperandType::Register;
        itemOp.value.reg = reg;
        itemOp.ssaVersion = version;
        const auto *def = m_currentFunction->GetDefinition(itemOp);
        if (!def)
            return false;
        if ((def->operation == LiftedOperation::NEWCLOSURE || def->operation == LiftedOperation::DUPCLOSURE) && isLaterName(def))
            return true;
        if (def->operation == LiftedOperation::LOAD)
            return false;
        if (def->operation == LiftedOperation::MOVE)
            return readsLaterName(def->operands[1]);
        const SSARef itemRef{static_cast<uint8_t>(reg), version};
        const int selfUses = (reg == startReg) ? 2 : 1;
        auto ucIt = m_currentFunction->useCounts.find(itemRef);
        const bool singleUse = ucIt == m_currentFunction->useCounts.end() || ucIt->second <= selfUses;
        const bool inlineTable = (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE) && ShouldInline(def);
        const bool canForce = !m_definedRegisters.contains(reg);
        if (inlineTable || (singleUse && (canForce || ShouldInline(def)))) {
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && readsLaterName(def->operands[oi]))
                    return true;
            if ((def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) && !def->operands.empty() &&
                readsLaterName(def->operands[0]))
                return true;
            if ((def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) && m_currentFunction->implicitUses.contains(def)) {
                const auto &vers = m_currentFunction->implicitUses.at(def);
                const int32_t base = def->operands[0].value.reg + 1;
                for (size_t a = 0; a < vers.size(); ++a) {
                    LiftedOperand arg{};
                    arg.type = LiftedOperandType::Register;
                    arg.value.reg = base + static_cast<int32_t>(a);
                    arg.ssaVersion = vers[a];
                    if (readsLaterName(arg))
                        return true;
                }
            }
            return false;
        }
        return isLaterName(def);
    };

    // A store into another table is an effect (and can raise) unless that table is a nested
    // constructor built inside this fold window.
    auto storesIntoForeignTable = [&](const LiftedInstruction &candidate, size_t tableOperand) {
        if (candidate.operands.size() <= tableOperand || candidate.operands[tableOperand].value.reg == tableReg)
            return false;
        const auto *target = m_currentFunction->GetDefinition(candidate.operands[tableOperand]);
        return !target || (target->operation != LiftedOperation::NEWTABLE && target->operation != LiftedOperation::DUPTABLE) ||
               target->instructionIndex <= inst.instructionIndex;
    };

    for (size_t i = inst.instructionIndex + 1; i < scanEnd; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
        if ((candidate.operation == LiftedOperation::SETLIST && storesIntoForeignTable(candidate, 0)) ||
            ((candidate.operation == LiftedOperation::SETTABLE || candidate.operation == LiftedOperation::SETTABLEKS ||
              candidate.operation == LiftedOperation::SETTABLEN) &&
             storesIntoForeignTable(candidate, 1)))
            break;
        if (inst.operation == LiftedOperation::DUPTABLE) {
            if (tableBlock < 0 || i > static_cast<size_t>(m_currentFunction->basicBlocks[tableBlock].lpTail->instructionIndex))
                break;
            const bool fieldStore = candidate.operation == LiftedOperation::SETTABLEKS && candidate.operands[1].value.reg == tableReg &&
                                    candidate.operands[1].ssaVersion == tableVersion;
            if (!fieldStore && candidate.operation != LiftedOperation::NOP) {
                if (!ShouldInline(&candidate))
                    break;
                bool readsTable = false;
                for (size_t operand = 1; operand < candidate.operands.size(); ++operand)
                    if (candidate.operands[operand].type == LiftedOperandType::Register && candidate.operands[operand].value.reg == tableReg &&
                        candidate.operands[operand].ssaVersion == tableVersion)
                        readsTable = true;
                if (readsTable)
                    break;
            }
        }

        if (candidate.operation == LiftedOperation::JUMP || candidate.operation == LiftedOperation::JUMPIF ||
            candidate.operation == LiftedOperation::JUMPIFNOT || candidate.operation == LiftedOperation::JUMPIFEQ ||
            candidate.operation == LiftedOperation::JUMPIFNOTEQ || candidate.operation == LiftedOperation::JUMPIFLE ||
            candidate.operation == LiftedOperation::JUMPIFNOTLE || candidate.operation == LiftedOperation::JUMPIFLT ||
            candidate.operation == LiftedOperation::JUMPIFNOTLT || candidate.operation == LiftedOperation::JUMPXEQK ||
            candidate.operation == LiftedOperation::RETURN || candidate.operation == LiftedOperation::BREAK ||
            candidate.operation == LiftedOperation::FORNPREP || candidate.operation == LiftedOperation::FORNLOOP ||
            candidate.operation == LiftedOperation::FORGLOOP || candidate.operation == LiftedOperation::FORGPREP ||
            candidate.operation == LiftedOperation::FORGPREP_INEXT || candidate.operation == LiftedOperation::FORGPREP_NEXT)
            break;

        if (candidate.operation == LiftedOperation::SETLIST) {
            if (candidate.operands[0].value.reg == tableReg && candidate.operands[0].ssaVersion != tableVersion)
                break; // register reused for a different table; stop here.
            if (candidate.operands[0].value.reg == tableReg) {
                if (m_currentFunction->implicitUses.contains(&candidate)) {
                    const auto &versions = m_currentFunction->implicitUses.at(&candidate);
                    const int startReg = candidate.operands[1].value.reg;

                    // Forward-reference guard: if this table is a real local and any element would
                    // name a value before its declaration (e.g. an inner populated table built
                    // after this NEWTABLE), do NOT fold this SETLIST. Stop coalescing here; the
                    // SETLIST (and any later stores) are emitted as `t[i] = elem` statements after
                    // every contributing local is declared (see the SETLIST case in
                    // LiftBlockInstructions). Inlined tables render at their last use, where every
                    // contributor already exists, so they are never deferred (deferral also needs a
                    // named target to assign into).
                    if (tableIsLocal) {
                        bool anyForwardRef = false;
                        for (size_t k = 0; k < versions.size(); ++k)
                            if (elementForwardRefs(startReg, startReg + static_cast<int>(k), versions[k])) {
                                anyForwardRef = true;
                                break;
                            }
                        if (anyForwardRef)
                            break;
                    }
                    if (tableIsLocal) {
                        bool anyDiamond = false;
                        const int startReg2 = candidate.operands[1].value.reg;
                        for (size_t k = 0; k < versions.size(); ++k) {
                            const int er = startReg2 + static_cast<int>(k);
                            LiftedOperand eop{};
                            eop.type = LiftedOperandType::Register;
                            eop.value.reg = static_cast<uint8_t>(er);
                            eop.ssaVersion = versions[k];
                            diamondVisited.clear();
                            if (dependsOnMaterializedDiamond(eop)) {
                                anyDiamond = true;
                                break;
                            }
                        }
                        if (anyDiamond)
                            break;
                    }

                    for (size_t k = 0; k < versions.size(); k++)
                        elements.push_back(LiftSetListElement(candidate, k, true));

                    // A fixed-count SETLIST (C != 0) truncated its last element to one value; a C == 0
                    // SETLIST already spreads its tail and needs no parens. If that last element is an
                    // inlined multiret call it renders bare (`{a, g()}`) and would tail-spread on
                    // recompile; mark it `{a, (g())}`. IsMultretCall skips single-return fast builtins;
                    // a non-inlined element is a bare name here, so the cast no-ops.
                    if (!versions.empty() && candidate.operands.size() > 2 && candidate.operands[2].value.imm.n != 0 && !elements.empty()) {
                        LiftedOperand lastOp{};
                        lastOp.type = LiftedOperandType::Register;
                        lastOp.value.reg = startReg + static_cast<int>(versions.size()) - 1;
                        lastOp.ssaVersion = versions.back();
                        const auto *ldef = m_currentFunction->GetDefinition(lastOp);
                        if (ldef &&
                            (ldef->operation == LiftedOperation::CALL || ldef->operation == LiftedOperation::CALLFB ||
                             ldef->operation == LiftedOperation::NAMECALL) &&
                            IsMultretCall(*ldef, ldef->instructionIndex)) {
                            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(elements.back()))
                                c->bAdjustToOne = true;
                            else if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(elements.back()))
                                n->bAdjustToOne = true;
                        }
                    }
                }
                bFoundSetList = true;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEKS) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (readsTableReg(candidate.operands[0]))
                    break;
                if (!valueFoldable(candidate.operands[0]))
                    break; // value can't be a constructor field; keep folded elements, emit this and later stores as statements
                auto kIdx = candidate.operands[2].value.imm.k;
                const auto &k = ConstantAt(kIdx);
                std::string keyStr = std::get<std::string>(k.constantData);

                auto keyExpr = MakeTableKey(keyStr);
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
                candidatesIndexes.emplace_back(candidate.instructionIndex);
                ConsumeInlinedDefs(candidate.operands[0]);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEN) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (readsTableReg(candidate.operands[0]))
                    break; // value reads the table being built; leave it (and later stores) as post-declaration statements
                if (!valueFoldable(candidate.operands[0]))
                    break; // value can't be a constructor field; keep folded elements, emit this and later stores as statements
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
                ConsumeInlinedDefs(candidate.operands[0]);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLE) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (readsTableReg(candidate.operands[0]) || readsTableReg(candidate.operands[2]))
                    break; // key/value reads table being built; keep source evaluation after declaration
                if (tableIsLocal && readsLaterName(candidate.operands[2]))
                    break;
                diamondVisited.clear();
                if (dependsOnMaterializedDiamond(candidate.operands[0]))
                    break;
                diamondVisited.clear();
                if (dependsOnMaterializedDiamond(candidate.operands[2]))
                    break;
                if (!valueFoldable(candidate.operands[0]))
                    break; // value can't be a constructor field; keep folded elements, emit this and later stores as statements
                // A closure key is emitted as its own `local function anon_N` statement, so force-inlining it
                // into the constructor renders a bare register name that is not yet declared there
                // (`{ [v1] = ... }` while the closure emits later; the key reads nil and `table index is nil`
                // fires). Decline to fold: emit this and later stores as `t[<closure>] = value` statements after
                // the closure is declared. Narrow to closures on purpose; a computed key (a call, arith) DOES
                // force-inline correctly into the constructor (`[f(x)] = ...`); deferring those would instead
                // spill them to a bare stale name, so leave them folded.
                closureVisited.clear();
                if (dependsOnClosure(candidate.operands[2]))
                    break;
                auto keyExpr = LiftExpression(candidate.operands[2]);
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<TableBinaryExpressionNode>("=", keyExpr, valExpr));
                candidatesIndexes.emplace_back(candidate.instructionIndex);
                ConsumeInlinedDefs(candidate.operands[0]);
                ConsumeInlinedDefs(candidate.operands[2]);
            }
        } else if (CanOperationRaise(candidate.operation) && !IsConstructorElement(&candidate)) {
            break;
        } else if (StaysAsStatement(&candidate) && !IsConstructorElement(&candidate)) {
            break;
        }
    }

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

void ASTLifter::ConsumeInlinedDefs(const LiftedOperand &operand) {
    if (operand.type != LiftedOperandType::Register)
        return;
    auto *def = m_currentFunction->GetDefinition(operand);
    if (!def || def->operation == LiftedOperation::PHI || m_foldConsumedDefs.contains(def->instructionIndex))
        return;
    // a method setup is part of the call expression that consumes it
    const bool callSetup = def->operation == LiftedOperation::NAMECALL || def->operation == LiftedOperation::NAMECALLUDATA;
    if (!callSetup && !ShouldInline(def))
        return;
    m_foldConsumedDefs.insert(def->instructionIndex);
    ConsumeInlinedInputs(*def);
}

void ASTLifter::ConsumeInlinedInputs(const LiftedInstruction &def) {
    const bool call = def.operation == LiftedOperation::CALL || def.operation == LiftedOperation::CALLFB;
    for (size_t i = call ? 0 : 1; i < def.operands.size(); ++i)
        if (SSABuilder::GetRegisterAccess(def, i) == AccessType::Read || SSABuilder::GetRegisterAccess(def, i) == AccessType::ReadWrite)
            ConsumeInlinedDefs(def.operands[i]);
    if (const auto implicit = m_currentFunction->implicitUses.find(&def); call && implicit != m_currentFunction->implicitUses.end())
        for (size_t k = 0; k < implicit->second.size(); ++k) {
            LiftedOperand argument{};
            argument.type = LiftedOperandType::Register;
            argument.value.reg = def.operands[0].value.reg + 1 + static_cast<int32_t>(k);
            argument.ssaVersion = implicit->second[k];
            ConsumeInlinedDefs(argument);
        }
}

// Cached wrapper. ShouldInline is pure w.r.t. the analysis state that is fixed for the whole lift of
// a function (users/useCounts/defs/phiConsumers/loopCondNoInline are all set before any statement is
// lifted), so the result can be memoised. The memo also breaks the potential quadratic blow-up of
// InliningReordersEffect -> StaysAsStatement -> ShouldInline recursion on deeply nested expressions.
bool ASTLifter::ShouldInline(const LiftedInstruction *inst) {
    if (const auto it = m_shouldInlineMemo.find(inst); it != m_shouldInlineMemo.end())
        return it->second;
    // input checks look backward and effect checks forward, so the query can cycle; a def whose
    // answer is still being computed stays materialized
    if (!m_shouldInlineActive.insert(inst).second)
        return false;
    const bool r = ShouldInlineImpl(inst);
    m_shouldInlineActive.erase(inst);
    m_shouldInlineMemo[inst] = r;
    return r;
}

bool ASTLifter::ShouldInlineImpl(const LiftedInstruction *inst) {
    if (!inst || inst->operands.size() < 1)
        return false;

    if (inst->operation == LiftedOperation::GETVARARGS && inst->operands.size() > 1 && inst->operands[1].value.imm.n > 2)
        return false;

    for (size_t i = 1; i < inst->operands.size(); ++i) {
        const auto &operand = inst->operands[i];
        if (operand.type != LiftedOperandType::Register || !m_diamondBoolRegs.contains(operand.value.reg))
            continue;
        const auto *def = m_currentFunction->GetDefinition(operand);
        if (def && (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::LOADNJUMP) && def->operands.size() >= 2 &&
            def->operands[1].type == LiftedOperandType::ImmediateBool)
            return false;
    }

    bool singleUse = false;
    const auto onlyUser = [&](const SSARef &ref) -> const LiftedInstruction * {
        const auto it = m_currentFunction->users.find(ref);
        if (it == m_currentFunction->users.end() || it->second.empty())
            return nullptr;
        const auto *first = it->second.front();
        if (!std::all_of(it->second.begin(), it->second.end(), [&](const auto *user) { return user == first; }))
            return nullptr;
        if (it->second.size() == 1 || first->operation != LiftedOperation::SETLIST || !m_currentFunction->implicitUses.contains(first) ||
            first->operands.size() < 2)
            return it->second.size() == 1 ? first : nullptr;
        const auto &versions = m_currentFunction->implicitUses.at(first);
        const int32_t startReg = first->operands[1].value.reg;
        for (size_t k = 0; k < versions.size(); ++k) {
            LiftedOperand element{};
            element.type = LiftedOperandType::Register;
            element.value.reg = startReg + static_cast<int32_t>(k);
            element.ssaVersion = versions[k];
            const auto *def = m_currentFunction->GetDefinition(element);
            if (def && m_diamondBoolRegs.contains(element.value.reg) &&
                (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::LOADNJUMP) && def->operands.size() >= 2 &&
                def->operands[1].type == LiftedOperandType::ImmediateBool)
                return first;
        }
        return nullptr;
    };
    if (inst->operands[0].type == LiftedOperandType::Register) {
        const SSARef resultRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto resultUsers = m_currentFunction->users.find(resultRef);
        singleUse =
            m_currentFunction->IsSingleUse(inst->operands[0]) ||
            (resultUsers != m_currentFunction->users.end() && !resultUsers->second.empty() &&
             resultUsers->second.front()->operation == LiftedOperation::SETLIST &&
             std::all_of(resultUsers->second.begin(), resultUsers->second.end(), [&](const auto *user) { return user == resultUsers->second.front(); }));
    }

    // loop-cond consumer from another block: inlining would move this call into the loop
    if (m_forcedMaterialization.contains(inst))
        return false;

    if (CanOperationRaise(inst->operation) && IsConstructorElement(inst) && inst->operands[0].type == LiftedOperandType::Register) {
        const SSARef ref{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto users = m_currentFunction->users.find(ref);
        const auto *user = users != m_currentFunction->users.end() && !users->second.empty() &&
                                   std::ranges::all_of(users->second, [&](const LiftedInstruction *candidate) { return candidate == users->second.front(); })
                               ? users->second.front()
                               : nullptr;
        if (user) {
            if (m_currentFunction->GetBlockId(inst) != m_currentFunction->GetBlockId(user)) {
                ExplainKeep(inst, "constructor consumer is in another CFG block", user);
                return false;
            }

            for (const auto &candidate : m_currentFunction->lpLiftedFunction->instructions) {
                if (candidate.instructionIndex <= inst->instructionIndex || candidate.instructionIndex >= user->instructionIndex)
                    continue;
                if (const auto defs = m_defsByInstruction.find(&candidate); defs != m_defsByInstruction.end())
                    for (size_t i = 1; i < inst->operands.size(); ++i)
                        if (inst->operands[i].type == LiftedOperandType::Register &&
                            std::ranges::any_of(defs->second, [&](const SSARef &defined) { return defined.regIndex == inst->operands[i].value.reg; })) {
                            ExplainKeep(inst, "constructor operand register is overwritten before population", user, &candidate);
                            return false;
                        }
            }

            if (user->operation == LiftedOperation::SETLIST && user->operands.size() > 1 && m_currentFunction->implicitUses.contains(user)) {
                const auto &versions = m_currentFunction->implicitUses.at(user);
                const int32_t startReg = user->operands[1].value.reg;
                for (size_t i = 0; i < versions.size(); ++i) {
                    LiftedOperand element{};
                    element.type = LiftedOperandType::Register;
                    element.value.reg = startReg + static_cast<int32_t>(i);
                    element.ssaVersion = versions[i];
                    const auto *def = m_currentFunction->GetDefinition(element);
                    if (def && def->instructionIndex > inst->instructionIndex && def->instructionIndex < user->instructionIndex && !ShouldInline(def)) {
                        ExplainKeep(inst, "later constructor element must stay materialized", user, def);
                        return false;
                    }
                }
            }
        }
    }

    // A register captured by a closure (VAL or REF) must remain a real local: the
    // closure's upvalue is aliased to this variable's name (no `local uv = source`
    // copy is emitted), so inlining its def into a single use would erase the
    // declaration the upvalue binds to; and for REF would desync writes. (LCT_UPVAL
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
    case LiftedOperation::NEWCLASSMEMBER: // V10: registers a member on a class table; mutation, never inlineable.
    case LiftedOperation::NEWCLASS:       // WIP: reifies a class value and binds its declaration.
        return false;
    default:
        break;
    }

    if (inst->operands[0].type == LiftedOperandType::Register && inst->operation != LiftedOperation::MOVE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_phiConsumers.contains(defRef))
            return false;
    }

    if (inst->operation == LiftedOperation::NEWTABLE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_currentFunction->useCounts.contains(defRef)) {
            const auto &users = m_currentFunction->users[defRef];
            for (const auto *user : users) {
                if (!user || user->operation != LiftedOperation::SETLIST || user->operands.size() < 2 ||
                    user->operands[0].value.reg != inst->operands[0].value.reg || user->operands[0].ssaVersion != inst->operands[0].ssaVersion ||
                    !m_currentFunction->implicitUses.contains(user))
                    continue;
                const auto &versions = m_currentFunction->implicitUses.at(user);
                const int32_t startReg = user->operands[1].value.reg;
                for (size_t k = 0; k < versions.size(); ++k) {
                    LiftedOperand element{};
                    element.type = LiftedOperandType::Register;
                    element.value.reg = startReg + static_cast<int32_t>(k);
                    element.ssaVersion = versions[k];
                    const LiftedInstruction *elementDef = m_currentFunction->GetDefinition(element);
                    for (int guard = 0; guard < 64 && elementDef && elementDef->operation == LiftedOperation::MOVE; ++guard)
                        elementDef = m_currentFunction->GetDefinition(elementDef->operands[1]);
                    if (elementDef && elementDef->operation == LiftedOperation::PHI)
                        return false;
                    if (elementDef && (elementDef->operation == LiftedOperation::NEWCLOSURE || elementDef->operation == LiftedOperation::DUPCLOSURE))
                        return false;
                }
            }
            // count DISTINCT user instructions: a SETLIST that takes this table as an element records it
            // twice (operand read + implicit element use), which would otherwise inflate the count and
            // wrongly keep a single-use (e.g. nested) table as a separate forward-referenced local.
            std::unordered_set<const LiftedInstruction *> realUsers;
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
                if (!realUsers.insert(user).second && user->operation != LiftedOperation::SETLIST)
                    return false;
            }
            if (realUsers.size() != 1)
                return false;
            return !InliningReordersEffect(inst, *realUsers.begin());
        }
    }

    if (inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB || inst->operation == LiftedOperation::NAMECALL) {
        if ((inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands[2].value.imm.n == 0)
            return true;

        if (inst->operation == LiftedOperation::NAMECALL) {
            int regA = inst->operands[0].value.reg;

            if ((inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands[2].value.imm.n == 0)
                return true;

            if (const auto defsIt = m_defsByInstruction.find(inst); defsIt != m_defsByInstruction.end())
                for (const auto &ref : defsIt->second) {
                    if (ref.regIndex != regA)
                        continue;
                    auto users = m_currentFunction->users[{static_cast<uint8_t>(regA), ref.version}];
                    if (users.size() == 1) {
                        auto op = users[0]->operation;
                        // allow inlining returns, other Calls, and arith ops.
                        if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::CALLFB || op == LiftedOperation::NAMECALL ||
                            op == LiftedOperation::ADD || op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV ||
                            op == LiftedOperation::IDIV || op == LiftedOperation::MOD || op == LiftedOperation::POW || op == LiftedOperation::CONCAT ||
                            op == LiftedOperation::MINUS || op == LiftedOperation::NOT || op == LiftedOperation::LENGTH || op == LiftedOperation::GETTABLE ||
                            op == LiftedOperation::GETTABLEKS || op == LiftedOperation::GETTABLEN || op == LiftedOperation::SETLIST) {
                            // a call has effects and can raise; don't inline it past an effect that stays put.
                            if (InliningReordersEffect(inst, users[0]))
                                return false;
                            return true;
                        }
                    }
                    return false;
                }

            return false;
        }

        int usedDefs = 0;
        SSARef usedRef;

        if (const auto defsIt = m_defsByInstruction.find(inst); defsIt != m_defsByInstruction.end())
            for (const auto &ref : defsIt->second) {
                if (m_currentFunction->useCounts[ref] > 0) {
                    usedDefs++;
                    usedRef = ref;
                }
            }

        if (usedDefs > 1) {
            ExplainKeep(inst, "multiple call results have consumers");
            return false;
        }
        if (usedDefs == 0)
            return false;

        // Count DISTINCT user instructions: a SETLIST records its *base* register twice (operand read +
        // implicit element use), so the first element of a `{ f(), ... }` would otherwise look 2-used and
        // leak as a standalone `local` alongside the folded constructor.
        const auto &rawUsers = m_currentFunction->users[usedRef];
        std::unordered_set<const LiftedInstruction *> users(rawUsers.begin(), rawUsers.end());
        if (users.size() == 1 && rawUsers.size() > 1 && (*users.begin())->operation != LiftedOperation::SETLIST) {
            ExplainKeep(inst, "call result is read more than once by its consumer", *users.begin());
            return false;
        }
        if (users.size() == 1) {
            auto op = (*users.begin())->operation;
            if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::CALLFB || op == LiftedOperation::NAMECALL ||
                op == LiftedOperation::ADD || op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV ||
                op == LiftedOperation::IDIV || op == LiftedOperation::MOD || op == LiftedOperation::POW || op == LiftedOperation::CONCAT ||
                op == LiftedOperation::MINUS || op == LiftedOperation::NOT || op == LiftedOperation::LENGTH || op == LiftedOperation::GETTABLE ||
                op == LiftedOperation::GETTABLEKS || op == LiftedOperation::GETTABLEN || op == LiftedOperation::SETLIST || op == LiftedOperation::JUMPIFEQ ||
                op == LiftedOperation::JUMPIFNOTEQ || op == LiftedOperation::JUMPIFLT || op == LiftedOperation::JUMPIFNOTLT ||
                op == LiftedOperation::JUMPIFLE || op == LiftedOperation::JUMPIFNOTLE || op == LiftedOperation::JUMPIF || op == LiftedOperation::JUMPIFNOT ||
                op == LiftedOperation::JUMPXEQK) {
                // a call has effects and can raise; don't inline it past an effect that stays put.
                if (InliningReordersEffect(inst, *users.begin()))
                    return false;
                return true;
            }
            ExplainKeep(inst, "call consumer is not an inlineable operation", *users.begin());
        } else {
            ExplainKeep(inst, "call result has multiple distinct consumers");
        }
        return false;
    }

    if (inst->operation == LiftedOperation::MOVE) {
        if (!singleUse || m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;

        const SSARef defRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto usersIt = m_currentFunction->users.find(defRef);
        if (inst->operands.size() < 2 || inst->operands[1].type != LiftedOperandType::Register || usersIt == m_currentFunction->users.end() ||
            usersIt->second.size() != 1)
            return true;

        const auto *user = usersIt->second.front();
        const int sourceReg = inst->operands[1].value.reg;
        for (const auto &candidate : m_currentFunction->lpLiftedFunction->instructions) {
            if (candidate.instructionIndex <= inst->instructionIndex || candidate.instructionIndex >= user->instructionIndex)
                continue;
            if (const auto defsIt = m_defsByInstruction.find(&candidate); defsIt != m_defsByInstruction.end())
                for (const auto &ref : defsIt->second)
                    if (ref.regIndex == sourceReg)
                        return false;
        }
        return true;
    }

    // A zero-use GETIMPORT with an index path (depth >= 2) is a real runtime read that can raise
    // (non-safeenv falls back to GETGLOBAL + GETTABLEKS at the use site); "inlining" a def with no
    // users deletes the read and with it the error it would have thrown. Keep it as a statement.
    // Call arguments are tracked via implicitUses (not useCounts), so scan those before deciding
    // the def is truly dead; this branch is rare enough that the scan doesn't matter for perf.
    if (inst->operation == LiftedOperation::GETIMPORT && inst->operands.size() >= 3 && (inst->operands[2].value.imm.u >> 30) >= 2) {
        const SSARef ref{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        const auto uc = m_currentFunction->useCounts.find(ref);
        if (uc == m_currentFunction->useCounts.end() || uc->second == 0) {
            bool implicitlyUsed = false;
            for (const auto &[user, regs] : m_currentFunction->implicitUses) {
                for (const int32_t r : regs)
                    if (r == inst->operands[0].value.reg && user->instructionIndex > inst->instructionIndex) {
                        implicitlyUsed = true;
                        break;
                    }
                if (implicitlyUsed)
                    break;
            }
            if (!implicitlyUsed)
                return false;
        }
    }

    // Keep raising operations at their original position so error order matches bytecode evaluation.
    const bool bareImport = inst->operation == LiftedOperation::GETIMPORT && (inst->operands.size() < 3 || (inst->operands[2].value.imm.u >> 30) < 2);
    const auto inlineTreeCanRaise = [&](auto &&self, const LiftedInstruction *node, int depth) -> bool {
        if (!node)
            return false;
        if (depth >= 64)
            return true;
        // a folded constructor evaluates its elements where it renders
        if (CanOperationRaise(node->operation) || node->operation == LiftedOperation::CALL || node->operation == LiftedOperation::CALLFB ||
            node->operation == LiftedOperation::NAMECALL || node->operation == LiftedOperation::NAMECALLUDATA || node->operation == LiftedOperation::NEWTABLE)
            return true;
        if (node->operation != LiftedOperation::MOVE && node->operation != LiftedOperation::NOT && node->operation != LiftedOperation::AND &&
            node->operation != LiftedOperation::ANDK && node->operation != LiftedOperation::OR && node->operation != LiftedOperation::ORK)
            return false;
        for (size_t i = 1; i < node->operands.size(); ++i) {
            if (node->operands[i].type != LiftedOperandType::Register)
                continue;
            const auto *input = m_currentFunction->GetDefinition(node->operands[i]);
            if (input && ShouldInline(input) && self(self, input, depth + 1))
                return true;
        }
        return false;
    };
    bool mayMoveRaisingInput = inst->operation == LiftedOperation::NOT;
    if (inst->operation == LiftedOperation::AND || inst->operation == LiftedOperation::ANDK || inst->operation == LiftedOperation::OR ||
        inst->operation == LiftedOperation::ORK) {
        for (size_t i = 1; !mayMoveRaisingInput && i < inst->operands.size(); ++i) {
            if (inst->operands[i].type != LiftedOperandType::Register)
                continue;
            const auto *input = m_currentFunction->GetDefinition(inst->operands[i]);
            mayMoveRaisingInput = input && ShouldInline(input) && inlineTreeCanRaise(inlineTreeCanRaise, input, 0);
        }
    }
    const bool raisesWhenInlined = CanOperationRaise(inst->operation) || mayMoveRaisingInput;
    if (raisesWhenInlined && !bareImport && inst->operands[0].type == LiftedOperandType::Register && singleUse) {
        const SSARef ref{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto *user = onlyUser(ref);
        if (!user) {
            ExplainKeep(inst, "raising expression has no unique terminal consumer");
            return false;
        }
        if (InliningReordersEffect(inst, user))
            return false;
    }

    if ((!raisesWhenInlined || singleUse) && (inst->operation != LiftedOperation::GETIMPORT || bareImport || singleUse) &&
        m_currentFunction->IsSimpleOrConstant(inst->operands[0]) && !m_currentFunction->IsConsumedByPhi(inst->operands[0]) &&
        inst->operands[0].ssaVersion != 1 /* first version cannot be inlined. It is a declaration */)
        return true;

    // A pure constant `LOAD` with no explicit users is a call argument (those are
    // tracked via implicitUses, not useCounts) or dead. Either way it is
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

    if (singleUse) {
        if (m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;
        if (inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE)
            return false; // do not omit NEWCLOSURE and DUPCLOSURE.
        // Inlining moves a def's evaluation from its bytecode position to its single use site. If the
        // def can RAISE and some instruction that STAYS at its own position (a store, a bare-effect
        // call, a materialised comparison) sits between them, the move reorders the def's throw past
        // that effect; a different runtime error fires. Keep the def as a statement in that case so
        // its evaluation stays put. A never-raising def, or an intervening op that itself inlines into
        // the same expression (so order is preserved), is fine.
        // GETIMPORT only raises on an index path (depth >= 2 -> GETGLOBAL + GETTABLEKS in non-safeenv);
        // a bare global import (`require`, `print`) never throws and must inline for the naming passes.
        const bool defCanRaise = (CanOperationRaise(inst->operation) || mayMoveRaisingInput) &&
                                 !(inst->operation == LiftedOperation::GETIMPORT && (inst->operands.size() < 3 || (inst->operands[2].value.imm.u >> 30) < 2));
        const SSARef ref{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (const auto *user = onlyUser(ref); user && (defCanRaise ? InliningReordersEffect(inst, user) : InputRebound(inst, user, -1, false)))
            return false;
        return true;
    }

    return false;
}

// True if an instruction that produces a value nonetheless keeps its evaluation at its own bytecode
// position (rather than being inlined into a use), so moving another def across it would reorder
// effects/throws. Stores always stay. A call stays unless it is a single-use value feeding an
// accepting user (then it inlines into that expression, preserving order); a bare-effect call like
// `print()`; no value user; always stays.
bool ASTLifter::StaysAsStatement(const LiftedInstruction *e) {
    switch (e->operation) {
    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETUDATAKS:
        return true; // always a real mutation of an escaping location
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
        // a store into a fresh NEWTABLE folds into a `{ ... }` literal (it builds a value operand,
        // not an independent effect), so it does not order against an unrelated def. Only a store
        // into an escaping table (a variable/field) is a real statement-effect that bars reordering.
        return !StoreTargetsFreshTable(e);
    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
        return !ShouldInline(e);
    case LiftedOperation::NAMECALL:
    case LiftedOperation::NAMECALLUDATA: {
        // NAMECALL executes with its following CALL.
        const auto &insts = m_currentFunction->lpLiftedFunction->instructions;
        for (int32_t j = e->instructionIndex + 1; j < static_cast<int32_t>(insts.size()); ++j) {
            const auto op = insts[j].operation;
            if (op == LiftedOperation::NOP)
                continue;
            if (op == LiftedOperation::CALL || op == LiftedOperation::CALLFB)
                return !ShouldInline(&insts[j]);
            break; // NAMECALL not immediately followed by its CALL (malformed); not a barrier
        }
        return false;
    }
    case LiftedOperation::NEWTABLE:
    case LiftedOperation::DUPTABLE:
        // a table constructor that spills to its own `local t = { ... }` stays at that position, and
        // building it can raise (a throwing element value, a nil key). Moving a raising def past it
        // reorders which error fires. An inlined table renders at its single use, in order, so is safe.
        return !ShouldInline(e);
    default:
        // a throw-capable read (index/arith/concat/length) that will itself spill to a `local`
        // stays at its own bytecode position; an earlier def inlined past it would then execute
        // after it -> reordered throw. A read that inlines moves into the shared expression, in
        // order, and is not a barrier. Bare non-raising GETIMPORT never throws, so never bars.
        if (CanOperationRaise(e->operation) &&
            !(e->operation == LiftedOperation::GETIMPORT && (e->operands.size() < 3 || (e->operands[2].value.imm.u >> 30) < 2))) {
            return !ShouldInline(e);
        }
        return false;
    }
}

// True if the value produced by `e` is consumed only by table-constructor building (a SETLIST
// element, or a keyed/array store into a fresh NEWTABLE), so it folds into a `{ ... }` literal
// rather than being emitted as its own statement.
bool ASTLifter::IsConstructorElement(const LiftedInstruction *e) {
    if (e->operands.empty() || e->operands[0].type != LiftedOperandType::Register)
        return false;
    if (e->operation == LiftedOperation::NAMECALL || e->operation == LiftedOperation::NAMECALLUDATA) {
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        for (int32_t i = e->instructionIndex + 1; i < static_cast<int32_t>(instructions.size()); ++i) {
            if (instructions[i].operation == LiftedOperation::NOP)
                continue;
            return (instructions[i].operation == LiftedOperation::CALL || instructions[i].operation == LiftedOperation::CALLFB) &&
                   IsConstructorElement(&instructions[i]);
        }
        return false;
    }

    std::vector<SSARef> refs;
    if (const auto defs = m_defsByInstruction.find(e); defs != m_defsByInstruction.end())
        refs = defs->second;
    else
        refs.push_back({e->operands[0].value.reg, e->operands[0].ssaVersion});

    bool found = false;
    for (const auto &ref : refs) {
        const auto users = m_currentFunction->users.find(ref);
        if (users == m_currentFunction->users.end() || users->second.empty())
            continue;
        found = true;
        for (const auto *user : users->second) {
            if (user->operation == LiftedOperation::SETLIST) {
                const auto *tableDef = user->operands.empty() ? nullptr : m_currentFunction->GetDefinition(user->operands[0]);
                if (tableDef && tableDef->instructionIndex < e->instructionIndex)
                    continue;
                return false;
            }
            if ((user->operation == LiftedOperation::CALL || user->operation == LiftedOperation::CALLFB) && user->instructionIndex > e->instructionIndex &&
                IsConstructorElement(user))
                continue;
            if (user->instructionIndex > e->instructionIndex && m_defsByInstruction.contains(user) && IsConstructorElement(user))
                continue;
            if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                 user->operation == LiftedOperation::SETTABLEN) &&
                StoreTargetsFreshTable(user)) {
                const auto *tableDef = user->operands.size() > 1 ? m_currentFunction->GetDefinition(user->operands[1]) : nullptr;
                if (tableDef && tableDef->instructionIndex < e->instructionIndex)
                    continue;
            }
            return false;
        }
    }
    return found;
}

// True if a SET* store writes into a register defined by NEWTABLE; i.e. it is filling a table
// constructor the lifter folds into a `{ ... }` literal, not mutating a pre-existing table.
bool ASTLifter::StoreTargetsFreshTable(const LiftedInstruction *e) {
    // table operand: SETLIST uses operand[0]; the keyed stores use operand[1].
    const size_t tableOp = (e->operation == LiftedOperation::SETLIST) ? 0 : 1;
    if (e->operands.size() <= tableOp || e->operands[tableOp].type != LiftedOperandType::Register)
        return false;
    const auto *def = m_currentFunction->GetDefinition(e->operands[tableOp]);
    return def && (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE);
}

// True if inlining `def` into its single use `use` would reorder its throw/effect past an instruction
// that stays at its own position between them. The use-call's own method-setup NAMECALL is part of the
// same call expression (`v[1]:Cross(v[2])` loads the arg before the NAMECALL), not a separate effect,
// so it is excluded. Comparison branches always count: whether an if-condition or a materialised bool,
// they evaluate in place and can raise.
bool ASTLifter::InputRebound(const LiftedInstruction *def, const LiftedInstruction *use, int32_t skipIndex, bool sameRegister) {
    const int32_t defIdx = def->instructionIndex;
    const int32_t useIdx = use->instructionIndex;
    if (useIdx <= defIdx + 1)
        return false;

    // Inputs of already-inlined operands are read at the use site too.
    std::vector<LiftedOperand> inputs;
    std::vector<const LiftedInstruction *> pending{def};
    std::unordered_set<const LiftedInstruction *> seen{def};
    while (!pending.empty() && inputs.size() < 64) {
        const auto *reader = pending.back();
        pending.pop_back();
        for (size_t i = 0; i < reader->operands.size(); ++i) {
            const auto &input = reader->operands[i];
            if (input.type != LiftedOperandType::Register)
                continue;
            const auto access = SSABuilder::GetRegisterAccess(*reader, i);
            if (access != AccessType::Read && access != AccessType::ReadWrite)
                continue;
            inputs.push_back(input);
            // single-use stands in for "inlined here"; asking ShouldInline would recurse back into this check
            const auto *inputDef = m_currentFunction->GetDefinition(input);
            if (inputDef && inputDef->operation != LiftedOperation::PHI && inputDef->instructionIndex < defIdx && m_currentFunction->IsSingleUse(input) &&
                seen.insert(inputDef).second)
                pending.push_back(inputDef);
        }
    }
    if (inputs.empty())
        return false;

    const auto &insts = m_currentFunction->lpLiftedFunction->instructions;
    for (int32_t k = defIdx + 1; k < useIdx && static_cast<size_t>(k) < insts.size(); ++k) {
        if (k == skipIndex)
            continue;
        const auto outputs = m_defsByInstruction.find(&insts[k]);
        if (outputs == m_defsByInstruction.end() || ShouldInline(&insts[k]))
            continue;
        for (const auto &input : inputs) {
            const auto inputName = m_currentFunction->GetVarName(input.value.reg, input.ssaVersion);
            for (const auto &output : outputs->second) {
                LiftedOperand written{};
                written.type = LiftedOperandType::Register;
                written.value.reg = output.regIndex;
                written.ssaVersion = output.version;
                // without the register rule only a phi-merged variable is one binding; other reuses get fresh names
                if (!sameRegister && !m_currentFunction->IsConsumedByPhi(written))
                    continue;
                if ((sameRegister && output.regIndex == input.value.reg) || m_currentFunction->GetVarName(output.regIndex, output.version) == inputName) {
                    ExplainKeep(def, "an input binding is overwritten before its use", use, &insts[k]);
                    return true;
                }
            }
        }
    }
    return false;
}

bool ASTLifter::InliningReordersEffect(const LiftedInstruction *def, const LiftedInstruction *use) {
    if (!def || !use)
        return false;
    while (use->operation == LiftedOperation::MOVE && ShouldInline(use)) {
        const SSARef moved{static_cast<uint8_t>(use->operands[0].value.reg), use->operands[0].ssaVersion};
        const auto users = m_currentFunction->users.find(moved);
        if (users == m_currentFunction->users.end() || users->second.size() != 1 || users->second.front()->instructionIndex <= use->instructionIndex)
            break;
        use = users->second.front();
    }
    const int32_t defIdx = def->instructionIndex;
    const int32_t useIdx = use->instructionIndex;
    if (useIdx <= defIdx + 1)
        return false; // nothing between (or use precedes def; not our shape)
    const auto &insts = m_currentFunction->lpLiftedFunction->instructions;
    const LiftedInstruction *calleeDef = nullptr;
    if ((use->operation == LiftedOperation::CALL || use->operation == LiftedOperation::CALLFB) && !use->operands.empty()) {
        calleeDef = m_currentFunction->GetDefinition(use->operands[0]);
        while (calleeDef && calleeDef->operation == LiftedOperation::MOVE && ShouldInline(calleeDef))
            calleeDef = m_currentFunction->GetDefinition(calleeDef->operands[1]);
    }
    const bool defIsCallCallee = calleeDef == def;
    // a constructor element renders inside its `{ ... }`; that keeps it after `def` only when `def` feeds a constructor too
    const bool useBuildsConstructor = def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE ||
                                      use->operation == LiftedOperation::SETLIST ||
                                      ((use->operation == LiftedOperation::SETTABLE || use->operation == LiftedOperation::SETTABLEKS ||
                                        use->operation == LiftedOperation::SETTABLEN) &&
                                       StoreTargetsFreshTable(use)) ||
                                      IsConstructorElement(use);

    int32_t useOwnNameCall = -1;
    if (useIdx < static_cast<int32_t>(insts.size()) &&
        (insts[useIdx].operation == LiftedOperation::CALL || insts[useIdx].operation == LiftedOperation::CALLFB)) {
        for (int32_t j = useIdx - 1; j > defIdx; --j) {
            const auto jop = insts[j].operation;
            if (jop == LiftedOperation::CALL || jop == LiftedOperation::CALLFB)
                break;
            if (jop == LiftedOperation::NAMECALL || jop == LiftedOperation::NAMECALLUDATA) {
                useOwnNameCall = j;
                break;
            }
        }
    }

    if (InputRebound(def, use, useOwnNameCall))
        return true;

    for (int32_t k = defIdx + 1; k < useIdx && static_cast<size_t>(k) < insts.size(); ++k) {
        if (k == useOwnNameCall)
            continue;
        if (defIsCallCallee && (insts[k].operation == LiftedOperation::CALL || insts[k].operation == LiftedOperation::CALLFB)) {
            ExplainKeep(def, "callee must be evaluated before argument calls", use, &insts[k]);
            return true; // callee evaluation precedes every argument call, even when that argument later spills.
        }
        switch (insts[k].operation) {
        case LiftedOperation::JUMP:
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::JUMPXEQK:
            ExplainKeep(def, "control-flow boundary", use, &insts[k]);
            return true;
        case LiftedOperation::SETTABLE:
        case LiftedOperation::SETTABLEKS:
        case LiftedOperation::SETTABLEN:
        case LiftedOperation::SETLIST:
            if (StoreTargetsFreshTable(&insts[k])) {
                const auto *tableDef = m_currentFunction->GetDefinition(insts[k].operands[insts[k].operation == LiftedOperation::SETLIST ? 0 : 1]);
                if (tableDef && tableDef->instructionIndex < defIdx) {
                    ExplainKeep(def, "store into a table built before the definition", use, &insts[k]);
                    return true;
                }
            }
            break;
        default:
            break;
        }
        if (insts[k].operation == LiftedOperation::CALL || insts[k].operation == LiftedOperation::CALLFB) {
            ExplainKeep(def, "intervening call would execute first", use, &insts[k]);
            return true;
        }
        if (CanOperationRaise(insts[k].operation) && !(useBuildsConstructor && IsConstructorElement(&insts[k]))) {
            ExplainKeep(def, "intervening evaluation can raise", use, &insts[k]);
            return true;
        }
        if (StaysAsStatement(&insts[k])) {
            ExplainKeep(def, "intervening statement stays at its original site", use, &insts[k]);
            return true;
        }
    }
    return false;
}

// Operations whose evaluation can raise a Luau runtime error (index/arith/concat/length metamethods
// or type errors). Determines whether inlining would reorder a throw.
bool ASTLifter::CanOperationRaise(LiftedOperation op) {
    switch (op) {
    case LiftedOperation::ADDK:
    case LiftedOperation::SUBK:
    case LiftedOperation::SUBRK:
    case LiftedOperation::MULK:
    case LiftedOperation::DIVK:
    case LiftedOperation::DIVRK:
    case LiftedOperation::IDIVK:
    case LiftedOperation::MODK:
    case LiftedOperation::POWK:
    case LiftedOperation::NAMECALL:
    case LiftedOperation::NAMECALLUDATA:
    case LiftedOperation::GETUDATAKS:
    case LiftedOperation::GETTABLE:
    case LiftedOperation::GETTABLEKS:
    case LiftedOperation::GETTABLEN:
    case LiftedOperation::GETIMPORT:
    case LiftedOperation::ADD:
    case LiftedOperation::SUB:
    case LiftedOperation::MUL:
    case LiftedOperation::DIV:
    case LiftedOperation::IDIV:
    case LiftedOperation::MOD:
    case LiftedOperation::POW:
    case LiftedOperation::CONCAT:
    case LiftedOperation::LENGTH:
    case LiftedOperation::MINUS:
        return true;
    default:
        return false;
    }
}

std::string ASTLifter::ResolveVariableName(const LiftedOperand &op, bool markDefined) {
    if (op.type != LiftedOperandType::Register)
        return "err_not_reg";

    // read-site resolutions pass false: naming a use is not a declaration, and marking it
    // "defined" made the next write to the slot a bare assignment (global leak) instead of a local
    if (markDefined)
        m_definedRegisters.insert(op.value.reg);
    std::string name = m_currentFunction->GetVarName(op.value.reg, op.ssaVersion);
    if (name.empty())
        return std::format("v{}", op.value.reg);

    return name;
}

void ASTLifter::SeedEnclosingNames(AnalyzedFunction &target) const {
    target.enclosingNames = m_currentFunction->enclosingNames;
    for (const auto &[_, name] : m_currentFunction->upvalueNames)
        target.enclosingNames.insert(name);
    for (const auto &[_, name] : m_currentFunction->upvalueNameOverrides)
        target.enclosingNames.insert(name);
    for (const int32_t reg : m_definedRegisters) {
        if (const auto it = m_currentFunction->receiverNameOverrides.find(static_cast<uint8_t>(reg)); it != m_currentFunction->receiverNameOverrides.end())
            target.enclosingNames.insert(it->second);
        else if (const auto it = m_currentFunction->globalRegNames.find(reg); it != m_currentFunction->globalRegNames.end())
            target.enclosingNames.insert(it->second);
        else
            target.enclosingNames.insert(std::format("v{}", reg));

        for (const auto &[ref, name] : m_currentFunction->ssaOverrides)
            if (ref.regIndex == reg)
                target.enclosingNames.insert(name);
        for (const auto &[ref, name] : m_currentFunction->variableNames)
            if (ref.regIndex == reg)
                target.enclosingNames.insert(name);
    }
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

    // Fall-through block F: exactly one real instruction, a `LOADB Rd,bF` that jumps over T
    // into the merge M, and reached only from the header.
    const auto &F = blocks[fIdx];
    if (countReal(F) != 1)
        return std::nullopt;
    const LiftedInstruction *fLoad = firstReal(F);
    if (!fLoad || fLoad->operation != LiftedOperation::LOADNJUMP || !isBoolLoad(fLoad))
        return std::nullopt;
    if (F.successors.size() != 1 || F.predecessors.size() != 1 || F.predecessors[0] != headerId)
        return std::nullopt;
    const uint32_t mIdx = F.successors[0];

    // Jump target T: exactly `LOADB Rd,bT` for the same register, falling into M.
    const auto &T = blocks[tIdx];
    if (countReal(T) != 1)
        return std::nullopt;
    const LiftedInstruction *tLoad = firstReal(T);
    if (!tLoad || tLoad->operation != LiftedOperation::LOAD || !isBoolLoad(tLoad))
        return std::nullopt;
    if (T.successors.size() != 1 || T.successors[0] != mIdx || T.predecessors.size() != 1 || T.predecessors[0] != headerId)
        return std::nullopt;

    const uint8_t reg = fLoad->operands[0].value.reg;
    if (tLoad->operands[0].value.reg != reg)
        return std::nullopt;
    const bool bF = fLoad->operands[1].value.imm.b;
    const bool bT = tLoad->operands[1].value.imm.b;
    if (bF == bT)
        return std::nullopt; // not a true/false split; leave it alone.

    // The merge must be entered only from the two loads, so that `reg` is provably the diamond's boolean.
    if (mIdx >= blocks.size() || std::set<uint32_t>(blocks[mIdx].predecessors.begin(), blocks[mIdx].predecessors.end()) != std::set<uint32_t>{tIdx, fIdx})
        return std::nullopt;

    // Only collapse genuine condition jumps; LiftCondition yields BooleanLiteral
    // for opcodes it cannot turn into a comparison/truth test.
    auto cond = LiftCondition(H.lpTail);
    if (!cond || std::dynamic_pointer_cast<BooleanLiteralNode>(cond))
        return std::nullopt;

    // Control reaches T directly when the jump is taken (cond true -> bT) and via
    // F when it is not (cond false -> bF). So reg == cond when bT is true, else !cond.
    std::shared_ptr<Expression> value = (bT && !bF) ? cond : InvertCondition(cond);

    LiftedOperand target = tLoad->operands[0];
    for (const auto &phi : blocks[mIdx].phiNodes)
        if (phi.operands[0].value.reg == reg)
            target = phi.operands[0];
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

    return BoolMaterialization{assignment, mIdx};
}

void ASTLifter::HoistPhiLocals(
    int32_t mergeIdx, uint32_t stopBlockId, const std::shared_ptr<IfStatementNode> &ifStmt, std::vector<std::shared_ptr<Statement>> &nodes,
    const boost::unordered_flat_set<int32_t> &definedBeforeBranches
) {
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

                // Calls stay in place; only their local-declaration marker moves to the hoisted binding.
                if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
                    auto retMatches = [&](const std::vector<std::shared_ptr<Expression>> &rets) -> bool {
                        if (rets.size() != 1)
                            return false;
                        auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(rets[0]);
                        return ident && ident->identifier && ident->identifier->name == name;
                    };
                    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression); nameCall && retMatches(nameCall->rets)) {
                        nameCall->bIsLocalDeclaration = false;
                        needsHoist = true;
                    } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression); call && retMatches(call->rets)) {
                        call->bIsLocalDeclaration = false;
                        needsHoist = true;
                    }
                }
            }
        }

        // Declare only new branch-local merges. Existing outer bindings and enclosing stop-block phis
        // must keep their owning scope or branch declarations will shadow the value seen after the merge.
        if (needsHoist && !definedBeforeBranches.contains(reg) && mergeIdx != static_cast<int32_t>(stopBlockId)) {
            nodes.push_back(std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
            m_definedRegisters.insert(reg);
        }
    }
}

bool ASTLifter::IsDuplicableValueArm(uint32_t blockId, uint32_t stopBlockId) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (blockId >= blocks.size() || stopBlockId >= blocks.size())
        return false;
    const auto &b = blocks[blockId];
    // only a plain value block whose single successor IS the branch merge; not a header/loop/return, and
    // not a block that flows somewhere else first (duplicating a larger region would be unsound).
    if (b.bType != BlockType::Standard || b.successors.size() != 1 || b.successors[0] != stopBlockId || !b.lpHead || !b.lpTail)
        return false;

    // every real instruction must be a pure, idempotent load/move: re-running it cannot raise, has no
    // side effect, and yields the same value. arithmetic (can raise on bad types), calls, and any
    // table/global/upvalue store are excluded; duplicating those would move or double an effect.
    bool feedsMergePhi = false;
    for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
        const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
        if (ins.operation == LiftedOperation::NOP)
            continue;
        if (ins.operation != LiftedOperation::LOAD && ins.operation != LiftedOperation::MOVE)
            return false;
        if (ins.operands.empty() || ins.operands[0].type != LiftedOperandType::Register)
            return false;
        const int32_t dst = ins.operands[0].value.reg;
        for (const auto &phi : blocks[stopBlockId].phiNodes)
            if (!phi.operands.empty() && phi.operands[0].type == LiftedOperandType::Register && phi.operands[0].value.reg == dst)
                feedsMergePhi = true;
    }
    // require the block to actually feed the merge phi; proves it is a value arm, not an empty block.
    return feedsMergePhi;
}

std::optional<std::vector<uint32_t>> ASTLifter::SharedTailRegion(uint32_t start, uint32_t stop) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (start >= blocks.size() || stop >= blocks.size() || start == stop)
        return std::nullopt;
    // small, forward-only, entered only through `start`, and ending at `stop`, a return, or a `break` out of the innermost loop
    constexpr size_t kMaxBlocks = 6, kMaxInstructions = 64;
    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    std::vector<uint32_t> region;
    boost::unordered_flat_set<uint32_t> inRegion;
    std::vector<uint32_t> pending{start};
    size_t instructions = 0;
    while (!pending.empty()) {
        const uint32_t id = pending.back();
        pending.pop_back();
        if (id == stop || id == loopExit)
            continue;
        if (id >= blocks.size())
            return std::nullopt;
        if (!inRegion.insert(id).second)
            continue;
        const auto &block = blocks[id];
        if (inRegion.size() > kMaxBlocks || !block.lpHead || (block.successors.empty() && block.bType != BlockType::Return) ||
            std::ranges::find(m_liftingBlocks, id) != m_liftingBlocks.end())
            return std::nullopt;
        instructions += static_cast<size_t>(block.lpTail - block.lpHead) + 1;
        if (instructions > kMaxInstructions)
            return std::nullopt;
        for (const uint32_t successor : block.successors) {
            // a loop wholly inside the tail is copied with it; a back-edge out of the region is not
            if (successor <= id) {
                if (!inRegion.contains(successor))
                    return std::nullopt;
                continue;
            }
            pending.push_back(successor);
        }
        region.push_back(id);
    }
    for (const uint32_t id : region)
        if (id != start)
            for (const uint32_t predecessor : blocks[id].predecessors)
                if (!inRegion.contains(predecessor))
                    return std::nullopt;
    return region;
}

bool ASTLifter::IsDuplicablePureRegion(uint32_t startId, uint32_t stopBlockId) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (startId >= blocks.size() || stopBlockId >= blocks.size() || startId == stopBlockId)
        return false;

    // cap region size: real short-circuit value regions are a handful of blocks. a hostile deeply nested
    // boolean DAG must not turn each duplication into unbounded work (the global m_valueArmDuplications
    // cap bounds the count of duplications; this bounds the cost of each).
    constexpr size_t kMaxRegionBlocks = 32;

    // a block is part of a pure short-circuit region iff it only loads/moves values (a value block) or
    // branches on a register's truthiness (JUMPIF/JUMPIFNOT terminator). anything else; a store, call,
    // arithmetic, comparison, global/table op; can raise or have an effect, so it is not duplicable.
    const auto isPureValueOrTruthiness = [&](const BasicBlock &b) -> bool {
        if (!b.lpHead || !b.lpTail)
            return false;
        if (b.bType != BlockType::Standard && b.bType != BlockType::IfHeader)
            return false;
        for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            switch (ins.operation) {
            case LiftedOperation::NOP:
            case LiftedOperation::LOAD:
            case LiftedOperation::MOVE:
                continue;
            case LiftedOperation::JUMPIF:
            case LiftedOperation::JUMPIFNOT:
                // a truthiness branch on a register is pure, but only valid as the block terminator.
                if (&ins != b.lpTail || ins.operands.empty() || ins.operands[0].type != LiftedOperandType::Register)
                    return false;
                continue;
            default:
                return false;
            }
        }
        return true;
    };

    // forward flood from startId, stopping at stopBlockId (the merge boundary). every block reached must
    // be pure and every successor must stay inside the region or hit stopBlockId; an escape to a
    // non-pure block, a return, or a dangling edge means stopBlockId does not post-dominate the region
    // and re-lifting up to it would pull in something with side effects.
    boost::unordered_flat_set<uint32_t> region;
    std::vector<uint32_t> stack{startId};
    bool feedsMergePhi = false;
    while (!stack.empty()) {
        const uint32_t cur = stack.back();
        stack.pop_back();
        if (cur == stopBlockId)
            continue; // the merge is the boundary; do not traverse into it.
        if (cur >= blocks.size())
            return false;
        if (!region.insert(cur).second)
            continue;
        if (region.size() > kMaxRegionBlocks)
            return false;
        const auto &b = blocks[cur];
        if (!isPureValueOrTruthiness(b) || b.successors.empty())
            return false; // impure, or a pure exit that is not the merge (region does not reconverge).
        // a value block that defines a register read by the merge phi proves this is a value-producing
        // short-circuit, not an arbitrary pure region.
        for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            if ((ins.operation != LiftedOperation::LOAD && ins.operation != LiftedOperation::MOVE) || ins.operands.empty() ||
                ins.operands[0].type != LiftedOperandType::Register)
                continue;
            const int32_t dst = ins.operands[0].value.reg;
            for (const auto &phi : blocks[stopBlockId].phiNodes)
                if (!phi.operands.empty() && phi.operands[0].type == LiftedOperandType::Register && phi.operands[0].value.reg == dst)
                    feedsMergePhi = true;
        }
        for (const uint32_t succ : b.successors) {
            if (succ >= blocks.size())
                return false;
            stack.push_back(succ);
        }
    }
    return feedsMergePhi;
}

int32_t ASTLifter::FindMergeBlock(uint32_t branchA, uint32_t branchB) {
    if (branchA == branchB)
        return static_cast<int32_t>(branchA);

    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    const auto cacheKey = std::make_tuple(branchA, branchB, loopExit);
    if (const auto it = m_mergeCache.find(cacheKey); it != m_mergeCache.end())
        return it->second;

    const auto &blocks = m_currentFunction->basicBlocks;
    // a conditional jump into the innermost loop's latch is a `continue`, not flow into the merge
    const auto continuesLoop = [&](const BasicBlock &from, uint32_t to) {
        if (loopExit == InvalidBlockId || from.bTerminator != BlockTerminator::Conditional || blocks[to].bType != BlockType::LoopLatch)
            return false;
        return std::ranges::find(blocks[to].successors, loopExit) != blocks[to].successors.end();
    };

    // true iff every forward path from x hits M before an exit (back-edges skipped to stay acyclic). The
    // merge must post-dominate both branches; a non-post-dominator causes exponential shared-tail re-lift.
    const auto postDominates = [&](uint32_t m, uint32_t x) -> bool {
        if (m == x)
            return true;
        boost::unordered_flat_set<uint32_t> seen;
        std::vector<uint32_t> stack{x};
        bool reached = false;
        while (!stack.empty()) {
            const uint32_t cur = stack.back();
            stack.pop_back();
            if (cur == m)
                continue; // this path reached M
            if (!seen.insert(cur).second)
                continue;
            const auto &block = blocks[cur];
            for (const uint32_t succ : block.successors) {
                // block ids follow instruction order, so a backward edge continues a loop (bridges and latches alike)
                if (succ <= cur) {
                    if (succ < x)
                        return false; // the enclosing loop's next iteration is reached without passing M
                    continue;         // an inner loop's back-edge: ignore to keep the walk acyclic
                }
                if (succ == m)
                    reached = true;
                else if (continuesLoop(block, succ) || succ == loopExit)
                    continue; // `continue` and `break` leave the region without flowing into any merge
                else
                    stack.push_back(succ);
            }
            // an exit is reachable from x without passing through M; inside a loop a `return` leaves like `break`
            if (block.successors.empty() && (loopExit == InvalidBlockId || block.bType != BlockType::Return))
                return false;
        }
        return reached;
    };

    int32_t result = -1;
    const auto usesTerminalFold = [&](uint32_t id) {
        const auto &block = blocks.at(id);
        if (block.bType != BlockType::Return)
            return false;
        if (block.lpTail->operands.size() > 1 && block.lpTail->operands[1].value.imm.n == 1)
            return true;
        return std::all_of(block.lpHead, block.lpTail + 1, [](const LiftedInstruction &instruction) {
            return instruction.operation == LiftedOperation::RETURN || instruction.operation == LiftedOperation::NOP;
        });
    };
    if (!usesTerminalFold(branchA) && !usesTerminalFold(branchB)) {
        // nearest block (BFS from branchB) that post-dominates both branches is the immediate merge.
        boost::unordered_flat_set<uint32_t> visited;
        std::queue<uint32_t> q;
        q.push(branchB);
        visited.insert(branchB);
        while (!q.empty()) {
            const uint32_t cur = q.front();
            q.pop();
            if (postDominates(cur, branchA) && postDominates(cur, branchB)) {
                result = static_cast<int32_t>(cur);
                break;
            }
            const auto &block = blocks[cur];
            for (const uint32_t succ : block.successors) {
                // the walk skips `break` edges, so the loop exit would trivially post-dominate a body that falls into it
                if (succ <= cur || (succ == loopExit && succ != branchA))
                    continue;
                if (visited.insert(succ).second)
                    q.push(succ);
            }
        }
    }

    m_mergeCache.emplace(cacheKey, result);
    return result;
}

std::optional<ASTLifter::OrChainInfo> ASTLifter::DetectOrChain(uint32_t headerId) {
    if (auto guard = DetectGuardRegion(headerId))
        return guard;
    const auto &blocks = m_currentFunction->basicBlocks;
    if (headerId >= blocks.size())
        return std::nullopt;
    const auto &head = blocks[headerId];
    if (head.bType != BlockType::IfHeader || !head.ifStatementTrue.has_value() || !head.ifStatementFalse.has_value())
        return std::nullopt;

    // The shared OR target is the head's jump-to-true edge. Every link of the
    // chain must reach this same block.
    const uint32_t body = head.ifStatementTrue.value();
    const auto truthyClosure = [&](uint32_t id) -> const LiftedInstruction * {
        if (id >= blocks.size())
            return nullptr;
        const auto &b = blocks[id];
        if (!b.lpTail || (b.lpTail->operation != LiftedOperation::JUMPIF && b.lpTail->operation != LiftedOperation::JUMPIFNOT) ||
            b.lpTail->operands.empty() || b.lpTail->operands[0].type != LiftedOperandType::Register)
            return nullptr;
        for (auto *inst = b.lpHead; inst && inst < b.lpTail; ++inst)
            if ((inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE) && !inst->operands.empty() &&
                inst->operands[0].type == LiftedOperandType::Register && inst->operands[0].value.reg == b.lpTail->operands[0].value.reg &&
                inst->operands[0].ssaVersion == b.lpTail->operands[0].ssaVersion)
                return inst;
        return nullptr;
    };
    const auto isLink = [&](uint32_t id) {
        if (id >= blocks.size())
            return false;
        const auto &b = blocks[id];
        if (b.bType != BlockType::IfHeader || !b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value() ||
            (b.ifStatementTrue.value() != body && b.ifStatementFalse.value() != body))
            return false;
        const auto *closure = truthyClosure(id);
        for (auto *inst = b.lpHead; inst && inst < b.lpTail; ++inst)
            if (inst != closure && inst->operation != LiftedOperation::NOP && inst->operation != LiftedOperation::PHI && !ShouldInline(inst))
                return false;
        return true;
    };

    // Structural walk first (no condition lifting): follow the run while each link
    // shares `body`. A true edge hitting body continues through the false edge; the
    // final false edge must hit body, distinguishing OR dispatch from value folding.
    struct Link {
        uint32_t blockId;
        bool invert;
    };
    std::vector<Link> links;
    std::set<uint32_t> guard;
    uint32_t cur = headerId;
    uint32_t elseIdx = InvalidBlockId;
    bool complete = false;
    bool invertedFinal = false;
    bool truthyFinal = false;

    while (cur < blocks.size() && !guard.contains(cur)) {
        const auto &b = blocks[cur];
        if (b.bType != BlockType::IfHeader || !b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value())
            break;
        if (!b.lpTail || b.bTerminator != BlockTerminator::Conditional)
            break;

        const uint32_t bt = b.ifStatementTrue.value();
        const uint32_t bf = b.ifStatementFalse.value();

        if (bt == body) {
            links.push_back({cur, false});
            guard.insert(cur);
            // a test also reached from outside the chain is a join of the enclosing code, not the next term
            const bool enteredFromChain = bf < blocks.size() && std::ranges::all_of(blocks[bf].predecessors, [&](uint32_t p) { return guard.contains(p); });
            if (isLink(bf) && !guard.contains(bf) && enteredFromChain) {
                cur = bf; // fall-through is the next link
                continue;
            }
            elseIdx = bf;
            complete = true;
            truthyFinal = truthyClosure(cur) != nullptr;
            break;
        }
        if (bf == body) {
            links.push_back({cur, true}); // inverted final term: false edge reaches body
            guard.insert(cur);
            elseIdx = bt;
            complete = true;
            invertedFinal = true;
            break;
        }
        break; // does not share the body -> end of (non-)chain
    }

    if (!complete || links.size() < 2 || elseIdx == InvalidBlockId)
        return std::nullopt;
    if (!invertedFinal && !truthyFinal) {
        // Every link jumps to `body`: an `and` guard, read as `not a or not b`. Only when `body` is a real
        // arm that joins `elseIdx` later; if `body` is itself the join, this is a folded `x = a and b` value.
        const int32_t join = FindMergeBlock(body, elseIdx);
        if (join < 0 || static_cast<uint32_t>(join) == body || static_cast<uint32_t>(join) == elseIdx ||
            CanReach(elseIdx, body, static_cast<uint32_t>(join), {headerId}))
            return std::nullopt;
    }

    // Structure confirmed. Lift each link's condition (the condition under which it
    // reaches `body`) and OR-fold left to right.
    std::vector<std::shared_ptr<Expression>> conditions;
    conditions.reserve(links.size());
    for (const auto &lk : links) {
        auto c = truthyClosure(lk.blockId)
                     ? std::shared_ptr<Expression>(std::make_shared<BooleanLiteralNode>(blocks[lk.blockId].lpTail->operation == LiftedOperation::JUMPIF))
                     : LiftCondition(blocks[lk.blockId].lpTail);
        if (lk.invert)
            c = InvertCondition(c);
        conditions.push_back(std::move(c));
    }
    while (conditions.size() > 1) {
        std::vector<std::shared_ptr<Expression>> next;
        next.reserve((conditions.size() + 1) / 2);
        for (size_t i = 0; i < conditions.size(); i += 2) {
            if (i + 1 == conditions.size())
                next.push_back(std::move(conditions[i]));
            else
                next.push_back(std::make_shared<BinaryExpressionNode>("or", std::move(conditions[i]), std::move(conditions[i + 1])));
        }
        conditions = std::move(next);
    }

    OrChainInfo info;
    info.condition = std::move(conditions.front());
    info.bodyIdx = body;
    info.elseIdx = elseIdx;
    info.chainBlocks.reserve(links.size());
    for (const auto &lk : links)
        info.chainBlocks.push_back(lk.blockId);
    return info;
}

std::optional<ASTLifter::OrChainInfo> ASTLifter::DetectGuardRegion(uint32_t headerId) {
    const auto &blocks = m_currentFunction->basicBlocks;
    std::set<uint32_t> headers, leaves;
    std::vector<uint32_t> pending{headerId};
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        if (id >= blocks.size() || id < headerId || headers.size() > 24 || leaves.size() > 2)
            return std::nullopt;
        if (headers.contains(id) || leaves.contains(id))
            continue;
        const auto &block = blocks[id];
        bool conditionOnly = block.bType == BlockType::IfHeader && block.ifStatementTrue && block.ifStatementFalse && block.phiNodes.empty();
        if (conditionOnly && id != headerId)
            for (auto *inst = block.lpHead; inst < block.lpTail; ++inst)
                if (inst->operation != LiftedOperation::NOP && !ShouldInline(inst))
                    conditionOnly = false;
        if (!conditionOnly) {
            leaves.insert(id);
            continue;
        }
        headers.insert(id);
        for (const auto successor : {*block.ifStatementTrue, *block.ifStatementFalse}) {
            if (successor <= id)
                return std::nullopt;
            pending.push_back(successor);
        }
    }
    if (headers.size() < 2 || leaves.size() != 2)
        return std::nullopt;
    const auto bareReturn = [&](uint32_t id) {
        const auto &block = blocks[id];
        return block.bType == BlockType::Return && block.lpTail->operands.size() > 1 && block.lpTail->operands[1].value.imm.n == 1 &&
               std::all_of(block.lpHead, block.lpTail + 1, [](const LiftedInstruction &inst) {
                   return inst.operation == LiftedOperation::NOP || inst.operation == LiftedOperation::RETURN;
               });
    };
    for (const auto id : headers)
        if (id != headerId)
            for (const auto pred : blocks[id].predecessors)
                if (!headers.contains(pred))
                    return std::nullopt;
    // a def folded into the condition no longer reaches a phi that reads it on the path out of its test
    for (const auto leaf : leaves)
        for (const auto &phi : blocks[leaf].phiNodes)
            for (size_t i = 1; i < phi.operands.size(); ++i)
                if (const auto *def = phi.operands[i].type == LiftedOperandType::Register ? m_currentFunction->GetDefinition(phi.operands[i]) : nullptr) {
                    const int defBlock = m_currentFunction->GetBlockId(def);
                    if (defBlock >= 0 && static_cast<uint32_t>(defBlock) != headerId && headers.contains(static_cast<uint32_t>(defBlock)))
                        return std::nullopt;
                }

    uint32_t body = *leaves.begin(), exit = *leaves.rbegin();
    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    const bool exitLeavesIteration =
        exit == loopExit || (loopExit != InvalidBlockId && blocks[exit].bType == BlockType::LoopLatch &&
                             std::ranges::find(blocks[exit].successors, loopExit) != blocks[exit].successors.end());
    if (bareReturn(body) || exitLeavesIteration)
        std::swap(body, exit);
    // pure value arms fold into one short-circuit value expression further up
    if (const int32_t join = FindMergeBlock(body, exit); join >= 0) {
        const auto pureArm = [&](uint32_t arm) {
            return arm == static_cast<uint32_t>(join) || IsDuplicableValueArm(arm, join) || IsDuplicablePureRegion(arm, join);
        };
        if (pureArm(body) && pureArm(exit))
            return std::nullopt;
    }

    using Expr = std::shared_ptr<Expression>;
    const auto binary = [](const char *op, const Expr &lhs, const Expr &rhs) -> Expr { return std::make_shared<BinaryExpressionNode>(op, lhs, rhs); };
    std::map<uint32_t, Expr> conditions;
    conditions[body] = std::make_shared<BooleanLiteralNode>(true);
    conditions[exit] = std::make_shared<BooleanLiteralNode>(false);
    for (auto it = headers.rbegin(); it != headers.rend(); ++it) {
        const auto &block = blocks[*it];
        auto condition = LiftCondition(block.lpTail);
        auto yes = conditions.at(*block.ifStatementTrue), no = conditions.at(*block.ifStatementFalse);
        const auto yesBool = std::dynamic_pointer_cast<BooleanLiteralNode>(yes);
        const auto noBool = std::dynamic_pointer_cast<BooleanLiteralNode>(no);
        Expr result;
        if (yesBool && noBool && yesBool->value != noBool->value)
            result = yesBool->value ? condition : InvertCondition(condition);
        else if (yesBool && noBool)
            result = std::make_shared<IfExpressionNode>(condition, yes, no);
        else if (yesBool)
            result = yesBool->value ? binary("or", condition, no) : binary("and", InvertCondition(condition), no);
        else if (noBool)
            result = noBool->value ? binary("or", InvertCondition(condition), yes) : binary("and", condition, yes);
        else if (auto either = std::dynamic_pointer_cast<BinaryExpressionNode>(no); either && either->op == "or" && either->right == yes)
            result = binary("or", binary("and", InvertCondition(condition), either->left), yes);
        else if (auto either = std::dynamic_pointer_cast<BinaryExpressionNode>(yes); either && either->op == "or" && either->right == no)
            result = binary("or", binary("and", condition, either->left), no);
        else
            result = std::make_shared<IfExpressionNode>(condition, yes, no);
        conditions[*it] = result;
    }
    return OrChainInfo{conditions.at(headerId), body, exit, {headers.begin(), headers.end()}};
}
