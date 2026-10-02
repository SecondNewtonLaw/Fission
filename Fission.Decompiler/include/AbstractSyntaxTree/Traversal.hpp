//
// Created by Dottik on 1/10/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <memory>
#include <vector>

// Calls `visit(body)` for each statement block nested directly in `statement`; function bodies are not blocks of the enclosing scope.
template <typename Visit> void ForEachChildBlock(const std::shared_ptr<Statement> &statement, Visit &&visit) {
    if (const auto branch = std::dynamic_pointer_cast<IfStatementNode>(statement)) {
        if (branch->thenBranch)
            visit(branch->thenBranch->body);
        if (branch->elseBranch)
            visit(branch->elseBranch->body);
    } else if (const auto loop = std::dynamic_pointer_cast<WhileStatementNode>(statement); loop && loop->body) {
        visit(loop->body->body);
    } else if (const auto repeat = std::dynamic_pointer_cast<RepeatStatementNode>(statement); repeat && repeat->body) {
        visit(repeat->body->body);
    } else if (const auto numeric = std::dynamic_pointer_cast<ForNumericNode>(statement); numeric && numeric->lpLoopBody) {
        visit(numeric->lpLoopBody->body);
    } else if (const auto generic = std::dynamic_pointer_cast<ForGeneralNode>(statement); generic && generic->body) {
        visit(generic->body->body);
    } else if (const auto block = std::dynamic_pointer_cast<BlockStatementNode>(statement)) {
        visit(block->body);
    }
}

// Calls `visit(expression)` for each expression slot `statement` owns, nested blocks excluded; a repeat's condition comes last.
template <typename Visit> void ForEachStatementExpression(const std::shared_ptr<Statement> &statement, Visit &&visit) {
    if (const auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(statement)) {
        visit(declaration->identifier);
        visit(declaration->value);
    } else if (const auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(statement)) {
        visit(assignment->left);
        visit(assignment->right);
    } else if (const auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(statement)) {
        visit(compound->left);
        visit(compound->right);
    } else if (const auto expression = std::dynamic_pointer_cast<ExpressionStatementNode>(statement)) {
        visit(expression->expression);
    } else if (const auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(statement)) {
        for (auto &value : ret->returnValues)
            visit(value);
    } else if (const auto branch = std::dynamic_pointer_cast<IfStatementNode>(statement)) {
        visit(branch->condition);
    } else if (const auto loop = std::dynamic_pointer_cast<WhileStatementNode>(statement)) {
        visit(loop->condition);
    } else if (const auto repeat = std::dynamic_pointer_cast<RepeatStatementNode>(statement)) {
        visit(repeat->condition);
    } else if (const auto numeric = std::dynamic_pointer_cast<ForNumericNode>(statement)) {
        visit(numeric->startVariable);
        visit(numeric->maxIncreased);
        visit(numeric->increaseBy);
    } else if (const auto generic = std::dynamic_pointer_cast<ForGeneralNode>(statement)) {
        visit(generic->generator);
        visit(generic->state);
        visit(generic->index);
    }
}

// Calls `visit(child)` for each operand of `expression`; a closure's body is statements and a method's name is not an operand.
template <typename Visit> void ForEachSubExpression(const std::shared_ptr<Expression> &expression, Visit &&visit) {
    if (const auto call = std::dynamic_pointer_cast<CallExpressionNode>(expression)) {
        visit(call->callee);
        for (auto &argument : call->arguments)
            visit(argument);
        for (auto &ret : call->rets)
            visit(ret);
    } else if (const auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expression)) {
        visit(nameCall->calledOn);
        for (auto &argument : nameCall->arguments)
            visit(argument);
        for (auto &ret : nameCall->rets)
            visit(ret);
    } else if (const auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expression)) {
        visit(binary->left);
        visit(binary->right);
    } else if (const auto compound = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expression)) {
        visit(compound->left);
        visit(compound->right);
    } else if (const auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expression)) {
        visit(index->left);
        visit(index->right);
    } else if (const auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expression)) {
        visit(member->table);
        visit(member->key);
    } else if (const auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expression)) {
        visit(unary->operand);
    } else if (const auto table = std::dynamic_pointer_cast<TableLiteralNode>(expression)) {
        for (auto &element : table->expressions)
            visit(element);
    } else if (const auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(expression)) {
        visit(conditional->condition);
        visit(conditional->thenExpr);
        visit(conditional->elseExpr);
    }
}

inline bool IsLoopStatement(const std::shared_ptr<Statement> &statement) {
    return std::dynamic_pointer_cast<WhileStatementNode>(statement) || std::dynamic_pointer_cast<RepeatStatementNode>(statement) ||
           std::dynamic_pointer_cast<ForNumericNode>(statement) || std::dynamic_pointer_cast<ForGeneralNode>(statement);
}
