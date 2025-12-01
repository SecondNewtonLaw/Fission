//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

class CommentNode final : public Expression {
  public:
    std::string comment;
    CommentNode(const std::string &commentContent) : comment(commentContent) {}
    void Accept(Visitor *visitor) override {
        visitor->Visit(this);
    }
};