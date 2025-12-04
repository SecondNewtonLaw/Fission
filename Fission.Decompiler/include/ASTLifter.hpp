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
    ASTFunction LiftFunctionInternal(AnalyzedFunction *analyzedFunction);

  public:
    ASTFunction LiftFunction(AnalyzedFunction *analyzedFunction);
};