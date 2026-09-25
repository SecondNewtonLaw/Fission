//
// Created by Dottik on 23/9/2026.
//

#include "ASTLifter.hpp"
#include "ASTLifterShared.hpp"
#include "DenominatorAnalysis.hpp"
#include "SSABuilder.hpp"

#include <algorithm>
#include <functional>
#include <unordered_set>

#ifndef NDEBUG
#if defined(__clang__)
#pragma clang optimize off
#endif
#endif

// A builtin fast path reads the call's arguments ahead of the CALL that consumes them.
static bool IsTableStore(const LiftedInstruction &inst) {
    return inst.operation == LiftedOperation::SETLIST || inst.operation == LiftedOperation::SETTABLE || inst.operation == LiftedOperation::SETTABLEKS ||
           inst.operation == LiftedOperation::SETTABLEN;
}

static const LiftedOperand &StoredTable(const LiftedInstruction &store) { return store.operands[store.operation == LiftedOperation::SETLIST ? 0 : 1]; }

static std::vector<LiftedOperand> RegisterRun(int32_t first, const std::vector<int32_t> &versions) {
    std::vector<LiftedOperand> run(versions.size());
    for (size_t k = 0; k < versions.size(); ++k) {
        run[k].type = LiftedOperandType::Register;
        run[k].value.reg = static_cast<uint8_t>(first + static_cast<int32_t>(k));
        run[k].ssaVersion = versions[k];
    }
    return run;
}

// Consumers that evaluate an inlined call in place: returns, calls, arithmetic, indexing, stores, and optionally branch tests.
static bool EvaluatesInlinedCall(LiftedOperation op, bool branches) {
    switch (op) {
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::RETURN:
    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
    case LiftedOperation::NAMECALL:
    case LiftedOperation::ADD:
    case LiftedOperation::SUB:
    case LiftedOperation::MUL:
    case LiftedOperation::DIV:
    case LiftedOperation::IDIV:
    case LiftedOperation::MOD:
    case LiftedOperation::POW:
    case LiftedOperation::CONCAT:
    case LiftedOperation::MINUS:
    case LiftedOperation::NOT:
    case LiftedOperation::LENGTH:
    case LiftedOperation::GETTABLE:
    case LiftedOperation::GETTABLEKS:
    case LiftedOperation::GETTABLEN:
    case LiftedOperation::SETLIST:
        return true;
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPIF:
    case LiftedOperation::JUMPIFNOT:
    case LiftedOperation::JUMPXEQK:
        return branches;
    default:
        return false;
    }
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
    if (call)
        for (const auto &argument : CallArguments(def))
            ConsumeInlinedDefs(argument);
}

bool ASTLifter::IsDiamondBoolLoad(const LiftedOperand &operand) const {
    if (operand.type != LiftedOperandType::Register || !m_diamondBoolRegs.contains(operand.value.reg))
        return false;
    const auto *def = m_currentFunction->GetDefinition(operand);
    return def && (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::LOADNJUMP) && def->operands.size() >= 2 &&
           def->operands[1].type == LiftedOperandType::ImmediateBool;
}

std::vector<LiftedOperand> ASTLifter::SetListElements(const LiftedInstruction &setList) const {
    const auto versions = m_currentFunction->implicitUses.find(&setList);
    if (versions == m_currentFunction->implicitUses.end() || setList.operands.size() < 2)
        return {};
    return RegisterRun(setList.operands[1].value.reg, versions->second);
}

std::vector<LiftedOperand> ASTLifter::CallArguments(const LiftedInstruction &call) const {
    const auto versions = m_currentFunction->implicitUses.find(&call);
    if (versions == m_currentFunction->implicitUses.end() || call.operands.empty())
        return {};
    return RegisterRun(call.operands[0].value.reg + 1, versions->second);
}

const LiftedInstruction *ASTLifter::SoleUser(const SSARef &ref) const {
    const auto users = m_currentFunction->users.find(ref);
    if (users == m_currentFunction->users.end() || users->second.empty() ||
        !std::ranges::all_of(users->second, [&](const LiftedInstruction *user) { return user == users->second.front(); }))
        return nullptr;
    return users->second.front();
}

bool ASTLifter::BeginsDebugLocal(const LiftedInstruction &inst, int32_t reg) const {
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    auto next = static_cast<size_t>(inst.instructionIndex) + 1;
    while (next < instructions.size() && instructions[next].operation == LiftedOperation::NOP)
        ++next;
    const auto &locals = m_currentFunction->lpLiftedFunction->lpDeserialized->locvars;
    return std::ranges::any_of(locals, [&](const auto &local) {
        if (local.reg != reg || local.startpc <= inst.instructionIndex || static_cast<size_t>(local.startpc) > next || local.varname.empty())
            return false;
        // a move-elided inline parameter aliases a local that stays live across it; the write assigns that local
        return std::ranges::none_of(locals, [&](const auto &owner) {
            return &owner != &local && owner.reg == reg && owner.startpc <= inst.instructionIndex && owner.endpc > local.startpc && !owner.varname.empty();
        });
    });
}

bool ASTLifter::DeclaresLocal(const LiftedInstruction &inst, const LiftedOperand &target) const {
    return !m_hoistedRegisters.contains(target.value.reg) &&
           (m_valueCapturedValues.contains({static_cast<uint8_t>(target.value.reg), target.ssaVersion}) || BeginsDebugLocal(inst, target.value.reg));
}

static bool SameRegisterValue(const LiftedOperand &a, const LiftedOperand &b) {
    return a.type == LiftedOperandType::Register && b.type == LiftedOperandType::Register && a.value.reg == b.value.reg && a.ssaVersion == b.ssaVersion;
}

// Does `load` read the location `store` writes?
static bool LoadsStoreTarget(const LiftedInstruction &load, const LiftedInstruction &store) {
    if (load.operands.size() < 2)
        return false;
    switch (store.operation) {
    case LiftedOperation::SETTABLEKS:
        return load.operation == LiftedOperation::GETTABLEKS && load.operands.size() > 2 && SameRegisterValue(load.operands[1], store.operands[1]) &&
               load.operands[2].value.imm.k == store.operands[2].value.imm.k;
    case LiftedOperation::SETTABLEN:
        return load.operation == LiftedOperation::GETTABLEN && load.operands.size() > 2 && SameRegisterValue(load.operands[1], store.operands[1]) &&
               load.operands[2].value.imm.n == store.operands[2].value.imm.n;
    case LiftedOperation::SETTABLE:
        return load.operation == LiftedOperation::GETTABLE && load.operands.size() > 2 && SameRegisterValue(load.operands[1], store.operands[1]) &&
               SameRegisterValue(load.operands[2], store.operands[2]);
    case LiftedOperation::SETUPVAL:
        return load.operation == LiftedOperation::GETUPVAL && load.operands[1].value.imm.n == store.operands[1].value.imm.n;
    case LiftedOperation::SETGLOBAL:
        return load.operation == LiftedOperation::GETGLOBAL && load.operands[1].value.imm.k == store.operands[1].value.imm.k;
    default:
        return false;
    }
}

// Luau compiles `target op= rhs` as `load R; op R, R, rhs; store R`; the plain `target = target op rhs` loads into another register.
void ASTLifter::CollectCompoundAssignments() {
    m_compoundAssignments.clear();
    m_compoundInlined.clear();
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    const auto refOf = [](const LiftedOperand &operand) { return SSARef{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion}; };
    for (const auto &store : instructions) {
        const size_t targetOperands = store.operation == LiftedOperation::SETTABLE ? 3 : 2;
        if (store.operands.size() < targetOperands || store.operands[0].type != LiftedOperandType::Register)
            continue;
        const auto *op = m_currentFunction->GetDefinition(store.operands[0]);
        const char *symbol = op ? BinaryOperatorSymbol(op->operation) : nullptr;
        if (!symbol || std::string_view(symbol) == "and" || std::string_view(symbol) == "or" || op->operands.size() < 3 ||
            op->operands[1].type != LiftedOperandType::Register || op->operands[1].value.reg != op->operands[0].value.reg)
            continue;
        const auto *load = m_currentFunction->GetDefinition(op->operands[1]);
        if (!load || load->operands[0].value.reg != op->operands[0].value.reg || !LoadsStoreTarget(*load, store))
            continue;
        const int32_t reg = op->operands[0].value.reg;
        if (!m_currentFunction->IsSingleUse(load->operands[0]) || SoleUser(refOf(load->operands[0])) != op ||
            !m_currentFunction->IsSingleUse(op->operands[0]) || SoleUser(refOf(op->operands[0])) != &store || BlockOf(load) != BlockOf(&store) ||
            BeginsDebugLocal(*load, reg) || BeginsDebugLocal(*op, reg))
            continue;
        auto &compound = m_compoundAssignments[&store];
        compound.operation = op;
        compound.inlined = {load, op};

        // a temporary table or key read only by the pair renders once in the target
        std::vector<const LiftedInstruction *> temporaries;
        for (size_t i = 1; i < targetOperands; ++i) {
            if (store.operands[i].type != LiftedOperandType::Register)
                continue;
            const auto *definition = m_currentFunction->GetDefinition(store.operands[i]);
            const auto users = m_currentFunction->users.find(refOf(store.operands[i]));
            if (!definition || definition->operation == LiftedOperation::PHI || users == m_currentFunction->users.end() || users->second.size() != 2 ||
                !std::ranges::contains(users->second, load) || !std::ranges::contains(users->second, &store) || BlockOf(definition) != BlockOf(load) ||
                BeginsDebugLocal(*definition, store.operands[i].value.reg))
                continue;
            if (const auto defs = m_defsByInstruction.find(definition); defs == m_defsByInstruction.end() || defs->second.size() != 1)
                continue;
            temporaries.push_back(definition);
        }
        const auto contiguous = [&](const LiftedInstruction *temporary) {
            for (auto k = static_cast<size_t>(temporary->instructionIndex) + 1; k < static_cast<size_t>(load->instructionIndex); ++k)
                if (instructions[k].operation != LiftedOperation::NOP && !std::ranges::contains(temporaries, &instructions[k]))
                    return false;
            return true;
        };
        for (const auto *temporary : temporaries)
            if (contiguous(temporary))
                compound.inlined.push_back(temporary);
        m_compoundInlined.insert(compound.inlined.begin(), compound.inlined.end());
    }
}

void ASTLifter::KeepOrderedCompoundAssignments() {
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    for (bool dropped = true; dropped;) {
        dropped = false;
        m_shouldInlineMemo.clear();
        m_inlineBinaryDepth.clear();
        for (auto it = m_compoundAssignments.begin(); it != m_compoundAssignments.end(); ++it) {
            const auto *store = it->first, *op = it->second.operation, *load = it->second.inlined.front();
            const SSARef loaded{static_cast<uint8_t>(load->operands[0].value.reg), load->operands[0].ssaVersion};
            bool ordered = true;
            for (auto k = static_cast<size_t>(load->instructionIndex) + 1; ordered && k < static_cast<size_t>(store->instructionIndex); ++k)
                ordered = &instructions[k] == op || instructions[k].operation == LiftedOperation::NOP ||
                          (k < static_cast<size_t>(op->instructionIndex) && RendersAfter(instructions[k], loaded, op));
            if (ordered)
                continue;
            for (const auto *member : it->second.inlined)
                m_compoundInlined.erase(member);
            m_compoundAssignments.erase(it);
            dropped = true;
            break;
        }
    }
    // the right-hand side renders inside the statement too, even where a block forces its definitions out
    for (const auto &entry : m_compoundAssignments)
        for (auto k = static_cast<size_t>(entry.second.inlined.front()->instructionIndex) + 1;
             k < static_cast<size_t>(entry.second.operation->instructionIndex); ++k)
            if (instructions[k].operation != LiftedOperation::NOP)
                m_compoundInlined.insert(&instructions[k]);
    m_shouldInlineMemo.clear();
    m_inlineBinaryDepth.clear();
}

// The analysis a decision reads is fixed for the whole function lift, so answers are memoized.
bool ASTLifter::ShouldInline(const LiftedInstruction *inst) {
    if (const auto it = m_shouldInlineMemo.find(inst); it != m_shouldInlineMemo.end())
        return it->second;
    // input checks look backward and effect checks forward, so the query can cycle; a def whose
    // answer is still being computed stays materialized
    if (!m_shouldInlineActive.insert(inst).second)
        return false;
    bool r = ShouldInlineImpl(inst);
    uint8_t binaryDepth = 0;
    if (r && !m_compoundInlined.contains(inst) && BinaryOperatorSymbol(inst->operation) && inst->operands.size() > 1) {
        binaryDepth = 1;
        if (inst->operands[1].type == LiftedOperandType::Register) {
            const auto *input = m_currentFunction->GetDefinition(inst->operands[1]);
            if (input && input->instructionIndex < inst->instructionIndex && BinaryOperatorSymbol(input->operation)) {
                const auto prior = m_inlineBinaryDepth.find(input);
                if (prior == m_inlineBinaryDepth.end())
                    r = false;
                else
                    binaryDepth += prior->second;
            }
        }
        if (binaryDepth >= 128)
            r = false;
    }
    m_shouldInlineActive.erase(inst);
    m_shouldInlineMemo[inst] = r;
    m_inlineBinaryDepth[inst] = r ? binaryDepth : 0;
    return r;
}

bool ASTLifter::ShouldInlineImpl(const LiftedInstruction *inst) {
    if (!inst || inst->operands.size() < 1)
        return false;

    if (inst->operation == LiftedOperation::GETVARARGS && inst->operands.size() > 1 && inst->operands[1].value.imm.n > 2)
        return false;

    if (m_compoundInlined.contains(inst))
        return true;

    for (size_t i = 1; i < inst->operands.size(); ++i)
        if (IsDiamondBoolLoad(inst->operands[i]))
            return false;

    const auto onlyUser = [&](const SSARef &ref) -> const LiftedInstruction * {
        const auto *user = SoleUser(ref);
        if (!user || m_currentFunction->users.at(ref).size() == 1)
            return user;
        if (user->operation != LiftedOperation::SETLIST)
            return nullptr;
        return std::ranges::any_of(SetListElements(*user), [&](const LiftedOperand &element) { return IsDiamondBoolLoad(element); }) ? user : nullptr;
    };
    bool singleUse = false;
    if (inst->operands[0].type == LiftedOperandType::Register) {
        const auto *user = SoleUser({static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion});
        singleUse = m_currentFunction->IsSingleUse(inst->operands[0]) || (user && user->operation == LiftedOperation::SETLIST);
    }

    // loop-cond consumer from another block: inlining would move this call into the loop
    if (m_forcedMaterialization.contains(inst))
        return false;

    // a multi-value call's consumer follows it directly and must receive every value
    const bool multiValueCall = (inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands.size() > 2 &&
                                inst->operands[2].value.imm.n == 0;
    if (!multiValueCall && ReadLocationChanged(inst)) {
        ExplainKeep(inst, "the location it reads may be written before a use");
        return false;
    }

    if (CanOperationRaise(inst->operation) && IsConstructorElement(inst) && inst->operands[0].type == LiftedOperandType::Register) {
        if (const auto *user = SoleUser({static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion})) {
            if (BlockOf(inst) != BlockOf(user)) {
                ExplainKeep(inst, "constructor consumer is in another CFG block", user);
                return false;
            }

            const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
            for (int32_t k = inst->instructionIndex + 1; k < user->instructionIndex; ++k)
                if (const auto defs = m_defsByInstruction.find(&instructions[k]); defs != m_defsByInstruction.end())
                    for (size_t i = 1; i < inst->operands.size(); ++i)
                        if (inst->operands[i].type == LiftedOperandType::Register &&
                            std::ranges::any_of(defs->second, [&](const SSARef &defined) { return defined.regIndex == inst->operands[i].value.reg; })) {
                            ExplainKeep(inst, "constructor operand register is overwritten before population", user, &instructions[k]);
                            return false;
                        }

            if (user->operation == LiftedOperation::SETLIST)
                for (const auto &element : SetListElements(*user)) {
                    const auto *def = m_currentFunction->GetDefinition(element);
                    if (def && def->instructionIndex > inst->instructionIndex && def->instructionIndex < user->instructionIndex && !ShouldInline(def)) {
                        ExplainKeep(inst, "later constructor element must stay materialized", user, def);
                        return false;
                    }
                }
        }
    }

    // a captured local stays declared: the closure's upvalue binds to its name, and a by-reference capture shares its writes
    if (inst->operands[0].type == LiftedOperandType::Register) {
        const SSARef defRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        if (m_referenceCapturedValues.contains(defRef))
            return false;
        if (auto it = m_currentFunction->users.find(defRef); it != m_currentFunction->users.end())
            for (const auto *user : it->second)
                if (user && user->operation == LiftedOperation::CAPTURE && user->operands.size() >= 1 && user->operands[0].value.imm.n <= 1)
                    return false;
    }

    switch (inst->operation) {
    case LiftedOperation::RETURN:
    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
    case LiftedOperation::NEWCLASSMEMBER:
    case LiftedOperation::NEWCLASS:
        return false;
    default:
        break;
    }

    if (inst->operands[0].type == LiftedOperandType::Register && inst->operation != LiftedOperation::MOVE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_phiConsumers.contains(defRef))
            return false;
    }

    // a constructor nothing else reads is still built once; inlined into its own stores it would render twice
    if ((inst->operation == LiftedOperation::NEWTABLE || inst->operation == LiftedOperation::DUPTABLE) &&
        inst->operands[0].type == LiftedOperandType::Register) {
        const SSARef defRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto users = m_currentFunction->users.find(defRef);
        if (users != m_currentFunction->users.end() && !users->second.empty() && std::ranges::all_of(users->second, [&](const LiftedInstruction *user) {
                const size_t table = user->operation == LiftedOperation::SETLIST ? 0 : 1;
                return (user->operation == LiftedOperation::SETLIST || user->operation == LiftedOperation::SETTABLE ||
                        user->operation == LiftedOperation::SETTABLEKS || user->operation == LiftedOperation::SETTABLEN) &&
                       user->operands.size() > table && user->operands[table].value.reg == defRef.regIndex &&
                       user->operands[table].ssaVersion == defRef.version;
            }))
            return false;

        // a table whose field stores all fold into its constructor renders at its one reader
        const auto ownStore = [&](const LiftedInstruction *user) {
            const size_t table = user->operation == LiftedOperation::SETLIST ? 0 : 1;
            return (user->operation == LiftedOperation::SETLIST || user->operation == LiftedOperation::SETTABLE ||
                    user->operation == LiftedOperation::SETTABLEKS || user->operation == LiftedOperation::SETTABLEN) &&
                   user->operands.size() > table && user->operands[table].value.reg == defRef.regIndex && user->operands[table].ssaVersion == defRef.version;
        };
        if (users != m_currentFunction->users.end() &&
            std::ranges::any_of(users->second, [&](const LiftedInstruction *user) { return ownStore(user) && user->operation != LiftedOperation::SETLIST; })) {
            boost::unordered_flat_set<const LiftedInstruction *> readers;
            for (const auto *user : users->second)
                if (!ownStore(user) && !IsFastCall(user->operation))
                    readers.insert(user);
            if (readers.size() != 1 || BlockOf(*readers.begin()) != BlockOf(inst)) {
                ExplainKeep(inst, "constructor has no single reader in its block");
                return false;
            }
            // a phi-defined list item needs the materialized local
            for (const auto *user : users->second)
                if (user->operation == LiftedOperation::SETLIST && ownStore(user) &&
                    std::ranges::any_of(SetListElements(*user), [&](const LiftedOperand &element) {
                        const auto *definition = m_currentFunction->GetDefinition(element);
                        return definition && definition->operation == LiftedOperation::PHI;
                    })) {
                    ExplainKeep(inst, "constructor list item is a merged value", user);
                    return false;
                }
            std::vector<int32_t> folded;
            LiftTableLiteral(*inst, &folded);
            for (const auto *user : users->second)
                if (ownStore(user) && !std::ranges::contains(folded, user->instructionIndex)) {
                    ExplainKeep(inst, "a population store does not fold into the constructor", user);
                    return false;
                }
            return !InliningReordersEffect(inst, *readers.begin());
        }
    }

    if (inst->operation == LiftedOperation::NEWTABLE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_currentFunction->useCounts.contains(defRef)) {
            const auto &users = m_currentFunction->users[defRef];
            for (const auto *user : users) {
                if (!user || user->operation != LiftedOperation::SETLIST || user->operands.size() < 2 ||
                    user->operands[0].value.reg != inst->operands[0].value.reg || user->operands[0].ssaVersion != inst->operands[0].ssaVersion)
                    continue;
                for (const auto &element : SetListElements(*user)) {
                    const LiftedInstruction *elementDef = m_currentFunction->GetDefinition(element);
                    for (int guard = 0; guard < 64 && elementDef && elementDef->operation == LiftedOperation::MOVE; ++guard)
                        elementDef = m_currentFunction->GetDefinition(elementDef->operands[1]);
                    if (elementDef && elementDef->operation == LiftedOperation::PHI)
                        return false;
                    if (elementDef && (elementDef->operation == LiftedOperation::NEWCLOSURE || elementDef->operation == LiftedOperation::DUPCLOSURE))
                        return false;
                }
            }
            // a SETLIST taking this table as an element records it twice; count distinct user instructions
            std::unordered_set<const LiftedInstruction *> realUsers;
            for (const auto *user : users) {
                if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                     user->operation == LiftedOperation::SETTABLEN) &&
                    user->operands[1].value.reg == inst->operands[0].value.reg)
                    return false;

                if ((user->operation == LiftedOperation::SETLIST && user->operands[0].value.reg == inst->operands[0].value.reg) || IsFastCall(user->operation))
                    continue;

                // a copy aliases the table under another name
                if (user->operation == LiftedOperation::MOVE)
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
            const int regA = inst->operands[0].value.reg;
            if (const auto defsIt = m_defsByInstruction.find(inst); defsIt != m_defsByInstruction.end())
                for (const auto &ref : defsIt->second) {
                    if (ref.regIndex != regA)
                        continue;
                    const auto &users = m_currentFunction->users[{static_cast<uint8_t>(regA), ref.version}];
                    return users.size() == 1 && EvaluatesInlinedCall(users[0]->operation, false) && !InliningReordersEffect(inst, users[0]);
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

        // a SETLIST records its base register twice; count distinct user instructions
        const auto &rawUsers = m_currentFunction->users[usedRef];
        std::unordered_set<const LiftedInstruction *> users(rawUsers.begin(), rawUsers.end());
        if (users.size() == 1 && rawUsers.size() > 1 && (*users.begin())->operation != LiftedOperation::SETLIST) {
            ExplainKeep(inst, "call result is read more than once by its consumer", *users.begin());
            return false;
        }
        if (users.size() == 1) {
            // `x = f()` into an existing local lands in a temporary and moves down into the local's register
            const auto *user = *users.begin();
            const bool assignsLocal = user->operation == LiftedOperation::MOVE && user->operands.size() > 1 &&
                                      user->operands[1].type == LiftedOperandType::Register && user->operands[0].value.reg < user->operands[1].value.reg;
            if (EvaluatesInlinedCall(user->operation, true) || assignsLocal)
                return !InliningReordersEffect(inst, *users.begin());
            ExplainKeep(inst, "call consumer is not an inlineable operation", *users.begin());
        } else {
            ExplainKeep(inst, "call result has multiple distinct consumers");
        }
        return false;
    }

    if (inst->operation == LiftedOperation::MOVE) {
        if (!singleUse || m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;
        // registers are allocated as a stack, so a copy into a lower register writes a live local: `x = value`
        if (inst->operands.size() > 1 && inst->operands[1].type == LiftedOperandType::Register && inst->operands[0].value.reg < inst->operands[1].value.reg)
            return false;

        const SSARef defRef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion};
        const auto usersIt = m_currentFunction->users.find(defRef);
        if (inst->operands.size() < 2 || inst->operands[1].type != LiftedOperandType::Register || usersIt == m_currentFunction->users.end() ||
            usersIt->second.size() != 1)
            return true;

        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        const int sourceReg = inst->operands[1].value.reg;
        for (int32_t k = inst->instructionIndex + 1; k < usersIt->second.front()->instructionIndex; ++k)
            if (const auto defsIt = m_defsByInstruction.find(&instructions[k]); defsIt != m_defsByInstruction.end())
                for (const auto &ref : defsIt->second)
                    if (ref.regIndex == sourceReg)
                        return false;
        return true;
    }

    // Keep raising operations at their original position so error order matches bytecode evaluation.
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
    const bool defCanRaise = raisesWhenInlined;
    if (defCanRaise && inst->operands[0].type == LiftedOperandType::Register && singleUse) {
        const auto *user = onlyUser({static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion});
        if (!user) {
            ExplainKeep(inst, "raising expression has no unique terminal consumer");
            return false;
        }
        if (InliningReordersEffect(inst, user))
            return false;
    }

    if ((!raisesWhenInlined || singleUse) && m_currentFunction->IsSimpleOrConstant(inst->operands[0]) &&
        !m_currentFunction->IsConsumedByPhi(inst->operands[0]) && inst->operands[0].ssaVersion != 1 /* the first version declares the local */)
        return true;

    // a constant may be read at every use unless a reader rewrites its register (`x = x + 1` needs the declaration)
    if (inst->operation == LiftedOperation::LOAD && inst->operands.size() >= 2 && inst->operands[0].type == LiftedOperandType::Register &&
        !m_currentFunction->IsConsumedByPhi(inst->operands[0])) {
        const auto vt = inst->operands[1].type;
        const bool isPureConst = vt == LiftedOperandType::ImmediateConstant || vt == LiftedOperandType::ImmediateInteger ||
                                 vt == LiftedOperandType::ImmediateBool || vt == LiftedOperandType::ImmediateNil;
        if (isPureConst) {
            const uint8_t reg = inst->operands[0].value.reg;
            const auto users = m_currentFunction->users.find({reg, inst->operands[0].ssaVersion});
            const bool selfReassigned =
                users != m_currentFunction->users.end() && std::ranges::any_of(users->second, [&](const LiftedInstruction *user) {
                    return !user->operands.empty() && user->operands[0].type == LiftedOperandType::Register && user->operands[0].value.reg == reg;
                });
            // a reader followed by an assignment that merges reads a variable: `result = f(result)`; an expression temp
            // written below its operands never merges
            const bool readThenAssigned = m_assignedRegisters.contains(reg) && users != m_currentFunction->users.end() &&
                                          std::ranges::any_of(users->second, [&](const LiftedInstruction *user) {
                                              const int block = BlockOf(user);
                                              if (block < 0)
                                                  return false;
                                              // the register's next write decides
                                              for (const auto *next = user + 1; next <= m_currentFunction->basicBlocks[block].lpTail; ++next)
                                                  if (!next->operands.empty() && next->operands[0].type == LiftedOperandType::Register &&
                                                      next->operands[0].value.reg == reg && SSABuilder::GetRegisterAccess(*next, 0) == AccessType::Write)
                                                      return AssignedLocal(*next) == reg && m_currentFunction->IsConsumedByPhi(next->operands[0]);
                                              return false;
                                          });
            if (!selfReassigned && !readThenAssigned)
                return true;
        }
    }

    if (singleUse) {
        if (m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;
        if (inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE)
            return false;
        const SSARef ref{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        const auto *user = onlyUser(ref);
        // a raising value already passed InliningReordersEffect above
        if (user && !defCanRaise && InputRebound(inst, user, -1, false))
            return false;
        // AND/OR read both registers eagerly; as the right side of `a or b` the value would only run when needed
        if (user && defCanRaise && (user->operation == LiftedOperation::AND || user->operation == LiftedOperation::OR) && user->operands.size() > 2 &&
            user->operands[2].value.reg == ref.regIndex && user->operands[2].ssaVersion == ref.version)
            return false;
        return true;
    }

    return false;
}

// True if `e` keeps its evaluation at its own position instead of inlining into a use, so moving
// another def across it reorders effects or throws.
bool ASTLifter::StaysAsStatement(const LiftedInstruction *e) {
    switch (e->operation) {
    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETUDATAKS:
        return true;
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
        // a store into a fresh constructor folds into its `{ ... }` literal
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
            break;
        }
        return false;
    }
    case LiftedOperation::NEWTABLE:
    case LiftedOperation::DUPTABLE:
        // building a spilled constructor can raise at its own position
        return !ShouldInline(e);
    default:
        return CanOperationRaise(e->operation) && !ShouldInline(e);
    }
}

void ASTLifter::DeferIntoCondition(
    const LiftedInstruction &def, const SSARef &value, const LiftedInstruction *reader,
    std::vector<std::pair<LiftedOperand, const LiftedInstruction *>> &pending
) {
    const auto users = m_currentFunction->users.find(value);
    // a constructor that inlines renders with its population stores at its one reader
    const bool constructor = (def.operation == LiftedOperation::NEWTABLE || def.operation == LiftedOperation::DUPTABLE) && ShouldInline(&def);
    if (users == m_currentFunction->users.end() || ValueReader(value) != reader ||
        (!constructor && !std::ranges::all_of(users->second, [&](const LiftedInstruction *user) { return user == reader || IsFastCall(user->operation); })))
        return;
    if (InliningReordersEffect(&def, reader) || !m_deferToConditionInline.insert(&def).second)
        return;

    for (size_t operandIndex = 1; operandIndex < def.operands.size(); ++operandIndex)
        pending.emplace_back(def.operands[operandIndex], &def);
    if (constructor)
        for (const auto *store : users->second) {
            if (store == reader || IsFastCall(store->operation))
                continue;
            if (store->operation == LiftedOperation::SETLIST) {
                for (const auto &element : SetListElements(*store))
                    pending.emplace_back(element, store);
                continue;
            }
            pending.emplace_back(store->operands[0], store);
            if (store->operation == LiftedOperation::SETTABLE && store->operands.size() > 2)
                pending.emplace_back(store->operands[2], store);
        }

    if ((def.operation != LiftedOperation::CALL && def.operation != LiftedOperation::CALLFB) || def.operands.empty() ||
        !m_currentFunction->implicitUses.contains(&def))
        return;
    for (const auto &argument : CallArguments(def))
        pending.emplace_back(argument, &def);
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    const auto index = def.instructionIndex;
    if (index >= 2 && instructions[index - 2].operation == LiftedOperation::NAMECALL && instructions[index - 2].operands.size() > 1 &&
        instructions[index - 2].operands[0].value.reg == def.operands[0].value.reg && BlockOf(&instructions[index - 2]) == BlockOf(&def) &&
        !InliningReordersEffect(&instructions[index - 2], &def))
        m_deferToConditionInline.insert(&instructions[index - 2]);
}

bool ASTLifter::DominatesMerges(const LiftedInstruction &def, const LiftedOperand &value) {
    const int32_t home = BlockOf(&def);
    if (home < 0)
        return false;
    if (!m_dominators)
        m_dominators = AnalyzeDenominators(*m_currentFunction);
    const auto &dominators = *m_dominators;
    const auto &blocks = m_currentFunction->basicBlocks;
    for (size_t id = 0; id < blocks.size(); ++id) {
        const bool merges = std::ranges::any_of(blocks[id].phiNodes, [&](const LiftedInstruction &phi) {
            return std::ranges::any_of(phi.operands.begin() + (phi.operands.empty() ? 0 : 1), phi.operands.end(), [&](const LiftedOperand &input) {
                return input.type == LiftedOperandType::Register && input.value.reg == value.value.reg && input.ssaVersion == value.ssaVersion;
            });
        });
        if (!merges)
            continue;
        int32_t cursor = static_cast<int32_t>(id);
        while (cursor != -1 && cursor != home) {
            const auto it = dominators.find(cursor);
            cursor = it == dominators.end() ? -1 : it->second.idom;
        }
        if (cursor != home)
            return false;
    }
    return true;
}

bool ASTLifter::RendersInline(const LiftedInstruction *inst) {
    if (IsFastCall(inst->operation))
        return true;
    if (IsTableStore(*inst) && StoreTargetsFreshTable(inst)) {
        const auto *table = m_currentFunction->GetDefinition(StoredTable(*inst));
        return table && ShouldInline(table);
    }
    return ShouldInline(inst);
}

// True if the value `e` produces only feeds table-constructor building, so it renders inside a `{ ... }` literal.
bool ASTLifter::IsConstructorElement(const LiftedInstruction *e) {
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    const auto evaluate = [&](const LiftedInstruction *current) {
        if (current->operands.empty() || current->operands[0].type != LiftedOperandType::Register)
            return false;
        if (current->operation == LiftedOperation::NAMECALL || current->operation == LiftedOperation::NAMECALLUDATA) {
            for (int32_t i = current->instructionIndex + 1; i < static_cast<int32_t>(instructions.size()); ++i) {
                if (instructions[i].operation == LiftedOperation::NOP)
                    continue;
                const auto next = m_constructorElementMemo.find(&instructions[i]);
                return (instructions[i].operation == LiftedOperation::CALL || instructions[i].operation == LiftedOperation::CALLFB) &&
                       next != m_constructorElementMemo.end() && next->second;
            }
            return false;
        }

        std::vector<SSARef> refs;
        if (const auto defs = m_defsByInstruction.find(current); defs != m_defsByInstruction.end())
            refs = defs->second;
        else
            refs.push_back({current->operands[0].value.reg, current->operands[0].ssaVersion});

        bool found = false;
        for (const auto &ref : refs) {
            const auto users = m_currentFunction->users.find(ref);
            if (users == m_currentFunction->users.end() || users->second.empty())
                continue;
            found = true;
            for (const auto *user : users->second) {
                if (user->operation == LiftedOperation::SETLIST) {
                    const auto *tableDef = user->operands.empty() ? nullptr : m_currentFunction->GetDefinition(user->operands[0]);
                    if (tableDef && tableDef->instructionIndex < current->instructionIndex)
                        continue;
                    return false;
                }
                if (user->instructionIndex > current->instructionIndex &&
                    (user->operation == LiftedOperation::CALL || user->operation == LiftedOperation::CALLFB || m_defsByInstruction.contains(user))) {
                    const auto next = m_constructorElementMemo.find(user);
                    if (next != m_constructorElementMemo.end() && next->second)
                        continue;
                }
                if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                     user->operation == LiftedOperation::SETTABLEN) &&
                    StoreTargetsFreshTable(user)) {
                    const auto *tableDef = user->operands.size() > 1 ? m_currentFunction->GetDefinition(user->operands[1]) : nullptr;
                    if (tableDef && tableDef->instructionIndex < current->instructionIndex)
                        continue;
                }
                return false;
            }
        }
        return found;
    };

    if (m_constructorElementMemo.empty()) {
        m_constructorElementMemo.reserve(instructions.size());
        for (auto it = instructions.rbegin(); it != instructions.rend(); ++it)
            m_constructorElementMemo.emplace(&*it, evaluate(&*it));
    }
    const auto result = m_constructorElementMemo.find(e);
    return result != m_constructorElementMemo.end() ? result->second : evaluate(e);
}

// True if a store fills a fresh NEWTABLE/DUPTABLE constructor rather than mutating an existing table.
bool ASTLifter::StoreTargetsFreshTable(const LiftedInstruction *e) {
    const size_t tableOp = (e->operation == LiftedOperation::SETLIST) ? 0 : 1;
    if (e->operands.size() <= tableOp || e->operands[tableOp].type != LiftedOperandType::Register)
        return false;
    const auto *def = m_currentFunction->GetDefinition(e->operands[tableOp]);
    return def && (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE);
}

ASTLifter::ValueReads ASTLifter::CollectReads(const LiftedInstruction *def) {
    ValueReads reads;
    std::vector<const LiftedInstruction *> pending{def};
    boost::unordered_flat_set<const LiftedInstruction *> seen{def};
    while (!pending.empty() && reads.registers.size() < 64) {
        const auto *reader = pending.back();
        pending.pop_back();
        if (reader->operation == LiftedOperation::GETGLOBAL && reader->operands.size() > 1)
            reads.globals.insert(reader->operands[1].value.imm.n);
        else if (reader->operation == LiftedOperation::GETUPVAL && reader->operands.size() > 1)
            reads.upvalues.insert(reader->operands[1].value.imm.n);
        for (size_t i = 0; i < reader->operands.size(); ++i) {
            const auto &input = reader->operands[i];
            const auto access = SSABuilder::GetRegisterAccess(*reader, i);
            if (input.type != LiftedOperandType::Register || (access != AccessType::Read && access != AccessType::ReadWrite))
                continue;
            reads.registers.push_back(input);
            if (reader == def)
                reads.directRegisters = reads.registers.size();
            reads.capturedLocal |= m_referenceCapturedValues.contains({input.value.reg, input.ssaVersion});
            // single-use stands in for "inlined here"; asking ShouldInline would recurse back into this check
            const auto *inputDef = m_currentFunction->GetDefinition(input);
            if (inputDef && inputDef->operation != LiftedOperation::PHI && inputDef->instructionIndex < def->instructionIndex &&
                m_currentFunction->IsSingleUse(input) && seen.insert(inputDef).second)
                pending.push_back(inputDef);
        }
    }
    reads.incomplete = !pending.empty();
    return reads;
}

bool ASTLifter::InputRebound(const LiftedInstruction *def, const LiftedInstruction *use, int32_t skipIndex, bool sameRegister) {
    const auto reads = CollectReads(def);
    if (reads.incomplete)
        return true;
    if (reads.registers.empty())
        return false;
    const LiftedInstruction *barrier = nullptr;
    const bool rebound = ChangedOnPath(def, use, [&](const LiftedInstruction &inst) {
        if (inst.instructionIndex == skipIndex)
            return false;
        const auto outputs = m_defsByInstruction.find(&inst);
        if (outputs == m_defsByInstruction.end() || ShouldInline(&inst))
            return false;
        // call results always declare a fresh local, which the renamer keeps apart from the input
        const bool declaresFresh =
            inst.operation == LiftedOperation::CALL || inst.operation == LiftedOperation::CALLFB || inst.operation == LiftedOperation::NAMECALL;
        for (size_t n = 0; n < reads.registers.size(); ++n) {
            const auto &input = reads.registers[n];
            const auto inputName = m_currentFunction->GetVarName(input.value.reg, input.ssaVersion);
            for (const auto &output : outputs->second) {
                LiftedOperand written{};
                written.type = LiftedOperandType::Register;
                written.value.reg = output.regIndex;
                written.ssaVersion = output.version;
                // a reused register keeps its name, so a same-named assignment rebinds an operand read at the use;
                // inputs of operands that stay statements only matter through a phi-merged binding
                if (!sameRegister && (n >= reads.directRegisters || declaresFresh) && !m_currentFunction->IsConsumedByPhi(written))
                    continue;
                if ((sameRegister && output.regIndex == input.value.reg) || m_currentFunction->GetVarName(output.regIndex, output.version) == inputName) {
                    barrier = &inst;
                    return true;
                }
            }
        }
        return false;
    });
    if (rebound)
        ExplainKeep(def, "an input binding is overwritten before its use", use, barrier);
    return rebound;
}

bool ASTLifter::ChangedOnPath(const LiftedInstruction *def, const LiftedInstruction *use, const std::function<bool(const LiftedInstruction &)> &changes) {
    const auto &blocks = m_currentFunction->basicBlocks;
    const int32_t defId = BlockOf(def), useId = BlockOf(use);
    if (defId < 0 || useId < 0)
        return true;
    const auto scan = [&](const LiftedInstruction *from, const LiftedInstruction *to) {
        for (const auto *inst = from; inst < to; ++inst)
            if (changes(*inst))
                return true;
        return false;
    };
    if (defId == useId && use > def)
        return scan(def + 1, use);
    if (scan(def + 1, blocks[defId].lpTail + 1))
        return true;

    constexpr size_t kMaxBlocks = 512;
    const auto walk = [&](std::vector<uint32_t> pending, bool forward, boost::unordered_flat_set<uint32_t> &seen) {
        while (!pending.empty()) {
            const uint32_t id = pending.back();
            pending.pop_back();
            if (id == static_cast<uint32_t>(defId) || id >= blocks.size() || !seen.insert(id).second)
                continue;
            if (seen.size() > kMaxBlocks)
                return false;
            for (const uint32_t next : forward ? blocks[id].successors : blocks[id].predecessors)
                pending.push_back(next);
        }
        return true;
    };
    boost::unordered_flat_set<uint32_t> after, before;
    if (!walk(blocks[defId].successors, true, after) || !walk({static_cast<uint32_t>(useId)}, false, before))
        return true;
    for (const uint32_t id : after)
        if (id != static_cast<uint32_t>(useId) && before.contains(id) && blocks[id].lpHead && scan(blocks[id].lpHead, blocks[id].lpTail + 1))
            return true;
    const auto &useBlock = blocks[useId];
    if (scan(useBlock.lpHead, use))
        return true;
    // a loop around the use runs the rest of its block before the next read
    return std::ranges::any_of(useBlock.successors, [&](uint32_t next) { return before.contains(next); }) && scan(use + 1, useBlock.lpTail + 1);
}

// Evaluation rank of `value` among the inputs of `use`; unknown where rendering may reorder operands.
static std::optional<int> InputRank(const LiftedInstruction &use, const SSARef &value) {
    switch (use.operation) {
    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
    case LiftedOperation::RETURN:
    case LiftedOperation::SETLIST:
    case LiftedOperation::CONCAT:
        return value.regIndex;
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFNOTLE:
        return std::nullopt;
    default:
        break;
    }
    const bool store =
        use.operation == LiftedOperation::SETTABLE || use.operation == LiftedOperation::SETTABLEKS || use.operation == LiftedOperation::SETTABLEN;
    std::optional<int> rank;
    for (size_t i = 0; i < use.operands.size(); ++i) {
        const auto access = SSABuilder::GetRegisterAccess(use, i);
        if (use.operands[i].type == LiftedOperandType::Register && use.operands[i].value.reg == value.regIndex && use.operands[i].ssaVersion == value.version &&
            (access == AccessType::Read || access == AccessType::ReadWrite))
            rank = (std::max)(rank.value_or(0), store && i == 0 ? static_cast<int>(use.operands.size()) : static_cast<int>(i));
    }
    return rank;
}

const LiftedInstruction *ASTLifter::ValueReader(const SSARef &ref) const {
    const auto users = m_currentFunction->users.find(ref);
    if (users == m_currentFunction->users.end())
        return nullptr;
    const LiftedInstruction *reader = nullptr;
    for (const auto *user : users->second) {
        if ((IsTableStore(*user) && user->operands.size() > 1 && StoredTable(*user).value.reg == ref.regIndex &&
             StoredTable(*user).ssaVersion == ref.version) ||
            IsFastCall(user->operation) || user == reader)
            continue;
        if (reader)
            return nullptr;
        reader = user;
    }
    return reader;
}

std::optional<SSARef> ASTLifter::InlinedChainInto(const LiftedInstruction &inst, const LiftedInstruction *target) {
    const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
    const LiftedInstruction *current = &inst;
    for (int depth = 0; depth < 16 && BlockOf(current) == BlockOf(target); ++depth) {
        // a method setup belongs to the call that follows it
        if (current->operation == LiftedOperation::NAMECALL || current->operation == LiftedOperation::NAMECALLUDATA) {
            auto next = static_cast<size_t>(current->instructionIndex) + 1;
            while (next < instructions.size() && instructions[next].operation == LiftedOperation::NOP)
                ++next;
            if (next >= instructions.size() ||
                (instructions[next].operation != LiftedOperation::CALL && instructions[next].operation != LiftedOperation::CALLFB))
                return std::nullopt;
            current = &instructions[next];
            continue;
        }
        const auto defs = m_defsByInstruction.find(current);
        if (defs == m_defsByInstruction.end() || defs->second.size() != 1 || !ShouldInline(current))
            return std::nullopt;
        const auto *next = ValueReader(defs->second.front());
        if (next == target)
            return defs->second.front();
        if (!next || next->instructionIndex <= current->instructionIndex)
            return std::nullopt;
        if (IsTableStore(*next) && next->operands.size() > 1) {
            const auto *table = m_currentFunction->GetDefinition(StoredTable(*next));
            if (table == target)
                return defs->second.front();
            if (!table || (table->operation != LiftedOperation::NEWTABLE && table->operation != LiftedOperation::DUPTABLE) || table == current)
                return std::nullopt;
            next = table;
        }
        current = next;
    }
    return std::nullopt;
}

bool ASTLifter::RendersAfter(const LiftedInstruction &inst, const SSARef &value, const LiftedInstruction *use) {
    const auto entry = InlinedChainInto(inst, use);
    if (!entry)
        return false;
    // a comparison of two inlined operands renders them in definition order
    switch (use->operation) {
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFNOTLE: {
        const auto valueDef = m_currentFunction->definitionMap.find(value), entryDef = m_currentFunction->definitionMap.find(*entry);
        return valueDef != m_currentFunction->definitionMap.end() && entryDef != m_currentFunction->definitionMap.end() &&
               valueDef->second->instructionIndex < entryDef->second->instructionIndex;
    }
    default:
        break;
    }
    const auto first = InputRank(*use, value), second = InputRank(*use, *entry);
    return first && second && *first < *second;
}

bool ASTLifter::ReadLocationChanged(const LiftedInstruction *def) {
    if (def->operands.size() < 2 || def->operands[0].type != LiftedOperandType::Register)
        return false;
    const auto reads = CollectReads(def);
    if (reads.incomplete)
        return true;
    if (!reads.capturedLocal && reads.globals.empty() && reads.upvalues.empty())
        return false;
    const SSARef value{static_cast<uint8_t>(def->operands[0].value.reg), def->operands[0].ssaVersion};
    const auto writes = [&](const LiftedInstruction &inst, const LiftedInstruction *user) {
        if (inst.operation == LiftedOperation::CALL || inst.operation == LiftedOperation::CALLFB)
            return !RendersAfter(inst, value, user);
        if (inst.operands.size() < 2)
            return false;
        return (inst.operation == LiftedOperation::SETGLOBAL && reads.globals.contains(inst.operands[1].value.imm.n)) ||
               (inst.operation == LiftedOperation::SETUPVAL && reads.upvalues.contains(inst.operands[1].value.imm.n));
    };
    const auto users = m_currentFunction->users.find(value);
    if (users == m_currentFunction->users.end())
        return false;
    return std::ranges::any_of(users->second, [&](const LiftedInstruction *user) {
        return ChangedOnPath(def, user, [&](const LiftedInstruction &inst) { return writes(inst, user); });
    });
}

// True if inlining `def` into its single use would move its throw or effect past an instruction that stays
// at its own position. The use call's own NAMECALL setup belongs to the same call expression.
bool ASTLifter::InliningReordersEffect(const LiftedInstruction *def, const LiftedInstruction *use) {
    if (!def || !use)
        return false;
    const LiftedInstruction *directUse = use;
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
        return false;
    const auto &insts = m_currentFunction->lpLiftedFunction->instructions;
    const LiftedInstruction *calleeDef = nullptr;
    if ((use->operation == LiftedOperation::CALL || use->operation == LiftedOperation::CALLFB) && !use->operands.empty()) {
        calleeDef = m_currentFunction->GetDefinition(use->operands[0]);
        while (calleeDef && calleeDef->operation == LiftedOperation::MOVE && ShouldInline(calleeDef))
            calleeDef = m_currentFunction->GetDefinition(calleeDef->operands[1]);
    }
    const bool defIsCallCallee = calleeDef == def;
    // a constructor element renders inside its `{ ... }`; that keeps it after `def` only when `def` feeds a constructor too
    const bool useBuildsConstructor =
        def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE || use->operation == LiftedOperation::SETLIST ||
        ((use->operation == LiftedOperation::SETTABLE || use->operation == LiftedOperation::SETTABLEKS || use->operation == LiftedOperation::SETTABLEN) &&
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

    // a fastcall loads its callee after the arguments whatever the source order
    const auto fastCallCallee = [&](int32_t k) {
        if (&insts[k] != calleeDef)
            return false;
        for (int32_t j = k - 1; j > defIdx; --j) {
            if (IsFastCall(insts[j].operation))
                return true;
            if (insts[j].operation == LiftedOperation::CALL || insts[j].operation == LiftedOperation::CALLFB)
                return false;
        }
        return false;
    };

    // work stored into the table's own fields renders inside its `{ ... }`
    const bool defIsTable = def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE;
    // a constructor element evaluates in its constructor's order, after `def` only when `def` was built inside that constructor
    const auto elementAfterDef = [&](const LiftedInstruction &inst) {
        if (!useBuildsConstructor || !IsConstructorElement(&inst))
            return false;
        const LiftedInstruction *current = &inst;
        for (int depth = 0; depth < 16; ++depth) {
            if (current->operation == LiftedOperation::NAMECALL || current->operation == LiftedOperation::NAMECALLUDATA) {
                auto next = static_cast<size_t>(current->instructionIndex) + 1;
                while (next < insts.size() && insts[next].operation == LiftedOperation::NOP)
                    ++next;
                if (next >= insts.size())
                    return false;
                current = &insts[next];
                continue;
            }
            const auto defs = m_defsByInstruction.find(current);
            if (defs == m_defsByInstruction.end() || defs->second.empty())
                return false;
            const auto *reader = ValueReader(defs->second.front());
            if (!reader || reader->instructionIndex <= current->instructionIndex)
                return false;
            if (IsTableStore(*reader) && reader->operands.size() > 1 && StoreTargetsFreshTable(reader)) {
                const auto *table = m_currentFunction->GetDefinition(StoredTable(*reader));
                return table && table->instructionIndex < defIdx;
            }
            current = reader;
        }
        return false;
    };

    const auto ownStore = [&](const LiftedInstruction &inst) {
        return defIsTable && IsTableStore(inst) && inst.operands.size() > 1 && def->operands[0].type == LiftedOperandType::Register &&
               StoredTable(inst).value.reg == def->operands[0].value.reg && StoredTable(inst).ssaVersion == def->operands[0].ssaVersion;
    };

    // a constructor evaluates its items contiguously up to its last population store; when it renders as one expression
    // its items go with it, so the scan steps over them
    const auto constructorEnd = [&](int32_t k) {
        const auto &table = insts[k];
        int32_t end = k;
        if ((table.operation != LiftedOperation::NEWTABLE && table.operation != LiftedOperation::DUPTABLE) || table.operands.empty() ||
            table.operands[0].type != LiftedOperandType::Register)
            return end;
        const SSARef ref{static_cast<uint8_t>(table.operands[0].value.reg), table.operands[0].ssaVersion};
        if (const auto users = m_currentFunction->users.find(ref); users != m_currentFunction->users.end())
            for (const auto *user : users->second)
                if (IsTableStore(*user) && user->operands.size() > 1 && StoredTable(*user).value.reg == ref.regIndex &&
                    StoredTable(*user).ssaVersion == ref.version)
                    end = (std::max)(end, user->instructionIndex);
        return end;
    };

    for (int32_t k = defIdx + 1; k < useIdx && static_cast<size_t>(k) < insts.size(); ++k) {
        if (k == useOwnNameCall || fastCallCallee(k) || ownStore(insts[k]))
            continue;
        if (defIsTable && InlinedChainInto(insts[k], def)) {
            k = constructorEnd(k);
            continue;
        }
        if (defIsCallCallee && (insts[k].operation == LiftedOperation::CALL || insts[k].operation == LiftedOperation::CALLFB)) {
            ExplainKeep(def, "callee must be evaluated before argument calls", use, &insts[k]);
            return true;
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
        case LiftedOperation::FORNPREP:
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGPREP:
        case LiftedOperation::FORGPREP_NEXT:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGLOOP:
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
        if (def->operands[0].type == LiftedOperandType::Register &&
            RendersAfter(insts[k], {static_cast<uint8_t>(def->operands[0].value.reg), def->operands[0].ssaVersion}, directUse)) {
            k = constructorEnd(k);
            continue;
        }
        if (insts[k].operation == LiftedOperation::CALL || insts[k].operation == LiftedOperation::CALLFB) {
            ExplainKeep(def, "intervening call would execute first", use, &insts[k]);
            return true;
        }
        if (CanOperationRaise(insts[k].operation) && !elementAfterDef(insts[k])) {
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

// Operations that can raise a runtime error (index, arithmetic, concat and length metamethods or type errors).
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
    case LiftedOperation::GETGLOBAL:
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
