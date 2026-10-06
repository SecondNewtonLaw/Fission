//
// Created by Dottik on 1/10/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "FissionAllocator.hpp"

#include <memory>
#include <vector>

std::shared_ptr<Statement> CloneStatement(const std::shared_ptr<Statement> &statement);

// Deep copy; Identifier objects stay shared so a rename still reaches every reference.
inline std::shared_ptr<Expression> CloneExpression(const std::shared_ptr<Expression> &expression) {
    if (!expression)
        return nullptr;
    const auto cloneAll = [](std::vector<std::shared_ptr<Expression>> &expressions) {
        for (auto &item : expressions)
            item = CloneExpression(item);
    };
    if (const auto node = std::dynamic_pointer_cast<TableBinaryExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<TableBinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<BinaryExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<BinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<CompoundBinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IfExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<IfExpressionNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        copy->thenExpr = CloneExpression(copy->thenExpr);
        copy->elseExpr = CloneExpression(copy->elseExpr);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<UnaryExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<UnaryExpressionNode>(*node);
        copy->operand = CloneExpression(copy->operand);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IndexExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<IndexExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<MemberExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<MemberExpressionNode>(*node);
        copy->table = CloneExpression(copy->table);
        copy->key = CloneExpression(copy->key);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IdentifierExpressionNode>(expression))
        return Fission::MakeShared<IdentifierExpressionNode>(*node);
    if (const auto node = AsLiteral<TableLiteralNode>(expression)) {
        auto copy = Fission::MakeShared<TableLiteralNode>(*node);
        cloneAll(copy->expressions);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<FunctionDeclarationNode>(expression)) {
        auto copy = Fission::MakeShared<FunctionDeclarationNode>(*node);
        for (auto &[index, argument] : copy->argumentsNames)
            if (argument) {
                argument = Fission::MakeShared<FunctionArgumentExpression>(*argument);
                argument->argumentName = CloneExpression(argument->argumentName);
                if (argument->type)
                    argument->type = CloneExpression(*argument->type);
            }
        copy->lpFunctionBody = std::static_pointer_cast<BlockStatementNode>(CloneStatement(copy->lpFunctionBody));
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<NameCallExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<NameCallExpressionNode>(*node);
        copy->calledOn = CloneExpression(copy->calledOn);
        copy->callWhat = CloneExpression(copy->callWhat);
        cloneAll(copy->arguments);
        cloneAll(copy->rets);
        cloneAll(copy->retTypes);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CallExpressionNode>(expression)) {
        auto copy = Fission::MakeShared<CallExpressionNode>(*node);
        copy->callee = CloneExpression(copy->callee);
        cloneAll(copy->arguments);
        cloneAll(copy->rets);
        cloneAll(copy->retTypes);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CommentNode>(expression))
        return Fission::MakeShared<CommentNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<VarArgExpression>(expression))
        return Fission::MakeShared<VarArgExpression>(*node);
    if (const auto node = AsLiteral<NilLiteralNode>(expression))
        return Fission::MakeShared<NilLiteralNode>(*node);
    if (const auto node = AsLiteral<BooleanLiteralNode>(expression))
        return Fission::MakeShared<BooleanLiteralNode>(*node);
    if (const auto node = AsLiteral<NumberLiteralNode>(expression))
        return Fission::MakeShared<NumberLiteralNode>(*node);
    if (const auto node = AsLiteral<IntegerLiteralNode>(expression))
        return Fission::MakeShared<IntegerLiteralNode>(*node);
    if (const auto node = AsLiteral<StringLiteralNode>(expression))
        return Fission::MakeShared<StringLiteralNode>(*node);
    if (const auto node = AsLiteral<VectorNode>(expression))
        return Fission::MakeShared<VectorNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<NoExpressionNode>(expression))
        return Fission::MakeShared<NoExpressionNode>(*node);
    return expression;
}

inline std::shared_ptr<Statement> CloneStatement(const std::shared_ptr<Statement> &statement) {
    if (!statement)
        return nullptr;
    if (const auto expression = std::dynamic_pointer_cast<Expression>(statement))
        return CloneExpression(expression);
    const auto cloneBlock = [](std::shared_ptr<BlockStatementNode> &block) { block = std::static_pointer_cast<BlockStatementNode>(CloneStatement(block)); };
    if (const auto node = std::dynamic_pointer_cast<BlockStatementNode>(statement)) {
        auto copy = Fission::MakeShared<BlockStatementNode>(*node);
        for (auto &child : copy->body)
            child = CloneStatement(child);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<VariableDeclarationNode>(statement)) {
        auto copy = Fission::MakeShared<VariableDeclarationNode>(*node);
        copy->identifier = CloneExpression(copy->identifier);
        copy->value = CloneExpression(copy->value);
        if (copy->type)
            copy->type = CloneExpression(*copy->type);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<AssignmentStatementNode>(statement)) {
        auto copy = Fission::MakeShared<AssignmentStatementNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ExpressionStatementNode>(statement)) {
        auto copy = Fission::MakeShared<ExpressionStatementNode>(*node);
        copy->expression = CloneExpression(copy->expression);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ReturnStatementNode>(statement)) {
        auto copy = Fission::MakeShared<ReturnStatementNode>(*node);
        for (auto &value : copy->returnValues)
            value = CloneExpression(value);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IfStatementNode>(statement)) {
        auto copy = Fission::MakeShared<IfStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->thenBranch);
        cloneBlock(copy->elseBranch);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<WhileStatementNode>(statement)) {
        auto copy = Fission::MakeShared<WhileStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<RepeatStatementNode>(statement)) {
        auto copy = Fission::MakeShared<RepeatStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ForNumericNode>(statement)) {
        auto copy = Fission::MakeShared<ForNumericNode>(*node);
        copy->loopVariable = CloneExpression(copy->loopVariable);
        copy->startVariable = CloneExpression(copy->startVariable);
        copy->increaseBy = CloneExpression(copy->increaseBy);
        copy->maxIncreased = CloneExpression(copy->maxIncreased);
        cloneBlock(copy->lpLoopBody);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ForGeneralNode>(statement)) {
        auto copy = Fission::MakeShared<ForGeneralNode>(*node);
        for (auto &variable : copy->loopVariables)
            variable = CloneExpression(variable);
        copy->generator = CloneExpression(copy->generator);
        copy->state = CloneExpression(copy->state);
        copy->index = CloneExpression(copy->index);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ClassDeclarationNode>(statement)) {
        auto copy = Fission::MakeShared<ClassDeclarationNode>(*node);
        copy->superclass = CloneExpression(copy->superclass);
        for (auto &method : copy->methods)
            method = std::static_pointer_cast<FunctionDeclarationNode>(CloneExpression(method));
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<BreakStatementNode>(statement))
        return Fission::MakeShared<BreakStatementNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<ContinueStatementNode>(statement))
        return Fission::MakeShared<ContinueStatementNode>(*node);
    return statement;
}
