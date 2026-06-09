//
// Created by Dottik on 1/12/2025.
//

#pragma once

class VectorNode;
class ExpressionStatementNode;
class ReturnStatementNode;
class MemberExpressionNode;
class IndexExpressionNode;
class UnaryExpressionNode;
class CallExpressionNode;
class RootNode;
class Identifier;
class FunctionDeclarationNode;
class CommentNode;
class BreakStatementNode;
class ContinueStatementNode;
class BlockStatementNode;
class WhileStatementNode;
class IfStatementNode;
class AssignmentStatementNode;
class BinaryExpressionNode;
class StringLiteralNode;
class NumberLiteralNode;
class BooleanLiteralNode;
class IdentifierExpressionNode;
class VariableDeclarationNode;
class NilLiteralNode;
class TableLiteralNode;
class NoExpressionNode;
class NameCallExpressionNode;
class ForNumericNode;
class ForGeneralNode;
class CompoundBinaryExpressionNode;
class RepeatStatementNode;
class VarArgExpression;
class FunctionArgumentExpression;
class TableBinaryExpressionNode;
class IntegerLiteralNode;

class Visitor {
  public:
    virtual void Visit(IntegerLiteralNode *lpNode) = 0;
    virtual void Visit(RootNode *lpNode) = 0;
    virtual void Visit(Identifier *lpNode) = 0;
    virtual void Visit(FunctionDeclarationNode *lpNode) = 0;
    virtual void Visit(CommentNode *lpNode) = 0;
    virtual void Visit(CallExpressionNode *lpNode) = 0;
    virtual void Visit(UnaryExpressionNode *lpNode) = 0;
    virtual void Visit(IndexExpressionNode *lpNode) = 0;
    virtual void Visit(MemberExpressionNode *lpNode) = 0;
    virtual void Visit(ReturnStatementNode *lpNode) = 0;
    virtual void Visit(ExpressionStatementNode *lpNode) = 0;
    virtual void Visit(BreakStatementNode *lpNode) = 0;
    virtual void Visit(ContinueStatementNode *lpNode) = 0;
    virtual void Visit(BlockStatementNode *lpNode) = 0;
    virtual void Visit(WhileStatementNode *lpNode) = 0;
    virtual void Visit(IfStatementNode *lpNode) = 0;
    virtual void Visit(AssignmentStatementNode *lpNode) = 0;
    virtual void Visit(TableBinaryExpressionNode *lpNode) = 0;
    virtual void Visit(BinaryExpressionNode *lpNode) = 0;
    virtual void Visit(StringLiteralNode *lpNode) = 0;
    virtual void Visit(NumberLiteralNode *lpNode) = 0;
    virtual void Visit(BooleanLiteralNode *lpNode) = 0;
    virtual void Visit(IdentifierExpressionNode *lpNode) = 0;
    virtual void Visit(VariableDeclarationNode *lpNode) = 0;
    virtual void Visit(NilLiteralNode *lpNode) = 0;
    virtual void Visit(TableLiteralNode *lpNode) = 0;
    virtual void Visit(NoExpressionNode *lpNode) = 0;
    virtual void Visit(NameCallExpressionNode *lpNode) = 0;
    virtual void Visit(ForNumericNode *lpNode) = 0;
    virtual void Visit(ForGeneralNode *lpNode) = 0;
    virtual void Visit(CompoundBinaryExpressionNode *lpNode) = 0;
    virtual void Visit(RepeatStatementNode *lpNode) = 0;
    virtual void Visit(VarArgExpression *lpNode) = 0;
    virtual void Visit(FunctionArgumentExpression *lpNode) = 0;
    virtual void Visit(VectorNode* lpNode) = 0;
};