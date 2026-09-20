// Generate Fission ASTs and render them through SourceGenerator.
#pragma once

#include "AbstractSyntaxTree/ASTNode.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class FissionAstGenerator {
  public:
    explicit FissionAstGenerator(uint32_t seed) : m_state(seed ? seed : 0x9e3779b9u) {}

    std::string Generate();

  private:
    uint32_t m_state;
    std::vector<std::string> m_locals; // names already declared, for valid references

    uint32_t Next();
    int Rand(int n); // [0, n)
    bool Chance(int pct);

    std::string FreshLocal();
    std::string AnyName(); // a declared local or a benign global

    std::shared_ptr<Expression> GenExpr(int depth);
    std::shared_ptr<Expression> GenLeaf();
    std::shared_ptr<Expression> GenTable(int depth);
    std::shared_ptr<Expression> GenCall(int depth);
    std::shared_ptr<Statement> GenStatement(int depth);
    std::string GenExecutable();
};
