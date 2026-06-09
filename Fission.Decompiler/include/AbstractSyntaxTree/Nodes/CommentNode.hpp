//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"

class CommentNode final : public Expression {
  public:
    std::string comment;
    bool bNewLine = true;
    // Fission info note (suppressed by OmitFissionComments). false by default so warnings always emit.
    bool bIsInformational = false;
    CommentNode(const std::string &commentContent, bool newline, bool informational = false)
        : comment(commentContent), bNewLine(newline), bIsInformational(informational) {}
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};