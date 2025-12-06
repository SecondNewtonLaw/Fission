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
            return std::format("i");
        }

        if (ver != -1)
            return std::format("v{}", reg);

        return std::format("v{}", reg);
    }

    bool IsInstructionConsumed(const AnalyzedFunction *func, int index) {
        const auto &inst = func->lpLiftedFunction->instructions[index];

        if (inst.operation == LiftedOperation::CALL) {
            if (inst.operands[2].value.imm.n == 0)
                return true;
        } else if (inst.operation == LiftedOperation::NAMECALL) {
            if (static_cast<size_t>(index) + 2 < func->lpLiftedFunction->instructions.size()) {
                const auto &call = func->lpLiftedFunction->instructions[index + 2];
                if (call.operation == LiftedOperation::CALL && call.operands[2].value.imm.n == 0)
                    return true;
            }
        }

        if (inst.operation == LiftedOperation::CALL || inst.operation == LiftedOperation::NAMECALL) {
            const auto *callInfo = &inst;
            if (inst.operation == LiftedOperation::NAMECALL) {
                if (static_cast<size_t>(index) + 2 < func->lpLiftedFunction->instructions.size())
                    callInfo = &func->lpLiftedFunction->instructions[index + 2];
                else
                    return false;
            }

            if (callInfo->operands[2].value.imm.n == 2) {
                int reg = inst.operands[0].value.reg;
                for (const auto &[ref, instrPtr] : func->definitionMap) {
                    if (instrPtr->instructionIndex == callInfo->instructionIndex && ref.regIndex == reg) {
                        if (func->useCounts.contains({reg, ref.version}) && func->useCounts.at({reg, ref.version}) == 1)
                            return true;
                        break;
                    }
                }
            }
            return false;
        }

        switch (inst.operation) {
        case LiftedOperation::ADD:
        case LiftedOperation::SUB:
        case LiftedOperation::MUL:
        case LiftedOperation::DIV:
        case LiftedOperation::MOD:
        case LiftedOperation::OR:
        case LiftedOperation::POW:
        case LiftedOperation::LOAD:
        case LiftedOperation::AND:
        case LiftedOperation::NOT:
        case LiftedOperation::MINUS: {
            int reg = inst.operands[0].value.reg;

            int definedVersion = inst.operands[0].ssaVersion;

            if (definedVersion <= 1)
                return false; // first version is initialization, we need it as a variable declaration.

            if (inst.operands[0].value.reg == inst.operands[1].value.reg)
                return false; // instruction self modifies register, cannot be consumed.

            if (func->useCounts.contains({reg, definedVersion}) && func->useCounts.at({reg, definedVersion}) > 0)
                return true;

            if (this->bLoopEnter)
                return true;

            return false;
        }
        case LiftedOperation::LENGTH:
        case LiftedOperation::ADDK:
        case LiftedOperation::SUBK:
        case LiftedOperation::MULK:
        case LiftedOperation::DIVK:
        case LiftedOperation::MODK:
        case LiftedOperation::POWK:
        case LiftedOperation::ANDK:
        case LiftedOperation::ORK:
        case LiftedOperation::CONCAT:
        case LiftedOperation::GETTABLE:
        case LiftedOperation::GETTABLEKS:
        case LiftedOperation::GETTABLEN:
        case LiftedOperation::GETGLOBAL:
        case LiftedOperation::GETIMPORT:
        case LiftedOperation::GETUPVAL:
        case LiftedOperation::MOVE: {
            int reg = inst.operands[0].value.reg;

            int definedVersion = inst.operands[0].ssaVersion;

            if (definedVersion != -1) {
                if (func->useCounts.contains({reg, definedVersion}) && func->useCounts.at({reg, definedVersion}) > 0) {
                    return true;
                }
            }
            return false;
        }
        default:
            break;
        }

        return false;
    }

    std::shared_ptr<Expression> LiftCallLikeInstruction(const AnalyzedFunction *func, int32_t index, bool isNested = false);

    std::shared_ptr<Expression> LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand, bool forceExpression = false);
    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const AnalyzedFunction *func, const BasicBlock &block);
    std::vector<std::shared_ptr<Statement>> LiftTree(AnalyzedFunction *func, uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    ASTFunction LiftFunctionInternal(AnalyzedFunction *analyzedFunction);

    AnalyzedFunction *m_lpRootFunction = nullptr;

  public:
    ASTFunction LiftFunction(AnalyzedFunction *analyzedFunction);
};