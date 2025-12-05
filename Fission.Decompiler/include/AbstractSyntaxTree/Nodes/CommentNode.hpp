//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

class CommentNode final : public Expression {
  public:
    std::string comment;
    bool bNewLine=true;
    CommentNode(const std::string &commentContent, bool newline) : comment(commentContent),bNewLine(newline) {}
    void Accept(Visitor *visitor) override {
        visitor->Visit(this);
    }
};