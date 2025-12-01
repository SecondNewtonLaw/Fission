//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "../ASTNode.hpp"
#include "AbstractSyntaxTree/Visitor.hpp"

#include <memory>
#include <vector>

class RootNode final : public ASTNode {
  public:
    std::vector<std::shared_ptr<Statement>> programBody{};
    explicit RootNode() = default;
    explicit RootNode(const std::vector<std::shared_ptr<Statement>> &programBody) { this->programBody = programBody; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};
