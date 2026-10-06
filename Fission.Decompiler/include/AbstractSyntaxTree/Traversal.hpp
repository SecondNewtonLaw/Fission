//
// Created by Dottik on 1/10/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

#include <memory>
#include <vector>

inline std::shared_ptr<Expression> AsExpression(const std::shared_ptr<Statement> &statement) {
    if (!statement)
        return {};
    switch (statement->nodeKind) {
    case ASTNodeKind::LiteralValue:
    case ASTNodeKind::FunctionDeclarationNode:
    case ASTNodeKind::IdentifierExpression:
    case ASTNodeKind::CallExpression:
    case ASTNodeKind::MethodCallExpression:
    case ASTNodeKind::BinaryExpression:
    case ASTNodeKind::IfExpression:
    case ASTNodeKind::UnaryExpression:
    case ASTNodeKind::MemberExpression:
    case ASTNodeKind::IndexExpression:
    case ASTNodeKind::CompoundAssignment:
    case ASTNodeKind::TableBinaryExpression:
    case ASTNodeKind::VarArgExpression:
    case ASTNodeKind::NoExpression:
    case ASTNodeKind::Comment:
        return std::static_pointer_cast<Expression>(statement);
    case ASTNodeKind::FunctionExpression:
    case ASTNodeKind::Unknown:
        return std::dynamic_pointer_cast<Expression>(statement);
    default:
        return {};
    }
}

// Calls `visit(body)` for each statement block nested directly in `statement`; function bodies are not blocks of the enclosing scope.
template <typename Visit> void ForEachChildBlock(const std::shared_ptr<Statement> &statement, Visit &&visit) {
    if (!statement)
        return;
    switch (statement->nodeKind) {
    case ASTNodeKind::IfStatement: {
        const auto branch = std::static_pointer_cast<IfStatementNode>(statement);
        if (branch->thenBranch)
            visit(branch->thenBranch->body);
        if (branch->elseBranch)
            visit(branch->elseBranch->body);
        break;
    }
    case ASTNodeKind::WhileStatement: {
        const auto loop = std::static_pointer_cast<WhileStatementNode>(statement);
        if (loop->body)
            visit(loop->body->body);
        break;
    }
    case ASTNodeKind::RepeatStatement: {
        const auto repeat = std::static_pointer_cast<RepeatStatementNode>(statement);
        if (repeat->body)
            visit(repeat->body->body);
        break;
    }
    case ASTNodeKind::ForNumeric: {
        const auto numeric = std::static_pointer_cast<ForNumericNode>(statement);
        if (numeric->lpLoopBody)
            visit(numeric->lpLoopBody->body);
        break;
    }
    case ASTNodeKind::ForGeneral: {
        const auto generic = std::static_pointer_cast<ForGeneralNode>(statement);
        if (generic->body)
            visit(generic->body->body);
        break;
    }
    case ASTNodeKind::BlockStatement:
        visit(std::static_pointer_cast<BlockStatementNode>(statement)->body);
        break;
    default:
        break;
    }
}

// Calls `visit(expression)` for each expression slot `statement` owns, nested blocks excluded; a repeat's condition comes last.
template <typename Visit> void ForEachStatementExpression(const std::shared_ptr<Statement> &statement, Visit &&visit) {
    if (!statement)
        return;
    switch (statement->nodeKind) {
    case ASTNodeKind::VariableDeclaration: {
        const auto declaration = std::static_pointer_cast<VariableDeclarationNode>(statement);
        visit(declaration->identifier);
        visit(declaration->value);
        break;
    }
    case ASTNodeKind::AssignmentStatement: {
        const auto assignment = std::static_pointer_cast<AssignmentStatementNode>(statement);
        visit(assignment->left);
        visit(assignment->right);
        break;
    }
    case ASTNodeKind::CompoundAssignment: {
        const auto compound = std::static_pointer_cast<CompoundBinaryExpressionNode>(statement);
        visit(compound->left);
        visit(compound->right);
        break;
    }
    case ASTNodeKind::ExpressionStatement:
        visit(std::static_pointer_cast<ExpressionStatementNode>(statement)->expression);
        break;
    case ASTNodeKind::ReturnExpression: {
        const auto ret = std::static_pointer_cast<ReturnStatementNode>(statement);
        for (auto &value : ret->returnValues)
            visit(value);
        break;
    }
    case ASTNodeKind::IfStatement:
        visit(std::static_pointer_cast<IfStatementNode>(statement)->condition);
        break;
    case ASTNodeKind::WhileStatement:
        visit(std::static_pointer_cast<WhileStatementNode>(statement)->condition);
        break;
    case ASTNodeKind::RepeatStatement:
        visit(std::static_pointer_cast<RepeatStatementNode>(statement)->condition);
        break;
    case ASTNodeKind::ForNumeric: {
        const auto numeric = std::static_pointer_cast<ForNumericNode>(statement);
        visit(numeric->startVariable);
        visit(numeric->maxIncreased);
        visit(numeric->increaseBy);
        break;
    }
    case ASTNodeKind::ForGeneral: {
        const auto generic = std::static_pointer_cast<ForGeneralNode>(statement);
        visit(generic->generator);
        visit(generic->state);
        visit(generic->index);
        break;
    }
    default:
        break;
    }
}

template <typename Call, typename Visit> void ForEachCallArgumentAndResult(Call &call, Visit &visit) {
    for (auto &argument : call.arguments)
        visit(argument);
    for (auto &ret : call.rets)
        visit(ret);
}

template <typename Visit> void VisitExpressionPair(std::shared_ptr<Expression> &left, std::shared_ptr<Expression> &right, Visit &visit) {
    visit(left);
    visit(right);
}

// Calls `visit(child)` for each operand of `expression`; a closure's body is statements and a method's name is not an operand.
template <typename Visit> void ForEachSubExpression(const std::shared_ptr<Expression> &expression, Visit &&visit) {
    if (!expression)
        return;
    switch (expression->nodeKind) {
    case ASTNodeKind::CallExpression: {
        const auto call = std::static_pointer_cast<CallExpressionNode>(expression);
        visit(call->callee);
        ForEachCallArgumentAndResult(*call, visit);
        break;
    }
    case ASTNodeKind::MethodCallExpression: {
        const auto nameCall = std::static_pointer_cast<NameCallExpressionNode>(expression);
        visit(nameCall->calledOn);
        ForEachCallArgumentAndResult(*nameCall, visit);
        break;
    }
    case ASTNodeKind::BinaryExpression:
    case ASTNodeKind::TableBinaryExpression: {
        const auto binary = std::static_pointer_cast<BinaryExpressionNode>(expression);
        VisitExpressionPair(binary->left, binary->right, visit);
        break;
    }
    case ASTNodeKind::CompoundAssignment: {
        const auto compound = std::static_pointer_cast<CompoundBinaryExpressionNode>(expression);
        VisitExpressionPair(compound->left, compound->right, visit);
        break;
    }
    case ASTNodeKind::IndexExpression: {
        const auto index = std::static_pointer_cast<IndexExpressionNode>(expression);
        VisitExpressionPair(index->left, index->right, visit);
        break;
    }
    case ASTNodeKind::MemberExpression: {
        const auto member = std::static_pointer_cast<MemberExpressionNode>(expression);
        VisitExpressionPair(member->table, member->key, visit);
        break;
    }
    case ASTNodeKind::UnaryExpression:
        visit(std::static_pointer_cast<UnaryExpressionNode>(expression)->operand);
        break;
    case ASTNodeKind::LiteralValue:
        if (std::static_pointer_cast<LiteralNode>(expression)->bIsTableLiteral) {
            const auto table = std::static_pointer_cast<TableLiteralNode>(expression);
            for (auto &element : table->expressions)
                visit(element);
        }
        break;
    case ASTNodeKind::IfExpression: {
        const auto conditional = std::static_pointer_cast<IfExpressionNode>(expression);
        visit(conditional->condition);
        visit(conditional->thenExpr);
        visit(conditional->elseExpr);
        break;
    }
    default:
        break;
    }
}

inline bool IsLoopStatement(const std::shared_ptr<Statement> &statement) {
    return statement && (statement->nodeKind == ASTNodeKind::WhileStatement || statement->nodeKind == ASTNodeKind::RepeatStatement ||
                         statement->nodeKind == ASTNodeKind::ForNumeric || statement->nodeKind == ASTNodeKind::ForGeneral);
}
