//
// Created by Dottik on 1/10/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

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
        auto copy = std::make_shared<TableBinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<BinaryExpressionNode>(expression)) {
        auto copy = std::make_shared<BinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expression)) {
        auto copy = std::make_shared<CompoundBinaryExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IfExpressionNode>(expression)) {
        auto copy = std::make_shared<IfExpressionNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        copy->thenExpr = CloneExpression(copy->thenExpr);
        copy->elseExpr = CloneExpression(copy->elseExpr);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<UnaryExpressionNode>(expression)) {
        auto copy = std::make_shared<UnaryExpressionNode>(*node);
        copy->operand = CloneExpression(copy->operand);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IndexExpressionNode>(expression)) {
        auto copy = std::make_shared<IndexExpressionNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<MemberExpressionNode>(expression)) {
        auto copy = std::make_shared<MemberExpressionNode>(*node);
        copy->table = CloneExpression(copy->table);
        copy->key = CloneExpression(copy->key);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IdentifierExpressionNode>(expression))
        return std::make_shared<IdentifierExpressionNode>(*node);
    if (const auto node = AsLiteral<TableLiteralNode>(expression)) {
        auto copy = std::make_shared<TableLiteralNode>(*node);
        cloneAll(copy->expressions);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<FunctionDeclarationNode>(expression)) {
        auto copy = std::make_shared<FunctionDeclarationNode>(*node);
        for (auto &[index, argument] : copy->argumentsNames)
            if (argument) {
                argument = std::make_shared<FunctionArgumentExpression>(*argument);
                argument->argumentName = CloneExpression(argument->argumentName);
                if (argument->type)
                    argument->type = CloneExpression(*argument->type);
            }
        copy->lpFunctionBody = std::static_pointer_cast<BlockStatementNode>(CloneStatement(copy->lpFunctionBody));
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<NameCallExpressionNode>(expression)) {
        auto copy = std::make_shared<NameCallExpressionNode>(*node);
        copy->calledOn = CloneExpression(copy->calledOn);
        copy->callWhat = CloneExpression(copy->callWhat);
        cloneAll(copy->arguments);
        cloneAll(copy->rets);
        cloneAll(copy->retTypes);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CallExpressionNode>(expression)) {
        auto copy = std::make_shared<CallExpressionNode>(*node);
        copy->callee = CloneExpression(copy->callee);
        cloneAll(copy->arguments);
        cloneAll(copy->rets);
        cloneAll(copy->retTypes);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<CommentNode>(expression))
        return std::make_shared<CommentNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<VarArgExpression>(expression))
        return std::make_shared<VarArgExpression>(*node);
    if (const auto node = AsLiteral<NilLiteralNode>(expression))
        return std::make_shared<NilLiteralNode>(*node);
    if (const auto node = AsLiteral<BooleanLiteralNode>(expression))
        return std::make_shared<BooleanLiteralNode>(*node);
    if (const auto node = AsLiteral<NumberLiteralNode>(expression))
        return std::make_shared<NumberLiteralNode>(*node);
    if (const auto node = AsLiteral<IntegerLiteralNode>(expression))
        return std::make_shared<IntegerLiteralNode>(*node);
    if (const auto node = AsLiteral<StringLiteralNode>(expression))
        return std::make_shared<StringLiteralNode>(*node);
    if (const auto node = AsLiteral<VectorNode>(expression))
        return std::make_shared<VectorNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<NoExpressionNode>(expression))
        return std::make_shared<NoExpressionNode>(*node);
    return expression;
}

inline std::shared_ptr<Statement> CloneStatement(const std::shared_ptr<Statement> &statement) {
    if (!statement)
        return nullptr;
    if (const auto expression = std::dynamic_pointer_cast<Expression>(statement))
        return CloneExpression(expression);
    const auto cloneBlock = [](std::shared_ptr<BlockStatementNode> &block) { block = std::static_pointer_cast<BlockStatementNode>(CloneStatement(block)); };
    if (const auto node = std::dynamic_pointer_cast<BlockStatementNode>(statement)) {
        auto copy = std::make_shared<BlockStatementNode>(*node);
        for (auto &child : copy->body)
            child = CloneStatement(child);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<VariableDeclarationNode>(statement)) {
        auto copy = std::make_shared<VariableDeclarationNode>(*node);
        copy->identifier = CloneExpression(copy->identifier);
        copy->value = CloneExpression(copy->value);
        if (copy->type)
            copy->type = CloneExpression(*copy->type);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<AssignmentStatementNode>(statement)) {
        auto copy = std::make_shared<AssignmentStatementNode>(*node);
        copy->left = CloneExpression(copy->left);
        copy->right = CloneExpression(copy->right);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ExpressionStatementNode>(statement)) {
        auto copy = std::make_shared<ExpressionStatementNode>(*node);
        copy->expression = CloneExpression(copy->expression);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ReturnStatementNode>(statement)) {
        auto copy = std::make_shared<ReturnStatementNode>(*node);
        for (auto &value : copy->returnValues)
            value = CloneExpression(value);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<IfStatementNode>(statement)) {
        auto copy = std::make_shared<IfStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->thenBranch);
        cloneBlock(copy->elseBranch);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<WhileStatementNode>(statement)) {
        auto copy = std::make_shared<WhileStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<RepeatStatementNode>(statement)) {
        auto copy = std::make_shared<RepeatStatementNode>(*node);
        copy->condition = CloneExpression(copy->condition);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ForNumericNode>(statement)) {
        auto copy = std::make_shared<ForNumericNode>(*node);
        copy->loopVariable = CloneExpression(copy->loopVariable);
        copy->startVariable = CloneExpression(copy->startVariable);
        copy->increaseBy = CloneExpression(copy->increaseBy);
        copy->maxIncreased = CloneExpression(copy->maxIncreased);
        cloneBlock(copy->lpLoopBody);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ForGeneralNode>(statement)) {
        auto copy = std::make_shared<ForGeneralNode>(*node);
        for (auto &variable : copy->loopVariables)
            variable = CloneExpression(variable);
        copy->generator = CloneExpression(copy->generator);
        copy->state = CloneExpression(copy->state);
        copy->index = CloneExpression(copy->index);
        cloneBlock(copy->body);
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<ClassDeclarationNode>(statement)) {
        auto copy = std::make_shared<ClassDeclarationNode>(*node);
        copy->superclass = CloneExpression(copy->superclass);
        for (auto &method : copy->methods)
            method = std::static_pointer_cast<FunctionDeclarationNode>(CloneExpression(method));
        return copy;
    }
    if (const auto node = std::dynamic_pointer_cast<BreakStatementNode>(statement))
        return std::make_shared<BreakStatementNode>(*node);
    if (const auto node = std::dynamic_pointer_cast<ContinueStatementNode>(statement))
        return std::make_shared<ContinueStatementNode>(*node);
    return statement;
}
