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
    std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond);
    explicit ASTLifter();

    ASTFunction Lift(AnalyzedFunction &analyzedFunction);
    std::shared_ptr<Expression> LiftCondition(const LiftedInstruction *inst);

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

    // Closures detected as single-use call-argument candidates. The NEWCLOSURE /
    // DUPCLOSURE handler builds a FunctionDeclarationNode marked
    // `bAnonymousInline = true`, parks it here keyed by the closure's SSA ref,
    // and skips pushing it as a top-level statement. LiftExpression substitutes
    // it in-place when the call argument lookup reaches the same SSA ref.
    std::unordered_map<SSARef, std::shared_ptr<FunctionDeclarationNode>> m_inlineableClosures;

    std::unordered_map<std::string, DeserializedFunction *> m_takenFunctionNames;
    int32_t m_dwLastFunctionIndex = 0;

  private:
    AnalyzedFunction *m_currentFunction = nullptr;

    std::vector<std::shared_ptr<Statement>> LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    std::string GetFunctionName(DeserializedFunction *lpDeserialized) {
        if (lpDeserialized->debugName.has_value())
            return std::format("{}", *lpDeserialized->debugName);

        if (this->m_dwLastFunctionIndex == 0)
            this->m_dwLastFunctionIndex = rand(); // randomize the starter value to prevent known value name duplication attacks.
        return std::format("anon_{}_{}", this->m_dwLastFunctionIndex++, lpDeserialized->bytecodeId);
    }

    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const BasicBlock &block);
    bool CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const std::set<uint32_t> &visitedScopes);
    std::shared_ptr<Expression> LiftExpression(const LiftedOperand &operand, bool forceExpression = false);
    std::shared_ptr<Expression> LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested);
    std::shared_ptr<TableLiteralNode> LiftTableLiteral(const LiftedInstruction &inst);
    bool ShouldInline(const LiftedInstruction *inst);
    std::string ResolveVariableName(const LiftedOperand &op);
    int32_t FindMergeBlock(uint32_t branchA, uint32_t branchB);
};