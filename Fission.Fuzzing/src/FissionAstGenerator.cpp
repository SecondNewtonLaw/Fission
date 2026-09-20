#include "FissionAstGenerator.hpp"

// Generator.hpp uses ASSERT (libassert) and std::ranges algorithms without including them itself.
#include "libassert/assert.hpp"
#include <algorithm>

#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "SourceGenerator/Generator.hpp"

#include <array>

uint32_t FissionAstGenerator::Next() {
    // xorshift32; deterministic, seedable, no global RNG state.
    uint32_t x = m_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    m_state = x;
    return x;
}
int FissionAstGenerator::Rand(int n) { return n <= 0 ? 0 : static_cast<int>(Next() % static_cast<uint32_t>(n)); }
bool FissionAstGenerator::Chance(int pct) { return Rand(100) < pct; }

std::string FissionAstGenerator::FreshLocal() {
    std::string name = "v" + std::to_string(m_locals.size());
    m_locals.push_back(name);
    return name;
}

std::string FissionAstGenerator::AnyName() {
    static const std::array<const char *, 14> globals = {"print",  "math", "table",  "string", "tostring", "tonumber", "pairs",
                                                         "ipairs", "next", "select", "t",      "obj",      "self",     "game"};
    if (!m_locals.empty() && Chance(60))
        return m_locals[Rand(static_cast<int>(m_locals.size()))];
    return globals[Rand(static_cast<int>(globals.size()))];
}

static std::shared_ptr<Expression> Ident(const std::string &name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); }

std::shared_ptr<Expression> FissionAstGenerator::GenLeaf() {
    static const std::array<const char *, 11> strs = {"", "x", "hello", "a-b", "key", "end", "value", "\n", "a\nb", "\nhello", "x\n]"};
    switch (Rand(6)) {
    case 0:
        return std::make_shared<NumberLiteralNode>(static_cast<double>(Rand(1000)) / (Chance(50) ? 1.0 : 7.0));
    case 1:
        return std::make_shared<IntegerLiteralNode>(Rand(256) - 64);
    case 2:
        return std::make_shared<StringLiteralNode>(strs[Rand(static_cast<int>(strs.size()))]);
    case 3:
        return std::make_shared<BooleanLiteralNode>(Chance(50));
    case 4:
        return std::make_shared<NilLiteralNode>();
    default:
        return Ident(AnyName());
    }
}

std::shared_ptr<Expression> FissionAstGenerator::GenTable(int depth) {
    static const std::array<const char *, 9> keys = {"x", "y", "field", "a-b", "end", "data", "\n", "a\nb", "x\n]"};
    auto table = std::make_shared<TableLiteralNode>();
    const int count = Rand(5);
    for (int i = 0; i < count; ++i) {
        if (Chance(40)) {
            // keyed string entry; the decompiler's `["k"] = v` convention: BinaryExpressionNode("=", str, v).
            auto key = std::make_shared<StringLiteralNode>(keys[Rand(static_cast<int>(keys.size()))]);
            table->expressions.push_back(std::make_shared<BinaryExpressionNode>("=", key, GenExpr(depth - 1)));
        } else {
            table->expressions.push_back(GenExpr(depth - 1)); // array element (may be `"s" * x`; fuzzes EmitTableEntry)
        }
    }
    return table;
}

std::shared_ptr<Expression> FissionAstGenerator::GenCall(int depth) {
    std::vector<std::shared_ptr<Expression>> args;
    const int n = Rand(3);
    for (int i = 0; i < n; ++i)
        args.push_back(GenExpr(depth - 1));
    return std::make_shared<CallExpressionNode>(Ident(AnyName()), std::move(args), std::vector<std::shared_ptr<Expression>>{}, false, /*inlineCall*/ true);
}

std::shared_ptr<Expression> FissionAstGenerator::GenExpr(int depth) {
    static const std::array<const char *, 16> binops = {"+", "-", "*", "/", "//", "%", "^", "..", "==", "~=", "<", "<=", ">", ">=", "and", "or"};
    static const std::array<const char *, 5> fields = {"x", "y", "field", "a-b", "value"};
    if (depth <= 0)
        return GenLeaf();
    switch (Rand(11)) {
    case 0:
    case 1:
        return GenLeaf();
    case 2:
        return std::make_shared<BinaryExpressionNode>(binops[Rand(static_cast<int>(binops.size()))], GenExpr(depth - 1), GenExpr(depth - 1));
    case 3: {
        // unary, sometimes nested `- -x` (fuzzes the double-minus comment guard).
        if (Chance(30))
            return std::make_shared<UnaryExpressionNode>("-", std::make_shared<UnaryExpressionNode>("-", GenExpr(depth - 1)));
        const char *op = Chance(50) ? "-" : (Chance(50) ? "not " : "#");
        return std::make_shared<UnaryExpressionNode>(op, GenExpr(depth - 1));
    }
    case 4:
        return std::make_shared<MemberExpressionNode>(GenExpr(depth - 1), std::string(fields[Rand(static_cast<int>(fields.size()))]));
    case 5:
        return std::make_shared<IndexExpressionNode>(GenExpr(depth - 1), GenExpr(depth - 1));
    case 6:
        return GenTable(depth - 1);
    case 7:
        return GenCall(depth - 1);
    case 8: {
        auto body = std::make_shared<BlockStatementNode>();
        body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{GenExpr(depth - 1)}));
        auto function =
            std::make_shared<FunctionDeclarationNode>("", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, body, false);
        function->bAnonymousInline = true;
        return function;
    }
    case 9:
        return std::make_shared<IfExpressionNode>(GenExpr(depth - 1), GenExpr(depth - 1), GenExpr(depth - 1));
    default:
        return GenLeaf();
    }
}

std::shared_ptr<Statement> FissionAstGenerator::GenStatement(int depth) {
    if (!m_locals.empty() && Chance(20)) {
        auto lhs = std::make_shared<IndexExpressionNode>(Ident(AnyName()), GenExpr(depth - 1));
        return std::make_shared<AssignmentStatementNode>(lhs, GenExpr(depth - 1));
    }
    if (Chance(25)) {
        // statement-level call: inlineCall=false renders an indented `f(args)` statement.
        auto call = std::dynamic_pointer_cast<CallExpressionNode>(GenCall(depth));
        call->inlineCall = false;
        return call;
    }
    // `local vN = <expr>`
    auto value = GenExpr(depth);
    return std::make_shared<VariableDeclarationNode>(Ident(FreshLocal()), value);
}

std::string FissionAstGenerator::Generate() {
    m_locals.clear();
    if (Chance(35))
        return GenExecutable();
    std::vector<std::shared_ptr<Statement>> body;
    const int stmts = 3 + Rand(8);
    for (int i = 0; i < stmts; ++i)
        body.push_back(GenStatement(2 + Rand(3)));

    // a trailing return so the chunk yields values (and exercises ReturnStatementNode).
    std::vector<std::shared_ptr<Expression>> rets;
    const int n = 1 + Rand(2);
    for (int i = 0; i < n; ++i)
        rets.push_back(GenExpr(2));
    body.push_back(std::make_shared<ReturnStatementNode>(std::move(rets)));

    RootNode root{body};
    SourceGenerator sg{};
    return sg.GenerateSource(&root);
}

std::string FissionAstGenerator::GenExecutable() {
    using Expr = std::shared_ptr<Expression>;
    using Stmt = std::shared_ptr<Statement>;
    const auto number = [](int value) -> Expr { return std::make_shared<NumberLiteralNode>(value); };
    const auto binary = [](const char *op, Expr lhs, Expr rhs) -> Expr { return std::make_shared<BinaryExpressionNode>(op, std::move(lhs), std::move(rhs)); };
    const auto call = [](Expr callee, std::vector<Expr> args) -> Expr {
        return std::make_shared<CallExpressionNode>(std::move(callee), std::move(args), std::vector<Expr>{}, false, true);
    };
    const auto statement = [](Expr value) -> Stmt {
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(value))
            call->inlineCall = false;
        return std::make_shared<ExpressionStatementNode>(std::move(value));
    };
    const auto block = [](std::vector<Stmt> body) {
        auto result = std::make_shared<BlockStatementNode>();
        result->body = std::move(body);
        return result;
    };
    const auto function = [&](const std::string &name, const std::vector<std::string> &params, std::vector<Stmt> body) {
        std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>> args;
        for (size_t i = 0; i < params.size(); ++i)
            args[static_cast<int32_t>(i)] = std::make_shared<FunctionArgumentExpression>(Ident(params[i]), std::nullopt);
        auto result =
            std::make_shared<FunctionDeclarationNode>(name, static_cast<int32_t>(params.size()), std::move(args), false, block(std::move(body)), !name.empty());
        result->bAnonymousInline = name.empty();
        return result;
    };
    const auto ret = [](std::vector<Expr> values) -> Stmt { return std::make_shared<ReturnStatementNode>(std::move(values)); };
    const auto index = [&](Expr table, int key) -> Expr { return std::make_shared<IndexExpressionNode>(std::move(table), number(key)); };
    std::vector<Stmt> body;
    const bool tableCase = Chance(60);
    if (tableCase) {
        const int width = 2 + Rand(5), delta = 1 + Rand(9);
        auto left = std::make_shared<TableLiteralNode>();
        auto right = std::make_shared<TableLiteralNode>();
        left->expressions.push_back(Ident("item"));
        right->expressions.push_back(Ident("item"));
        for (int i = 1; i < width; ++i)
            left->expressions.push_back(number(Rand(100)));
        Expr condition = Ident("flag");
        if (Chance(50)) {
            condition = std::make_shared<UnaryExpressionNode>("not ", condition);
            std::swap(left, right);
        }
        auto read = function("read", {}, {ret({Ident("selected")})});
        const auto observe = [&] {
            return statement(call(
                Ident("print"),
                {call(Ident("rawequal"), {call(Ident("read"), {}), Ident("selected")}), index(Ident("selected"), 1), index(Ident("selected"), 2)}
            ));
        };
        auto base = call(function("", {}, {ret({Ident("selected")})}), {});
        auto mutate = std::make_shared<AssignmentStatementNode>(index(base, 1), binary("+", index(Ident("selected"), 1), number(delta)));
        body.push_back(function(
            "choose", {"flag", "item"},
            {std::make_shared<VariableDeclarationNode>(Ident("selected"), std::make_shared<IfExpressionNode>(condition, left, right)), read, observe(), mutate,
             observe(), ret({index(call(Ident("read"), {}), 1)})}
        ));
        const int initial = Rand(100);
        body.push_back(statement(call(
            Ident("print"), {call(Ident("choose"), {std::make_shared<BooleanLiteralNode>(true), number(initial)}),
                             call(Ident("choose"), {std::make_shared<BooleanLiteralNode>(false), number(initial)})}
        )));
    } else {
        const int width = 24 + Rand(89), initial = Rand(20);
        body.push_back(
            function("advance", {"value", "tag"}, {statement(call(Ident("print"), {Ident("tag")})), ret({binary("+", Ident("value"), Ident("tag"))})})
        );
        std::shared_ptr<IfStatementNode> chain;
        for (int i = width - 1; i >= 0; --i) {
            auto branch = std::make_shared<IfStatementNode>();
            branch->condition = binary("==", Ident("selector"), number(i));
            branch->thenBranch = block({std::make_shared<AssignmentStatementNode>(Ident("result"), call(Ident("advance"), {Ident("result"), number(i)}))});
            if (chain)
                branch->elseBranch = block({chain});
            chain = branch;
        }
        body.push_back(
            function("dispatch", {"selector"}, {std::make_shared<VariableDeclarationNode>(Ident("result"), number(initial)), chain, ret({Ident("result")})})
        );
        for (const int selector : {0, width / 2, width - 1, -1})
            body.push_back(statement(call(Ident("print"), {call(Ident("dispatch"), {number(selector)})})));
    }
    RootNode root{body};
    SourceGenerator generator;
    return std::string("-- fuzz-case: ") + (tableCase ? "branch-table-capture\n" : "effectful-dispatch\n") + generator.GenerateSource(&root);
}
