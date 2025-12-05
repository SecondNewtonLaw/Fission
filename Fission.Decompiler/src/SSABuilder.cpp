//
// Created by Pixeluted on 30/11/2025.
//
#include "SSABuilder.hpp"
#include "Deserializer.hpp"
#include <algorithm>
#include <array>
#include <vector>

static const std::array<AccessType, 256> kOpcodeAccessTable = [] {
    std::array<AccessType, 256> table{};
    table.fill(AccessType::NoAccess);

    auto set = [&](LiftedOperation op, AccessType type) { table[static_cast<size_t>(op)] = type; };

    for (auto op :
         {LiftedOperation::SETGLOBAL,  LiftedOperation::SETUPVAL, LiftedOperation::SETTABLE,    LiftedOperation::SETTABLEKS,  LiftedOperation::SETTABLEN,
          LiftedOperation::SETLIST,    LiftedOperation::RETURN,   LiftedOperation::JUMPIF,      LiftedOperation::JUMPIFNOT,   LiftedOperation::JUMPIFEQ,
          LiftedOperation::JUMPIFLE,   LiftedOperation::JUMPIFLT, LiftedOperation::JUMPIFNOTEQ, LiftedOperation::JUMPIFNOTLE, LiftedOperation::JUMPIFNOTLT,
          LiftedOperation::JUMPXEQK,   LiftedOperation::CAPTURE,  LiftedOperation::FASTCALL,    LiftedOperation::FASTCALL1,   LiftedOperation::FASTCALL2,
          LiftedOperation::FASTCALL2K, LiftedOperation::FASTCALL3}) {
        set(op, AccessType::Read);
    }

    for (auto op : {LiftedOperation::LOAD,         LiftedOperation::LOADNJUMP,  LiftedOperation::MOVE,       LiftedOperation::GETGLOBAL,
                    LiftedOperation::GETUPVAL,     LiftedOperation::GETIMPORT,  LiftedOperation::GETTABLE,   LiftedOperation::GETTABLEKS,
                    LiftedOperation::GETTABLEN,    LiftedOperation::NEWCLOSURE, LiftedOperation::NAMECALL,   LiftedOperation::ADD,
                    LiftedOperation::SUB,          LiftedOperation::MUL,        LiftedOperation::DIV,        LiftedOperation::MOD,
                    LiftedOperation::POW,          LiftedOperation::ADDK,       LiftedOperation::SUBK,       LiftedOperation::MULK,
                    LiftedOperation::DIVK,         LiftedOperation::MODK,       LiftedOperation::POWK,       LiftedOperation::AND,
                    LiftedOperation::OR,           LiftedOperation::ANDK,       LiftedOperation::ORK,        LiftedOperation::CONCAT,
                    LiftedOperation::NOT,          LiftedOperation::MINUS,      LiftedOperation::LENGTH,     LiftedOperation::NEWTABLE,
                    LiftedOperation::DUPTABLE,     LiftedOperation::GETVARARGS, LiftedOperation::DUPCLOSURE, LiftedOperation::SUBRK,
                    LiftedOperation::DIVRK,        LiftedOperation::IDIV,       LiftedOperation::IDIVK,      LiftedOperation::FORNPREP,
                    LiftedOperation::FORNLOOP,     LiftedOperation::FORGLOOP,   LiftedOperation::FORGPREP,   LiftedOperation::FORGPREP_INEXT,
                    LiftedOperation::FORGPREP_NEXT}) {
        set(op, AccessType::Write);
    }

    set(LiftedOperation::CALL, AccessType::Read); // THIS NEEDS TO BE CHANGED, READWRITE BREAKS AST LIFTING FOR CALLS.

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

int32_t SSABuilder::NewVersion(int32_t reg) {
    const int32_t nVer = versionCounter[reg]++;
    versionStack[reg].push_back(nVer);
    return nVer;
}

int32_t SSABuilder::CurrentVersion(int32_t reg) {
    if (versionStack[reg].empty()) {
        return -1;
    }
    return versionStack[reg].back();
}

void SSABuilder::CreatePhiNodes(AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo) {
    if (lpOriginalFunction->basicBlocks.empty())
        return;

    int maxRegs = 255;
    if (lpOriginalFunction->lpLiftedFunction && lpOriginalFunction->lpLiftedFunction->lpDeserialized) {
        maxRegs = lpOriginalFunction->lpLiftedFunction->lpDeserialized->maxstacksize;
    }

    std::vector<std::vector<int>> defBlocks(maxRegs + 1);

    if (lpOriginalFunction->lpLiftedFunction) {
        for (int i = 0; i <= maxRegs; ++i) {
            defBlocks[i].push_back(0);
        }
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
    }

    if (block.lpHead) {
        for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
            if (inst->operation == LiftedOperation::NOP)
                continue;

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
                argVersions.reserve(argCount > 0 ? argCount : 0);

                if (argCount == -1 /* var arg */) {
                    // get previous instruction, almost guaranteed always to be a GETVARARGS
                    if ((inst - 1)->operation == LiftedOperation::GETVARARGS) {
                        // ASSERT((inst - 1)->operation == LiftedOperation::GETVARARGS, "no GETVARARGS previous to a call that uses a VARARG argument count!");
                        auto topCallRegister = (inst - 1)->operands[0];
                        argCount = topCallRegister.value.reg;
                    }
                }

                for (int32_t k = 0; k < argCount; ++k) {
                    int32_t argReg = regFunc + 1 + k;

                    int32_t v = CurrentVersion(argReg);

                    if (v == -1) {
                        v = NewVersion(argReg);
                    }

                    argVersions.push_back(v);
                }

                func.implicitUses[inst] = std::move(argVersions);
                int32_t retCount = inst->operands[2].value.imm.n;

                if (retCount > 1) {
                    int32_t baseReg = inst->operands[0].value.reg;
                    for (int32_t k = 0; k < retCount - 1; ++k) {
                        uint8_t retReg = baseReg + k;

                        int32_t newVer = NewVersion(retReg);
                        varsDefinedHere.push_back(retReg);

                        func.definitionMap[{retReg, newVer}] = inst;
                    }
                }
            } else if (inst->operation == LiftedOperation::RETURN) {
                int regStart = inst->operands[0].value.reg;
                int count = inst->operands[1].value.imm.n - 1;

                std::vector<int32_t> retVersions;
                for (int i = 0; i < count; ++i) {
                    int reg = regStart + i;
                    int32_t v = CurrentVersion(reg);
                    if (v == -1)
                        v = NewVersion(reg);
                    retVersions.push_back(v);
                }

                func.implicitUses[inst] = std::move(retVersions);
            } else if (inst->operation == LiftedOperation::SETLIST) {
                // int tableReg = inst->operands[0].value.reg;
                int newItemsStartReg = inst->operands[1].value.reg;
                int newItemsCount = inst->operands[2].value.imm.n;
                // unused.
                // int startFrom = inst->operands[3].value.imm.n;

                std::vector<int32_t> itemVersions;
                itemVersions.reserve(newItemsCount > 0 ? newItemsCount : 0);

                if (newItemsCount > 0) {
                    for (int32_t k = 0; k < newItemsCount; ++k) {
                        int32_t argReg = newItemsStartReg + k;

                        int32_t v = CurrentVersion(argReg);

                        if (v == -1) {
                            v = NewVersion(argReg);
                        }

                        itemVersions.push_back(v);
                    }
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