//
// Created by Dottik on 10/12/2025.
//

#include "ASTLifter.hpp"
#include "ASTLifterShared.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "SSABuilder.hpp"
#include "SafetyGuard.hpp"

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

static std::shared_ptr<BlockStatementNode> CreateBlock(const std::vector<std::shared_ptr<Statement>> &stmts) {
    auto block = std::make_shared<BlockStatementNode>();
    block->body = stmts;
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
        void await_suspend(Handle handle) const noexcept { *handle.promise().next = handle.promise().continuation; }
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

static std::string DisambiguateClosureName(
    AnalyzedFunction &parent, const AnalyzedFunction &child, const LiftedOperand &destination, int32_t bytecodeId, bool createsBinding, std::string name
) {
    if (!createsBinding || parent.IsConsumedByPhi(destination) || !child.enclosingNames.contains(name))
        return name;

    const std::string base = std::format("{}_{}", name, bytecodeId);
    std::string unique = base;
    for (size_t index = 2; child.enclosingNames.contains(unique); ++index)
        unique = std::format("{}_{}", base, index);
    parent.ssaOverrides[{static_cast<uint8_t>(destination.value.reg), destination.ssaVersion}] = unique;
    return unique;
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
        m_debugFunction = std::format(
            "F{} ({})",
            analyzedFunction.lpLiftedFunction->lpDeserialized ? static_cast<int>(analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId) : -1,
            analyzedFunction.lpLiftedFunction->name
        );
    Explain("function {}: lifting {} CFG blocks", m_debugFunction, analyzedFunction.basicBlocks.size());
    this->m_definedRegisters.clear();
    this->m_pinnedRegisters.clear();
    this->m_capturedRegisters.clear();
    this->m_referenceCapturedValues.clear();
    this->m_valueCapturedValues.clear();
    this->m_assignedRegisters.clear();
    for (const auto &instruction : analyzedFunction.lpLiftedFunction->instructions)
        if (const int32_t local = AssignedLocal(instruction); local >= 0)
            this->m_assignedRegisters.insert(local);
    this->m_hoistedRegisters.clear();
    this->m_capturedVariableWrites.clear();
    this->m_processedInstructions.clear();
    this->m_earlyForExits.clear();
    this->m_setListKeySnapshots.clear();
    this->m_inlineConsumedDefs.clear();
    this->m_foldConsumedDefs.clear();
    this->m_pendingClasses.clear();
    this->m_closureSlots.clear();
    this->m_phiConsumers.clear();
    this->m_deferToConditionInline.clear();
    this->m_shouldInlineMemo.clear(); // keyed by this function's instructions
    this->m_inlineBinaryDepth.clear();
    this->m_constructorElementMemo.clear();
    this->m_shouldInlineActive.clear();
    this->m_mergeCache.clear(); // keyed by this function's block ids
    this->m_forwardReach.clear();
    this->m_dominators.reset();
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

    const auto &instructions = analyzedFunction.lpLiftedFunction->instructions;
    this->m_blockOfInstruction.assign(instructions.size(), -1);
    for (const auto &block : analyzedFunction.basicBlocks)
        if (block.lpHead)
            for (const auto *inst = block.lpHead; inst <= block.lpTail; ++inst)
                this->m_blockOfInstruction[inst - instructions.data()] = static_cast<int32_t>(block.dwBlockId);
    CollectCompoundAssignments();

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
        const int definitionBlock = BlockOf(definition);
        const LiftedInstruction *crossBlockUser = nullptr;
        for (const auto &ref : refs) {
            if (const auto users = analyzedFunction.users.find(ref); users != analyzedFunction.users.end())
                for (const auto *user : users->second)
                    if (BlockOf(user) != definitionBlock) {
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
            if (sideEffectful && BlockOf(def) != static_cast<int>(block.dwBlockId)) {
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
                    setList->operands[0].value.reg != table->operands[0].value.reg || setList->operands[0].ssaVersion != table->operands[0].ssaVersion ||
                    !analyzedFunction.implicitUses.contains(setList))
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

    Explain(
        "function {}: forced {} definitions to remain statements because moving them could change effects or closure order", m_debugFunction,
        m_forcedMaterialization.size()
    );

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
        std::unordered_map<std::string, SSARef> captureNameOwners;
        std::unordered_map<SSARef, std::string> referenceCaptureNames;
        std::optional<std::array<int, 256>> definitionCountsCache;
        const auto definitionCounts = [&]() -> const std::array<int, 256> & {
            if (!definitionCountsCache) {
                definitionCountsCache.emplace();
                definitionCountsCache->fill(0);
                for (const auto &[ref, definition] : analyzedFunction.definitionMap)
                    if (definition && definition->operation != LiftedOperation::PHI)
                        ++(*definitionCountsCache)[ref.regIndex];
            }
            return *definitionCountsCache;
        };
        std::optional<std::array<std::vector<std::pair<int32_t, const LiftedInstruction *>>, 256>> definitionsByRegisterCache;
        // a by-reference captured local's values: the captured version, its register's writes until the CLOSEUPVALS ending its scope, and phis of those
        const auto referenceCapturedVersions = [&](uint8_t reg, int32_t capturedVersion, int32_t captureIndex, size_t capturesEnd) {
            if (!definitionsByRegisterCache) {
                definitionsByRegisterCache.emplace();
                for (const auto &[ref, definition] : analyzedFunction.definitionMap)
                    if (definition)
                        (*definitionsByRegisterCache)[ref.regIndex].emplace_back(ref.version, definition);
            }
            const auto &definitions = (*definitionsByRegisterCache)[reg];
            auto closeIndex = static_cast<int32_t>(instrs.size());
            for (const auto &[index, lowest] : analyzedFunction.lpLiftedFunction->upvalueCloses)
                if (index >= static_cast<int32_t>(capturesEnd) && lowest <= reg) {
                    closeIndex = index;
                    break;
                }
            boost::unordered_flat_set<int32_t> versions{capturedVersion};
            for (const auto &[version, definition] : definitions)
                if (definition->operation != LiftedOperation::PHI && definition->instructionIndex > captureIndex && definition->instructionIndex < closeIndex)
                    versions.insert(version);
            for (bool grew = true; grew;) {
                grew = false;
                for (const auto &[version, definition] : definitions)
                    if (definition->operation == LiftedOperation::PHI && !versions.contains(version) &&
                        std::any_of(definition->operands.begin() + 1, definition->operands.end(), [&](const LiftedOperand &input) {
                            return versions.contains(input.ssaVersion);
                        }))
                        grew |= versions.insert(version).second;
            }
            return versions;
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
            // a `local function` enters its debug range only after its captures
            size_t capturesEnd = i + 1;
            while (capturesEnd < instrs.size() && instrs[capturesEnd].operation == LiftedOperation::CAPTURE)
                ++capturesEnd;
            for (size_t capIdx = 0; i + 1 + capIdx < instrs.size() && instrs[i + 1 + capIdx].operation == LiftedOperation::CAPTURE; ++capIdx) {
                const auto &cap = instrs[i + 1 + capIdx];
                const int mode = cap.operands[0].value.imm.n;
                const bool srcIsRegister = cap.operands.size() >= 2 && cap.operands[1].type == LiftedOperandType::Register;
                if ((mode == 0 || mode == 1) && srcIsRegister) {
                    m_capturedRegisters.insert(cap.operands[1].value.reg);
                    const uint8_t reg = static_cast<uint8_t>(cap.operands[1].value.reg);
                    boost::unordered_flat_set<int32_t> capturedVersions;
                    if (mode == 1) {
                        capturedVersions = referenceCapturedVersions(reg, cap.operands[1].ssaVersion, cap.instructionIndex, capturesEnd);
                        for (const int32_t version : capturedVersions)
                            m_referenceCapturedValues.insert({reg, version});
                    } else {
                        m_valueCapturedValues.insert({reg, cap.operands[1].ssaVersion});
                    }
                    const bool selfCapture = reg == inst.operands[0].value.reg && cap.operands[1].ssaVersion == inst.operands[0].ssaVersion;
                    // An auto-shaped debug name (`v1`, from recompiled output) would alias another register's default name.
                    if (proto->upvalueNames.size() > capIdx && !AnalyzedFunction::IsAutoNameShaped(proto->upvalueNames[capIdx])) {
                        const SSARef ref{reg, cap.operands[1].ssaVersion};
                        std::string name = localName(proto->upvalueNames[capIdx], reg);
                        if (const auto shared = referenceCaptureNames.find(ref); shared != referenceCaptureNames.end())
                            name = shared->second;
                        else if (
                            const auto existing = analyzedFunction.ssaOverrides.find(ref);
                            existing != analyzedFunction.ssaOverrides.end() && captureNameOwners.contains(existing->second)
                        )
                            name = existing->second;
                        else {
                            const std::string base = name;
                            for (int suffix = 2; captureNameOwners.contains(name) && !(captureNameOwners.at(name) == ref); ++suffix)
                                name = std::format("{}_{}", base, suffix);
                        }
                        captureNameOwners.try_emplace(name, ref);
                        analyzedFunction.ssaOverrides[ref] = name;
                        if (mode == 1)
                            for (const int32_t version : capturedVersions)
                                referenceCaptureNames.try_emplace(SSARef{reg, version}, name);
                        // a parameter is one local for the whole body; reads before the capture must agree
                        if (cap.operands[1].value.reg < analyzedFunction.lpLiftedFunction->lpDeserialized->numparams && IsValidLuauIdent(name))
                            analyzedFunction.SetGlobalName(cap.operands[1].value.reg, name);
                        if (mode == 1)
                            for (const auto &local : analyzedFunction.lpLiftedFunction->lpDeserialized->locvars)
                                if (local.reg == cap.operands[1].value.reg && local.startpc <= static_cast<int32_t>(capturesEnd) &&
                                    cap.instructionIndex < local.endpc)
                                    for (const auto &[ref, definition] : analyzedFunction.definitionMap)
                                        if (ref.regIndex == local.reg && definition && local.startpc <= definition->instructionIndex &&
                                            definition->instructionIndex < local.endpc)
                                            analyzedFunction.ssaOverrides[ref] = name;
                    } else if (mode == 1 && proto->upvalueNames.size() <= capIdx && reg >= analyzedFunction.lpLiftedFunction->numparams) {
                        // no debug locals: every value of the captured variable is one local
                        const std::string name =
                            selfCapture && proto->debugName ? localName(*proto->debugName, reg)
                            : definitionCounts()[reg] > static_cast<int>(capturedVersions.size())
                                ? localName(
                                      std::format(
                                          "upv{}_{}_{}", reg, cap.operands[1].ssaVersion, analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId
                                      ),
                                      reg
                                  )
                                : analyzedFunction.GetVarName(reg, cap.operands[1].ssaVersion);
                        for (const int32_t version : capturedVersions) {
                            const SSARef ref{reg, version};
                            analyzedFunction.ssaOverrides.try_emplace(ref, name);
                            const auto definition = analyzedFunction.definitionMap.find(ref);
                            if (definition != analyzedFunction.definitionMap.end() && definition->second &&
                                definition->second->operation != LiftedOperation::PHI && definition->second->instructionIndex > cap.instructionIndex)
                                m_capturedVariableWrites.insert(ref);
                        }
                    } else if (
                        mode == 0 && proto->upvalueNames.size() <= capIdx &&
                        cap.operands[1].value.reg >= analyzedFunction.lpLiftedFunction->lpDeserialized->numparams
                    ) {
                        // without debug names the captured local would share its register's default name with a
                        // later local reusing the slot; the hoister then merges them and a loop's closures share one variable
                        const SSARef ref{static_cast<uint8_t>(cap.operands[1].value.reg), cap.operands[1].ssaVersion};
                        const auto def = analyzedFunction.definitionMap.find(ref);
                        const bool closure = def != analyzedFunction.definitionMap.end() && def->second &&
                                             (def->second->operation == LiftedOperation::NEWCLOSURE || def->second->operation == LiftedOperation::DUPCLOSURE);
                        if (!closure && definitionCounts()[ref.regIndex] > 1)
                            analyzedFunction.ssaOverrides.try_emplace(
                                ref, localName(
                                         std::format("upv{}_{}_{}", ref.regIndex, ref.version, analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId),
                                         ref.regIndex
                                     )
                            );
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
            if (analyzedFunction.IsConsumedByPhi(definition->operands[0]))
                continue;
            if ((definition->operation == LiftedOperation::GETIMPORT || definition->operation == LiftedOperation::GETGLOBAL) && !ShouldInline(definition)) {
                const auto users = analyzedFunction.users.find(ref);
                if (users != analyzedFunction.users.end() && std::ranges::any_of(users->second, [&](const LiftedInstruction *user) {
                        return (user->operation == LiftedOperation::CALL || user->operation == LiftedOperation::CALLFB) && !user->operands.empty() &&
                               user->operands[0].type == LiftedOperandType::Register && user->operands[0].value.reg == reg &&
                               user->operands[0].ssaVersion == ref.version;
                    }))
                    analyzedFunction.ssaOverrides[ref] = std::format("v{}_{}", reg, ref.version);
            }
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
            const auto block = BlockOf(definition);
            if (block < 0)
                continue;
            bool overlaps = false;
            for (size_t j = i + 1; j < definitions.size() && definitions[j].second->instructionIndex < lastUse; ++j)
                if (BlockOf(definitions[j].second) == block) {
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

        KeepOrderedCompoundAssignments();
        boost::unordered_flat_set<uint32_t> visited;
        ast.statements = LiftControlFlow(0, InvalidBlockId, visited).Run();
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

void ASTLifter::ExplainKeep(
    const LiftedInstruction *definition, std::string_view reason, const LiftedInstruction *consumer, const LiftedInstruction *barrier
) const {
    if (!m_debugNotes || !m_debugNotes->Enabled() || !definition)
        return;
    const int blockId = BlockOf(definition);
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
        message += std::format(" before phi in B{}", BlockOf(consumer));
    else if (consumer)
        message += std::format(" before _{} in B{}", consumer->instructionIndex, BlockOf(consumer));
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
    const auto markEarlyForExit = [&](uint32_t target) {
        for (auto it = m_earlyForExits.rbegin(); it != m_earlyForExits.rend(); ++it)
            if (it->first == target) {
                auto name = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(it->second));
                nodes.push_back(std::make_shared<AssignmentStatementNode>(name, std::make_shared<BooleanLiteralNode>(true)));
                break;
            }
    };

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
            markEarlyForExit(currentBlockId);
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

        // Duplicate only return-only merges; every other merge may carry an effect. Arms jumping to a shared exit leave it
        // to render once after them.
        if (currentBlockId == stopBlockId &&
            (!isReturnOnly(currentBlockId) || std::ranges::any_of(m_currentFunction->basicBlocks[currentBlockId].predecessors, [&](uint32_t pred) {
                const auto *tail = m_currentFunction->basicBlocks[pred].lpTail;
                return tail && tail->operation == LiftedOperation::JUMP;
            })))
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
                } else if (
                    auto region = canDup || siblingTail ? SharedTailRegion(currentBlockId, stopBlockId) : std::nullopt;
                    // a pure tail reached through pure tests folds back into one short-circuit value
                    region && (canDup || std::ranges::any_of(*region, blockHasEffect) || std::ranges::any_of(walked, blockHasEffect))
                ) {
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
        m_liftingBlocks.push_back(currentBlockId);
        const LiftingScope liftingScope{m_liftingBlocks};

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
        // a header exits by itself only when one of its arms is the loop exit; an arm that runs code before
        // leaving (an inlined `return`) is a break arm of a `while true`
        const bool deferredWhileHeader =
            block.bType == BlockType::LoopHeader && block.loopLatch && (block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop &&
            block.successors.size() == 2 &&
            (std::all_of(
                 block.successors.begin(), block.successors.end(),
                 [&](uint32_t successor) { return CanReach(successor, *block.loopLatch, currentBlockId, {currentBlockId}); }
             ) ||
             (block.loopExit && *block.loopExit != *block.loopLatch && std::ranges::find(block.successors, *block.loopExit) == block.successors.end()));
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
                const bool otherArmBreaks = !m_loopExitStack.empty() && otherArm == m_loopExitStack.back();
                const bool mergeIsNextIterationArm = !mergeTargetsLoopExit && mergeFromGraph && mergeIsDirectArm && mergeIdx != stopBlockId &&
                                                     !otherArmBreaks && !CanReach(otherArm, mergeIdx, currentBlockId, {currentBlockId});
                if ((mergeTargetsLoopExit && !mergeTargetsActiveLoopExit) || mergeIsNextIterationArm)
                    mergeIdx = InvalidBlockId;
                // one arm flows to this region's end while the other jumps into the latch: join at the region end and
                // let the latch edge lift as `continue`
                const uint32_t latchArm = isInnermostLatch(trueIdx) ? trueIdx : falseIdx;
                const uint32_t flowingArm = latchArm == trueIdx ? falseIdx : trueIdx;
                if (mergeIdx != stopBlockId && stopBlockId != InvalidBlockId && (mergeIdx == InvalidBlockId || isInnermostLatch(mergeIdx)) &&
                    isInnermostLatch(latchArm) && (flowingArm == stopBlockId || CanReach(flowingArm, stopBlockId, latchArm, {currentBlockId})))
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
                        // the arm the test falls into is the source's `then`
                        ifStmt->bFallthroughInElse = true;
                    }
                    if (!elseStmts.empty()) {
                        ifStmt->elseBranch = CreateBlock(elseStmts);
                    }

                    // only swap when there's an else to swap in; else-less empty-then would
                    // leave thenBranch null and feed a malformed node to the fold passes.
                    if (ifStmt->thenBranch->body.empty() && ifStmt->elseBranch) {
                        std::swap(ifStmt->thenBranch, ifStmt->elseBranch);
                        ifStmt->condition = InvertCondition(trueCond);
                        ifStmt->bFallthroughInElse = false;
                    }
                }

                HoistPhiLocals(static_cast<int32_t>(mergeIdx), stopBlockId, ifStmt, nodes, definedBeforeBranches);
                nodes.push_back(ifStmt);

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
            if (infiniteWhileLatch && isRepeatHeader && (!block.loopExit || !CanReach(*block.loopExit, *infiniteWhileLatch, currentBlockId, {currentBlockId})))
                infiniteWhileLatch.reset();
            if (infiniteWhileLatch)
                Explain(block, "wrap inner loop in while true because enclosing latch B{} returns to this shared header", *infiniteWhileLatch);
            const size_t loopNodesStart = nodes.size();

            if (block.loopLatch.has_value()) {
                uint32_t latchIdx = block.loopLatch.value();
                uint32_t exitIdx = block.loopLatch.value();
                std::optional<std::pair<uint32_t, std::string>> earlyForExit;
                constexpr uint32_t kForFlags = static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
                                               static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                               static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed);
                if ((block.dwBlockFlags & kForFlags) != 0 && block.loopExit) {
                    const uint32_t natural = ResolveLoopExitFromLatch(m_currentFunction->basicBlocks, latchIdx, currentBlockId);
                    if (natural != InvalidBlockId && natural != *block.loopExit && CanReach(natural, *block.loopExit, currentBlockId, {currentBlockId})) {
                        std::string name = std::format("__fission_early_for_{}", currentBlockId);
                        const auto occupied = [&](const std::string &candidate) {
                            const auto named = [&](const auto &names) {
                                return std::ranges::any_of(names, [&](const auto &item) { return item.second == candidate; });
                            };
                            return m_currentFunction->enclosingNames.contains(candidate) || named(m_currentFunction->variableNames) ||
                                   named(m_currentFunction->ssaOverrides) || named(m_currentFunction->globalRegNames) ||
                                   named(m_currentFunction->upvalueNames) ||
                                   std::ranges::any_of(
                                       m_currentFunction->lpLiftedFunction->lpDeserialized->locvars,
                                       [&](const auto &local) { return local.varname == candidate; }
                                   ) ||
                                   std::ranges::any_of(m_currentFunction->lpLiftedFunction->lpDeserialized->constants, [&](const auto &constant) {
                                       return constant.kType == LUA_TSTRING && std::get<std::string>(constant.constantData) == candidate;
                                   });
                        };
                        for (int suffix = 2; occupied(name); ++suffix)
                            name = std::format("__fission_early_for_{}_{}", currentBlockId, suffix);
                        earlyForExit = std::make_pair(natural, name);
                        nodes.push_back(
                            std::make_shared<VariableDeclarationNode>(
                                std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)), std::make_shared<BooleanLiteralNode>(false)
                            )
                        );
                        for (const auto &phi : m_currentFunction->basicBlocks[*block.loopExit].phiNodes) {
                            if (phi.operands.empty() || phi.operands[0].type != LiftedOperandType::Register ||
                                m_definedRegisters.contains(phi.operands[0].value.reg))
                                continue;
                            bool early = false, normal = false;
                            for (size_t inputIndex = 1; inputIndex < phi.operands.size(); ++inputIndex) {
                                const auto &input = phi.operands[inputIndex];
                                const auto *definition = input.type == LiftedOperandType::Register ? m_currentFunction->GetDefinition(input) : nullptr;
                                const int origin = definition ? BlockOf(definition) : -1;
                                early |= origin > static_cast<int>(currentBlockId) && origin < static_cast<int>(latchIdx);
                                normal |= origin >= static_cast<int>(natural) && origin < static_cast<int>(*block.loopExit);
                            }
                            if (early && normal) {
                                nodes.push_back(
                                    std::make_shared<VariableDeclarationNode>(
                                        std::make_shared<Identifier>(m_currentFunction->GetVarName(phi.operands[0].value.reg, phi.operands[0].ssaVersion))
                                    )
                                );
                                m_definedRegisters.insert(phi.operands[0].value.reg);
                                m_hoistedRegisters.insert(phi.operands[0].value.reg);
                            }
                        }
                    }
                }

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
                                if (candidate.bTerminator != BlockTerminator::Conditional || candidate.successors.size() != 2 || !candidate.lpTail ||
                                    candidate.lpTail->instructionIndex >= pred.lpTail->instructionIndex)
                                    continue;
                                const bool firstLoops = CanReach(candidate.successors[0], predId, currentBlockId, {currentBlockId});
                                const bool secondLoops = CanReach(candidate.successors[1], predId, currentBlockId, {currentBlockId});
                                if (firstLoops == secondLoops)
                                    continue;
                                const uint32_t other = firstLoops ? candidate.successors[1] : candidate.successors[0];
                                if (other != latchIdx && !CanReach(other, latchIdx, currentBlockId, {currentBlockId, predId}))
                                    continue;
                                if (testId == InvalidBlockId ||
                                    candidate.lpTail->instructionIndex > m_currentFunction->basicBlocks[testId].lpTail->instructionIndex) {
                                    testId = candidate.dwBlockId;
                                    sharedWhileExit = other;
                                }
                            }
                            if (testId != InvalidBlockId) {
                                sharedWhileLatch = predId;
                                Explain(block, "preserve enclosing while through shared repeat header; latch B{} exits through B{}", predId, *sharedWhileExit);
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
                            if (!conditionDef || BlockOf(conditionDef) != static_cast<int>(latch.dwBlockId))
                                continue;

                            DeferIntoCondition(
                                *conditionDef, {static_cast<uint8_t>(conditionOperand.value.reg), conditionOperand.ssaVersion}, expectedUser,
                                pendingConditionDefs
                            );
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
                            constexpr uint32_t kAllLoopFlags =
                                static_cast<uint32_t>(LoopBlockFlags::WhileLoop) | static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) |
                                static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed) | static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
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
                                    if (!conditionDef || BlockOf(conditionDef) != static_cast<int>(block.dwBlockId))
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
                                    DeferIntoCondition(*conditionDef, {reg, conditionOperand.ssaVersion}, expectedUser, pendingConditionDefs);
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
                            if (earlyForExit)
                                m_earlyForExits.emplace_back(exitIdx, earlyForExit->second);
                            const auto definedBeforeLoopBody = m_definedRegisters;
                            forNode->lpLoopBody = CreateBlock(co_await LiftControlFlow(bodyIdx, *block.loopLatch, visited));
                            m_definedRegisters = definedBeforeLoopBody;
                            if (hasBreakTarget)
                                m_loopExitStack.pop_back();
                            if (earlyForExit)
                                m_earlyForExits.pop_back();

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
                        constexpr uint32_t kAllLoopFlags =
                            static_cast<uint32_t>(LoopBlockFlags::WhileLoop) | static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) |
                            static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                            static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed) | static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
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
                            // a value first written by the loop's break arms is read after it: declare it ahead of the loop
                            for (const auto &phi : m_currentFunction->basicBlocks[*block.loopExit].phiNodes) {
                                if (phi.operands.empty() || phi.operands[0].type != LiftedOperandType::Register ||
                                    m_definedRegisters.contains(phi.operands[0].value.reg))
                                    continue;
                                const bool writtenInLoop = std::all_of(phi.operands.begin() + 1, phi.operands.end(), [&](const LiftedOperand &input) {
                                    const auto *def = input.type == LiftedOperandType::Register ? m_currentFunction->GetDefinition(input) : nullptr;
                                    return def && BlockOf(def) > static_cast<int32_t>(currentBlockId);
                                });
                                if (!writtenInLoop)
                                    continue;
                                nodes.push_back(
                                    std::make_shared<VariableDeclarationNode>(
                                        std::make_shared<Identifier>(m_currentFunction->GetVarName(phi.operands[0].value.reg, phi.operands[0].ssaVersion))
                                    )
                                );
                                m_definedRegisters.insert(phi.operands[0].value.reg);
                                m_hoistedRegisters.insert(phi.operands[0].value.reg);
                            }
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

                            if (headerHasBody && block.bTerminator == BlockTerminator::Conditional) {
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
                        if (earlyForExit)
                            m_earlyForExits.emplace_back(exitIdx, earlyForExit->second);
                        const auto definedBeforeLoopBody = m_definedRegisters;
                        // the loop variables are bound by the `for`; a write in the body assigns them
                        for (int i = 0; i < numVars; ++i)
                            m_definedRegisters.insert(baseReg + 3 + i);
                        forNode->body = CreateBlock(co_await LiftControlFlow(bodyIdx, *block.loopLatch, visited));
                        m_definedRegisters = definedBeforeLoopBody;
                        if (hasBreakTarget)
                            m_loopExitStack.pop_back();
                        if (earlyForExit)
                            m_earlyForExits.pop_back();

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
                            if (indexDef && indexDef->operation == LiftedOperation::LOAD && indexDef->operands.size() > 1 &&
                                indexDef->operands[1].type == LiftedOperandType::ImmediateNil)
                                forNode->index = std::make_shared<NilLiteralNode>();

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

                if (earlyForExit) {
                    auto normalVisited = visited;
                    auto normal = co_await LiftControlFlow(earlyForExit->first, exitIdx, normalVisited);
                    if (!normal.empty()) {
                        auto guard = std::make_shared<IfStatementNode>();
                        guard->condition = std::make_shared<UnaryExpressionNode>(
                            "not ", std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(earlyForExit->second))
                        );
                        guard->thenBranch = CreateBlock(normal);
                        nodes.push_back(guard);
                    }
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
                            if (succ == *infiniteWhileLatch || succ == currentBlockId || succ >= m_currentFunction->basicBlocks.size() ||
                                !seen.insert(succ).second)
                                continue;
                            // an inner loop's latch returns to its header; CanReach does not follow latch back-edges
                            const auto &candidate = m_currentFunction->basicBlocks[succ];
                            const bool innerLatch = candidate.bType == BlockType::LoopLatch && candidate.loopHeader && seen.contains(*candidate.loopHeader);
                            if (candidate.bType != BlockType::Return && !innerLatch && !CanReach(succ, *infiniteWhileLatch, currentBlockId, {currentBlockId})) {
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
            if (!block.successors.empty())
                markEarlyForExit(block.successors.front());
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

    const auto snapshotableKey = [&](const LiftedInstruction &table, const LiftedInstruction &store) {
        if (store.operation != LiftedOperation::SETTABLE || store.operands.size() < 3 || store.operands[2].type != LiftedOperandType::Register ||
            BlockOf(&store) != BlockOf(&table) || !m_definedRegisters.contains(store.operands[2].value.reg))
            return false;
        const auto *definition = m_currentFunction->GetDefinition(store.operands[2]);
        if (!definition || (definition->operation != LiftedOperation::PHI && ShouldInline(definition)))
            return false;
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        for (int index = table.instructionIndex + 1; index < store.instructionIndex; ++index)
            if (instructions[index].operation != LiftedOperation::NOP &&
                (instructions[index].operation != LiftedOperation::LOAD ||
                 (!instructions[index].operands.empty() && instructions[index].operands[0].value.reg == store.operands[2].value.reg)))
                return false;
        return true;
    };

    const auto storeSiteSnapshotableKey = [&](const LiftedInstruction &table, const LiftedInstruction &store) {
        if (store.operation != LiftedOperation::SETTABLE || store.operands.size() < 3 || store.operands[2].type != LiftedOperandType::Register)
            return false;
        const auto *definition = m_currentFunction->GetDefinition(store.operands[2]);
        if (definition && definition->operation == LiftedOperation::PHI)
            return store.instructionIndex > table.instructionIndex;
        if (!definition || definition->instructionIndex <= table.instructionIndex || BlockOf(definition) != BlockOf(&store))
            return false;
        if (!ShouldInline(definition))
            return true;
        if (definition->operation != LiftedOperation::CALL && definition->operation != LiftedOperation::CALLFB &&
            definition->operation != LiftedOperation::NAMECALL && definition->operation != LiftedOperation::NAMECALLUDATA)
            return false;
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        for (int index = definition->instructionIndex + 1; index < store.instructionIndex; ++index)
            if (instructions[index].operation != LiftedOperation::NOP && instructions[index].operation != LiftedOperation::LOAD)
                return false;
        return true;
    };

    const auto freshKeyName = [&](const LiftedInstruction &store) {
        std::string name = std::format("__fission_key_{}", store.instructionIndex);
        const auto occupied = [&](const std::string &candidate) {
            if (m_currentFunction->enclosingNames.contains(candidate))
                return true;
            for (const auto &[_, value] : m_currentFunction->variableNames)
                if (value == candidate)
                    return true;
            for (const auto &[_, value] : m_currentFunction->ssaOverrides)
                if (value == candidate)
                    return true;
            for (const auto &[_, value] : m_currentFunction->globalRegNames)
                if (value == candidate)
                    return true;
            for (const auto &[_, value] : m_currentFunction->upvalueNames)
                if (value == candidate)
                    return true;
            for (const auto &local : m_currentFunction->lpLiftedFunction->lpDeserialized->locvars)
                if (local.varname == candidate)
                    return true;
            for (const auto &constant : m_currentFunction->lpLiftedFunction->lpDeserialized->constants)
                if (constant.kType == LUA_TSTRING && std::get<std::string>(constant.constantData) == candidate)
                    return true;
            for (const auto &[_, value] : m_setListKeySnapshots)
                if (value == candidate)
                    return true;
            return false;
        };
        for (int suffix = 2; occupied(name); ++suffix)
            name = std::format("__fission_key_{}_{}", store.instructionIndex, suffix);
        return name;
    };

    const auto delayedVariadicSetList = [&](const LiftedInstruction &table) -> const LiftedInstruction * {
        if (table.operation != LiftedOperation::NEWTABLE || table.operands.empty() || ShouldInline(&table))
            return nullptr;
        const SSARef ref{table.operands[0].value.reg, table.operands[0].ssaVersion};
        const auto found = m_currentFunction->users.find(ref);
        if (found == m_currentFunction->users.end())
            return nullptr;
        const LiftedInstruction *setList = nullptr;
        for (const auto *user : found->second)
            if (user->operation == LiftedOperation::SETLIST && user->operands.size() > 2 && user->operands[0].value.reg == ref.regIndex &&
                user->operands[0].ssaVersion == ref.version && user->operands[2].value.imm.n == 0) {
                if (setList)
                    return nullptr;
                setList = user;
            }
        if (!setList || setList->instructionIndex <= table.instructionIndex)
            return nullptr;
        if (BlockOf(setList) < 0)
            return nullptr;
        std::vector<const LiftedInstruction *> prior;
        for (const auto *user : found->second) {
            if (user == setList)
                continue;
            if (user->instructionIndex < setList->instructionIndex) {
                const bool list = user->operation == LiftedOperation::SETLIST && user->operands.size() > 3 && user->operands[0].value.reg == ref.regIndex &&
                                  user->operands[0].ssaVersion == ref.version && user->operands[2].value.imm.n > 1;
                const bool field = (user->operation == LiftedOperation::SETTABLEKS || user->operation == LiftedOperation::SETTABLEN) &&
                                   user->operands.size() > 2 && user->operands[1].value.reg == ref.regIndex && user->operands[1].ssaVersion == ref.version;
                bool computedField = user->operation == LiftedOperation::SETTABLE && user->operands.size() > 2 && user->operands[1].value.reg == ref.regIndex &&
                                     user->operands[1].ssaVersion == ref.version && user->operands[2].type == LiftedOperandType::Register;
                if (computedField) {
                    const auto &key = user->operands[2];
                    const SSARef keyRef{key.value.reg, key.ssaVersion};
                    const auto *definition = m_currentFunction->GetDefinition(key);
                    const bool literal = definition && definition->operation == LiftedOperation::LOAD;
                    computedField = literal || (definition && (definition->operation == LiftedOperation::PHI || !ShouldInline(definition)) &&
                                                !m_referenceCapturedValues.contains(keyRef));
                    if (computedField)
                        for (const auto &[other, write] : m_currentFunction->definitionMap)
                            if (!literal && other.regIndex == keyRef.regIndex && write && write->instructionIndex > user->instructionIndex &&
                                write->instructionIndex < setList->instructionIndex) {
                                computedField = false;
                                break;
                            }
                }
                if (!list && !field && !computedField && !snapshotableKey(table, *user) && !storeSiteSnapshotableKey(table, *user))
                    return nullptr;
                prior.push_back(user);
            } else if (BlockOf(user) != BlockOf(setList)) {
                return nullptr;
            }
        }
        std::ranges::sort(prior, {}, &LiftedInstruction::instructionIndex);
        int nextArrayIndex = 1;
        for (const auto *store : prior)
            if (store->operation == LiftedOperation::SETLIST) {
                if (store->operands[3].value.imm.n != nextArrayIndex)
                    return nullptr;
                nextArrayIndex += store->operands[2].value.imm.n - 1;
            }
        if (setList->operands.size() < 4 || setList->operands[3].value.imm.n != nextArrayIndex)
            return nullptr;
        return setList;
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
        if (ShouldInline(&inst) && (!forceDefinitions || m_deferToConditionInline.contains(&inst) || openCall ||
                                    inst.operation == LiftedOperation::GETVARARGS || m_compoundInlined.contains(&inst)))
            continue;
        if (forceDefinitions && m_foldConsumedDefs.contains(inst.instructionIndex))
            continue; // already rendered inside a folded table constructor

        if (const auto compound = m_compoundAssignments.find(&inst); compound != m_compoundAssignments.end()) {
            const auto &op = *compound->second.operation;
            auto target = LiftStoreTarget(inst);
            auto rhs = HasConstantRightOperand(op.operation) ? ConstantLiteral(op.operands[2].value.imm.k) : LiftExpression(op.operands[2]);
            statements.push_back(std::make_shared<CompoundBinaryExpressionNode>(BinaryOperatorSymbol(op.operation), target, rhs));
            continue;
        }

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
        case LiftedOperation::SETGLOBAL:
        case LiftedOperation::SETTABLE:
        case LiftedOperation::SETTABLEKS:
        case LiftedOperation::SETTABLEN:
        case LiftedOperation::SETUPVAL: {
            if (inst.operation == LiftedOperation::SETTABLE)
                if (const auto *table = m_currentFunction->GetDefinition(inst.operands[1]);
                    table && delayedVariadicSetList(*table) && storeSiteSnapshotableKey(*table, inst)) {
                    const auto snapshot = m_setListKeySnapshots.find(&inst);
                    const std::string name = snapshot == m_setListKeySnapshots.end() ? freshKeyName(inst) : snapshot->second;
                    auto key = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
                    statements.push_back(std::make_shared<VariableDeclarationNode>(key, LiftExpression(inst.operands[2])));
                    m_setListKeySnapshots[&inst] = name;
                    statements.push_back(
                        std::make_shared<AssignmentStatementNode>(
                            std::make_shared<IndexExpressionNode>(LiftExpression(inst.operands[1]), key), LiftExpression(inst.operands[0])
                        )
                    );
                    break;
                }
            // a store folding into a constructor that inlines at its reader renders there
            if (StoreTargetsFreshTable(&inst))
                if (const auto *table = m_currentFunction->GetDefinition(inst.operands[1]); table && ShouldInline(table))
                    break;
            auto target = LiftStoreTarget(inst);
            statements.push_back(std::make_shared<AssignmentStatementNode>(target, LiftExpression(inst.operands[0])));
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
        case LiftedOperation::SETLIST: {
            if (const auto *table = m_currentFunction->GetDefinition(inst.operands[0]); table && delayedVariadicSetList(*table) == &inst) {
                std::vector<std::shared_ptr<Expression>> elements;
                const SSARef ref{inst.operands[0].value.reg, inst.operands[0].ssaVersion};
                const std::string name = ResolveVariableName(inst.operands[0], false);
                const auto tableName = [&]() { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
                std::vector<const LiftedInstruction *> prior;
                for (const auto *user : m_currentFunction->users.at(ref))
                    if (user->instructionIndex < inst.instructionIndex)
                        prior.push_back(user);
                std::ranges::sort(prior, {}, &LiftedInstruction::instructionIndex);
                for (const auto *store : prior) {
                    if (store->operation == LiftedOperation::SETLIST) {
                        const int first = store->operands[3].value.imm.n;
                        const int count = store->operands[2].value.imm.n - 1;
                        for (int k = 0; k < count; ++k)
                            elements.push_back(std::make_shared<IndexExpressionNode>(tableName(), std::make_shared<NumberLiteralNode>(first + k)));
                    } else {
                        std::shared_ptr<Expression> key;
                        if (store->operation == LiftedOperation::SETTABLEKS)
                            key = std::make_shared<StringLiteralNode>(std::get<std::string>(ConstantAt(store->operands[2].value.imm.k).constantData));
                        else if (store->operation == LiftedOperation::SETTABLEN)
                            key = std::make_shared<NumberLiteralNode>(store->operands[2].value.imm.n + 1);
                        else {
                            const auto snapshot = m_setListKeySnapshots.find(store);
                            const auto *keyDefinition = m_currentFunction->GetDefinition(store->operands[2]);
                            if (snapshot != m_setListKeySnapshots.end())
                                key = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(snapshot->second));
                            else if (keyDefinition && keyDefinition->operation == LiftedOperation::LOAD)
                                key = LiftExpression(keyDefinition->operands[1]);
                            else
                                key = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(store->operands[2], false)));
                        }
                        elements.push_back(std::make_shared<TableBinaryExpressionNode>("=", key, std::make_shared<IndexExpressionNode>(tableName(), key)));
                    }
                }
                const auto operands = SetListElements(inst);
                for (size_t k = 0; k < operands.size(); ++k)
                    elements.push_back(LiftSetListElement(inst, k, false));
                auto target = tableName();
                if (prior.empty())
                    statements.push_back(std::make_shared<VariableDeclarationNode>(target, std::make_shared<TableLiteralNode>(elements)));
                else
                    statements.push_back(std::make_shared<AssignmentStatementNode>(target, std::make_shared<TableLiteralNode>(elements)));
                break;
            }
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
            // results bind by position, so unused ones before a used one still take a slot
            int32_t lastUsed = -1;
            for (const auto &ref : defs)
                if (m_currentFunction->useCounts[ref] > 0)
                    lastUsed = ref.regIndex;

            for (const auto &ref : defs)
                if (ref.regIndex <= lastUsed)
                    lhs.push_back(
                        std::make_shared<IdentifierExpressionNode>(
                            std::make_shared<Identifier>(ResolveVariableName({LiftedOperandType::Register, {ref.regIndex}, ref.version}))
                        )
                    );

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

            if (duplicatedFunction->numparams >= 1 && IsSingleUseClassMemberValue(m_currentFunction, inst.operands[0].value.reg, inst.operands[0].ssaVersion))
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

            std::string funcName = this->GetFunctionName(duplicatedFunction);
            if (const auto name = m_currentFunction->ssaOverrides.find({inst.operands[0].value.reg, inst.operands[0].ssaVersion});
                name != m_currentFunction->ssaOverrides.end())
                funcName = name->second;
            funcName = DisambiguateClosureName(
                *m_currentFunction, *targetFunc, inst.operands[0], duplicatedFunction->bytecodeId,
                !m_definedRegisters.contains(inst.operands[0].value.reg) || DeclaresLocal(inst, inst.operands[0]), std::move(funcName)
            );

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
                // A `local function` capturing itself reads the register it is being stored into.
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister)
                    action.upName = cap.operands[1].value.reg == inst.operands[0].value.reg && cap.operands[1].ssaVersion == inst.operands[0].ssaVersion &&
                                            !m_currentFunction->IsConsumedByPhi(inst.operands[0])
                                        ? funcName
                                        : m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);
                else if (captureMode == 2)
                    action.upName = m_currentFunction->GetUpvalueName(cap.operands[1].value.reg);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) -> alias above; LCT_UPVAL (2) -> parent's
                // upvalue at that index. override survives the sub-lift's PopulateNames().
                if (((captureMode == 0 || captureMode == 1) && srcIsRegister) || captureMode == 2)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), action.upName);

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
            if (RenderClosureInPlace(inst, fnDecl, !duplicatedFunction->debugName.has_value()))
                break;

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
            if (m_currentFunction->IsConsumedByPhi(saveWhere) ||
                (m_definedRegisters.contains(saveWhere.value.reg) &&
                 (localName || m_capturedVariableWrites.contains({saveWhere.value.reg, saveWhere.ssaVersion})))) {
                if (localName) {
                    m_currentFunction->SetVariableName(saveWhere.value.reg, saveWhere.ssaVersion, *localName);
                    if (const auto users = m_currentFunction->users.find({saveWhere.value.reg, saveWhere.ssaVersion}); users != m_currentFunction->users.end())
                        for (const auto *user : users->second)
                            if (user->operation == LiftedOperation::PHI && !user->operands.empty())
                                m_currentFunction->SetVariableName(user->operands[0].value.reg, user->operands[0].ssaVersion, *localName);
                }
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                // a merged value defined ahead of its merges opens the variable nothing declared yet; inside an arm, the
                // merge's hoist declares it. `local f = function` would hide a self-reference.
                if (!m_definedRegisters.contains(saveWhere.value.reg) && !m_hoistedRegisters.contains(saveWhere.value.reg) &&
                    DominatesMerges(inst, saveWhere)) {
                    statements.push_back(std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(ResolveVariableName(saveWhere))));
                    m_definedRegisters.insert(saveWhere.value.reg);
                }
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
                    if (user->operation == LiftedOperation::SETGLOBAL &&
                        std::all_of(users.begin(), users.end(), [&](const auto *other) { return other == user; })) {
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

            std::string funcName = this->GetFunctionName(proto);
            if (const auto name = m_currentFunction->ssaOverrides.find({inst.operands[0].value.reg, inst.operands[0].ssaVersion});
                name != m_currentFunction->ssaOverrides.end())
                funcName = name->second;
            funcName = DisambiguateClosureName(
                *m_currentFunction, *targetFunc, inst.operands[0], proto->bytecodeId,
                !m_definedRegisters.contains(inst.operands[0].value.reg) || DeclaresLocal(inst, inst.operands[0]), std::move(funcName)
            );

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
                // A `local function` capturing itself reads the register it is being stored into.
                if ((captureMode == 0 || captureMode == 1) && srcIsRegister)
                    action.upName = cap.operands[1].value.reg == inst.operands[0].value.reg && cap.operands[1].ssaVersion == inst.operands[0].ssaVersion &&
                                            !m_currentFunction->IsConsumedByPhi(inst.operands[0])
                                        ? funcName
                                        : m_currentFunction->GetVarName(cap.operands[1].value.reg, cap.operands[1].ssaVersion);
                else if (captureMode == 2)
                    action.upName = m_currentFunction->GetUpvalueName(cap.operands[1].value.reg);

                // with a debug name, rename the source reg too so both read the meaningful name.
                action.willPropagate = action.hasDebugName && (captureMode == 0 || captureMode == 1) && srcIsRegister;
                action.shouldEmit = false; // captures are aliased, never copied

                // resolve the closure's GETUPVAL: VAL/REF (0/1) -> alias above; LCT_UPVAL (2) -> parent's
                // upvalue at that index. override survives the sub-lift's PopulateNames().
                if (((captureMode == 0 || captureMode == 1) && srcIsRegister) || captureMode == 2)
                    targetFunc->SetUpvalueNameOverride(static_cast<int32_t>(capIdx), action.upName);

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
            if (RenderClosureInPlace(inst, fnDecl, !proto->debugName.has_value()))
                break;

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
            if (m_currentFunction->IsConsumedByPhi(saveWhere) ||
                (m_definedRegisters.contains(saveWhere.value.reg) &&
                 (localName || m_capturedVariableWrites.contains({saveWhere.value.reg, saveWhere.ssaVersion})))) {
                if (localName) {
                    m_currentFunction->SetVariableName(saveWhere.value.reg, saveWhere.ssaVersion, *localName);
                    if (const auto users = m_currentFunction->users.find({saveWhere.value.reg, saveWhere.ssaVersion}); users != m_currentFunction->users.end())
                        for (const auto *user : users->second)
                            if (user->operation == LiftedOperation::PHI && !user->operands.empty())
                                m_currentFunction->SetVariableName(user->operands[0].value.reg, user->operands[0].ssaVersion, *localName);
                }
                fnDecl->bAnonymousInline = true;
                fnDecl->bIsLocalDeclaration = false;
                // a merged value defined ahead of its merges opens the variable nothing declared yet; inside an arm, the
                // merge's hoist declares it. `local f = function` would hide a self-reference.
                if (!m_definedRegisters.contains(saveWhere.value.reg) && !m_hoistedRegisters.contains(saveWhere.value.reg) &&
                    DominatesMerges(inst, saveWhere)) {
                    statements.push_back(std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(ResolveVariableName(saveWhere))));
                    m_definedRegisters.insert(saveWhere.value.reg);
                }
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
                    if (user->operation == LiftedOperation::SETGLOBAL &&
                        std::all_of(users.begin(), users.end(), [&](const auto *other) { return other == user; })) {
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
            const bool isNewDef = !m_definedRegisters.contains(inst.operands[0].value.reg) || DeclaresLocal(inst, inst.operands[0]);
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

        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            break;

        default: {
            if (inst.operands.empty())
                break;
            if (inst.operands[0].type != LiftedOperandType::Register)
                break;

            if (inst.operation == LiftedOperation::NEWTABLE)
                if (const auto *setList = delayedVariadicSetList(inst);
                    setList && std::ranges::none_of(
                                   m_currentFunction->users.at(SSARef{inst.operands[0].value.reg, inst.operands[0].ssaVersion}),
                                   [&](const LiftedInstruction *user) { return user->instructionIndex < setList->instructionIndex; }
                               )) {
                    m_processedInstructions.insert(inst.instructionIndex);
                    break;
                }

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
                if (inst.operation == LiftedOperation::NEWTABLE)
                    if (const auto *setList = delayedVariadicSetList(inst); setList)
                        for (const auto *user : m_currentFunction->users.at(SSARef{inst.operands[0].value.reg, inst.operands[0].ssaVersion}))
                            if (user->instructionIndex < setList->instructionIndex && storeSiteSnapshotableKey(inst, *user))
                                m_setListKeySnapshots.try_emplace(user, freshKeyName(*user));
                auto val = LiftExpression(inst.operands[0], true);

                const bool isParameterWrite = inst.operands[0].value.reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams;
                if ((inst.operands[0].ssaVersion <= 1 && !isParameterWrite && !m_hoistedRegisters.contains(inst.operands[0].value.reg)) || !isDefined ||
                    DeclaresLocal(inst, inst.operands[0]))
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
                if (inst.operation == LiftedOperation::NEWTABLE)
                    if (const auto *setList = delayedVariadicSetList(inst); setList)
                        for (const auto *user : m_currentFunction->users.at(SSARef{inst.operands[0].value.reg, inst.operands[0].ssaVersion}))
                            if (user->instructionIndex < setList->instructionIndex && snapshotableKey(inst, *user)) {
                                const std::string name = freshKeyName(*user);
                                m_setListKeySnapshots[user] = name;
                                statements.push_back(
                                    std::make_shared<VariableDeclarationNode>(
                                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)), LiftExpression(user->operands[2], false)
                                    )
                                );
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
