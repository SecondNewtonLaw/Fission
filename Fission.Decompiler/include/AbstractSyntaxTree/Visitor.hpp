//
// Created by Dottik on 1/12/2025.
//

#pragma once

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
};