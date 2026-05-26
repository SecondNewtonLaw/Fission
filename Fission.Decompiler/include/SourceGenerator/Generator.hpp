//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "AbstractSyntaxTree/Visitor.hpp"

#include <cstdint>
#include <format>
#include <sstream>
#include <string>

class SourceGenerator : public Visitor {
  public:
    std::stringstream buffer;
    size_t dwIndentationLevel = 0;
    static constexpr size_t kIndentationSpaceCount = 4;

    // Minimum binary-operator precedence the surrounding context expects from
    // the next expression. A child whose own precedence is *lower* than this
    // value must wrap itself in parentheses to preserve grouping. 0 means
    // "statement context" - no parens needed for any expression.
    int m_minPrecedence = 0;

    // Luau operator precedence table (low number = low precedence). Matches the
    // grammar in lparser.cpp. Unary operators (`not`, `-`, `#`) sit at level 7.
    static int OperatorPrecedence(const std::string &op) {
        if (op == "or")
            return 1;
        if (op == "and")
            return 2;
        if (op == "<" || op == ">" || op == "<=" || op == ">=" || op == "==" || op == "~=")
            return 3;
        if (op == "..")
            return 4;
        if (op == "+" || op == "-")
            return 5;
        if (op == "*" || op == "/" || op == "//" || op == "%")
            return 6;
        if (op == "^")
            return 8;
        return 0;
    }

    // `..` and `^` are right-associative; everything else is left-associative.
    static bool IsRightAssociative(const std::string &op) { return op == ".." || op == "^"; }

    // Mathematically associative operators where `a op (b op c)` and
    // `(a op b) op c` agree. We treat `..` as associative so flat-chained
    // string concatenations don't gain redundant parens.
    static bool IsAssociative(const std::string &op) { return op == "or" || op == "and" || op == "+" || op == "*" || op == ".."; }

    // Helper that emits the child expression with the requested minimum
    // precedence pushed onto the context, restoring the previous value when
    // done. Use this for every recursive Accept on an expression child.
    template <typename Node> void EmitWithPrecedence(int minPrec, Node *child) {
        const int saved = m_minPrecedence;
        m_minPrecedence = minPrec;
        child->Accept(this);
        m_minPrecedence = saved;
    }

    std::string GetIndentation() { return std::string(this->dwIndentationLevel * kIndentationSpaceCount, ' '); } // NOLINT(*-return-braced-init-list)

    void NextLine() { buffer << "\n"; }
    void IncreaseIndentation() { this->dwIndentationLevel++; }
    void DecreaseIndentation() {
        ASSERT(this->dwIndentationLevel - 1 >= 0, "indentation out of range. Overpopped");
        this->dwIndentationLevel--;
    }

    bool IsLegalLuauIndex(const std::string &str) {
        if (str.empty() || isdigit(str[0]))
            return false;
        return std::ranges::all_of(str, [](const char c) { return isalnum(c) || c == '_'; });
    }

    void EmitQuotedString(const std::string &value) {
        buffer << "\"";
        for (char c : value) {
            switch (c) {
            case '\\':
                buffer << "\\\\";
                break;
            case '"':
                buffer << "\\\"";
                break;
            case '\r':
                buffer << "\\r";
                break;
            case '\t':
                buffer << "\\t";
                break;
            default:
                buffer << c;
                break;
            }
        }
        buffer << "\"";
    }

    void Visit(NoExpressionNode *lpNode) override { (void)lpNode; }

    void Visit(RootNode *lpNode) override {
        (void)lpNode;
        for (const auto &body : lpNode->programBody)
            body->Accept(this);
    }

    void Visit(Identifier *lpNode) override { buffer << lpNode->name; }

    void EmitFunctionArguments(FunctionDeclarationNode *lpNode) {
        for (int32_t i = 0; i < lpNode->argumentCount; i++) {
            lpNode->argumentsNames.at(i)->Accept(this);
            if (i < (lpNode->argumentCount - 1))
                buffer << ", ";
        }
        if (lpNode->bIsVarArg) {
            if (lpNode->argumentCount > 0)
                buffer << ", ";
            buffer << "...";
        }
    }

    void Visit(FunctionDeclarationNode *lpNode) override {
        (void)lpNode;

        // Anonymous inline form: `function(args) ... end` rendered in-place at
        // an expression site (typically a call argument). No leading indent,
        // no `local`, no name. Caller controls surrounding punctuation.
        if (lpNode->bAnonymousInline) {
            buffer << "function(";
            EmitFunctionArguments(lpNode);
            buffer << ")";
            this->NextLine();

            this->IncreaseIndentation();
            lpNode->lpFunctionBody->Accept(this);
            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "end";
            return;
        }

        buffer << this->GetIndentation();
        if (lpNode->bIsLocalDeclaration)
            buffer << "local ";
        buffer << std::format("function {}(", lpNode->functionName);

        EmitFunctionArguments(lpNode);

        buffer << ")";
        this->NextLine();

        this->IncreaseIndentation();
        lpNode->lpFunctionBody->Accept(this);
        this->DecreaseIndentation();
        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    void Visit(CommentNode *lpNode) override {

        if (lpNode->comment.find('\n') == std::string::npos) {
            buffer << this->GetIndentation() << "-- " << lpNode->comment;
            if (lpNode->bNewLine)
                this->NextLine();
            return;
        }

        std::string indent = this->GetIndentation();
        buffer << indent << "--[[ ";

        const std::string &text = lpNode->comment;
        for (size_t i = 0; i < text.length(); ++i) {
            buffer << text[i];

            if (text[i] == '\n' && i != text.length() - 1) {
                buffer << indent;
            }
        }

        if (*text.rbegin() != '\n')
            this->NextLine();

        buffer << indent << "]]";

        if (lpNode->bNewLine)
            this->NextLine();
    }

    void Visit(FunctionArgumentExpression *lpNode) override {
        lpNode->argumentName->Accept(this);
        if (lpNode->type) {
            buffer << ": ";
            lpNode->type.value()->Accept(this);
        }
    }

    void Visit(CallExpressionNode *lpNode) override {
        (void)lpNode;
        if (!lpNode->inlineCall) {
            buffer << this->GetIndentation();
            if (!lpNode->rets.empty()) {
                buffer << "local ";
                for (size_t i = 0; i < lpNode->rets.size(); i++) {
                    lpNode->rets.at(i)->Accept(this);
                    if (i < lpNode->rets.size() - 1)
                        buffer << ", ";
                }
                buffer << " = ";
            }
            lpNode->callee->Accept(this);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }

            buffer << ")";
            this->NextLine();
        } else {
            lpNode->callee->Accept(this);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }
            buffer << ")";
        }
    }

    void Visit(UnaryExpressionNode *lpNode) override {
        (void)lpNode;
        constexpr int kUnaryPrec = 7;
        const bool wrap = kUnaryPrec < m_minPrecedence;
        if (wrap)
            buffer << "(";
        buffer << lpNode->op;
        // A unary operator binds tighter than every binary except `^`; require
        // the operand to wrap if it is a lower-precedence binary.
        EmitWithPrecedence(kUnaryPrec, lpNode->operand.get());
        if (wrap)
            buffer << ")";
    }

    void Visit(IndexExpressionNode *lpNode) override {
        (void)lpNode;
        lpNode->left->Accept(this);
        buffer << "[";
        lpNode->right->Accept(this);
        buffer << "]";
    }

    void Visit(MemberExpressionNode *lpNode) override {
        (void)lpNode;
        if (lpNode->table != nullptr) {
            lpNode->table->Accept(this);
            if (auto lpStringLiteral = std::dynamic_pointer_cast<StringLiteralNode>(lpNode->key)) {
                if (this->IsLegalLuauIndex(lpStringLiteral->value))
                    buffer << "." << lpStringLiteral->value;
                return;
            }
        }
        buffer << "[";
        lpNode->key->Accept(this);
        buffer << "]";
    }

    void Visit(ReturnStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        buffer << "return";

        if (!lpNode->returnValues.empty()) {
            buffer << " ";
            for (size_t i = 0; i < lpNode->returnValues.size(); i++) {
                lpNode->returnValues.at(i)->Accept(this);
                if (i < lpNode->returnValues.size() - 1)
                    buffer << ", ";
            }
        }
        this->NextLine();
    }

    void Visit(ExpressionStatementNode *lpNode) override {
        (void)lpNode;
        lpNode->expression->Accept(this);
    }

    void Visit(BreakStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "break";
        this->NextLine();
    }

    void Visit(ContinueStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "continue";
        this->NextLine();
    }

    void Visit(BlockStatementNode *lpNode) override {
        (void)lpNode;
        for (const auto &node : lpNode->body) {
            node->Accept(this);
        }
    }

    void Visit(WhileStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "while ";
        lpNode->condition->Accept(this);
        buffer << " do";
        this->NextLine();
        this->IncreaseIndentation();
        lpNode->body->Accept(this);
        this->DecreaseIndentation();
        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    void Visit(IfStatementNode *lpNode) override {
        (void)lpNode;
        if (lpNode->thenBranch == nullptr && lpNode->elseBranch == nullptr) {
            buffer << this->GetIndentation()
                   << "--[[ Fission: conditional branches not lifted. This could indicate bad decompilation if the instructions inside these branches modified "
                      "outer state. ]]";
            this->NextLine();
            return;
        }
        if (lpNode->thenBranch != nullptr) {
            buffer << this->GetIndentation() << "if ";
            lpNode->condition->Accept(this);
            buffer << " then";
            this->NextLine();
            this->IncreaseIndentation();
            lpNode->thenBranch->Accept(this);
            if (lpNode->elseBranch != nullptr && !lpNode->elseBranch->body.empty()) {
                this->DecreaseIndentation();
                buffer << this->GetIndentation() << "else";
                this->NextLine();
                this->IncreaseIndentation();
                lpNode->elseBranch->Accept(this);
            }

            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "end";
            this->NextLine();
        } else {
            buffer << this->GetIndentation() << "if ";
            lpNode->condition->Accept(this);
            buffer << " then";
            this->NextLine();
            this->IncreaseIndentation();
            lpNode->elseBranch->Accept(this);
            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "end";
            this->NextLine();
        }
    }

    void Visit(AssignmentStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        // LHS is a write target (Identifier / IndexExpression / MemberExpression);
        // never needs precedence wrapping. RHS is at statement context, so reset
        // the minimum precedence — any binary expression there is unambiguous.
        EmitWithPrecedence(0, lpNode->left.get());
        buffer << " = ";
        EmitWithPrecedence(0, lpNode->right.get());
        this->NextLine();
    }

    void Visit(BinaryExpressionNode *lpNode) override {
        (void)lpNode;
        const int prec = OperatorPrecedence(lpNode->op);
        const bool rightAssoc = IsRightAssociative(lpNode->op);
        // Wrap when our precedence is lower than the surrounding context
        // requires. Equal precedence is fine when associativity matches.
        const bool wrap = prec < m_minPrecedence;
        if (wrap)
            buffer << "(";
        // Left child of a left-associative op may keep equal precedence; for a
        // right-associative op it must be strictly higher (so `a^b^c` keeps the
        // parenthesisation that mirrors the parse tree).
        int leftMin = rightAssoc ? prec + 1 : prec;
        // Right child mirrors: left-assoc requires strictly higher precedence
        // on the right, right-assoc allows equal.
        int rightMin = rightAssoc ? prec : prec + 1;

        // For associative operators (`or`, `and`, `+`, `*`, `..`) a flat chain
        // like `a or b or c` does not change meaning regardless of grouping, so
        // skip parens around an identically-keyed child on the "tight" side.
        if (IsAssociative(lpNode->op)) {
            if (auto rb = std::dynamic_pointer_cast<BinaryExpressionNode>(lpNode->right); rb && rb->op == lpNode->op)
                rightMin = prec;
            if (auto lb = std::dynamic_pointer_cast<BinaryExpressionNode>(lpNode->left); lb && lb->op == lpNode->op)
                leftMin = prec;
        }

        EmitWithPrecedence(leftMin, lpNode->left.get());
        buffer << " " << lpNode->op << " ";
        EmitWithPrecedence(rightMin, lpNode->right.get());
        if (wrap)
            buffer << ")";
    }

    void Visit(StringLiteralNode *lpNode) override {
        (void)lpNode;

        if (lpNode->bUseParenthesis)
            buffer << "(";
        if (lpNode->value.find('\n') != std::string::npos) {
            buffer << "[[" << lpNode->value << "]]";
            if (lpNode->bUseParenthesis)
                buffer << ")";
            return;
        }

        EmitQuotedString(lpNode->value);

        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(NumberLiteralNode *lpNode) override {
        (void)lpNode;
        auto num = std::format(
            "{:.{}f}", std::stod(std::format("{:.{}f}", lpNode->value, std::numeric_limits<double>::max_digits10)), std::numeric_limits<double>::max_digits10
        );
        const auto dot = num.find('.');
        auto resultLength = num.length();
        if (dot != std::string::npos)
            if (const auto lastThatIsNotZero = num.find_last_not_of('0'); lastThatIsNotZero != std::string::npos && lastThatIsNotZero >= dot)
                resultLength = lastThatIsNotZero + (lastThatIsNotZero != dot);
        num.resize(resultLength);
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << num;
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(BooleanLiteralNode *lpNode) override {
        (void)lpNode;
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << (lpNode->value ? "true" : "false");
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(IdentifierExpressionNode *lpNode) override {
        (void)lpNode;
        lpNode->identifier->Accept(this);
    }

    std::string GenerateSource(RootNode *lpRoot) {
        lpRoot->Accept(this);
        return buffer.str();
    }

    void Visit(VariableDeclarationNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        buffer << "local ";
        lpNode->identifier->Accept(this);
        if (lpNode->value != nullptr) {
            buffer << " = ";
            lpNode->value->Accept(this);
        }
        this->NextLine();
    }
    void Visit(NilLiteralNode *lpNode) override {
        (void)lpNode;
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << "nil";
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(TableLiteralNode *lpNode) override {
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << "{ ";
        for (size_t i = 0; i < lpNode->expressions.size(); i++) {
            lpNode->expressions.at(i)->Accept(this);
            if (i < lpNode->expressions.size() - 1)
                buffer << ", ";
        }
        buffer << " }";
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(NameCallExpressionNode *lpNode) override {
        if (lpNode->rets.empty()) {
            if (!lpNode->inlineCall)
                buffer << this->GetIndentation();
            lpNode->calledOn->Accept(this);
            buffer << ":";
            lpNode->callWhat->Accept(this);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }

            buffer << ")";

            if (!lpNode->inlineCall)
                this->NextLine();
            return;
        }
        buffer << this->GetIndentation();
        buffer << "local ";
        for (size_t i = 0; i < lpNode->rets.size(); i++) {
            lpNode->rets.at(i)->Accept(this);
            if (i < lpNode->rets.size() - 1)
                buffer << ", ";
        }
        buffer << " = ";
        lpNode->calledOn->Accept(this);
        buffer << ":";
        lpNode->callWhat->Accept(this);
        buffer << "(";
        for (size_t i = 0; i < lpNode->arguments.size(); i++) {
            lpNode->arguments.at(i)->Accept(this);
            if (i < lpNode->arguments.size() - 1)
                buffer << ", ";
        }

        buffer << ")";

        if (!lpNode->inlineCall)
            this->NextLine();
    }

    void Visit(ForNumericNode *lpNode) override {
        buffer << this->GetIndentation();
        buffer << "for ";
        lpNode->loopVariable->Accept(this);
        buffer << " = ";
        lpNode->startVariable->Accept(this);
        buffer << ", ";
        lpNode->maxIncreased->Accept(this);
        buffer << ", ";
        lpNode->increaseBy->Accept(this);
        buffer << " do";
        this->NextLine();
        this->IncreaseIndentation();
        if (lpNode->lpLoopBody != nullptr)
            lpNode->lpLoopBody->Accept(this);
        this->DecreaseIndentation();
        buffer << this->GetIndentation();
        buffer << "end";
        this->NextLine();
    }

    void Visit(ForGeneralNode *lpNode) override {
        buffer << this->GetIndentation() << "for ";
        for (size_t i = 0; i < lpNode->loopVariables.size(); ++i) {
            if (i > 0)
                buffer << ", ";
            lpNode->loopVariables[i]->Accept(this);
        }
        buffer << " in ";
        if (lpNode->index != nullptr) {
            lpNode->generator->Accept(this);
            buffer << ", ";
            lpNode->state->Accept(this);
            buffer << ", ";
            lpNode->index->Accept(this);
        } else {
            lpNode->generator->Accept(this);
        }
        buffer << " do";
        this->NextLine();
        this->IncreaseIndentation();
        if (lpNode->body != nullptr)
            lpNode->body->Accept(this);
        this->DecreaseIndentation();
        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    void Visit(CompoundBinaryExpressionNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        lpNode->left->Accept(this);
        buffer << " " << lpNode->op << "= ";
        lpNode->right->Accept(this);
        this->NextLine();
    }

    void Visit(RepeatStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "repeat";
        this->NextLine();
        this->IncreaseIndentation();
        lpNode->body->Accept(this);
        this->DecreaseIndentation();
        this->NextLine();
        buffer << this->GetIndentation() << "until (";
        lpNode->condition->Accept(this);
        buffer << ")";
        this->NextLine();
    }

    void Visit(VarArgExpression *lpNode) override {
        (void)lpNode;
        buffer << "..."; /* legitimately. */
    }

    void Visit(TableBinaryExpressionNode *lpNode) override {
        // in binary expressions present in tables, the key may require to be wrapped in [], or else it will not be real compilable code.
        (void)lpNode;
        buffer << "[";
        lpNode->left->Accept(this);
        buffer << "] " << lpNode->op << " (";
        lpNode->right->Accept(this);
        buffer << ")";
    }
    void Visit(IntegerLiteralNode *lpNode) override {
        (void)lpNode;
        auto num = std::format(
            "{}", std::stod(std::format("{}", lpNode->value, std::numeric_limits<int64_t>::max_digits10)), std::numeric_limits<int64_t>::max_digits10
        );
        const auto dot = num.find('.');
        auto resultLength = num.length();
        if (dot != std::string::npos)
            if (const auto lastThatIsNotZero = num.find_last_not_of('0'); lastThatIsNotZero != std::string::npos && lastThatIsNotZero >= dot)
                resultLength = lastThatIsNotZero + (lastThatIsNotZero != dot);
        num.resize(resultLength);
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << num;
        buffer << 'i';
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }
};
