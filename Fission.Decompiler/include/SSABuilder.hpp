//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "DenominatorAnalysis.hpp"

#include <map>
#include <set>
#include <stack>

enum class AccessType { NoAccess, Read, Write, ReadWrite };

class SSABuilder {
    /*
     *  Stack of active SSA versions.
     */
    std::map<int, std::stack<int>> versionStack;
    /*
     *  RegID to next available SSA version.
     */
    std::map<int, int> versionCounter;

    /*
     *  Used for tracking Phi insertions (Register ID -> Set<Block Ids>)
     */
    std::map<int, std::set<int>> blocksDefining;

    std::map<int, std::set<int>> globals;

    AccessType GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex) {
        switch (op.operation) {
        default:
        case LiftedOperation::NOP:
        case LiftedOperation::BREAK:
            return AccessType::NoAccess;

        case LiftedOperation::CALL: {
            if (operandIndex == 0)
                return AccessType::ReadWrite;
            return AccessType::Read;
        }
        case LiftedOperation::FASTCALL3:
        case LiftedOperation::FASTCALL:
        case LiftedOperation::FASTCALL1:
        case LiftedOperation::FASTCALL2:
        case LiftedOperation::FASTCALL2K:
        case LiftedOperation::RETURN:
        case LiftedOperation::JUMP:
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::PREPVARARGS:
        case LiftedOperation::CAPTURE:
        case LiftedOperation::JUMPXEQK:
            return AccessType::Read;

        case LiftedOperation::NEWCLOSURE:
        case LiftedOperation::GETGLOBAL:
        case LiftedOperation::LOAD:
        case LiftedOperation::LOADNJUMP:
        case LiftedOperation::MOVE:
        case LiftedOperation::GETUPVAL:
        case LiftedOperation::GETIMPORT:
        case LiftedOperation::GETTABLE:
        case LiftedOperation::GETTABLEKS:
        case LiftedOperation::GETTABLEN:
        case LiftedOperation::DUPCLOSURE:
        case LiftedOperation::NAMECALL:
            if (operandIndex == 0)
                return AccessType::Write;
            return AccessType::Read;

        case LiftedOperation::SETGLOBAL:
        case LiftedOperation::SETUPVAL:
            if (operandIndex == 0)
                return AccessType::Read;
            return AccessType::Write;

        case LiftedOperation::SETTABLE:
        case LiftedOperation::SETTABLEKS:
        case LiftedOperation::SETTABLEN:
            if (operandIndex == 1)
                return AccessType::Write;
            return AccessType::Read;

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
        case LiftedOperation::SETLIST:
        case LiftedOperation::FORNPREP:
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGPREP_NEXT:
        case LiftedOperation::FORGPREP:
        case LiftedOperation::GETVARARGS:
        case LiftedOperation::SUBRK:
        case LiftedOperation::DIVRK:
        case LiftedOperation::IDIV:
        case LiftedOperation::IDIVK:
            if (operandIndex == 0)
                return AccessType::Write;
            return AccessType::Read;

        case LiftedOperation::PHI:
            return AccessType::NoAccess;
        }
        return AccessType::NoAccess;
    }

    int32_t NewVersion(int32_t reg) {
        const int32_t nVer = versionCounter[reg]++;
        versionStack[reg].push(nVer);
        return nVer;
    }

    int32_t CurrentVersion(int32_t reg) {
        if (versionStack[reg].empty())
            return -1;
        return versionStack[reg].top();
    }

    void CreatePhiNodes(const AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo) {
        this->blocksDefining.clear();
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
                        // Create a PHI node in the sidecar
                        BasicBlock *frontierBlock = &lpOriginalFunction->basicBlocks[frontierId];

                        LiftedInstruction phi;
                        phi.operation = LiftedOperation::PHI;
                        // Op 0: Destination Register
                        // Ops 1..N: Incoming values (one per predecessor)
                        phi.operands.resize(1 + frontierBlock->predecessors.size());

                        // Setup Dest
                        phi.operands[0].type = LiftedOperandType::Register;
                        phi.operands[0].value.reg = reg;

                        // Setup Sources (Placeholders)
                        for (size_t k = 1; k < phi.operands.size(); ++k) {
                            phi.operands[k].type = LiftedOperandType::Register;
                            phi.operands[k].value.reg = reg;
                            phi.operands[k].ssaVersion = -1; // To be filled in Rename
                        }

                        frontierBlock->phiNodes.push_back(phi);
                        hasPhi.insert(frontierId);

                        // A Phi is a definition, so this block now defines the variable too
                        if (!inWorkList.contains(frontierId)) {
                            workList.push_back(frontierId);
                            inWorkList.insert(frontierId);
                        }
                    }
                }
            }
        }
    }

    void Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo) {
        BasicBlock &block = func.basicBlocks[blockId];

        // Keep track of which registers got new versions in THIS block
        // so we can pop them from the stack when we leave (Scope management)
        std::map<int, int> pushedCount;

        // A. Rename PHI Destinations (The Sidecar)
        for (auto &phi : block.phiNodes) {
            int reg = phi.operands[0].value.reg;
            int v = NewVersion(reg);
            phi.operands[0].ssaVersion = v;
            pushedCount[reg]++;
        }

        // B. Rename Original Instructions
        if (block.lpHead) {
            LiftedInstruction *inst = block.lpHead;
            while (true) {
                if (inst->operation != LiftedOperation::NOP) {

                    // 1. Rename Reads (RHS) - Consume current version
                    for (size_t i = 0; i < inst->operands.size(); ++i) {
                        auto &op = inst->operands[i];
                        if (op.type != LiftedOperandType::Register)
                            continue;

                        AccessType mode = GetRegisterAccess(*inst, i);
                        if (mode == AccessType::Read || mode == AccessType::ReadWrite) {
                            op.ssaVersion = CurrentVersion(op.value.reg);
                        }
                    }

                    // 2. Rename Writes (LHS) - Create new version
                    for (size_t i = 0; i < inst->operands.size(); ++i) {
                        auto &op = inst->operands[i];
                        if (op.type != LiftedOperandType::Register)
                            continue;

                        AccessType mode = GetRegisterAccess(*inst, i);
                        if (mode == AccessType::Write || mode == AccessType::ReadWrite) {
                            int v = NewVersion(op.value.reg);
                            op.ssaVersion = v;
                            pushedCount[op.value.reg]++;
                        }
                    }
                }
                if (inst == block.lpTail)
                    break;
                inst++;
            }
        }

        // C. Update Successors' Phi Inputs
        // We are leaving this block. Any block we jump to might have a PHI node waiting for input from US.
        for (uint32_t succId : block.successors) {
            BasicBlock &succ = func.basicBlocks[succId];

            // For every PHI in the successor...
            for (auto &phi : succ.phiNodes) {
                int reg = phi.operands[0].value.reg; // The register the Phi merges

                // Find which operand index corresponds to the edge coming from 'blockId'
                // Phi operands [1..N] match Predecessors [0..N-1]
                // We need to find the index of 'blockId' in 'succ.predecessors'

                for (size_t i = 0; i < succ.predecessors.size(); ++i) {
                    if (succ.predecessors[i] == block.dwBlockId) {
                        // Found it! Operand index is i + 1 (because Op 0 is dest)
                        phi.operands[i + 1].ssaVersion = CurrentVersion(reg);
                        break;
                    }
                }
            }
        }

        // D. Recurse down the Dominator Tree
        if (domInfo.count(blockId)) {
            for (int childId : domInfo.at(blockId).children) {
                Rename(childId, func, domInfo);
            }
        }

        // E. Pop Stacks (Backtracking)
        // We are done with the subtree dominated by this block.
        // Restore variable versions to what they were before entering this block.
        for (auto const &[reg, count] : pushedCount) {
            for (int i = 0; i < count; ++i) {
                if (!versionStack[reg].empty())
                    versionStack[reg].pop();
            }
        }
    }

  public:
    void Build(AnalyzedFunction &func) {
        auto domInfo = AnalyzeDenominators(func);

        this->CreatePhiNodes(&func, domInfo);

        Rename(0, func, domInfo);

        for (auto &sub : func.innerFunctions) {
            Build(sub);
        }
    }
};