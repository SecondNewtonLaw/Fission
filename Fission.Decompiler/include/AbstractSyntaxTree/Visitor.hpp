//
// Created by Dottik on 1/12/2025.
//

#pragma once

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

class Visitor {
  public:
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
};