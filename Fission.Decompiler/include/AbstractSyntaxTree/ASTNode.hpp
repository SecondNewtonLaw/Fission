//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "Visitor.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Visitor;
enum class ASTNodeKind {
    NilLiteral,
    BooleanLiteral,
    NumberLiteral,
    StringLiteral,
    VarargLiteral,
    FunctionDeclarationNode,

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
    /**
     * @brief Represents a index expression with stuff to return
     */
    ReturnExpression,

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
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class AssignmentStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;
    AssignmentStatementNode(const std::shared_ptr<Expression> &l, const std::shared_ptr<Expression> &r) : left(l), right(r) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IfStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> thenBranch;
    std::shared_ptr<BlockStatementNode> elseBranch;

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class WhileStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> body;

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BinaryExpressionNode : public Expression {
  public:
    std::string op;
    std::shared_ptr<Expression> left, right;
    BinaryExpressionNode(const std::string &op, const std::shared_ptr<Expression> &left, const std::shared_ptr<Expression> &right)
        : op(op), left(left), right(right) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class UnaryExpressionNode : public Expression {
  public:
    std::string op;
    std::shared_ptr<Expression> operand;

    UnaryExpressionNode(std::string op, std::shared_ptr<Expression> operand) : op(std::move(op)), operand(std::move(operand)) {
        this->nodeKind = ASTNodeKind::UnaryExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IndexExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> left, right;

    IndexExpressionNode(std::shared_ptr<Expression> left, std::shared_ptr<Expression> right) : left(std::move(left)), right(std::move(right)) {
        this->nodeKind = ASTNodeKind::IndexExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class MemberExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> table;
    std::string keyName;

    MemberExpressionNode(std::shared_ptr<Expression> table, std::string keyName) : table(std::move(table)), keyName(std::move(keyName)) {
        this->nodeKind = ASTNodeKind::MemberExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IdentifierExpressionNode : public Expression {
  public:
    std::shared_ptr<Identifier> identifier;
    IdentifierExpressionNode(std::shared_ptr<Identifier> id) : identifier(id) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class VariableDeclarationNode : public Declaration {
  public:
    std::shared_ptr<Expression> identifier;
    std::shared_ptr<Expression> value;
    VariableDeclarationNode(std::shared_ptr<Identifier> identifier) : identifier(std::make_shared<IdentifierExpressionNode>(identifier)), value(nullptr) {}
    VariableDeclarationNode(std::shared_ptr<Expression> identifier, std::shared_ptr<Expression> expr) : identifier(identifier), value(expr) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NilLiteralNode : public Expression {
  public:
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BooleanLiteralNode : public Expression {
  public:
    bool value;
    BooleanLiteralNode(bool v) : value(v) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NumberLiteralNode : public Expression {
  public:
    double value;
    NumberLiteralNode(double v) : value(v) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class StringLiteralNode : public Expression {
  public:
    std::string value;
    StringLiteralNode(std::string v) : value(v) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class TableLiteralNode : public Expression {
  public:
    std::vector<std::shared_ptr<Expression>> expressions;
    TableLiteralNode() : expressions() {}
    TableLiteralNode(const std::vector<std::shared_ptr<Expression>> &expressions) : expressions(expressions) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class FunctionDeclarationNode : public Expression {
  public:
    std::string functionName;
    int32_t argumentCount = 0;
    std::unordered_map<int32_t, std::string> argumentsNames{}; // arg1 -> it's name inside syntax
    bool bIsVarArg = false;
    bool bIsLocalDeclaration = false; // to be defined in lifter. If the only usage of this is inside of a function, and such function holds no debug name.
    std::shared_ptr<BlockStatementNode> lpFunctionBody = nullptr;

    FunctionDeclarationNode(
        std::string functionName, const int32_t argumentCount, std::unordered_map<int32_t, std::string> names, bool isVarArg,
        std::shared_ptr<BlockStatementNode> funcBody, bool bIsLocalDeclaration
    )
        : functionName(std::move(functionName)), argumentCount(argumentCount), argumentsNames(std::move(names)), bIsVarArg(isVarArg),
          bIsLocalDeclaration(bIsLocalDeclaration), lpFunctionBody(std::move(funcBody)) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NoExpressionNode : public Expression {
  public:
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NameCallExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> calledOn;
    std::shared_ptr<Expression> callWhat;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Expression>> rets;
    bool bIsVariadicCall;

    NameCallExpressionNode(
        std::shared_ptr<Expression> calledOn, std::shared_ptr<Expression> calledWhat, std::vector<std::shared_ptr<Expression>> args,
        std::vector<std::shared_ptr<Expression>> rets, bool bIsVariadicCall
    )
        : calledOn(std::move(calledOn)), callWhat(std::move(calledWhat)), arguments(std::move(args)), rets(std::move(rets)), bIsVariadicCall(bIsVariadicCall) {
        this->nodeKind = ASTNodeKind::CallExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class CallExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> callee;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Expression>> rets;
    bool bIsVariadicCall;

    CallExpressionNode(
        std::shared_ptr<Expression> func, std::vector<std::shared_ptr<Expression>> args, std::vector<std::shared_ptr<Expression>> rets, bool bIsVariadicCall
    )
        : callee(std::move(func)), arguments(std::move(args)), rets(std::move(rets)), bIsVariadicCall(bIsVariadicCall) {
        this->nodeKind = ASTNodeKind::CallExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ExpressionStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> expression;
    ExpressionStatementNode(std::shared_ptr<Expression> expr) : expression(std::move(expr)) { this->nodeKind = ASTNodeKind::ExpressionStatement; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ReturnStatementNode : public Statement {
  public:
    std::vector<std::shared_ptr<Expression>> returnValues;
    ReturnStatementNode(std::vector<std::shared_ptr<Expression>> values) : returnValues(std::move(values)) { this->nodeKind = ASTNodeKind::ReturnExpression; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BreakStatementNode : public Statement {
  public:
    ~BreakStatementNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ContinueStatementNode : public Statement {
  public:
    ~ContinueStatementNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};