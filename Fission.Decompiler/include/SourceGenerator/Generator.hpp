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

    // min precedence ctx wants; child lower than this wraps in parens. 0 = stmt ctx.
    int m_minPrecedence = 0;

    // luau precedence table (low num = low prec), matches lparser.cpp. unary at 7.
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

    static bool IsRightAssociative(const std::string &op) { return op == ".." || op == "^"; }

    // `..` treated as associative so flat concat chains don't gain redundant parens.
    static bool IsAssociative(const std::string &op) { return op == "or" || op == "and" || op == "+" || op == "*" || op == ".."; }

    // emit child with pushed min precedence, restore after. use for every expr child Accept.
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

        // anon inline `function(args) ... end` at expr site. no indent/local/name, caller does punctuation.
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

    // drops informational comments only; warnings still emitted.
    bool bOmitInformationalComments = false;

    void Visit(CommentNode *lpNode) override {

        if (bOmitInformationalComments && lpNode->bIsInformational)
            return;

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
                if (lpNode->bIsLocalDeclaration)
                    buffer << "local ";
                for (size_t i = 0; i < lpNode->rets.size(); i++) {
                    lpNode->rets.at(i)->Accept(this);
                    if (lpNode->bIsLocalDeclaration && i < lpNode->retTypes.size() && lpNode->retTypes[i] != nullptr) {
                        buffer << ": ";
                        lpNode->retTypes[i]->Accept(this);
                    }
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
        // unary binds tighter than everything but `^`, so wrap lower-prec operand.
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
        // Walk left chain `a.b.c.d.e.f` iteratively. Recursive lpNode->table->Accept
        // stacks one frame per hop and blows the stack on long Roblox lookup chains
        // (e.g. game.Workspace.PlayerScripts.X.Y.Z.W...).
        std::vector<MemberExpressionNode *> chain;
        MemberExpressionNode *cur = lpNode;
        while (cur != nullptr) {
            chain.push_back(cur);
            auto inner = std::dynamic_pointer_cast<MemberExpressionNode>(cur->table);
            if (!inner)
                break;
            cur = inner.get();
        }

        // emit innermost base once (non-MemberExpression subtree), then unwind suffixes.
        MemberExpressionNode *innermost = chain.back();
        if (innermost->table != nullptr)
            innermost->table->Accept(this);

        // Each MemberExpressionNode emits its own suffix: `.name` (legal ident with table),
        // empty (StringLiteral-but-not-legal-ident with table → preserves original `return`-without-emit
        // semantics), or `[key]` otherwise.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            MemberExpressionNode *node = *it;
            if (node->table != nullptr) {
                if (auto lpStringLiteral = std::dynamic_pointer_cast<StringLiteralNode>(node->key)) {
                    if (IsLegalLuauIndex(lpStringLiteral->value))
                        buffer << "." << lpStringLiteral->value;
                    continue;
                }
            }
            buffer << "[";
            node->key->Accept(this);
            buffer << "]";
        }
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
        // Only-else form (no then branch): render as-is.
        if (lpNode->thenBranch == nullptr) {
            buffer << this->GetIndentation() << "if ";
            lpNode->condition->Accept(this);
            buffer << " then";
            this->NextLine();
            this->IncreaseIndentation();
            lpNode->elseBranch->Accept(this);
            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "end";
            this->NextLine();
            return;
        }

        // walk if/elseif/else iteratively: else == single if -> render `elseif`, not nested.
        IfStatementNode *cur = lpNode;
        bool first = true;
        while (true) {
            buffer << this->GetIndentation() << (first ? "if " : "elseif ");
            first = false;
            cur->condition->Accept(this);
            buffer << " then";
            this->NextLine();
            this->IncreaseIndentation();
            if (cur->thenBranch != nullptr)
                cur->thenBranch->Accept(this);
            this->DecreaseIndentation();

            IfStatementNode *nextIf = nullptr;
            if (cur->elseBranch != nullptr && cur->elseBranch->body.size() == 1)
                if (auto n = std::dynamic_pointer_cast<IfStatementNode>(cur->elseBranch->body[0]); n && n->thenBranch != nullptr)
                    nextIf = n.get();

            if (nextIf != nullptr) {
                cur = nextIf;
                continue;
            }

            if (cur->elseBranch != nullptr && !cur->elseBranch->body.empty()) {
                buffer << this->GetIndentation() << "else";
                this->NextLine();
                this->IncreaseIndentation();
                cur->elseBranch->Accept(this);
                this->DecreaseIndentation();
            }
            break;
        }

        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    void Visit(AssignmentStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        // LHS is a write target, never wraps. RHS at stmt ctx so reset min prec.
        EmitWithPrecedence(0, lpNode->left.get());
        buffer << " = ";
        EmitWithPrecedence(0, lpNode->right.get());
        this->NextLine();
    }

    void Visit(BinaryExpressionNode *lpNode) override {
        (void)lpNode;
        // Walk the left spine iteratively: chained binops `a+b+c+d+...` Visit-recurse one frame
        // per link via left->Accept and blow the stack on deeply-nested expressions emitted by
        // optimised/obfuscated bytecode. Collect spine entries top-down, then emit bottom-up.
        const int savedMin = m_minPrecedence;
        struct SpineEntry {
            BinaryExpressionNode *node;
            int leftMin;
            int rightMin;
            bool wrap;
        };
        std::vector<SpineEntry> spine;
        BinaryExpressionNode *cur = lpNode;
        while (true) {
            const int prec = OperatorPrecedence(cur->op);
            const bool rightAssoc = IsRightAssociative(cur->op);
            const bool wrap = prec < m_minPrecedence;
            int leftMin = rightAssoc ? prec + 1 : prec;
            int rightMin = rightAssoc ? prec : prec + 1;
            if (IsAssociative(cur->op)) {
                if (auto rb = std::dynamic_pointer_cast<BinaryExpressionNode>(cur->right); rb && rb->op == cur->op)
                    rightMin = prec;
                if (auto lb = std::dynamic_pointer_cast<BinaryExpressionNode>(cur->left); lb && lb->op == cur->op)
                    leftMin = prec;
            }
            spine.push_back({cur, leftMin, rightMin, wrap});
            auto lb = std::dynamic_pointer_cast<BinaryExpressionNode>(cur->left);
            if (!lb)
                break;
            // mirror the recursive Accept's m_minPrecedence push so inner wrap decisions match.
            m_minPrecedence = leftMin;
            cur = lb.get();
        }

        for (auto it = spine.begin(); it != spine.end(); ++it)
            if (it->wrap)
                buffer << "(";

        const auto &innermost = spine.back();
        m_minPrecedence = innermost.leftMin;
        innermost.node->left->Accept(this);
        for (auto it = spine.rbegin(); it != spine.rend(); ++it) {
            buffer << " " << it->node->op << " ";
            EmitWithPrecedence(it->rightMin, it->node->right.get());
            if (it->wrap)
                buffer << ")";
        }

        m_minPrecedence = savedMin;
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
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << std::format("{}", lpNode->value);
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
        if (lpNode->type) {
            buffer << ": ";
            lpNode->type.value()->Accept(this);
        }
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
            if (auto lpBinExpr = std::dynamic_pointer_cast<BinaryExpressionNode>(lpNode->expressions.at(i)); lpBinExpr) {
                if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(lpBinExpr->left)) {
                    buffer << "[";
                    lpBinExpr->left->Accept(this);
                    buffer << "] ";
                    buffer << lpBinExpr->op;
                    buffer << " ";
                    lpBinExpr->right->Accept(this);
                } else {
                    lpNode->expressions.at(i)->Accept(this);
                }
            } else {
                lpNode->expressions.at(i)->Accept(this);
            }
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
        if (lpNode->bIsLocalDeclaration)
            buffer << "local ";
        for (size_t i = 0; i < lpNode->rets.size(); i++) {
            lpNode->rets.at(i)->Accept(this);
            if (lpNode->bIsLocalDeclaration && i < lpNode->retTypes.size() && lpNode->retTypes[i] != nullptr) {
                buffer << ": ";
                lpNode->retTypes[i]->Accept(this);
            }
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
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << std::format("{}i", lpNode->value);
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(VectorNode *lpNode) override {
        const float x = lpNode->x;
        const float y = lpNode->y;
        const float z = lpNode->z;

        if (x == 0 && y == 0 && z == 0) {
            buffer << "Vector3.zero";
        }
        else if (x == 1 && y == 0 && z == 0) {
            buffer << "Vector3.xAxis";
        }
        else if (x == -1 && y == 0 && z == 0) {
            buffer << "-Vector3.xAxis";
        }
        else if (x == 0 && y == 1 && z == 0) {
            buffer << "Vector3.yAxis";
        }
        else if (x == 0 && y == -1 && z == 0) {
            buffer << "-Vector3.yAxis";
        }
        else if (x == 0 && y == 0 && z == 1) {
            buffer << "Vector3.zAxis";
        }
        else if (x == 0 && y == 0 && z == -1) {
            buffer << "-Vector3.zAxis";
        }
        else {
            buffer << "Vector3.new(" << std::format("{}", x) << ", " << std::format("{}", y) << ", " << std::format("{}", z) << ")";
        }
    }
};
