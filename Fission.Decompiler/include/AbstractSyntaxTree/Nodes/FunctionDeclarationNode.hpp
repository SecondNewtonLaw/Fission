//
// Created by Dottik on 1/12/2025.
//
#pragma once
#include "../ASTNode.hpp"
#include <memory>
#include <vector>

class FunctionDeclarationNode final : public Declaration {
  public:
    std::shared_ptr<Identifier> name;
    std::shared_ptr<BlockStatementNode> body;

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
    ~FunctionDeclarationNode() override = default;
};