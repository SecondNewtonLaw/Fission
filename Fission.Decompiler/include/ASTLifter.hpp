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

    std::shared_ptr<Expression> LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand);
    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const AnalyzedFunction *func, const BasicBlock &block);
    std::vector<std::shared_ptr<Statement>> LiftTree(AnalyzedFunction *func, uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    ASTFunction LiftFunctionInternal(AnalyzedFunction *analyzedFunction);

    AnalyzedFunction *m_lpRootFunction = nullptr;

  public:
    ASTFunction LiftFunction(AnalyzedFunction *analyzedFunction);
};