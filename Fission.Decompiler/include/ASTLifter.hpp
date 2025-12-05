//
// Created by Pixeluted on 04/12/2025.
//
#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"

#include <unordered_set>
#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // Not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

    std::vector<ASTFunction> subFunctions;
};

class ASTLifter {
    std::unordered_set<int32_t> m_pinnedRegisters;

    struct PinnedRegisterScope {
        ASTLifter *m_lpLifter;
        int32_t dwReg;

        PinnedRegisterScope(ASTLifter *lifter, int32_t reg) : m_lpLifter(lifter), dwReg(reg) { m_lpLifter->m_pinnedRegisters.insert(reg); }

        ~PinnedRegisterScope() { m_lpLifter->m_pinnedRegisters.erase(dwReg); }

        PinnedRegisterScope(const PinnedRegisterScope &) = delete;
        PinnedRegisterScope &operator=(const PinnedRegisterScope &) = delete;
    };

    bool bLoopEnter = false;
    int32_t loopRegister = 0;
    void EnterLoop(int32_t reg) {
        loopRegister = reg;
        bLoopEnter = true;
    }

    void ExitLoop() {
        bLoopEnter = false;
        loopRegister = 0;
    }

    std::string GetVarName(const LiftedOperand &op) { return GetVarName(op.value.reg, op.ssaVersion); }

    std::string GetVarName(int reg, int ver) {
        if (bLoopEnter && reg == loopRegister) {
            return std::format("i_{}", reg);
        }

        if (ver != -1)
            return std::format("v{}_{}", reg, ver);

        return std::format("v{}", reg);
    }

    bool IsInstructionConsumed(const AnalyzedFunction *func, int index) {
        const auto &inst = func->lpLiftedFunction->instructions[index];
        if (inst.operation != LiftedOperation::CALL && inst.operation != LiftedOperation::NAMECALL)
            return false;

        if (inst.operation == LiftedOperation::CALL) {
            if (inst.operands[2].value.imm.n == 0)
                return true;
        } else {
            if (static_cast<size_t>(index) + 2 < func->lpLiftedFunction->instructions.size()) {
                const auto &callInst = func->lpLiftedFunction->instructions[index + 2];
                if (callInst.operation == LiftedOperation::CALL && callInst.operands[2].value.imm.n == 0)
                    return true;
            }
        }

        int reg = inst.operands[0].value.reg;
        const auto *callInfoInst = &inst;
        if (inst.operation == LiftedOperation::NAMECALL) {
            if (static_cast<size_t>(index) + 2 < func->lpLiftedFunction->instructions.size())
                callInfoInst = &func->lpLiftedFunction->instructions[index + 2];
            else
                return false;
        }

        int nResults = callInfoInst->operands[2].value.imm.n;
        if (nResults == 2) {
            int definedVersion = -1;
            for (const auto &[ref, instrPtr] : func->definitionMap) {
                if (instrPtr == callInfoInst && ref.regIndex == reg) {
                    definedVersion = ref.version;
                    break;
                }
            }

            if (definedVersion != -1) {
                if (func->useCounts.contains({reg, definedVersion}) && func->useCounts.at({reg, definedVersion}) == 1)
                    return true;
            }
        }
        return false;
    }

    std::shared_ptr<Expression> LiftCallLikeInstruction(const AnalyzedFunction *func, int32_t index, bool isNested = false);

    std::shared_ptr<Expression> LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand);
    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const AnalyzedFunction *func, const BasicBlock &block);
    std::vector<std::shared_ptr<Statement>> LiftTree(AnalyzedFunction *func, uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    ASTFunction LiftFunctionInternal(AnalyzedFunction *analyzedFunction);

    AnalyzedFunction *m_lpRootFunction = nullptr;

  public:
    ASTFunction LiftFunction(AnalyzedFunction *analyzedFunction);
};