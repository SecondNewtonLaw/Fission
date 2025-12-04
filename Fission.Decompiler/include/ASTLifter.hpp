//
// Created by Pixeluted on 04/12/2025.
//
#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"

#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // Not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

    std::vector<ASTFunction> subFunctions;
};

class ASTLifter {
    std::shared_ptr<Expression> LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand);
    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const AnalyzedFunction *func, const BasicBlock &block);
    std::vector<std::shared_ptr<Statement>> LiftTree(AnalyzedFunction *func, uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    ASTFunction LiftFunctionInternal(AnalyzedFunction *analyzedFunction);

    AnalyzedFunction *m_lpRootFunction =nullptr;

  public:
    ASTFunction LiftFunction(AnalyzedFunction *analyzedFunction);
};