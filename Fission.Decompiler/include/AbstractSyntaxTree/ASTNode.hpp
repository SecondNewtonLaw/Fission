//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "Visitor.hpp"

#include <memory>
#include <string>
#include <vector>

class Visitor;
enum class ASTNodeKind {
    NilLiteral,
    BooleanLiteral,
    NumberLiteral,
    StringLiteral,
    VarargLiteral,

    /**
     *  @brief Represents a generic expression.
     ***/
    ExpressionStatement,
    /**
     *  @brief Represents a variable/ identifier.
     ***/
    Identifier,

    /**
     *  @brief Represents function expressions for anonymous functions (functions with no debugname)
     ***/
    FunctionExpression,
    /**
     *  @brief Represents a call expression.
     ***/
    CallExpression,
    /**
     *  @brief Represents a call to a method of an object/ table.
     ***/
    MethodCallExpression,
    /**
     *  @brief Represents binary expressions.
     ***/
    BinaryExpression,
    /**
     *  @brief Represents unary expressions.
     ***/
    UnaryExpression,
    /**
     *  @brief Represents indexing into a table with a known key that can be indexed via dot-indexing.
     ***/
    MemberExpression,
    /**
     *  @brief Represents indexing into a table with a key that is not known in the constants table or cannot be
     *  represented by dot-indexing.
     ***/
    IndexExpression,

    Unknown
};

class ASTNode {
  public:
    virtual ~ASTNode() = default;
    ASTNodeKind nodeKind = ASTNodeKind::Unknown;
    virtual void Accept(Visitor *visitor) { (void)visitor; }
};

class Statement : public ASTNode {
  public:
};
class Expression : public Statement {
  public:
};
class Declaration : public Statement {
  public:
};

class Identifier : public Declaration {
  public:
    std::string name{};
    explicit Identifier(std::string name) : name(std::move(name)) { this->nodeKind = ASTNodeKind::Identifier; }
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BlockStatementNode : public Statement {
  public:
    std::vector<std::shared_ptr<Statement>> body;
};

class AssignmentStatementNode : public Statement {
  public:
    std::shared_ptr<Identifier> left;
    std::shared_ptr<Expression> right;
    AssignmentStatementNode(const std::shared_ptr<Identifier> &l, const std::shared_ptr<Expression> &r) : left(l), right(r) {}
};

class IfStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> thenBranch;
    std::shared_ptr<BlockStatementNode> elseBranch;
};

class WhileStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> body;
};

class BinaryExpressionNode : public Expression {
  public:
    std::string op;
    std::shared_ptr<Expression> left, right;
    BinaryExpressionNode(const std::string &op, const std::shared_ptr<Expression> &left, const std::shared_ptr<Expression> &right)
        : op(op), left(left), right(right) {}
};

class IdentifierExpressionNode : public Expression {
  public:
    std::shared_ptr<Identifier> identifier;
    IdentifierExpressionNode(std::shared_ptr<Identifier> id) : identifier(id) {}
};

class NilLiteralNode : public Expression {
  public:
};

class BooleanLiteralNode : public Expression {
  public:
    bool value;
    BooleanLiteralNode(bool v) : value(v) {}
};

class NumberLiteralNode : public Expression {
  public:
    double value;
    NumberLiteralNode(double v) : value(v) {}
};

class StringLiteralNode : public Expression {
  public:
    std::string value;
    StringLiteralNode(std::string v) : value(v) {}
};
