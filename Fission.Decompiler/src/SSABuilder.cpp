//
// Created by Pixeluted on 30/11/2025.
//
#include "SSABuilder.hpp"

#include "Deserializer.hpp"

AccessType SSABuilder::GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex) {
    switch (op.operation) {
    case LiftedOperation::NOP:
    case LiftedOperation::BREAK:
    case LiftedOperation::JUMP:
    case LiftedOperation::PREPVARARGS:
    case LiftedOperation::PHI:
        return AccessType::NoAccess;

    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
    case LiftedOperation::RETURN:
    case LiftedOperation::JUMPIF:
    case LiftedOperation::JUMPIFNOT:
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPXEQK:
    case LiftedOperation::CAPTURE:
    case LiftedOperation::FASTCALL:
    case LiftedOperation::FASTCALL1:
    case LiftedOperation::FASTCALL2:
    case LiftedOperation::FASTCALL2K:
    case LiftedOperation::FASTCALL3:
        return AccessType::Read;

    case LiftedOperation::CALL:
        if (operandIndex == 0)
            return AccessType::ReadWrite;
        return AccessType::Read;

    case LiftedOperation::LOAD:
    case LiftedOperation::LOADNJUMP:
    case LiftedOperation::MOVE:
    case LiftedOperation::GETGLOBAL:
    case LiftedOperation::GETUPVAL:
    case LiftedOperation::GETIMPORT:
    case LiftedOperation::GETTABLE:
    case LiftedOperation::GETTABLEKS:
    case LiftedOperation::GETTABLEN:
    case LiftedOperation::NEWCLOSURE:
    case LiftedOperation::NAMECALL:
    case LiftedOperation::ADD:
    case LiftedOperation::SUB:
    case LiftedOperation::MUL:
    case LiftedOperation::DIV:
    case LiftedOperation::MOD:
    case LiftedOperation::POW:
    case LiftedOperation::ADDK:
    case LiftedOperation::SUBK:
    case LiftedOperation::MULK:
    case LiftedOperation::DIVK:
    case LiftedOperation::MODK:
    case LiftedOperation::POWK:
    case LiftedOperation::AND:
    case LiftedOperation::OR:
    case LiftedOperation::ANDK:
    case LiftedOperation::ORK:
    case LiftedOperation::CONCAT:
    case LiftedOperation::NOT:
    case LiftedOperation::MINUS:
    case LiftedOperation::LENGTH:
    case LiftedOperation::NEWTABLE:
    case LiftedOperation::DUPTABLE:
    case LiftedOperation::GETVARARGS:
    case LiftedOperation::DUPCLOSURE:
    case LiftedOperation::SUBRK:
    case LiftedOperation::DIVRK:
    case LiftedOperation::IDIV:
    case LiftedOperation::IDIVK:
    case LiftedOperation::FORNPREP:
    case LiftedOperation::FORNLOOP:
    case LiftedOperation::FORGLOOP:
    case LiftedOperation::FORGPREP:
    case LiftedOperation::FORGPREP_INEXT:
    case LiftedOperation::FORGPREP_NEXT:
        if (operandIndex == 0)
            return AccessType::Write;
        return AccessType::Read;

    default:
        return AccessType::NoAccess;
    }
}

int32_t SSABuilder::NewVersion(int32_t reg) {
    const int32_t nVer = versionCounter[reg]++;
    versionStack[reg].push(nVer);
    return nVer;
}

int32_t SSABuilder::CurrentVersion(int32_t reg) {
    if (versionStack[reg].empty()) {
        return -1;
    }
    return versionStack[reg].top();
}

void SSABuilder::CreatePhiNodes(AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo) {
    this->blocksDefining.clear();
    if (lpOriginalFunction->lpLiftedFunction) {
        for (uint8_t i = 0; i < lpOriginalFunction->lpLiftedFunction->lpDeserialized->maxstacksize; ++i) {
            blocksDefining[i].insert(0);
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
                    const AccessType mode = GetRegisterAccess(*inst, i);
                    if (mode == AccessType::Write || mode == AccessType::ReadWrite)
                        blocksDefining[inst->operands[i].value.reg].insert(block.dwBlockId);
                }
            }
            if (inst == block.lpTail)
                break;
        }
    }

    for (auto const &[reg, defBlocks] : blocksDefining) {
        std::vector<int> workList(defBlocks.begin(), defBlocks.end());
        std::set<int> hasPhi;
        std::set<int> inWorkList(defBlocks.begin(), defBlocks.end());

        size_t idx = 0;
        while (idx < workList.size()) {
            int blockId = workList[idx++];

            if (!domInfo.contains(blockId))
                continue;

            for (int frontierId : domInfo.at(blockId).dominanceFrontier) {
                if (!hasPhi.contains(frontierId)) {
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

                    frontierBlock->phiNodes.push_back(phi);
                    hasPhi.insert(frontierId);

                    if (!inWorkList.contains(frontierId)) {
                        workList.push_back(frontierId);
                        inWorkList.insert(frontierId);
                    }
                }
            }
        }
    }
}

void SSABuilder::Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo) {
    BasicBlock &block = func.basicBlocks[blockId];

    std::map<int, int> pushedCount;

    for (auto &phi : block.phiNodes) {
        int reg = phi.operands[0].value.reg;
        int v = NewVersion(reg);
        phi.operands[0].ssaVersion = v;
        pushedCount[reg]++;
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
                    if (CurrentVersion(op.value.reg) == -1)
                        op.ssaVersion = NewVersion(op.value.reg);
                    else
                        op.ssaVersion = CurrentVersion(op.value.reg);
                }
            }

            for (size_t i = 0; i < inst->operands.size(); ++i) {
                auto &op = inst->operands[i];
                if (op.type != LiftedOperandType::Register)
                    continue;

                AccessType mode = GetRegisterAccess(*inst, i);
                if (mode == AccessType::Write || mode == AccessType::ReadWrite) {
                    if (mode == AccessType::Write) {
                        op.ssaVersion = NewVersion(op.value.reg);
                    } else {
                        NewVersion(op.value.reg);
                    }
                    pushedCount[op.value.reg]++;
                }
            }

            if (inst == block.lpTail)
                break;
        }
    }

    for (uint32_t succId : block.successors) {
        BasicBlock &succ = func.basicBlocks[succId];

        for (auto &phi : succ.phiNodes) {
            int reg = phi.operands[0].value.reg;

            for (size_t i = 0; i < succ.predecessors.size(); ++i) {
                if (succ.predecessors[i] == block.dwBlockId) {
                    phi.operands[i + 1].ssaVersion = CurrentVersion(reg);
                    break;
                }
            }
        }
    }

    if (domInfo.contains(blockId)) {
        for (const int childId : domInfo.at(blockId).children) {
            Rename(childId, func, domInfo);
        }
    }

    for (auto const &[reg, count] : pushedCount) {
        for (auto i = 0; i < count; ++i) {
            if (!versionStack[reg].empty())
                versionStack[reg].pop();
        }
    }
}

void SSABuilder::Build(AnalyzedFunction &func) {
    this->versionStack.clear();
    this->versionCounter.clear();
    this->blocksDefining.clear();

    const auto domInfo = AnalyzeDenominators(func);

    this->CreatePhiNodes(&func, domInfo);

    int totalRegs = func.lpLiftedFunction ? func.lpLiftedFunction->lpDeserialized->maxstacksize : 255;
    for (int r = 0; r < totalRegs; ++r) {
        if (versionStack[r].empty())
            NewVersion(r);
    }

    Rename(0, func, domInfo);

    for (auto &sub : func.innerFunctions)
        this->Build(sub);
}