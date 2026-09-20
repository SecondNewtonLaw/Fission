//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "Visitor.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

class Visitor;
enum class ASTNodeKind {
    LiteralValue,
    FunctionDeclarationNode,

    /**
     *  @brief Represents a generic expression.
     ***/
    ExpressionStatement,
    /**
     *  @brief Represents a variable/ identifier.
     ***/
    Identifier,

    IdentifierExpression,

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
     *  @brief Represents a Luau `if <c> then <a> else <b>` expression (ternary / value-if).
     ***/
    IfExpression,
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
    /**
     *  @brief Represents the program root (main procedure/prototype).
     ***/
    Root,
    /**
     *  @brief Represents a source comment.
     ***/
    Comment,
    /**
     *  @brief Represents a function parameter/ argument.
     ***/
    FunctionArgument,
    /**
     *  @brief Represents a local variable declaration.
     ***/
    VariableDeclaration,
    /**
     *  @brief Represents a compound assignment (e.g. `a += b`).
     ***/
    CompoundAssignment,
    /**
     *  @brief Represents a bracketed binary expression used as a table key.
     ***/
    TableBinaryExpression,
    /**
     *  @brief Represents a vararg expression (`...`).
     ***/
    VarArgExpression,
    /**
     *  @brief Represents an absent/ elided expression placeholder.
     ***/
    NoExpression,
    /**
     *  @brief Represents a block of statements.
     ***/
    BlockStatement,
    /**
     *  @brief Represents an assignment statement.
     ***/
    AssignmentStatement,
    /**
     *  @brief Represents an if/ elseif/ else statement.
     ***/
    IfStatement,
    /**
     *  @brief Represents a while loop.
     ***/
    WhileStatement,
    /**
     *  @brief Represents a repeat-until loop.
     ***/
    RepeatStatement,
    /**
     *  @brief Represents a numeric for loop.
     ***/
    ForNumeric,
    /**
     *  @brief Represents a generic for-in loop.
     ***/
    ForGeneral,
    /**
     *  @brief Represents a break statement.
     ***/
    BreakStatement,
    /**
     *  @brief Represents a continue statement.
     ***/
    ContinueStatement,
    /**
     *  @brief Represents a V10 Luau `class ... end` declaration.
     ***/
    ClassDeclaration,

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

class FunctionArgumentExpression : public Statement {
  public:
    std::shared_ptr<Expression> argumentName;
    std::optional<std::shared_ptr<Expression>> type = std::nullopt;
    explicit FunctionArgumentExpression(std::shared_ptr<Expression> argName, const std::optional<std::shared_ptr<Expression>> &tt)
        : argumentName(std::move(argName)), type(tt) {
        this->nodeKind = ASTNodeKind::FunctionArgument;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
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
    // Emit an explicit scope for register-reuse phases.
    bool bEmitAsDoBlock = false;
    BlockStatementNode() { this->nodeKind = ASTNodeKind::BlockStatement; }
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class AssignmentStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> left;
    std::shared_ptr<Expression> right;
    AssignmentStatementNode(const std::shared_ptr<Expression> &l, const std::shared_ptr<Expression> &r) : left(l), right(r) {
        this->nodeKind = ASTNodeKind::AssignmentStatement;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IfStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> thenBranch;
    std::shared_ptr<BlockStatementNode> elseBranch;
    IfStatementNode() { this->nodeKind = ASTNodeKind::IfStatement; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class WhileStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> body;
    WhileStatementNode() { this->nodeKind = ASTNodeKind::WhileStatement; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class RepeatStatementNode : public Statement {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<BlockStatementNode> body;
    RepeatStatementNode() { this->nodeKind = ASTNodeKind::RepeatStatement; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class CompoundBinaryExpressionNode : public Expression {
  public:
    std::string op;
    std::shared_ptr<Expression> left, right;
    CompoundBinaryExpressionNode(const std::string &op, const std::shared_ptr<Expression> &left, const std::shared_ptr<Expression> &right)
        : op(op), left(left), right(right) {
        this->nodeKind = ASTNodeKind::CompoundAssignment;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BinaryExpressionNode : public Expression {
  public:
    std::string op;
    std::shared_ptr<Expression> left, right;
    BinaryExpressionNode(const std::string &op, const std::shared_ptr<Expression> &left, const std::shared_ptr<Expression> &right)
        : op(op), left(left), right(right) {
        this->nodeKind = ASTNodeKind::BinaryExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class TableBinaryExpressionNode : public BinaryExpressionNode {
  public:
    TableBinaryExpressionNode(const std::string &op, const std::shared_ptr<Expression> &left, const std::shared_ptr<Expression> &right)
        : BinaryExpressionNode(op, left, right) {
        this->nodeKind = ASTNodeKind::TableBinaryExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

// A Luau if-expression. Nested else expressions form elseif chains.
class IfExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> condition;
    std::shared_ptr<Expression> thenExpr;
    std::shared_ptr<Expression> elseExpr;
    IfExpressionNode(std::shared_ptr<Expression> condition, std::shared_ptr<Expression> thenExpr, std::shared_ptr<Expression> elseExpr)
        : condition(std::move(condition)), thenExpr(std::move(thenExpr)), elseExpr(std::move(elseExpr)) {
        this->nodeKind = ASTNodeKind::IfExpression;
    }

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
    std::shared_ptr<Expression> table; // may be nil for inlined expressions, particularly used in TABLE initializations.
    std::shared_ptr<Expression> key;

    MemberExpressionNode(std::shared_ptr<Expression> keyExpression) : key(std::move(keyExpression)) {
        this->nodeKind = ASTNodeKind::MemberExpression;
        table = nullptr;
    }

    MemberExpressionNode(std::shared_ptr<Expression> table, std::string keyName)
        : table(std::move(table)), key(std::dynamic_pointer_cast<Expression>(std::make_shared<StringLiteralNode>(std::move(keyName)))) {
        this->nodeKind = ASTNodeKind::MemberExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IdentifierExpressionNode : public Expression {
  public:
    std::shared_ptr<Identifier> identifier;
    IdentifierExpressionNode(std::shared_ptr<Identifier> id) : identifier(id) { this->nodeKind = ASTNodeKind::IdentifierExpression; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class VariableDeclarationNode : public Declaration {
  public:
    std::shared_ptr<Expression> identifier;
    std::shared_ptr<Expression> value;
    std::optional<std::shared_ptr<Expression>> type = std::nullopt;
    VariableDeclarationNode(std::shared_ptr<Identifier> identifier) : identifier(std::make_shared<IdentifierExpressionNode>(identifier)), value(nullptr) {
        this->nodeKind = ASTNodeKind::VariableDeclaration;
    }
    VariableDeclarationNode(std::shared_ptr<Expression> identifier, std::shared_ptr<Expression> expr) : identifier(identifier), value(expr) {
        this->nodeKind = ASTNodeKind::VariableDeclaration;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class LiteralNode : public Expression {
  public:
    bool bUseParenthesis = false;
};

class NilLiteralNode : public LiteralNode {
  public:
    NilLiteralNode() { this->nodeKind = ASTNodeKind::LiteralValue; }
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class BooleanLiteralNode : public LiteralNode {
  public:
    bool value;
    BooleanLiteralNode(bool v) : value(v) { this->nodeKind = ASTNodeKind::LiteralValue; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NumberLiteralNode : public LiteralNode {
  public:
    double value;
    NumberLiteralNode(double v) : value(v) { this->nodeKind = ASTNodeKind::LiteralValue; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class IntegerLiteralNode : public LiteralNode {
  public:
    int64_t value;
    IntegerLiteralNode(int64_t v) : value(v) { this->nodeKind = ASTNodeKind::LiteralValue; }
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class StringLiteralNode : public LiteralNode {
  public:
    std::string value;
    StringLiteralNode(std::string v) : value(v) { this->nodeKind = ASTNodeKind::LiteralValue; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class TableLiteralNode : public LiteralNode {
  public:
    std::vector<std::shared_ptr<Expression>> expressions;
    TableLiteralNode() : expressions() { this->nodeKind = ASTNodeKind::LiteralValue; }
    TableLiteralNode(const std::vector<std::shared_ptr<Expression>> &expressions) : expressions(expressions) { this->nodeKind = ASTNodeKind::LiteralValue; }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class VectorNode : public LiteralNode {
  public:
    using Float = std::array<float, 4>;
    using Double = std::array<double, 4>;

    std::variant<Float, Double> components{Float{}};

    VectorNode() { this->nodeKind = ASTNodeKind::LiteralValue; }
    VectorNode(Float values) : components(values) { this->nodeKind = ASTNodeKind::LiteralValue; }
    VectorNode(Double values) : components(values) { this->nodeKind = ASTNodeKind::LiteralValue; }
    VectorNode(const float x, const float y, const float z, const float w) : VectorNode(Float{x, y, z, w}) {}
    VectorNode(const double x, const double y, const double z, const double w) : VectorNode(Double{x, y, z, w}) {}

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class FunctionDeclarationNode : public Expression {
  public:
    std::string functionName;
    int32_t argumentCount = 0;
    std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> argumentsNames{}; // arg1 -> it's name inside syntax
    bool bIsVarArg = false;
    bool bIsLocalDeclaration = false; // to be defined in lifter. If the only usage of this is inside of a function, and such function holds no debug name.
    // Emit a single-use call-argument closure at its expression site.
    bool bAnonymousInline = false;
    std::shared_ptr<BlockStatementNode> lpFunctionBody = nullptr;
    std::unordered_set<std::string> capturedNames{};

    FunctionDeclarationNode(
        std::string functionName, const int32_t argumentCount, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> names, bool isVarArg,
        std::shared_ptr<BlockStatementNode> funcBody, bool bIsLocalDeclaration
    )
        : functionName(std::move(functionName)), argumentCount(argumentCount), argumentsNames(std::move(names)), bIsVarArg(isVarArg),
          bIsLocalDeclaration(bIsLocalDeclaration), lpFunctionBody(std::move(funcBody)) {
        this->nodeKind = ASTNodeKind::FunctionDeclarationNode;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

// Luau class declaration with named properties and method bodies.
class ClassDeclarationNode : public Statement {
  public:
    std::string className;
    bool bExported = false;
    std::vector<std::string> propertyNames{};
    std::vector<std::shared_ptr<FunctionDeclarationNode>> methods{};

    ClassDeclarationNode(
        std::string className, std::vector<std::string> propertyNames, std::vector<std::shared_ptr<FunctionDeclarationNode>> methods, bool bExported = false
    )
        : className(std::move(className)), bExported(bExported), propertyNames(std::move(propertyNames)), methods(std::move(methods)) {
        this->nodeKind = ASTNodeKind::ClassDeclaration;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NoExpressionNode : public Expression {
  public:
    NoExpressionNode() { this->nodeKind = ASTNodeKind::NoExpression; }
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class NameCallExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> calledOn;
    std::shared_ptr<Expression> callWhat;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Expression>> rets;
    std::vector<std::shared_ptr<Expression>> retTypes;
    bool bIsVariadicCall;
    bool inlineCall = false;
    bool bIsLocalDeclaration = true;
    // Parentheses truncate a fixed-result method call in a spread position.
    bool bAdjustToOne = false;

    NameCallExpressionNode(
        std::shared_ptr<Expression> calledOn, std::shared_ptr<Expression> calledWhat, std::vector<std::shared_ptr<Expression>> args,
        std::vector<std::shared_ptr<Expression>> rets, bool bIsVariadicCall, bool inlineCall
    )
        : calledOn(std::move(calledOn)), callWhat(std::move(calledWhat)), arguments(std::move(args)), rets(std::move(rets)), bIsVariadicCall(bIsVariadicCall),
          inlineCall(inlineCall) {
        this->nodeKind = ASTNodeKind::MethodCallExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class CallExpressionNode : public Expression {
  public:
    std::shared_ptr<Expression> callee;
    std::vector<std::shared_ptr<Expression>> arguments;
    std::vector<std::shared_ptr<Expression>> rets;
    std::vector<std::shared_ptr<Expression>> retTypes;
    bool bIsVariadicCall;
    bool inlineCall;
    bool bIsLocalDeclaration = true;
    // Parentheses truncate a fixed-result call in a spread position.
    bool bAdjustToOne = false;

    CallExpressionNode(
        std::shared_ptr<Expression> func, std::vector<std::shared_ptr<Expression>> args, std::vector<std::shared_ptr<Expression>> rets, bool bIsVariadicCall,
        bool inlineCall
    )
        : callee(std::move(func)), arguments(std::move(args)), rets(std::move(rets)), bIsVariadicCall(bIsVariadicCall), inlineCall(inlineCall) {
        this->nodeKind = ASTNodeKind::CallExpression;
    }

    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class VarArgExpression : public Expression {
  public:
    bool bAdjustToOne = false;
    VarArgExpression() { this->nodeKind = ASTNodeKind::VarArgExpression; }

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
    BreakStatementNode() { this->nodeKind = ASTNodeKind::BreakStatement; }
    ~BreakStatementNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ContinueStatementNode : public Statement {
  public:
    ContinueStatementNode() { this->nodeKind = ASTNodeKind::ContinueStatement; }
    ~ContinueStatementNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ForNumericNode : public Statement {
  public:
    std::shared_ptr<Expression> loopVariable = nullptr;
    std::shared_ptr<Expression> startVariable = nullptr;
    std::shared_ptr<Expression> increaseBy = nullptr;
    std::shared_ptr<Expression> maxIncreased = nullptr;
    std::shared_ptr<BlockStatementNode> lpLoopBody = nullptr;
    ForNumericNode() { this->nodeKind = ASTNodeKind::ForNumeric; }

    ForNumericNode(
        std::shared_ptr<Expression> loopVariable, std::shared_ptr<Expression> startVariable, std::shared_ptr<Expression> increaseBy,
        std::shared_ptr<Expression> maxIncreased, std::shared_ptr<BlockStatementNode> lpLoopBody
    )
        : loopVariable(loopVariable), startVariable(startVariable), increaseBy(increaseBy), maxIncreased(maxIncreased), lpLoopBody(lpLoopBody) {
        this->nodeKind = ASTNodeKind::ForNumeric;
    }
    ~ForNumericNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};

class ForGeneralNode : public Statement {
  public:
    std::vector<std::shared_ptr<Expression>> loopVariables;
    std::shared_ptr<Expression> generator = nullptr;
    std::shared_ptr<Expression> state = nullptr;
    std::shared_ptr<Expression> index = nullptr;
    std::shared_ptr<BlockStatementNode> body = nullptr;
    ForGeneralNode() { this->nodeKind = ASTNodeKind::ForGeneral; }
    ~ForGeneralNode() override = default;
    void Accept(Visitor *visitor) override { visitor->Visit(this); }
};
