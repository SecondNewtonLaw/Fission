#pragma once
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "AbstractSyntaxTree/Visitor.hpp"

#include <cmath>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// Serialize the lifted AST without adding another dependency.
class AstJsonSerializer : public Visitor {
    std::stringstream buffer;

    // Bound hostile nesting while preserving an otherwise successful decompile.
    static constexpr int kMaxDepth = 4000;
    int m_depth = 0;

    static std::string Quote(const std::string &value) {
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (unsigned char c : value) {
            switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                // JSON forbids raw control bytes in strings.
                if (c < 0x20)
                    out += std::format("\\u{:04x}", static_cast<int>(c));
                else
                    out.push_back(static_cast<char>(c));
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    template <typename T> static std::string Num(T v) {
        // JSON has no NaN or infinity values.
        if (!std::isfinite(v))
            return Quote(std::format("{}", v));
        return std::format("{}", v);
    }

    static std::string Bool(bool v) { return v ? "true" : "false"; }

    // Isolate child output so parent objects can compose complete fragments.
    std::string Render(ASTNode *node) {
        if (node == nullptr)
            return "null";
        if (m_depth >= kMaxDepth)
            return "{\"kind\":\"Truncated\"}";
        ++m_depth;
        std::stringstream scratch;
        scratch.swap(buffer);
        node->Accept(this);
        std::string out = buffer.str();
        buffer.swap(scratch);
        --m_depth;
        return out;
    }

    template <typename T> std::string Render(const std::shared_ptr<T> &node) { return Render(node.get()); }

    template <typename T> std::string Render(const std::optional<std::shared_ptr<T>> &node) { return node.has_value() ? Render(node.value()) : "null"; }

    template <typename T> std::string Arr(const std::vector<std::shared_ptr<T>> &items) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0)
                out += ",";
            out += Render(items[i]);
        }
        out += "]";
        return out;
    }

    // Match the internal ASTNodeKind classification.
    static const char *KindName(ASTNodeKind kind) {
        switch (kind) {
        case ASTNodeKind::Root:
            return "Root";
        case ASTNodeKind::Comment:
            return "Comment";
        case ASTNodeKind::LiteralValue:
            return "LiteralValue";
        case ASTNodeKind::Identifier:
            return "Identifier";
        case ASTNodeKind::IdentifierExpression:
            return "IdentifierExpression";
        case ASTNodeKind::FunctionArgument:
            return "FunctionArgument";
        case ASTNodeKind::VariableDeclaration:
            return "VariableDeclaration";
        case ASTNodeKind::FunctionDeclarationNode:
            return "FunctionDeclaration";
        case ASTNodeKind::FunctionExpression:
            return "FunctionExpression";
        case ASTNodeKind::BinaryExpression:
            return "BinaryExpression";
        case ASTNodeKind::IfExpression:
            return "IfExpression";
        case ASTNodeKind::CompoundAssignment:
            return "CompoundAssignment";
        case ASTNodeKind::TableBinaryExpression:
            return "TableBinaryExpression";
        case ASTNodeKind::UnaryExpression:
            return "UnaryExpression";
        case ASTNodeKind::MemberExpression:
            return "MemberExpression";
        case ASTNodeKind::IndexExpression:
            return "IndexExpression";
        case ASTNodeKind::CallExpression:
            return "CallExpression";
        case ASTNodeKind::MethodCallExpression:
            return "MethodCallExpression";
        case ASTNodeKind::VarArgExpression:
            return "VarArgExpression";
        case ASTNodeKind::NoExpression:
            return "NoExpression";
        case ASTNodeKind::BlockStatement:
            return "BlockStatement";
        case ASTNodeKind::AssignmentStatement:
            return "AssignmentStatement";
        case ASTNodeKind::IfStatement:
            return "IfStatement";
        case ASTNodeKind::WhileStatement:
            return "WhileStatement";
        case ASTNodeKind::RepeatStatement:
            return "RepeatStatement";
        case ASTNodeKind::ForNumeric:
            return "ForNumeric";
        case ASTNodeKind::ForGeneral:
            return "ForGeneral";
        case ASTNodeKind::BreakStatement:
            return "BreakStatement";
        case ASTNodeKind::ContinueStatement:
            return "ContinueStatement";
        case ASTNodeKind::ExpressionStatement:
            return "ExpressionStatement";
        case ASTNodeKind::ReturnExpression:
            return "ReturnExpression";
        case ASTNodeKind::ClassDeclaration:
            return "ClassDeclaration";
        case ASTNodeKind::Unknown:
            return "Unknown";
        }
        return "Unknown";
    }

    // Report the stable base-class category.
    static const char *Category(const ASTNode *node) {
        if (dynamic_cast<const RootNode *>(node))
            return "Program";
        if (dynamic_cast<const LiteralNode *>(node))
            return "Literal";
        if (dynamic_cast<const Declaration *>(node))
            return "Declaration";
        if (dynamic_cast<const Expression *>(node))
            return "Expression";
        if (dynamic_cast<const Statement *>(node))
            return "Statement";
        return "Node";
    }

    // Emit type tags before node fields.
    void Obj(const ASTNode *node, const char *kind, std::initializer_list<std::pair<const char *, std::string>> fields) {
        buffer << "{\"kind\":\"" << kind << "\",\"category\":\"" << Category(node) << "\",\"nodeKind\":\"" << KindName(node->nodeKind) << "\"";
        for (const auto &[key, value] : fields)
            buffer << ",\"" << key << "\":" << value;
        buffer << "}";
    }

  public:
    std::string Serialize(RootNode *root) {
        buffer.str("");
        buffer.clear();
        m_depth = 0;
        return Render(root);
    }

    void Visit(RootNode *lpNode) override { Obj(lpNode, "Root", {{"body", Arr(lpNode->programBody)}}); }

    void Visit(Identifier *lpNode) override { Obj(lpNode, "Identifier", {{"name", Quote(lpNode->name)}}); }

    void Visit(IdentifierExpressionNode *lpNode) override { Obj(lpNode, "IdentifierExpression", {{"identifier", Render(lpNode->identifier)}}); }

    void Visit(CommentNode *lpNode) override {
        Obj(lpNode, "Comment", {{"comment", Quote(lpNode->comment)}, {"informational", Bool(lpNode->bIsInformational)}, {"newLine", Bool(lpNode->bNewLine)}});
    }

    void Visit(FunctionArgumentExpression *lpNode) override {
        Obj(lpNode, "FunctionArgument", {{"name", Render(lpNode->argumentName)}, {"type", Render(lpNode->type)}});
    }

    void Visit(FunctionDeclarationNode *lpNode) override {
        std::string args = "[";
        for (int32_t i = 0; i < lpNode->argumentCount; ++i) {
            if (i > 0)
                args += ",";
            auto it = lpNode->argumentsNames.find(i);
            args += (it != lpNode->argumentsNames.end()) ? Render(it->second) : "null";
        }
        args += "]";
        Obj(lpNode, "FunctionDeclaration",
            {{"name", Quote(lpNode->functionName)},
             {"exported", Bool(lpNode->bExported)},
             {"argumentCount", std::to_string(lpNode->argumentCount)},
             {"arguments", args},
             {"isVararg", Bool(lpNode->bIsVarArg)},
             {"isLocal", Bool(lpNode->bIsLocalDeclaration)},
             {"anonymousInline", Bool(lpNode->bAnonymousInline)},
             {"body", Render(lpNode->lpFunctionBody)}});
    }

    void Visit(ClassDeclarationNode *lpNode) override {
        std::string props = "[";
        for (size_t i = 0; i < lpNode->propertyNames.size(); ++i) {
            if (i > 0)
                props += ",";
            props += Quote(lpNode->propertyNames[i]);
        }
        props += "]";
        std::string methods = "[";
        for (size_t i = 0; i < lpNode->methods.size(); ++i) {
            if (i > 0)
                methods += ",";
            methods += Render(lpNode->methods[i]);
        }
        methods += "]";
        Obj(lpNode, "ClassDeclaration",
            {{"name", Quote(lpNode->className)},
             {"exported", Bool(lpNode->bExported)},
             {"open", Bool(lpNode->bOpen)},
             {"superclass", Render(lpNode->superclass)},
             {"properties", props},
             {"methods", methods}});
    }

    void Visit(CallExpressionNode *lpNode) override {
        Obj(lpNode, "CallExpression",
            {{"callee", Render(lpNode->callee)},
             {"arguments", Arr(lpNode->arguments)},
             {"rets", Arr(lpNode->rets)},
             {"isVariadicCall", Bool(lpNode->bIsVariadicCall)},
             {"inlineCall", Bool(lpNode->inlineCall)},
             {"isLocal", Bool(lpNode->bIsLocalDeclaration)}});
    }

    void Visit(NameCallExpressionNode *lpNode) override {
        Obj(lpNode, "NameCallExpression",
            {{"calledOn", Render(lpNode->calledOn)},
             {"callWhat", Render(lpNode->callWhat)},
             {"arguments", Arr(lpNode->arguments)},
             {"rets", Arr(lpNode->rets)},
             {"isVariadicCall", Bool(lpNode->bIsVariadicCall)},
             {"inlineCall", Bool(lpNode->inlineCall)},
             {"isLocal", Bool(lpNode->bIsLocalDeclaration)}});
    }

    void Visit(UnaryExpressionNode *lpNode) override { Obj(lpNode, "UnaryExpression", {{"op", Quote(lpNode->op)}, {"operand", Render(lpNode->operand)}}); }

    void Visit(IndexExpressionNode *lpNode) override { Obj(lpNode, "IndexExpression", {{"left", Render(lpNode->left)}, {"right", Render(lpNode->right)}}); }

    void Visit(MemberExpressionNode *lpNode) override { Obj(lpNode, "MemberExpression", {{"table", Render(lpNode->table)}, {"key", Render(lpNode->key)}}); }

    void Visit(ReturnStatementNode *lpNode) override { Obj(lpNode, "ReturnStatement", {{"values", Arr(lpNode->returnValues)}}); }

    void Visit(ExpressionStatementNode *lpNode) override { Obj(lpNode, "ExpressionStatement", {{"expression", Render(lpNode->expression)}}); }

    void Visit(BreakStatementNode *lpNode) override {
        (void)lpNode;
        Obj(lpNode, "BreakStatement", {});
    }

    void Visit(ContinueStatementNode *lpNode) override {
        (void)lpNode;
        Obj(lpNode, "ContinueStatement", {});
    }

    void Visit(BlockStatementNode *lpNode) override { Obj(lpNode, "BlockStatement", {{"body", Arr(lpNode->body)}}); }

    void Visit(WhileStatementNode *lpNode) override {
        Obj(lpNode, "WhileStatement", {{"condition", Render(lpNode->condition)}, {"body", Render(lpNode->body)}});
    }

    void Visit(RepeatStatementNode *lpNode) override {
        Obj(lpNode, "RepeatStatement", {{"condition", Render(lpNode->condition)}, {"body", Render(lpNode->body)}});
    }

    void Visit(IfStatementNode *lpNode) override {
        Obj(lpNode, "IfStatement",
            {{"condition", Render(lpNode->condition)}, {"thenBranch", Render(lpNode->thenBranch)}, {"elseBranch", Render(lpNode->elseBranch)}});
    }

    void Visit(AssignmentStatementNode *lpNode) override {
        Obj(lpNode, "AssignmentStatement", {{"left", Render(lpNode->left)}, {"right", Render(lpNode->right)}});
    }

    void Visit(BinaryExpressionNode *lpNode) override {
        Obj(lpNode, "BinaryExpression", {{"op", Quote(lpNode->op)}, {"left", Render(lpNode->left)}, {"right", Render(lpNode->right)}});
    }

    void Visit(IfExpressionNode *lpNode) override {
        Obj(lpNode, "IfExpression", {{"condition", Render(lpNode->condition)}, {"then", Render(lpNode->thenExpr)}, {"else", Render(lpNode->elseExpr)}});
    }

    void Visit(TableBinaryExpressionNode *lpNode) override {
        Obj(lpNode, "TableBinaryExpression", {{"op", Quote(lpNode->op)}, {"left", Render(lpNode->left)}, {"right", Render(lpNode->right)}});
    }

    void Visit(CompoundBinaryExpressionNode *lpNode) override {
        Obj(lpNode, "CompoundBinaryExpression", {{"op", Quote(lpNode->op)}, {"left", Render(lpNode->left)}, {"right", Render(lpNode->right)}});
    }

    void Visit(StringLiteralNode *lpNode) override { Obj(lpNode, "StringLiteral", {{"value", Quote(lpNode->value)}}); }

    void Visit(NumberLiteralNode *lpNode) override { Obj(lpNode, "NumberLiteral", {{"value", Num(lpNode->value)}}); }

    void Visit(IntegerLiteralNode *lpNode) override { Obj(lpNode, "IntegerLiteral", {{"value", std::to_string(lpNode->value)}}); }

    void Visit(BooleanLiteralNode *lpNode) override { Obj(lpNode, "BooleanLiteral", {{"value", Bool(lpNode->value)}}); }

    void Visit(NilLiteralNode *lpNode) override {
        (void)lpNode;
        Obj(lpNode, "NilLiteral", {});
    }

    void Visit(TableLiteralNode *lpNode) override { Obj(lpNode, "TableLiteral", {{"expressions", Arr(lpNode->expressions)}}); }

    void Visit(VectorNode *lpNode) override {
        std::visit(
            [&](const auto &components) {
                Obj(lpNode, "Vector", {{"x", Num(components[0])}, {"y", Num(components[1])}, {"z", Num(components[2])}, {"w", Num(components[3])}});
            },
            lpNode->components
        );
    }

    void Visit(VariableDeclarationNode *lpNode) override {
        Obj(lpNode, "VariableDeclaration",
            {{"identifier", Render(lpNode->identifier)}, {"value", Render(lpNode->value)}, {"type", Render(lpNode->type)}, {"exported", Bool(lpNode->bExported)}});
    }

    void Visit(NoExpressionNode *lpNode) override {
        (void)lpNode;
        Obj(lpNode, "NoExpression", {});
    }

    void Visit(VarArgExpression *lpNode) override {
        (void)lpNode;
        Obj(lpNode, "VarArg", {});
    }

    void Visit(ForNumericNode *lpNode) override {
        Obj(lpNode, "ForNumeric",
            {{"loopVariable", Render(lpNode->loopVariable)},
             {"start", Render(lpNode->startVariable)},
             {"max", Render(lpNode->maxIncreased)},
             {"step", Render(lpNode->increaseBy)},
             {"body", Render(lpNode->lpLoopBody)}});
    }

    void Visit(ForGeneralNode *lpNode) override {
        Obj(lpNode, "ForGeneral",
            {{"loopVariables", Arr(lpNode->loopVariables)},
             {"generator", Render(lpNode->generator)},
             {"state", Render(lpNode->state)},
             {"index", Render(lpNode->index)},
             {"body", Render(lpNode->body)}});
    }
};
