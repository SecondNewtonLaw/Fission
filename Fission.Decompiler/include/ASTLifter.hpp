//
// Created by Dottik on 10/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "lua.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

    std::vector<ASTFunction> subFunctions;
};

class ASTLifter {
  public:
    explicit ASTLifter();

    ASTFunction Lift(AnalyzedFunction &analyzedFunction);

    std::unordered_set<int32_t> m_definedRegisters;
    std::unordered_set<int32_t> m_pinnedRegisters;

    std::unordered_set<int32_t> m_processedInstructions;

    struct PinnedRegisterScope {
        ASTLifter *m_lpLifter;
        int32_t dwReg;

        PinnedRegisterScope(ASTLifter *lifter, int32_t reg) : m_lpLifter(lifter), dwReg(reg) { m_lpLifter->m_pinnedRegisters.insert(reg); }

        ~PinnedRegisterScope() { m_lpLifter->m_pinnedRegisters.erase(dwReg); }

        PinnedRegisterScope(const PinnedRegisterScope &) = delete;
        PinnedRegisterScope &operator=(const PinnedRegisterScope &) = delete;
    };

    std::set<SSARef> m_phiConsumers;

  private:
    AnalyzedFunction *m_currentFunction = nullptr;

    std::vector<std::shared_ptr<Statement>> LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const BasicBlock &block);
    std::shared_ptr<Expression> LiftExpression(const LiftedOperand &operand, bool forceExpression = false);
    std::shared_ptr<Expression> LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested);
    std::shared_ptr<TableLiteralNode> LiftTableLiteral(const LiftedInstruction &inst);
    bool ShouldInline(const LiftedInstruction *inst);
    std::string ResolveVariableName(const LiftedOperand &op);
    int32_t FindMergeBlock(uint32_t branchA, uint32_t branchB);
};