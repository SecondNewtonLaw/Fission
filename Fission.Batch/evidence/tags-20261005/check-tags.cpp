//
// Created by Dottik on 5/10/2026.
//
#include "AbstractSyntaxTree/Traversal.hpp"
#include <cassert>
#include <cstdio>

template <typename T> void Check(const std::vector<std::shared_ptr<Expression>> &nodes) {
    for (const auto &node : nodes) {
        assert(AsLiteral<T>(node) == std::dynamic_pointer_cast<T>(node));
        const std::shared_ptr<Statement> statement = node;
        assert(AsLiteral<T>(statement) == std::dynamic_pointer_cast<T>(statement));
    }
    assert(!AsLiteral<T>(std::shared_ptr<Expression>{}));
}

int main() {
    std::vector<std::shared_ptr<Expression>> leaves{
        std::make_shared<NilLiteralNode>(), std::make_shared<BooleanLiteralNode>(true),
        std::make_shared<NumberLiteralNode>(1.25), std::make_shared<IntegerLiteralNode>(42),
        std::make_shared<StringLiteralNode>("x"), std::make_shared<VectorNode>(),
        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("x")),
        std::make_shared<VarArgExpression>(), std::make_shared<NoExpressionNode>(), std::make_shared<Expression>()
    };
    auto tagged = leaves;
    tagged.push_back(std::make_shared<TableLiteralNode>());
    tagged.push_back(std::make_shared<VectorNode>(VectorNode::Float{1, 2, 3, 4}));
    tagged.push_back(std::make_shared<VectorNode>(VectorNode::Double{1, 2, 3, 4}));
    Check<NilLiteralNode>(tagged);
    Check<BooleanLiteralNode>(tagged);
    Check<NumberLiteralNode>(tagged);
    Check<IntegerLiteralNode>(tagged);
    Check<StringLiteralNode>(tagged);
    Check<TableLiteralNode>(tagged);
    Check<VectorNode>(tagged);
    struct ExtendedNumber : NumberLiteralNode { ExtendedNumber() : NumberLiteralNode(7) {} };
    const std::shared_ptr<Expression> extended = std::make_shared<ExtendedNumber>();
    assert(AsLiteral<ExtendedNumber>(extended) == std::dynamic_pointer_cast<ExtendedNumber>(extended));
    assert(!AsLiteral<ExtendedNumber>(tagged[2]));
    auto legacy = std::make_shared<NumberLiteralNode>(9);
    legacy->literalKind = LiteralNodeKind::Unknown;
    assert(AsLiteral<NumberLiteralNode>(std::shared_ptr<Expression>(legacy)) == legacy);
    legacy->nodeKind = ASTNodeKind::Unknown;
    assert(AsLiteral<NumberLiteralNode>(std::shared_ptr<Expression>(legacy)) == legacy);
    for (const auto &e : leaves) {
        assert(AsExpression(e) == std::dynamic_pointer_cast<Expression>(std::shared_ptr<Statement>(e)));
        size_t count = 0;
        ForEachSubExpression(e, [&](auto &) { ++count; });
        assert(count == 0);
    }
    assert(!AsExpression(std::make_shared<Statement>()));
    assert(!AsExpression(std::make_shared<Identifier>("x")));
    assert(!AsExpression(std::make_shared<FunctionArgumentExpression>(leaves[0], std::nullopt)));
    assert(!AsExpression(std::make_shared<ExpressionStatementNode>(leaves[0])));
    assert(!AsExpression(std::make_shared<ForNumericNode>()));
    assert(!AsExpression(std::make_shared<ForGeneralNode>()));
    assert(!AsExpression({}));
    std::shared_ptr<Expression> table = std::make_shared<TableLiteralNode>(leaves);
    std::weak_ptr<Expression> weak = table;
    size_t index = 0;
    ForEachSubExpression(table, [&](std::shared_ptr<Expression> &child) {
        assert(!weak.expired());
        assert(child == leaves[index++]);
        table.reset();
        child = std::make_shared<NilLiteralNode>();
    });
    assert(index == leaves.size() && weak.expired());
    auto copy = std::make_shared<TableLiteralNode>(TableLiteralNode{});
    assert(copy->nodeKind == ASTNodeKind::LiteralValue && copy->bIsTableLiteral);
    auto binary = std::make_shared<TableBinaryExpressionNode>("=", leaves[0], leaves[1]);
    assert(AsExpression(binary) == binary);
    index = 0;
    ForEachSubExpression(binary, [&](auto &child) { assert(child == leaves[index++]); });
    assert(index == 2);
    struct OriginalLiteralLayout : Expression { bool bUseParenthesis = false; };
    assert(sizeof(LiteralNode) == sizeof(OriginalLiteralLayout));
    struct OriginalNodeLayout { virtual ~OriginalNodeLayout() = default; ASTNodeKind nodeKind = ASTNodeKind::Unknown; virtual void Accept(Visitor *) {} };
    assert(sizeof(ASTNode) == sizeof(OriginalNodeLayout));
    std::printf("leaf dispatch, expression classification, table copy, child order, mutable ownership and literal size passed (%zu bytes)\n", sizeof(LiteralNode));
}
