//
// Created by Pixeluted on 30/11/2025.
//
#include "SSABuilder.hpp"
#include "Deserializer.hpp"
#include <algorithm>
#include <array>
#include <queue>
#include <set>
#include <vector>

static const std::array<AccessType, 256> kOpcodeAccessTable = [] {
    std::array<AccessType, 256> table{};
    table.fill(AccessType::NoAccess);

    auto set = [&](LiftedOperation op, AccessType type) { table[static_cast<size_t>(op)] = type; };

    for (auto op :
         {LiftedOperation::SETGLOBAL,  LiftedOperation::SETUPVAL, LiftedOperation::SETTABLE,      LiftedOperation::SETTABLEKS,  LiftedOperation::SETTABLEN,
          LiftedOperation::SETLIST,    LiftedOperation::RETURN,   LiftedOperation::JUMPIF,        LiftedOperation::JUMPIFNOT,   LiftedOperation::JUMPIFEQ,
          LiftedOperation::JUMPIFLE,   LiftedOperation::JUMPIFLT, LiftedOperation::JUMPIFNOTEQ,   LiftedOperation::JUMPIFNOTLE, LiftedOperation::JUMPIFNOTLT,
          LiftedOperation::JUMPXEQK,   LiftedOperation::CAPTURE,  LiftedOperation::FASTCALL,      LiftedOperation::FASTCALL1,   LiftedOperation::FASTCALL2,
          LiftedOperation::FASTCALL2K, LiftedOperation::FORGLOOP, LiftedOperation::FORGPREP_NEXT, LiftedOperation::FORGPREP,    LiftedOperation::FORGPREP_INEXT,
          LiftedOperation::FORNPREP,   LiftedOperation::FASTCALL3}) {
        set(op, AccessType::Read);
    }

    for (auto op :
         {LiftedOperation::LOAD,      LiftedOperation::LOADNJUMP, LiftedOperation::MOVE,       LiftedOperation::GETGLOBAL,  LiftedOperation::GETUPVAL,
          LiftedOperation::GETIMPORT, LiftedOperation::GETTABLE,  LiftedOperation::GETTABLEKS, LiftedOperation::GETTABLEN,  LiftedOperation::NEWCLOSURE,
          LiftedOperation::NAMECALL,  LiftedOperation::ADD,       LiftedOperation::SUB,        LiftedOperation::MUL,        LiftedOperation::DIV,
          LiftedOperation::MOD,       LiftedOperation::POW,       LiftedOperation::ADDK,       LiftedOperation::SUBK,       LiftedOperation::MULK,
          LiftedOperation::DIVK,      LiftedOperation::MODK,      LiftedOperation::POWK,       LiftedOperation::AND,        LiftedOperation::OR,
          LiftedOperation::ANDK,      LiftedOperation::ORK,       LiftedOperation::NOT,        LiftedOperation::MINUS,      LiftedOperation::LENGTH,
          LiftedOperation::NEWTABLE,  LiftedOperation::DUPTABLE,  LiftedOperation::GETVARARGS, LiftedOperation::DUPCLOSURE, LiftedOperation::SUBRK,
          LiftedOperation::CONCAT,    LiftedOperation::DIVRK,     LiftedOperation::IDIV,       LiftedOperation::IDIVK,      LiftedOperation::FORNLOOP}) {
        set(op, AccessType::Write);
    }

    set(LiftedOperation::CALL, AccessType::Read);
    set(LiftedOperation::RETURN, AccessType::Deferred);

    return table;
}();

AccessType SSABuilder::GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex) {
    const AccessType baseType = kOpcodeAccessTable[static_cast<size_t>(op.operation)];

    if (baseType == AccessType::Write) {
        return (operandIndex == 0) ? AccessType::Write : AccessType::Read;
    }

    if (baseType == AccessType::ReadWrite) {
        return (operandIndex == 0) ? AccessType::ReadWrite : AccessType::Read;
    }

    return baseType;
}

std::vector<int> SSABuilder::GetImplicitDefinitions(const LiftedInstruction &inst) {
    std::vector<int> defs;
    switch (inst.operation) {
    case LiftedOperation::CALL: {
        int regStart = inst.operands[0].value.reg;
        int retCount = inst.operands[2].value.imm.n;
        int effectiveRetCount = (retCount == 0) ? 1 : (retCount - 1);
        for (int k = 0; k < effectiveRetCount; ++k) {
            defs.push_back(regStart + k);
        }
        break;
    }
    case LiftedOperation::NAMECALL: {
        int regA = inst.operands[0].value.reg;
        defs.push_back(regA + 1); // Implicit Self
        break;
    }
    case LiftedOperation::GETVARARGS: {
        int baseReg = inst.operands[0].value.reg;
        int count = inst.operands[1].value.imm.n;
        for (int k = 1; k < count; ++k) {
            defs.push_back(baseReg + k);
        }
        break;
    }
    case LiftedOperation::SETLIST: {
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

static void ComputeLiveness(AnalyzedFunction *func, int maxRegs, std::vector<std::vector<bool>> &liveIn) {
    size_t numBlocks = func->basicBlocks.size();
    liveIn.assign(numBlocks, std::vector<bool>(maxRegs + 1, false));
    std::vector<std::vector<bool>> use(numBlocks, std::vector<bool>(maxRegs + 1, false));
    std::vector<std::vector<bool>> def(numBlocks, std::vector<bool>(maxRegs + 1, false));

    for (const auto &block : func->basicBlocks) {
        if (!block.lpHead)
            continue;
        uint32_t bid = block.dwBlockId;

        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

            auto markRead = [&](int r) {
                if (r >= 0 && r <= maxRegs && !def[bid][r]) {
                    use[bid][r] = true;
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

            if (inst->operation == LiftedOperation::CONCAT) {
                int start = inst->operands[1].value.reg;
                int end = inst->operands[2].value.reg;
                for (int r = start; r <= end; ++r)
                    markRead(r);
            } else if (inst->operation == LiftedOperation::CALL) {
                int32_t regFunc = inst->operands[0].value.reg;
                int32_t argCount = inst->operands[1].value.imm.n - 1;

                int effectiveArgCount = (argCount == -1) ? 1 : argCount;
                if (effectiveArgCount > 0)
                    for (int32_t k = 0; k < effectiveArgCount; ++k)
                        markRead(regFunc + 1 + k);

                int32_t retCount = inst->operands[2].value.imm.n;
                int32_t baseReg = inst->operands[0].value.reg;

                int effectiveRetCount = (retCount == 0) ? 1 : (retCount - 1);

                for (int32_t k = 0; k < effectiveRetCount; ++k) {
                    int32_t retReg = baseReg + k;
                    markWrite(retReg);
                }
            } else if (inst->operation == LiftedOperation::NAMECALL) {
                int base = inst->operands[0].value.reg;
                markWrite(base); // self
                markRead(inst->operands[1].value.reg);
            } else if (inst->operation == LiftedOperation::RETURN) {
                int base = inst->operands[0].value.reg;
                int count = inst->operands[1].value.imm.n - 1;
                if (count == -1)
                    count = 0;
                for (int k = 0; k < count; ++k)
                    markRead(base + k);
            } else if (inst->operation == LiftedOperation::SETLIST) { // TODO: Handle vararg SETLIST properly.
                int base = inst->operands[1].value.reg;
                int count = inst->operands[2].value.imm.n;
                int effectiveCount = (count > 0) ? (count - 1) : 1; // assume base is read, as we may use it.
                for (int k = 0; k < effectiveCount; ++k)
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

    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto &block : func->basicBlocks) {
            uint32_t bid = block.dwBlockId;
            bool blockChanged = false;

            for (int r = 0; r <= maxRegs; ++r) {
                bool isLiveOut = false;
                for (uint32_t succ : block.successors) {
                    if (liveIn[succ][r]) {
                        isLiveOut = true;
                        break;
                    }
                }

                bool isLiveIn = use[bid][r] || (isLiveOut && !def[bid][r]);
                if (liveIn[bid][r] != isLiveIn) {
                    liveIn[bid][r] = isLiveIn;
                    blockChanged = true;
                }
            }
            if (blockChanged)
                changed = true;
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

    std::vector<std::vector<bool>> liveIn;
    ComputeLiveness(lpOriginalFunction, maxRegs, liveIn);

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

    std::vector<int> hasPhi(lpOriginalFunction->basicBlocks.size() + 1, 0);
    std::vector<int> inWorkList(lpOriginalFunction->basicBlocks.size() + 1, 0);
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

                // skip dead Phis
                if (!liveIn[frontierId][reg])
                    continue;

                if (hasPhi[frontierId] != visitedToken) {
                    hasPhi[frontierId] = visitedToken;

                    BasicBlock *frontierBlock = &lpOriginalFunction->basicBlocks[frontierId];

                    LiftedInstruction phi;
                    phi.operation = LiftedOperation::PHI;
                    phi.operands.resize(1 + frontierBlock->predecessors.size());

                    phi.operands[0].type = LiftedOperandType::Register;
                    phi.operands[0].value.reg = reg;

                    for (size_t k = 1; k < phi.operands.size(); ++k) {
                        phi.operands[k].type = LiftedOperandType::Register;
                        phi.operands[k].value.reg = reg;
                        phi.operands[k].ssaVersion = -1;
                    }

                    frontierBlock->phiNodes.push_back(std::move(phi));

                    if (inWorkList[frontierId] != visitedToken) {
                        inWorkList[frontierId] = visitedToken;
                        workList.push_back(frontierId);
                    }
                }
            }
        }
    }
}

void SSABuilder::Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo) {
    BasicBlock &block = func.basicBlocks[blockId];

    std::vector<int> varsDefinedHere;
    varsDefinedHere.reserve(16);

    for (auto &phi : block.phiNodes) {
        int reg = phi.operands[0].value.reg;
        int v = NewVersion(reg);
        phi.operands[0].ssaVersion = v;
        varsDefinedHere.push_back(reg);
        func.definitionMap[{static_cast<uint8_t>(reg), v}] = &phi;
    }

    if (block.lpHead) {
        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

            if (inst->operation == LiftedOperation::CONCAT) {
                func.definitionMap[{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion}] = inst;
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

            if (inst->operation == LiftedOperation::CALL) {
                int32_t regFunc = inst->operands[0].value.reg;
                int32_t argCount = inst->operands[1].value.imm.n - 1;

                std::vector<int32_t> argVersions;
                int effectiveArgCount = (argCount == -1) ? 1 : argCount;
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

            } else if (inst->operation == LiftedOperation::NAMECALL) {
                int32_t regA = inst->operands[0].value.reg;
                int32_t regSelf = regA + 1;

                int32_t newVer = NewVersion(regSelf);
                varsDefinedHere.push_back(regSelf);
                func.definitionMap[{static_cast<uint8_t>(regSelf), newVer}] = inst;

            } else if (inst->operation == LiftedOperation::RETURN) {
                int regStart = inst->operands[0].value.reg;
                int count = inst->operands[1].value.imm.n - 1;

                std::vector<int32_t> retVersions;
                int effectiveCount = (count == -1) ? 1 : count;

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
                    // set version on first argument, which is a reg, fixing some issues relating to lifting and representation for RETURN IR ops.
                    int reg = inst->operands[0].value.reg;
                    if (CurrentVersion(reg) == -1) {
                        inst->operands[0].ssaVersion = NewVersion(reg);
                    } else {
                        inst->operands[0].ssaVersion = CurrentVersion(reg);
                    }
                }

                func.implicitUses[inst] = std::move(retVersions);

            } else if (inst->operation == LiftedOperation::SETLIST) {
                int newItemsStartReg = inst->operands[1].value.reg;
                int newItemsCount = inst->operands[2].value.imm.n;
                int effectiveCount = (newItemsCount == 0) ? 1 : (newItemsCount - 1);

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
            } else if (inst->operation == LiftedOperation::GETVARARGS) {
                int32_t count = inst->operands[1].value.imm.n;
                if (count > 1) {
                    uint8_t baseReg = inst->operands[0].value.reg;
                    for (uint8_t k = 1; k < count; ++k) {
                        int32_t newVer = NewVersion(baseReg + k);
                        varsDefinedHere.push_back(baseReg + k);

                        func.definitionMap[{static_cast<uint8_t>(baseReg + k), newVer}] = inst;
                    }
                }
            } else if (inst->operation == LiftedOperation::FORNPREP) {
                int32_t baseReg = inst->operands[0].value.reg;
                std::vector<int32_t> loopInputs;
                loopInputs.reserve(3);

                for (int i = 0; i < 3; ++i) {
                    int32_t r = baseReg + i;
                    int32_t v = CurrentVersion(r);
                    if (v == -1)
                        v = NewVersion(r);
                    loopInputs.push_back(v);
                    func.useCounts[{r, v}]++;
                    func.users[{r, v}].push_back(inst);
                }
                func.implicitUses[inst] = std::move(loopInputs);
            } else if (inst->operation == LiftedOperation::FORNLOOP) {
                NewVersion(inst->operands[2].value.reg);
            }
            if (inst == block.lpTail)
                break;
        }
    }

    for (uint32_t succId : block.successors) {
        BasicBlock &succ = func.basicBlocks[succId];

        int predIndex = -1;
        for (size_t i = 0; i < succ.predecessors.size(); ++i) {
            if (succ.predecessors[i] == block.dwBlockId) {
                predIndex = static_cast<int>(i);
                break;
            }
        }

        if (predIndex != -1) {
            for (auto &phi : succ.phiNodes) {
                int reg = phi.operands[0].value.reg;
                if (size_t(predIndex + 1) < phi.operands.size()) {
                    phi.operands[predIndex + 1].ssaVersion = CurrentVersion(reg);
                    func.useCounts[SSARef{reg, CurrentVersion(reg)}]++;
                    func.users[SSARef{static_cast<uint8_t>(reg), CurrentVersion(reg)}].push_back(&phi);
                }
            }
        }
    }

    if (auto it = domInfo.find(blockId); it != domInfo.end()) {
        for (const int childId : it->second.children) {
            Rename(childId, func, domInfo);
        }
    }

    for (int reg : varsDefinedHere) {
        if (!versionStack[reg].empty()) {
            versionStack[reg].pop_back();
        }
    }
}

void SSABuilder::Build(AnalyzedFunction &func) {
    int totalRegs = 255;
    if (func.lpLiftedFunction && func.lpLiftedFunction->lpDeserialized) {
        totalRegs = func.lpLiftedFunction->lpDeserialized->maxstacksize;
    }

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

    for (auto &sub : func.innerFunctions)
        this->Build(sub);
}