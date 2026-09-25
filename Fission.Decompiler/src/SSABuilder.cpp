#include "SSABuilder.hpp"
#include "Deserializer.hpp"
#include "SafetyGuard.hpp"

#include <algorithm>
#include <array>
#include <libassert/assert.hpp>
#include <queue>
#include <set>
#include <vector>

static const std::array<AccessType, 256> kOpcodeAccessTable = [] {
    std::array<AccessType, 256> table{};
    table.fill(AccessType::NoAccess);

    auto set = [&](LiftedOperation op, AccessType type) { table[static_cast<size_t>(op)] = type; };

    for (auto op : {LiftedOperation::SETGLOBAL,      LiftedOperation::SETUPVAL,    LiftedOperation::SETTABLE,      LiftedOperation::SETTABLEKS,
                    LiftedOperation::SETTABLEN,      LiftedOperation::SETLIST,     LiftedOperation::RETURN,        LiftedOperation::JUMPIF,
                    LiftedOperation::JUMPIFNOT,      LiftedOperation::JUMPIFEQ,    LiftedOperation::JUMPIFLE,      LiftedOperation::JUMPIFLT,
                    LiftedOperation::JUMPIFNOTEQ,    LiftedOperation::JUMPIFNOTLE, LiftedOperation::JUMPIFNOTLT,   LiftedOperation::JUMPXEQK,
                    LiftedOperation::CAPTURE,        LiftedOperation::FASTCALL,    LiftedOperation::FASTPCALL,     LiftedOperation::FASTCALL1,
                    LiftedOperation::FASTCALL2,      LiftedOperation::FASTCALL2K,  LiftedOperation::FORGLOOP,      LiftedOperation::FORGPREP_NEXT,
                    LiftedOperation::FORGPREP,
                    LiftedOperation::FORGPREP_INEXT, LiftedOperation::FORNPREP,    LiftedOperation::FASTCALL3,     LiftedOperation::SETUDATAKS,
                    LiftedOperation::NEWCLASSMEMBER, LiftedOperation::CMPPROTO,    LiftedOperation::FORNLOOP}) {
        set(op, AccessType::Read);
    }

    for (auto op :
         {LiftedOperation::LOAD,       LiftedOperation::LOADNJUMP,    LiftedOperation::MOVE,       LiftedOperation::GETGLOBAL,  LiftedOperation::GETUPVAL,
          LiftedOperation::GETIMPORT,  LiftedOperation::GETTABLE,     LiftedOperation::GETTABLEKS, LiftedOperation::GETTABLEN,  LiftedOperation::NEWCLOSURE,
          LiftedOperation::NAMECALL,   LiftedOperation::ADD,          LiftedOperation::SUB,        LiftedOperation::MUL,        LiftedOperation::DIV,
          LiftedOperation::MOD,        LiftedOperation::POW,          LiftedOperation::ADDK,       LiftedOperation::SUBK,       LiftedOperation::MULK,
          LiftedOperation::DIVK,       LiftedOperation::MODK,         LiftedOperation::POWK,       LiftedOperation::AND,        LiftedOperation::OR,
          LiftedOperation::ANDK,       LiftedOperation::ORK,          LiftedOperation::NOT,        LiftedOperation::MINUS,      LiftedOperation::LENGTH,
          LiftedOperation::NEWTABLE,   LiftedOperation::DUPTABLE,     LiftedOperation::GETVARARGS, LiftedOperation::DUPCLOSURE, LiftedOperation::SUBRK,
          LiftedOperation::CONCAT,     LiftedOperation::DIVRK,        LiftedOperation::IDIV,       LiftedOperation::IDIVK,
          LiftedOperation::GETUDATAKS, LiftedOperation::NAMECALLUDATA, LiftedOperation::NEWCLASS}) {
        set(op, AccessType::Write);
    }

    set(LiftedOperation::CALL, AccessType::Read);
    set(LiftedOperation::CALLFB, AccessType::Read);
    set(LiftedOperation::RETURN, AccessType::Deferred);

    return table;
}();

AccessType SSABuilder::GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex) {
    if (op.operation == LiftedOperation::CAPTURE && operandIndex == 1 && op.operands.size() > 1 && op.operands[0].value.imm.n == 2)
        return AccessType::NoAccess;

    const AccessType baseType = kOpcodeAccessTable[static_cast<size_t>(op.operation)];

    if (baseType == AccessType::Write) {
        return (operandIndex == 0) ? AccessType::Write : AccessType::Read;
    }

    if (baseType == AccessType::ReadWrite) {
        return (operandIndex == 0) ? AccessType::ReadWrite : AccessType::Read;
    }

    return baseType;
}

// Resolve a variadic SETLIST or RETURN through its preceding multret producer.
int VariadicTailCount(AnalyzedFunction &func, const LiftedInstruction &inst, int startReg) {
    const auto &instrs = func.lpLiftedFunction->instructions;
    // Malformed bytecode can carry an instruction index beyond this function.
    int32_t idx = inst.instructionIndex - 1;
    if (idx >= static_cast<int32_t>(instrs.size()))
        idx = static_cast<int32_t>(instrs.size()) - 1;
    for (; idx >= 0; --idx) {
        const auto &p = instrs[idx];
        if (p.operation == LiftedOperation::NOP)
            continue;
        const bool callMulti =
            (p.operation == LiftedOperation::CALL || p.operation == LiftedOperation::CALLFB) && p.operands.size() > 2 && p.operands[2].value.imm.n == 0;
        const bool varargMulti = p.operation == LiftedOperation::GETVARARGS && p.operands.size() > 1 && p.operands[1].value.imm.n == 0;
        if ((callMulti || varargMulti) && p.operands[0].value.reg >= startReg)
            return p.operands[0].value.reg - startReg + 1;
        break; // the multret tail is the last element: the instruction right before `inst`
    }
    return 1;
}

// A B==0 CALL ends at the preceding multret producer, not the highest register used by the function.
int CalculateLuaStackForInstruction(AnalyzedFunction &func, const LiftedInstruction &inst) {
    if (LiftedOperation::CALL != inst.operation && LiftedOperation::CALLFB != inst.operation)
        return 0; // why bro.

    if (inst.operands.size() < 2)
        return 0; // hostile bytecode: truncated CALL

    if (inst.operands[1].value.imm.n != 0 /* not actually var arg, why the fuck was this called? */)
        return 0;

    const int regFunc = inst.operands[0].value.reg;
    // Setup instructions can separate a variadic call from its producer. Stop when a write leaves the
    // call frame; higher unrelated registers do not extend the argument list.
    const auto &instrs = func.lpLiftedFunction->instructions;
    int32_t idx = inst.instructionIndex - 1;
    if (idx >= static_cast<int32_t>(instrs.size()))
        idx = static_cast<int32_t>(instrs.size()) - 1;
    for (; idx >= 0; --idx) {
        const auto &p = instrs[idx];
        if (p.operation == LiftedOperation::NOP)
            continue;
        const bool callMulti =
            (p.operation == LiftedOperation::CALL || p.operation == LiftedOperation::CALLFB) && p.operands.size() > 2 && p.operands[2].value.imm.n == 0;
        const bool varargMulti = p.operation == LiftedOperation::GETVARARGS && p.operands.size() > 1 && p.operands[1].value.imm.n == 0;
        if ((callMulti || varargMulti) && !p.operands.empty() && p.operands[0].value.reg >= regFunc + 1)
            return p.operands[0].value.reg - regFunc; // R(A+1)..R(producerBase) inclusive

        const bool writesInFrame = !p.operands.empty() && p.operands[0].type == LiftedOperandType::Register && p.operands[0].value.reg >= regFunc;
        const bool noRegDest = p.operands.empty() || p.operands[0].type != LiftedOperandType::Register;
        if (writesInFrame || noRegDest)
            continue;
        break; // wrote a register below the call frame -> outside this call's arg setup
    }
    return 0;
}

std::vector<int> SSABuilder::GetImplicitDefinitions(const LiftedInstruction &inst) {
    std::vector<int> defs;
    switch (inst.operation) {
    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB: {
        if (inst.operands.size() < 3)
            break; // hostile bytecode: truncated instruction
        int regStart = inst.operands[0].value.reg;
        int retCount = inst.operands[2].value.imm.n;
        int effectiveRetCount = (retCount == 0) ? 1 : (retCount - 1);
        for (int k = 0; k < effectiveRetCount; ++k) {
            defs.push_back(regStart + k);
        }
        break;
    }
    case LiftedOperation::NAMECALL: {
        if (inst.operands.empty())
            break;
        int regA = inst.operands[0].value.reg;
        defs.push_back(regA + 1); // Implicit Self
        break;
    }
    case LiftedOperation::GETVARARGS: {
        if (inst.operands.size() < 2)
            break;
        int baseReg = inst.operands[0].value.reg;
        int count = inst.operands[1].value.imm.n;
        int effectiveCount = (count == 0) ? 1 : (count - 1);
        for (int k = 0; k < effectiveCount; ++k) {
            defs.push_back(baseReg + k);
        }
        break;
    }
    case LiftedOperation::SETLIST: {
        break;
    }
    case LiftedOperation::FORNLOOP: {
        if (!inst.operands.empty())
            defs.push_back(inst.operands[0].value.reg + 2);
        break;
    }
    case LiftedOperation::FORGLOOP: {
        if (inst.operands.empty())
            break;
        // Loop variables need definitions at the latch so header phis separate pre-loop values.
        const int base = inst.operands[0].value.reg;
        const int numVars = inst.operands.size() > 2 ? (inst.operands[2].value.imm.n & 0xFF) : 0;
        defs.push_back(base + 2);
        for (int k = 0; k < numVars; ++k)
            defs.push_back(base + 3 + k);
        break;
    }
    default:
        break;
    }
    return defs;
}

int32_t SSABuilder::NewVersion(int32_t reg) {
    if (versionCounter.size() <= static_cast<size_t>(reg))
        while (versionCounter.size() <= static_cast<size_t>(reg))
            versionCounter.emplace_back(0);
    const int32_t nVer = versionCounter[reg]++;
    versionStack[reg].push_back(nVer);
    return nVer;
}

int32_t SSABuilder::CurrentVersion(int32_t reg) {
    if (versionStack.size() <= static_cast<size_t>(reg) || versionStack[reg].empty()) {
        if (versionStack.size() <= static_cast<size_t>(reg))
            while (versionStack.size() <= static_cast<size_t>(reg))
                versionStack.emplace_back();

        return -1;
    }
    return versionStack[reg].back();
}

static bool IsLoopPrep(LiftedOperation operation) {
    return operation == LiftedOperation::FORNPREP || operation == LiftedOperation::FORGPREP || operation == LiftedOperation::FORGPREP_INEXT ||
           operation == LiftedOperation::FORGPREP_NEXT;
}

// FORGLOOP assigns a generic-for's variables before every body run, so the header's entry edge carries none of them.
static bool IsGenericForVariable(const AnalyzedFunction &func, const BasicBlock &header, int reg) {
    if (!header.loopLatch || *header.loopLatch >= func.basicBlocks.size())
        return false;
    const auto *latchTail = func.basicBlocks[*header.loopLatch].lpTail;
    if (!latchTail || latchTail->operation != LiftedOperation::FORGLOOP || latchTail->operands.size() < 3)
        return false;
    const int base = latchTail->operands[0].value.reg;
    const int numVars = latchTail->operands[2].value.imm.n & 0xFF;
    return reg >= base + 3 && reg < base + 3 + numVars;
}

// The CFG routes the loop latch back into the FOR*PREP block, but the VM runs the prep once: its reads
// happen on entry edges only. `liveInNormal` omits those reads and is what the latch edge observes.
static void ComputeLiveness(
    AnalyzedFunction *func, int maxRegs, std::vector<std::vector<bool>> &liveIn, std::vector<std::vector<bool>> &liveInNormal
) {
    size_t numBlocks = func->basicBlocks.size();
    liveIn.assign(numBlocks, std::vector<bool>(maxRegs + 1, false));
    liveInNormal.assign(numBlocks, std::vector<bool>(maxRegs + 1, false));
    std::vector<std::vector<bool>> use(numBlocks, std::vector<bool>(maxRegs + 1, false));
    std::vector<std::vector<bool>> entryUse(numBlocks, std::vector<bool>(maxRegs + 1, false));
    std::vector<std::vector<bool>> def(numBlocks, std::vector<bool>(maxRegs + 1, false));

    for (const auto &block : func->basicBlocks) {
        if (!block.lpHead)
            continue;
        uint32_t bid = block.dwBlockId;

        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

            const bool entryRead = IsLoopPrep(inst->operation) && block.loopLatch.has_value();
            auto markRead = [&](int r) {
                if (r >= 0 && r <= maxRegs && !def[bid][r]) {
                    (entryRead ? entryUse : use)[bid][r] = true;
                }
            };
            auto markWrite = [&](int r) {
                if (r >= 0 && r <= maxRegs) {
                    def[bid][r] = true;
                }
            };

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                const auto &op = inst->operands[i];
                if (op.type == LiftedOperandType::Register) {
                    AccessType mode = SSABuilder::GetRegisterAccess(*inst, i);
                    if (mode == AccessType::Read || mode == AccessType::ReadWrite) {
                        markRead(op.value.reg);
                    }
                }
            }

            if (inst->operation == LiftedOperation::CONCAT && inst->operands.size() > 2) {
                int start = inst->operands[1].value.reg;
                int end = inst->operands[2].value.reg;
                for (int r = start; r <= end; ++r)
                    markRead(r);
            } else if ((inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands.size() > 2) {
                int32_t regFunc = inst->operands[0].value.reg;
                int32_t argCount = inst->operands[1].value.imm.n - 1;

                int effectiveArgCount = argCount;
                if (effectiveArgCount > 0)
                    for (int32_t k = 0; k < effectiveArgCount; ++k)
                        markRead(regFunc + 1 + k);
                else {
                    auto max = CalculateLuaStackForInstruction(*func, *inst);
                    for (int32_t k = 0; k < max; ++k)
                        markRead(regFunc + 1 + k);
                }

                int32_t retCount = inst->operands[2].value.imm.n;
                int32_t baseReg = inst->operands[0].value.reg;

                int effectiveRetCount = (retCount == 0) ? 1 : (retCount - 1);

                for (int32_t k = 0; k < effectiveRetCount; ++k) {
                    int32_t retReg = baseReg + k;
                    markWrite(retReg);
                }
            } else if (inst->operation == LiftedOperation::NAMECALL && inst->operands.size() > 1) {
                int base = inst->operands[0].value.reg;
                markWrite(base); // self
                markRead(inst->operands[1].value.reg);
            } else if (inst->operation == LiftedOperation::RETURN && inst->operands.size() > 1) {
                int base = inst->operands[0].value.reg;
                int count = inst->operands[1].value.imm.n - 1;
                // Variadic returns read through the multret producer so merge phis remain live.
                int effectiveCount = (count == -1) ? VariadicTailCount(*func, *inst, base) : count;
                for (int k = 0; k < effectiveCount; ++k)
                    markRead(base + k);
            } else if (inst->operation == LiftedOperation::SETLIST && inst->operands.size() > 2) {
                int base = inst->operands[1].value.reg;
                int count = inst->operands[2].value.imm.n;
                int effectiveCount = (count == 0) ? VariadicTailCount(*func, *inst, base) : (count - 1);
                for (int k = 0; k < effectiveCount; ++k)
                    markRead(base + k);
            } else if (
                (inst->operation == LiftedOperation::FORNPREP || inst->operation == LiftedOperation::FORGPREP ||
                 inst->operation == LiftedOperation::FORGPREP_INEXT || inst->operation == LiftedOperation::FORGPREP_NEXT ||
                 inst->operation == LiftedOperation::FORNLOOP || inst->operation == LiftedOperation::FORGLOOP) &&
                !inst->operands.empty()
            ) {
                const int base = inst->operands[0].value.reg;
                for (int k = 0; k < 3; ++k)
                    markRead(base + k);
            }

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                const auto &op = inst->operands[i];
                if (op.type == LiftedOperandType::Register) {
                    AccessType mode = SSABuilder::GetRegisterAccess(*inst, i);
                    if (mode == AccessType::Write || mode == AccessType::ReadWrite) {
                        markWrite(op.value.reg);
                    }
                }
            }
            std::vector<int> implicitDefs = SSABuilder::GetImplicitDefinitions(*inst);
            for (int r : implicitDefs)
                markWrite(r);
        }
    }

    // backward dataflow: a worklist seeded last-block-first revisits only predecessors of a block whose live-in changed
    std::vector<std::vector<uint32_t>> predecessors(numBlocks);
    for (const auto &block : func->basicBlocks)
        for (const uint32_t succ : block.successors)
            if (succ < numBlocks)
                predecessors[succ].push_back(block.dwBlockId);
    std::vector<uint32_t> worklist(numBlocks);
    for (size_t i = 0; i < numBlocks; ++i)
        worklist[i] = static_cast<uint32_t>(i);
    std::vector<bool> queued(numBlocks, true);
    while (!worklist.empty()) {
        Fission::CheckDecompileDeadline();
        const uint32_t bid = worklist.back();
        worklist.pop_back();
        queued[bid] = false;
        {
            const auto &block = func->basicBlocks[bid];
            bool blockChanged = false;

            for (int r = 0; r <= maxRegs; ++r) {
                bool isLiveOut = false;
                for (uint32_t succ : block.successors) {
                    if (succ >= liveIn.size())
                        continue; // hostile bytecode: wild jump target
                    const bool latchEdge = func->basicBlocks[succ].loopLatch == bid;
                    if (!latchEdge && IsGenericForVariable(*func, func->basicBlocks[succ], r))
                        continue;
                    if ((latchEdge ? liveInNormal : liveIn)[succ][r]) {
                        isLiveOut = true;
                        break;
                    }
                }

                const bool isLiveInNormal = use[bid][r] || (isLiveOut && !def[bid][r]);
                const bool isLiveIn = isLiveInNormal || entryUse[bid][r];
                if (liveIn[bid][r] != isLiveIn || liveInNormal[bid][r] != isLiveInNormal) {
                    liveIn[bid][r] = isLiveIn;
                    liveInNormal[bid][r] = isLiveInNormal;
                    blockChanged = true;
                }
            }
            if (blockChanged)
                for (const uint32_t pred : predecessors[bid])
                    if (!queued[pred]) {
                        queued[pred] = true;
                        worklist.push_back(pred);
                    }
        }
    }
}

void SSABuilder::CreatePhiNodes(AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo) {
    if (lpOriginalFunction->basicBlocks.empty())
        return;

    int numParams = 0;
    int maxRegs = 255;
    if (lpOriginalFunction->lpLiftedFunction && lpOriginalFunction->lpLiftedFunction->lpDeserialized) {
        maxRegs = lpOriginalFunction->lpLiftedFunction->lpDeserialized->maxstacksize;
        numParams = lpOriginalFunction->lpLiftedFunction->lpDeserialized->numparams;
    }

    std::vector<std::vector<bool>> liveIn, liveInNormal;
    ComputeLiveness(lpOriginalFunction, maxRegs, liveIn, liveInNormal);
    entryOnlyPhis.assign(lpOriginalFunction->basicBlocks.size(), std::vector<bool>(maxRegs + 1, false));

    std::vector<std::vector<int>> defBlocks(maxRegs + 1);

    if (lpOriginalFunction->lpLiftedFunction) {
        for (int i = 0; i < numParams; ++i) {
            defBlocks[i].push_back(0);
        }
    }

    for (auto &block : lpOriginalFunction->basicBlocks) {
        if (block.predecessors.size() < 2)
            continue;

        std::ranges::stable_sort(block.predecessors, [&](uint32_t a, uint32_t b) {
            bool aIsLatch = (a == block.loopLatch.value_or(-1));
            bool bIsLatch = (b == block.loopLatch.value_or(-1));

            if (aIsLatch != bIsLatch) {
                return !aIsLatch;
            }
            return a < b;
        });
    }

    for (const auto &block : lpOriginalFunction->basicBlocks) {
        if (!block.lpHead)
            continue;

        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                if (inst->operands[i].type == LiftedOperandType::Register) {
                    AccessType mode = GetRegisterAccess(*inst, i);
                    if (mode == AccessType::Write || mode == AccessType::ReadWrite) {
                        int reg = inst->operands[i].value.reg;
                        if (reg <= maxRegs) {
                            if (defBlocks[reg].empty() || static_cast<uint32_t>(defBlocks[reg].back()) != block.dwBlockId) {
                                defBlocks[reg].push_back(block.dwBlockId);
                            }
                        }
                    }
                }
            }

            std::vector<int> implicitDefs = GetImplicitDefinitions(*inst);
            for (int reg : implicitDefs) {
                if (reg <= maxRegs) {
                    if (defBlocks[reg].empty() || static_cast<uint32_t>(defBlocks[reg].back()) != block.dwBlockId) {
                        defBlocks[reg].push_back(block.dwBlockId);
                    }
                }
            }

            if (inst == block.lpTail)
                break;
        }
    }

    std::vector<int> workList;
    workList.reserve(lpOriginalFunction->basicBlocks.size());

    // Matching block count lets one bounds check protect frontier and liveness arrays.
    std::vector<int> hasPhi(lpOriginalFunction->basicBlocks.size(), 0);
    std::vector<int> inWorkList(lpOriginalFunction->basicBlocks.size(), 0);
    int visitedToken = 0;

    for (int reg = 0; reg <= maxRegs; ++reg) {
        if (defBlocks[reg].empty())
            continue;

        visitedToken++;
        workList = defBlocks[reg];

        for (uint32_t blk : workList) {
            if (blk < inWorkList.size())
                inWorkList[blk] = visitedToken;
        }

        size_t idx = 0;
        while (idx < workList.size()) {
            int blockId = workList[idx++];

            auto it = domInfo.find(blockId);
            if (it == domInfo.end())
                continue;

            for (uint32_t frontierId : it->second.dominanceFrontier) {
                if (frontierId >= hasPhi.size())
                    continue;

                // Skip dead phis.
                if (!liveIn[frontierId][reg])
                    continue;

                BasicBlock *frontierBlock = &lpOriginalFunction->basicBlocks[frontierId];
                const bool entryOnly = !liveInNormal[frontierId][reg];
                if (entryOnly) {
                    const auto entryEdges = std::ranges::count_if(frontierBlock->predecessors, [&](uint32_t p) { return frontierBlock->loopLatch != p; });
                    if (entryEdges < 2)
                        continue;
                }

                if (hasPhi[frontierId] != visitedToken) {
                    hasPhi[frontierId] = visitedToken;
                    entryOnlyPhis[frontierId][reg] = entryOnly;

                    LiftedInstruction phi{LiftedOperation::PHI, -1};
                    // An entry-block phi carries a trailing operand for the function-entry edge.
                    phi.operands.resize(1 + frontierBlock->predecessors.size() + (frontierId == 0 ? 1 : 0));

                    phi.operands[0].type = LiftedOperandType::Register;
                    phi.operands[0].value.reg = reg;

                    for (size_t k = 1; k < phi.operands.size(); ++k) {
                        phi.operands[k].type = LiftedOperandType::Register;
                        phi.operands[k].value.reg = reg;
                        phi.operands[k].ssaVersion = -1;
                    }

                    frontierBlock->phiNodes.push_back(std::move(phi));
                    Explain(*frontierBlock, "inserted phi for R{} because B{} is in B{}'s dominance frontier and R{} is live on entry", reg, frontierId,
                            blockId, reg);

                    if (inWorkList[frontierId] != visitedToken) {
                        inWorkList[frontierId] = visitedToken;
                        workList.push_back(frontierId);
                    }
                }
            }
        }
    }
}

std::vector<int> SSABuilder::RenameBlock(int blockId, AnalyzedFunction &func) {
    if (blockId < 0 || static_cast<size_t>(blockId) >= func.basicBlocks.size())
        return {};

    BasicBlock &block = func.basicBlocks[blockId];

    std::vector<int> varsDefinedHere;
    varsDefinedHere.reserve(16);

    for (auto &phi : block.phiNodes) {
        int reg = phi.operands[0].value.reg;
        if (blockId == 0 && phi.operands.size() > block.predecessors.size() + 1) {
            const int32_t entry = CurrentVersion(reg);
            phi.operands.back().ssaVersion = entry;
            func.useCounts[SSARef{reg, entry}]++;
            func.users[SSARef{reg, entry}].push_back(&phi);
        }
        int v = NewVersion(reg);
        phi.operands[0].ssaVersion = v;
        varsDefinedHere.push_back(reg);
        func.definitionMap[{static_cast<uint8_t>(reg), v}] = &phi;
    }

    if (block.lpHead) {
        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

            if (inst->operation == LiftedOperation::CONCAT && inst->operands.size() > 2) {
                // The generic write pass records the destination after assigning its SSA version.
                int32_t startReg = inst->operands[1].value.reg;
                int32_t endReg = inst->operands[2].value.reg;
                std::vector<int32_t> rangeVersions;

                for (int32_t r = startReg; r <= endReg; ++r) {
                    int32_t v = CurrentVersion(r);
                    if (v == -1)
                        v = NewVersion(r);

                    rangeVersions.push_back(v);

                    if (r != startReg && r != endReg) {
                        func.useCounts[{r, v}]++;
                        func.users[{r, v}].push_back(inst);
                    }
                }
                func.implicitUses[inst] = std::move(rangeVersions);
            }

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                auto &op = inst->operands[i];
                if (op.type != LiftedOperandType::Register)
                    continue;

                AccessType mode = GetRegisterAccess(*inst, i);
                if (mode == AccessType::Read || mode == AccessType::ReadWrite) {
                    int reg = op.value.reg;
                    if (CurrentVersion(reg) == -1) {
                        op.ssaVersion = NewVersion(reg);
                    } else {
                        op.ssaVersion = CurrentVersion(reg);
                    }

                    auto ref = SSARef{reg, CurrentVersion(reg)};
                    func.useCounts[ref]++;
                    func.users[ref].push_back(inst);
                }
            }

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                auto &op = inst->operands[i];
                if (op.type != LiftedOperandType::Register)
                    continue;

                AccessType mode = GetRegisterAccess(*inst, i);
                if (mode == AccessType::Write || mode == AccessType::ReadWrite) {
                    int32_t newVer = NewVersion(op.value.reg);
                    op.ssaVersion = newVer;
                    varsDefinedHere.push_back(op.value.reg);

                    func.definitionMap[{op.value.reg, newVer}] = inst;
                }
            }

            if ((inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::CALLFB) && inst->operands.size() > 2) {
                int32_t regFunc = inst->operands[0].value.reg;
                int32_t argCount = inst->operands[1].value.imm.n - 1;

                std::vector<int32_t> argVersions;
                int effectiveArgCount = argCount;
                if (effectiveArgCount > 0) {
                    argVersions.reserve(effectiveArgCount);
                    for (int32_t k = 0; k < effectiveArgCount; ++k) {
                        int32_t argReg = regFunc + 1 + k;
                        int32_t v = CurrentVersion(argReg);
                        if (v == -1)
                            v = NewVersion(argReg);
                        argVersions.push_back(v);
                        func.useCounts[{argReg, v}]++;
                        func.users[{argReg, v}].push_back(inst);
                    }
                } else {
                    auto max = CalculateLuaStackForInstruction(func, *inst);
                    for (int32_t k = 0; k < max; ++k) {
                        int32_t argReg = regFunc + 1 + k;
                        int32_t v = CurrentVersion(argReg);
                        if (v == -1)
                            v = NewVersion(argReg);
                        argVersions.push_back(v);
                        func.useCounts[{argReg, v}]++;
                        func.users[{argReg, v}].push_back(inst);
                    }
                }
                func.implicitUses[inst] = std::move(argVersions);

                int32_t retCount = inst->operands[2].value.imm.n;
                int32_t baseReg = inst->operands[0].value.reg;

                int effectiveRetCount = (retCount == 0) ? 1 : (retCount - 1);

                for (int32_t k = 0; k < effectiveRetCount; ++k) {
                    int32_t retReg = baseReg + k;
                    int32_t newVer = NewVersion(retReg);
                    varsDefinedHere.push_back(retReg);
                    func.definitionMap[{static_cast<uint8_t>(retReg), newVer}] = inst;
                }

            } else if (inst->operation == LiftedOperation::NAMECALL && !inst->operands.empty()) {
                int32_t regA = inst->operands[0].value.reg;
                int32_t regSelf = regA + 1;

                int32_t newVer = NewVersion(regSelf);
                varsDefinedHere.push_back(regSelf);
                func.definitionMap[{static_cast<uint8_t>(regSelf), newVer}] = inst;

            } else if (inst->operation == LiftedOperation::RETURN && inst->operands.size() > 1) {
                int regStart = inst->operands[0].value.reg;
                int count = inst->operands[1].value.imm.n - 1;

                std::vector<int32_t> retVersions;
                // A zero count returns values through the multret producer.
                int effectiveCount = (count == -1) ? VariadicTailCount(func, *inst, regStart) : count;

                for (int i = 0; i < effectiveCount; ++i) {
                    int reg = regStart + i;
                    int32_t v = CurrentVersion(reg);
                    if (v == -1)
                        v = NewVersion(reg);
                    retVersions.push_back(v);
                    func.useCounts[{reg, v}]++;
                    func.users[{reg, v}].push_back(inst);
                }

                if (effectiveCount > 0) {
                    // RETURN's first operand carries the base register version.
                    int reg = inst->operands[0].value.reg;
                    if (CurrentVersion(reg) == -1) {
                        inst->operands[0].ssaVersion = NewVersion(reg);
                    } else {
                        inst->operands[0].ssaVersion = CurrentVersion(reg);
                    }
                }

                func.implicitUses[inst] = std::move(retVersions);

            } else if (inst->operation == LiftedOperation::SETLIST && inst->operands.size() > 2) {
                int newItemsStartReg = inst->operands[1].value.reg;
                int newItemsCount = inst->operands[2].value.imm.n;
                // A zero count gathers values through the multret producer.
                int effectiveCount = (newItemsCount == 0) ? VariadicTailCount(func, *inst, newItemsStartReg) : (newItemsCount - 1);

                std::vector<int32_t> itemVersions;
                itemVersions.reserve(effectiveCount);

                for (int32_t k = 0; k < effectiveCount; ++k) {
                    int32_t argReg = newItemsStartReg + k;
                    int32_t v = CurrentVersion(argReg);
                    if (v == -1)
                        v = NewVersion(argReg);

                    itemVersions.push_back(v);
                    func.useCounts[{argReg, v}]++;
                    func.users[{argReg, v}].push_back(inst);
                }

                func.implicitUses[inst] = std::move(itemVersions);
            } else if (inst->operation == LiftedOperation::GETVARARGS && inst->operands.size() > 1) {
                int32_t count = inst->operands[1].value.imm.n;
                int32_t effectiveCount = (count == 0) ? 1 : (count - 1);
                uint8_t baseReg = inst->operands[0].value.reg;
                // The generic write pass already defines GETVARARGS base register.
                for (int32_t k = 1; k < effectiveCount; ++k) {
                    int32_t newVer = NewVersion(baseReg + k);
                    varsDefinedHere.push_back(baseReg + k);

                    func.definitionMap[{static_cast<uint8_t>(baseReg + k), newVer}] = inst;
                }
            } else if (
                (inst->operation == LiftedOperation::FORNPREP || inst->operation == LiftedOperation::FORGPREP ||
                 inst->operation == LiftedOperation::FORGPREP_INEXT || inst->operation == LiftedOperation::FORGPREP_NEXT ||
                 inst->operation == LiftedOperation::FORNLOOP) &&
                !inst->operands.empty()
            ) {
                int32_t baseReg = inst->operands[0].value.reg;
                std::vector<int32_t> loopInputs;
                loopInputs.reserve(3);

                for (int i = 0; i < 3; ++i) {
                    int32_t r = baseReg + i;
                    int32_t v = CurrentVersion(r);
                    if (inst->operation != LiftedOperation::FORNLOOP && inst->operation != LiftedOperation::FORNPREP && block.loopLatch &&
                        block.predecessors.size() == 2 && std::ranges::find(block.predecessors, *block.loopLatch) != block.predecessors.end()) {
                        for (const auto &phi : block.phiNodes)
                            if (phi.operands[0].value.reg == r && phi.operands[0].ssaVersion == v)
                                for (size_t pred = 0; pred < block.predecessors.size(); ++pred)
                                    if (block.predecessors[pred] != *block.loopLatch && pred + 1 < phi.operands.size() &&
                                        phi.operands[pred + 1].ssaVersion >= 0)
                                        v = phi.operands[pred + 1].ssaVersion;
                    }
                    if (v == -1)
                        v = NewVersion(r);
                    loopInputs.push_back(v);
                    func.useCounts[{r, v}]++;
                    func.users[{r, v}].push_back(inst);
                }
                func.implicitUses[inst] = std::move(loopInputs);

                if (inst->operation == LiftedOperation::FORNLOOP) {
                    const int32_t indexVersion = NewVersion(baseReg + 2);
                    varsDefinedHere.push_back(baseReg + 2);
                    func.definitionMap[{static_cast<uint8_t>(baseReg + 2), indexVersion}] = inst;
                }
            } else if (inst->operation == LiftedOperation::FORGLOOP && inst->operands.size() > 2) {
                int32_t baseReg = inst->operands[0].value.reg;
                int numVars = (inst->operands[2].value.imm.n & 0xFF);
                std::vector<int32_t> loopInputs{inst->operands[0].ssaVersion};
                for (int i = 1; i < 3; ++i) {
                    const int32_t reg = baseReg + i;
                    int32_t version = CurrentVersion(reg);
                    if (version == -1)
                        version = NewVersion(reg);
                    loopInputs.push_back(version);
                    func.useCounts[{reg, version}]++;
                    func.users[{reg, version}].push_back(inst);
                }
                func.implicitUses[inst] = std::move(loopInputs);
                int32_t stateVersion = NewVersion(baseReg + 2);
                varsDefinedHere.push_back(baseReg + 2);
                func.definitionMap[{static_cast<uint8_t>(baseReg + 2), stateVersion}] = inst;
                for (int i = 0; i < numVars; ++i) {
                    const int32_t loopVersion = NewVersion(baseReg + 3 + i);
                    varsDefinedHere.push_back(baseReg + 3 + i);
                    func.definitionMap[{static_cast<uint8_t>(baseReg + 3 + i), loopVersion}] = inst;
                }
            }
            if (inst == block.lpTail)
                break;
        }
    }

    for (uint32_t succId : block.successors) {
        if (succId >= func.basicBlocks.size())
            continue; // hostile bytecode: wild jump target
        BasicBlock &succ = func.basicBlocks[succId];

        int predIndex = -1;
        for (size_t i = 0; i < succ.predecessors.size(); ++i) {
            if (succ.predecessors[i] == block.dwBlockId) {
                predIndex = static_cast<int>(i);
                break;
            }
        }

        // Generic-for variables enter only from the back-edge; entry phis must remain undefined.
        const auto isEntryEdgeLoopVar = [&](const BasicBlock &header, int reg) -> bool {
            if (!header.loopLatch.has_value() || header.loopLatch.value() == block.dwBlockId)
                return false; // only the non-latch (entry) edge into the header
            const uint32_t lid = header.loopLatch.value();
            if (lid >= func.basicBlocks.size())
                return false;
            const BasicBlock &latch = func.basicBlocks[lid];
            if (!latch.lpTail || latch.lpTail->operation != LiftedOperation::FORGLOOP || latch.lpTail->operands.size() < 3)
                return false;
            const int base = latch.lpTail->operands[0].value.reg;
            const int numVars = latch.lpTail->operands[2].value.imm.n & 0xFF;
            return reg >= base + 3 && reg < base + 3 + numVars;
        };

        if (predIndex != -1) {
            for (auto &phi : succ.phiNodes) {
                int reg = phi.operands[0].value.reg;
                if (size_t(predIndex + 1) < phi.operands.size()) {
                    if (isEntryEdgeLoopVar(succ, reg)) {
                        phi.operands[predIndex + 1].ssaVersion = -1; // loop var: nothing flows in from the entry edge
                        Explain(succ, "phi R{} input[{}] from B{} = undefined because generic-for entry has no loop value", reg, predIndex, block.dwBlockId);
                        continue;
                    }
                    if (succ.loopLatch == block.dwBlockId && entryOnlyPhis[succId][reg]) {
                        phi.operands[predIndex + 1].ssaVersion = -1;
                        Explain(succ, "phi R{} input[{}] from latch B{} = undefined because only the loop prep reads it", reg, predIndex, block.dwBlockId);
                        continue;
                    }
                    phi.operands[predIndex + 1].ssaVersion = CurrentVersion(reg);
                    func.useCounts[SSARef{reg, CurrentVersion(reg)}]++;
                    func.users[SSARef{static_cast<uint8_t>(reg), CurrentVersion(reg)}].push_back(&phi);
                    Explain(succ, "phi R{} input[{}] from B{} = R{}#{} from the current dominator stack", reg, predIndex, block.dwBlockId, reg,
                            phi.operands[predIndex + 1].ssaVersion);
                }
            }
        }
    }

    if (!varsDefinedHere.empty() || !block.phiNodes.empty())
        ExplainDetail(block, "renamed {} definitions, including {} phi outputs, under dominator-stack state", varsDefinedHere.size(), block.phiNodes.size());

    return varsDefinedHere;
}

void SSABuilder::Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo) {
    struct Frame {
        int blockId;
        bool exiting = false;
        std::vector<int> definitions;
    };

    std::vector<Frame> frames;
    frames.push_back({blockId, false, {}});
    while (!frames.empty()) {
        Frame frame = std::move(frames.back());
        frames.pop_back();

        if (frame.exiting) {
            for (int reg : frame.definitions)
                if (!versionStack[reg].empty())
                    versionStack[reg].pop_back();
            continue;
        }
        if (frame.blockId < 0 || static_cast<size_t>(frame.blockId) >= func.basicBlocks.size())
            continue;

        frames.push_back({frame.blockId, true, RenameBlock(frame.blockId, func)});
        if (auto it = domInfo.find(frame.blockId); it != domInfo.end())
            for (auto child = it->second.children.rbegin(); child != it->second.children.rend(); ++child)
                frames.push_back({*child, false, {}});
    }
}

void SSABuilder::Build(AnalyzedFunction &func) {
    DEBUG_ASSERT(!func.basicBlocks.empty());
    DEBUG_ASSERT(func.lpLiftedFunction != nullptr);
    if (m_debugNotes && m_debugNotes->Enabled())
        m_debugFunction = std::format("F{} ({})", func.lpLiftedFunction->lpDeserialized ? static_cast<int>(func.lpLiftedFunction->lpDeserialized->bytecodeId) : -1,
                                      func.lpLiftedFunction->name);
    int totalRegs = 255;
    if (func.lpLiftedFunction && func.lpLiftedFunction->lpDeserialized) {
        totalRegs = func.lpLiftedFunction->lpDeserialized->maxstacksize;
    }
    Explain("function {}: building SSA for {} blocks and {} registers", m_debugFunction, func.basicBlocks.size(), totalRegs);

    this->versionStack.assign(totalRegs + 1, {});
    this->versionCounter.assign(totalRegs + 1, 0);

    for (auto &stack : this->versionStack) {
        stack.reserve(8);
    }

    const auto domInfo = AnalyzeDenominators(func);

    this->CreatePhiNodes(&func, domInfo);

    for (int r = 0; r <= totalRegs; ++r) {
        NewVersion(r);
    }

    Rename(0, func, domInfo);

    size_t phiCount = 0;
    for (const auto &block : func.basicBlocks)
        phiCount += block.phiNodes.size();
    Explain("function {}: SSA complete with {} phi nodes and {} definitions", m_debugFunction, phiCount, func.definitionMap.size());

    for (auto &sub : func.innerFunctions)
        this->Build(sub);
}
