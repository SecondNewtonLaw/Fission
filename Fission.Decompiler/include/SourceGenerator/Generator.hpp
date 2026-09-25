//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "AbstractSyntaxTree/Visitor.hpp"
#include "SafetyGuard.hpp"

#include <bit>
#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

class SourceGenerator : public Visitor {
  public:
    std::stringstream buffer;
    size_t dwIndentationLevel = 0;
    static constexpr size_t kIndentationSpaceCount = 4;
    // Split table literals that exceed this column.
    static constexpr size_t kTableInlineColumnLimit = 80;

    // Wrap children below this precedence; zero denotes statement context.
    int m_minPrecedence = 0;

    // Luau rejects a disambiguating semicolon before a block's first statement.
    bool m_firstStmtInBlock = false;

    // Cache table renderings and bound hostile or cyclic nesting.
    static constexpr int kMaxTableDepth = 256;
    int m_tableDepth = 0;
    std::unordered_map<const TableLiteralNode *, std::string> m_tableInlineCache;
    std::string m_vectorConstructorAlias;
    bool m_sawVectorConstant = false;

    bool SawVectorConstant() const { return m_sawVectorConstant; }

    // Luau precedence from lparser.cpp; lower numbers bind less tightly.
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

    // Treat concatenation chains as associative for rendering.
    static bool IsAssociative(const std::string &op) { return op == "or" || op == "and" || op == ".."; }

    // Emit a child under a temporary minimum precedence.
    template <typename Node> void EmitWithPrecedence(int minPrec, Node *child) {
        const int saved = m_minPrecedence;
        m_minPrecedence = minPrec;
        child->Accept(this);
        m_minPrecedence = saved;
    }

    // Parenthesize non-prefix expressions before index, member, call, or method suffixes.
    static bool IsPrefixSafe(const std::shared_ptr<Expression> &e) {
        return std::dynamic_pointer_cast<IdentifierExpressionNode>(e) != nullptr || std::dynamic_pointer_cast<IndexExpressionNode>(e) != nullptr ||
               std::dynamic_pointer_cast<MemberExpressionNode>(e) != nullptr || std::dynamic_pointer_cast<CallExpressionNode>(e) != nullptr ||
               std::dynamic_pointer_cast<NameCallExpressionNode>(e) != nullptr;
    }

    static bool ShouldEmitTypeAnnotation(const std::shared_ptr<Expression> &type) {
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(type);
        return !id || !id->identifier || id->identifier->name != "nil";
    }
    void EmitPrefix(const std::shared_ptr<Expression> &e) {
        if (e == nullptr || IsPrefixSafe(e)) {
            if (e != nullptr)
                e->Accept(this);
            return;
        }
        buffer << "(";
        EmitWithPrecedence(0, e.get());
        buffer << ")";
    }

    // A parenthesized statement can attach to the preceding line as call arguments.
    bool StartsWithOpenParen(const std::shared_ptr<Expression> &e) {
        if (e == nullptr)
            return false;
        if (!IsPrefixSafe(e))
            return true;
        if (auto m = std::dynamic_pointer_cast<MemberExpressionNode>(e))
            return StartsWithOpenParen(m->table);
        if (auto i = std::dynamic_pointer_cast<IndexExpressionNode>(e))
            return StartsWithOpenParen(i->left);
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(e))
            return StartsWithOpenParen(c->callee);
        if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(e))
            return StartsWithOpenParen(n->calledOn);
        return false;
    }

    std::string GetIndentation() { return std::string(this->dwIndentationLevel * kIndentationSpaceCount, ' '); } // NOLINT(*-return-braced-init-list)

    void NextLine() { buffer << "\n"; }
    void IncreaseIndentation() { this->dwIndentationLevel++; }
    void DecreaseIndentation() {
        ASSERT(this->dwIndentationLevel > 0, "indentation out of range. Overpopped");
        this->dwIndentationLevel--;
    }

    static bool IsReservedLuauWord(const std::string &s) {
        static const std::unordered_set<std::string> kKeywords = {"and", "break",    "do",     "else", "elseif", "end",   "false",
                                                                  "for", "function", "if",     "in",   "local",  "nil",   "not",
                                                                  "or",  "repeat",   "return", "then", "true",   "until", "while"};
        return kKeywords.contains(s);
    }

    bool IsLegalLuauIndex(const std::string &str) {
        if (str.empty() || isdigit(static_cast<unsigned char>(str[0])))
            return false;
        if (IsReservedLuauWord(str)) // `t.function` is illegal; caller falls back to `t["function"]`
            return false;
        return std::ranges::all_of(str, [](const char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; });
    }

    // Return a valid multi-byte UTF-8 sequence length, or zero.
    static int ValidUtf8Length(const std::string &v, size_t i) {
        const size_t n = v.size();
        const auto at = [&](size_t k) { return static_cast<unsigned char>(v[k]); };
        const auto cont = [&](size_t k) { return k < n && (at(k) & 0xC0) == 0x80; };
        const unsigned char c = at(i);
        if (c < 0x80)
            return 0;
        if (c >= 0xC2 && c <= 0xDF)
            return cont(i + 1) ? 2 : 0;
        if (c == 0xE0)
            return (i + 2 < n && at(i + 1) >= 0xA0 && at(i + 1) <= 0xBF && cont(i + 2)) ? 3 : 0; // no overlong
        if (c >= 0xE1 && c <= 0xEC)
            return (cont(i + 1) && cont(i + 2)) ? 3 : 0;
        if (c == 0xED)
            return (i + 2 < n && at(i + 1) >= 0x80 && at(i + 1) <= 0x9F && cont(i + 2)) ? 3 : 0; // no surrogates
        if (c >= 0xEE && c <= 0xEF)
            return (cont(i + 1) && cont(i + 2)) ? 3 : 0;
        if (c == 0xF0)
            return (i + 3 < n && at(i + 1) >= 0x90 && at(i + 1) <= 0xBF && cont(i + 2) && cont(i + 3)) ? 4 : 0; // no overlong
        if (c >= 0xF1 && c <= 0xF3)
            return (cont(i + 1) && cont(i + 2) && cont(i + 3)) ? 4 : 0;
        if (c == 0xF4)
            return (i + 3 < n && at(i + 1) >= 0x80 && at(i + 1) <= 0x8F && cont(i + 2) && cont(i + 3)) ? 4 : 0; // <= U+10FFFF
        return 0; // lone continuation (0x80-0xBF), 0xC0/0xC1, 0xF5-0xFF
    }

    void EmitQuotedString(const std::string &value) {
        buffer << "\"";
        const size_t n = value.size();
        for (size_t i = 0; i < n;) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            switch (c) {
            case '\\':
                buffer << "\\\\";
                ++i;
                continue;
            case '"':
                buffer << "\\\"";
                ++i;
                continue;
            case '\n':
                buffer << "\\n";
                ++i;
                continue;
            case '\r':
                buffer << "\\r";
                ++i;
                continue;
            case '\t':
                buffer << "\\t";
                ++i;
                continue;
            default:
                break;
            }
            // Escape control bytes and DEL as fixed-width decimals.
            if (c < 0x20 || c == 0x7f) {
                buffer << std::format("\\{:03}", static_cast<int>(c));
                ++i;
                continue;
            }
            if (c < 0x80) { // printable ASCII
                buffer << static_cast<char>(c);
                ++i;
                continue;
            }
            // Preserve valid UTF-8; escape lone high bytes without changing string bytes.
            if (const int len = ValidUtf8Length(value, i); len > 0) {
                buffer.write(value.data() + i, len);
                i += static_cast<size_t>(len);
            } else {
                buffer << std::format("\\{:03}", static_cast<int>(c));
                ++i;
            }
        }
        buffer << "\"";
    }

    void Visit(NoExpressionNode *lpNode) override { (void)lpNode; }

    void Visit(RootNode *lpNode) override {
        (void)lpNode;
        // Header comments do not end first-statement context.
        bool first = true;
        for (const auto &body : lpNode->programBody) {
            if (first && !m_vectorConstructorAlias.empty() && !std::dynamic_pointer_cast<CommentNode>(body)) {
                buffer << "local " << m_vectorConstructorAlias << " = Vector3.new\n";
                first = false;
            }
            m_firstStmtInBlock = first;
            body->Accept(this);
            if (!std::dynamic_pointer_cast<CommentNode>(body))
                first = false;
        }
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

        // Caller owns punctuation around an inline anonymous function.
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

    // Emit Luau class syntax.
    void Visit(ClassDeclarationNode *lpNode) override {
        buffer << this->GetIndentation();
        if (lpNode->bExported)
            buffer << "export ";
        if (lpNode->bOpen)
            buffer << "open ";
        buffer << "class " << lpNode->className;
        if (lpNode->superclass) {
            buffer << " extends ";
            EmitPrefix(lpNode->superclass);
        }
        this->NextLine();

        this->IncreaseIndentation();
        for (const auto &prop : lpNode->propertyNames) {
            buffer << this->GetIndentation() << "public " << prop;
            this->NextLine();
        }
        for (const auto &method : lpNode->methods)
            if (method)
                method->Accept(this); // renders `function name(self, ...) ... end` at the class-body indent.
        this->DecreaseIndentation();

        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    // Drop informational comments but retain warnings.
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
        if (lpNode->type && ShouldEmitTypeAnnotation(*lpNode->type)) {
            buffer << ": ";
            lpNode->type.value()->Accept(this);
        }
    }

    void Visit(CallExpressionNode *lpNode) override {
        (void)lpNode;
        if (!lpNode->inlineCall) {
            buffer << this->GetIndentation();
            if (!m_firstStmtInBlock && lpNode->rets.empty() && StartsWithOpenParen(lpNode->callee))
                buffer << ";"; // a bare `(expr)(...)` statement would merge with the previous line
            if (!lpNode->rets.empty()) {
                if (lpNode->bIsLocalDeclaration)
                    buffer << "local ";
                for (size_t i = 0; i < lpNode->rets.size(); i++) {
                    lpNode->rets.at(i)->Accept(this);
                    if (lpNode->bIsLocalDeclaration && i < lpNode->retTypes.size() && lpNode->retTypes[i] != nullptr &&
                        ShouldEmitTypeAnnotation(lpNode->retTypes[i])) {
                        buffer << ": ";
                        lpNode->retTypes[i]->Accept(this);
                    }
                    if (i < lpNode->rets.size() - 1)
                        buffer << ", ";
                }
                buffer << " = ";
            }
            EmitPrefix(lpNode->callee);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }

            buffer << ")";
            this->NextLine();
        } else {
            if (lpNode->bAdjustToOne)
                buffer << "("; // truncate a multiret call to one value in a spread position
            EmitPrefix(lpNode->callee);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }
            buffer << ")";
            if (lpNode->bAdjustToOne)
                buffer << ")";
        }
    }

    void Visit(UnaryExpressionNode *lpNode) override {
        (void)lpNode;
        constexpr int kUnaryPrec = 7;
        const bool wrap = kUnaryPrec < m_minPrecedence;
        if (wrap)
            buffer << "(";
        // Separate adjacent minus tokens; Luau lexes `--` as a comment.
        std::stringstream operandBuf;
        operandBuf.swap(buffer);
        // Unary operators bind tighter than every operator except exponentiation.
        EmitWithPrecedence(kUnaryPrec, lpNode->operand.get());
        operandBuf.swap(buffer);
        const std::string operand = operandBuf.str();
        buffer << lpNode->op;
        if (!lpNode->op.empty() && lpNode->op.back() == '-' && !operand.empty() && operand.front() == '-')
            buffer << ' ';
        const bool integerNegation = lpNode->op == "-" && dynamic_cast<IntegerLiteralNode *>(lpNode->operand.get());
        if (integerNegation)
            buffer << '(';
        buffer << operand;
        if (integerNegation)
            buffer << ')';
        if (wrap)
            buffer << ")";
    }

    void Visit(IndexExpressionNode *lpNode) override {
        (void)lpNode;
        EmitPrefix(lpNode->left);
        buffer << "[";
        if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(lpNode->right))
            EmitQuotedString(str->value);
        else
            EmitWithPrecedence(11, lpNode->right.get());
        buffer << "]";
    }

    void Visit(MemberExpressionNode *lpNode) override {
        (void)lpNode;
        // Walk long member chains iteratively to bound stack use.
        std::vector<MemberExpressionNode *> chain;
        MemberExpressionNode *cur = lpNode;
        while (cur != nullptr) {
            chain.push_back(cur);
            auto inner = std::dynamic_pointer_cast<MemberExpressionNode>(cur->table);
            if (!inner)
                break;
            cur = inner.get();
        }

        // Emit the base once, then unwind suffixes.
        MemberExpressionNode *innermost = chain.back();
        if (innermost->table != nullptr)
            EmitPrefix(innermost->table);

        // Use bracket syntax for keys that are not legal identifiers.
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            MemberExpressionNode *node = *it;
            if (node->table != nullptr) {
                if (auto lpStringLiteral = std::dynamic_pointer_cast<StringLiteralNode>(node->key);
                    lpStringLiteral && IsLegalLuauIndex(lpStringLiteral->value)) {
                    buffer << "." << lpStringLiteral->value;
                    continue;
                }
            }
            buffer << "[";
            if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(node->key))
                EmitQuotedString(str->value);
            else
                EmitWithPrecedence(11, node->key.get());
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
        // Preserve explicit register-reuse scopes.
        const bool asDo = lpNode->bEmitAsDoBlock;
        if (asDo) {
            buffer << this->GetIndentation() << "do";
            this->NextLine();
            this->IncreaseIndentation();
        }
        // Leading comments do not end first-statement context.
        bool first = true;
        for (const auto &node : lpNode->body) {
            m_firstStmtInBlock = first;
            node->Accept(this);
            if (!std::dynamic_pointer_cast<CommentNode>(node))
                first = false;
            // a block must end at break/continue/return; anything after is unreachable
            if (node->nodeKind == ASTNodeKind::BreakStatement || node->nodeKind == ASTNodeKind::ContinueStatement ||
                std::dynamic_pointer_cast<ReturnStatementNode>(node))
                break;
        }
        if (asDo) {
            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "end";
            this->NextLine();
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
        // Preserve an else-only recovered branch.
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

        // Flatten single-if else branches into elseif chains.
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
        if (!m_firstStmtInBlock && StartsWithOpenParen(lpNode->left))
            buffer << ";"; // `(expr).f = v` as a statement would merge with the previous line
        // Assignment targets never wrap; values use statement precedence.
        EmitWithPrecedence(0, lpNode->left.get());
        buffer << " = ";
        EmitWithPrecedence(0, lpNode->right.get());
        this->NextLine();
    }

    void Visit(BinaryExpressionNode *lpNode) override {
        (void)lpNode;
        // Walk deep binary left spines iteratively to bound stack use.
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
                // `(a .. b) .. c` concatenates `a .. b` first; without parentheses it would reparse as `a .. (b .. c)`
                if (auto lb = std::dynamic_pointer_cast<BinaryExpressionNode>(cur->left); lb && lb->op == cur->op && !rightAssoc)
                    leftMin = prec;
            }
            spine.push_back({cur, leftMin, rightMin, wrap});
            auto lb = std::dynamic_pointer_cast<BinaryExpressionNode>(cur->left);
            if (!lb)
                break;
            // Match recursive precedence propagation.
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

    void Visit(IfExpressionNode *lpNode) override {
        // If-expressions need parentheses inside operators; nested else arms form elseif chains.
        const bool wrap = m_minPrecedence > 0;
        if (wrap)
            buffer << "(";
        IfExpressionNode *cur = lpNode;
        buffer << "if ";
        EmitWithPrecedence(1, cur->condition.get());
        buffer << " then ";
        EmitWithPrecedence(1, cur->thenExpr.get());
        while (auto elseIf = std::dynamic_pointer_cast<IfExpressionNode>(cur->elseExpr)) {
            buffer << " elseif ";
            EmitWithPrecedence(1, elseIf->condition.get());
            buffer << " then ";
            EmitWithPrecedence(1, elseIf->thenExpr.get());
            cur = elseIf.get();
        }
        buffer << " else ";
        EmitWithPrecedence(1, cur->elseExpr.get());
        if (wrap)
            buffer << ")";
    }

    void Visit(StringLiteralNode *lpNode) override {
        (void)lpNode;

        if (lpNode->bUseParenthesis)
            buffer << "(";

        // Use long brackets only when contents cannot terminate or invalidate the literal.
        const std::string &v = lpNode->value;
        // A trailing bracket would fuse with the delimiter.
        const bool longSafe = v.find('\n') != std::string::npos && v.front() != '\n' && v.find("]]") == std::string::npos && v.back() != ']' &&
                              std::ranges::all_of(v, [](unsigned char c) { return c >= 0x20 || c == '\n' || c == '\t'; });
        if (longSafe)
            buffer << "[[" << v << "]]";
        else
            EmitQuotedString(v);

        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(NumberLiteralNode *lpNode) override {
        if (lpNode->bUseParenthesis)
            buffer << "(";
        // Floating-point predicates can fold to false under fast-math.
        const uint64_t bits = std::bit_cast<uint64_t>(lpNode->value);
        const uint64_t magnitude = bits & 0x7fffffffffffffffULL;
        if (magnitude > 0x7ff0000000000000ULL)
            buffer << "(0 / 0)";
        else if (magnitude == 0x7ff0000000000000ULL)
            buffer << ((bits >> 63) ? "(-1 / 0)" : "(1 / 0)");
        else
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

    std::string GenerateSource(RootNode *lpRoot, std::string vectorConstructorAlias = {}) {
        // A SourceGenerator instance is reused across decompiles.
        buffer.str("");
        buffer.clear();
        m_vectorConstructorAlias = std::move(vectorConstructorAlias);
        m_sawVectorConstant = false;
        dwIndentationLevel = 0;
        m_minPrecedence = 0;
        m_tableDepth = 0;
        m_tableInlineCache.clear();
        lpRoot->Accept(this);
        return buffer.str();
    }

    void Visit(VariableDeclarationNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation();
        buffer << "local ";
        lpNode->identifier->Accept(this);
        if (lpNode->type && ShouldEmitTypeAnnotation(*lpNode->type)) {
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

    // Table entries use primary-expression precedence.
    void EmitTableEntry(const std::shared_ptr<Expression> &entry) {
        // Only assignment nodes encode keyed entries; other binary expressions remain values.
        if (auto lpBinExpr = std::dynamic_pointer_cast<BinaryExpressionNode>(entry); lpBinExpr && lpBinExpr->op == "=") {
            if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(lpBinExpr->left)) {
                buffer << "[";
                EmitQuotedString(str->value);
                buffer << "] " << lpBinExpr->op << " ";
                lpBinExpr->right->Accept(this);
                return;
            }
        }
        entry->Accept(this);
    }

    // Render entries separately to choose inline or multi-line layout.
    std::string RenderTableEntriesInline(const std::vector<std::shared_ptr<Expression>> &entries) {
        std::stringstream scratch;
        scratch.swap(buffer); // buffer now empty; the real output is parked in `scratch`
        const int savedMin = m_minPrecedence;
        m_minPrecedence = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            EmitTableEntry(entries[i]);
            if (i + 1 < entries.size())
                buffer << ", ";
        }
        m_minPrecedence = savedMin;
        std::string rendered = buffer.str();
        buffer.swap(scratch); // restore the real output stream
        return rendered;
    }

    void Visit(TableLiteralNode *lpNode) override {
        // Enforce the decompile budget during table measurement.
        Fission::CheckDecompileDeadline();
        if (lpNode->bUseParenthesis)
            buffer << "(";

        if (lpNode->expressions.empty()) {
            buffer << "{  }";
            if (lpNode->bUseParenthesis)
                buffer << ")";
            return;
        }

        if (m_tableDepth >= kMaxTableDepth)
            throw Fission::DecompilerError("table nesting too deep");
        ++m_tableDepth;

        // Cache inline forms so nested width measurement remains linear.
        std::string inlineForm;
        if (const auto it = m_tableInlineCache.find(lpNode); it != m_tableInlineCache.end()) {
            inlineForm = it->second;
        } else {
            inlineForm = RenderTableEntriesInline(lpNode->expressions);
            m_tableInlineCache.emplace(lpNode, inlineForm);
        }
        const size_t indentWidth = dwIndentationLevel * kIndentationSpaceCount;
        // Split nested blocks or forms that exceed the estimated column limit.
        const bool multiline = inlineForm.find('\n') != std::string::npos || (indentWidth + inlineForm.size() + 4) > kTableInlineColumnLimit;

        if (!multiline) {
            buffer << "{ " << inlineForm << " }";
        } else {
            buffer << "{";
            this->NextLine();
            this->IncreaseIndentation();
            const int savedMin = m_minPrecedence;
            m_minPrecedence = 0;
            for (const auto &entry : lpNode->expressions) {
                buffer << this->GetIndentation();
                EmitTableEntry(entry);
                buffer << ","; // trailing comma on each line; luau permits a dangling separator
                this->NextLine();
            }
            m_minPrecedence = savedMin;
            this->DecreaseIndentation();
            buffer << this->GetIndentation() << "}";
        }

        --m_tableDepth;
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(NameCallExpressionNode *lpNode) override {
        if (lpNode->rets.empty()) {
            if (!lpNode->inlineCall)
                buffer << this->GetIndentation();
            if (!m_firstStmtInBlock && !lpNode->inlineCall && StartsWithOpenParen(lpNode->calledOn))
                buffer << ";"; // a bare `(expr):m(...)` statement would merge with the previous line
            const bool wrapOne = lpNode->inlineCall && lpNode->bAdjustToOne;
            if (wrapOne)
                buffer << "("; // truncate a multiret method call to one value in a spread position
            EmitPrefix(lpNode->calledOn);
            buffer << ":";
            lpNode->callWhat->Accept(this);
            buffer << "(";
            for (size_t i = 0; i < lpNode->arguments.size(); i++) {
                lpNode->arguments.at(i)->Accept(this);
                if (i < lpNode->arguments.size() - 1)
                    buffer << ", ";
            }

            buffer << ")";
            if (wrapOne)
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
            if (lpNode->bIsLocalDeclaration && i < lpNode->retTypes.size() && lpNode->retTypes[i] != nullptr && ShouldEmitTypeAnnotation(lpNode->retTypes[i])) {
                buffer << ": ";
                lpNode->retTypes[i]->Accept(this);
            }
            if (i < lpNode->rets.size() - 1)
                buffer << ", ";
        }
        buffer << " = ";
        EmitPrefix(lpNode->calledOn);
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
        if (!m_firstStmtInBlock && StartsWithOpenParen(lpNode->left))
            buffer << ";"; // `(expr).f op= v` as a statement would merge with the previous line
        // Luau lacks compound logical assignments.
        const std::string &op = lpNode->op;
        const bool compoundable = op == "+" || op == "-" || op == "*" || op == "/" || op == "//" || op == "%" || op == "^" || op == "..";
        if (compoundable) {
            lpNode->left->Accept(this);
            buffer << " " << op << "= ";
            lpNode->right->Accept(this);
        } else {
            const int prec = OperatorPrecedence(op);
            lpNode->left->Accept(this);
            buffer << " = ";
            EmitWithPrecedence(prec, lpNode->left.get());
            buffer << " " << op << " ";
            EmitWithPrecedence(prec + 1, lpNode->right.get());
        }
        this->NextLine();
    }

    void Visit(RepeatStatementNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "repeat";
        this->NextLine();
        this->IncreaseIndentation();
        if (lpNode->body) // defensive: a malformed (null-body) loop must not segfault the generator
            lpNode->body->Accept(this);
        this->DecreaseIndentation();
        this->NextLine();
        buffer << this->GetIndentation() << "until (";
        if (lpNode->condition)
            lpNode->condition->Accept(this);
        else
            buffer << "true"; // a missing condition is malformed; emit a parseable placeholder
        buffer << ")";
        this->NextLine();
    }

    void Visit(VarArgExpression *lpNode) override {
        if (lpNode->bAdjustToOne)
            buffer << "(";
        buffer << "..."; /* legitimately. */
        if (lpNode->bAdjustToOne)
            buffer << ")";
    }

    void Visit(TableBinaryExpressionNode *lpNode) override {
        // Computed table keys require brackets; values do not.
        (void)lpNode;
        buffer << "[";
        lpNode->left->Accept(this);
        buffer << "] " << lpNode->op << " ";
        lpNode->right->Accept(this);
    }
    void Visit(IntegerLiteralNode *lpNode) override {
        if (lpNode->bUseParenthesis)
            buffer << "(";
        buffer << std::format("{}i", lpNode->value);
        if (lpNode->bUseParenthesis)
            buffer << ")";
    }

    void Visit(VectorNode *lpNode) override {
        m_sawVectorConstant = true;
        std::visit(
            [&](const auto &components) {
                const auto [x, y, z, w] = components;
                (void)w;

                if (!m_vectorConstructorAlias.empty()) {
                    buffer << m_vectorConstructorAlias << "(" << std::format("{}", x) << ", " << std::format("{}", y) << ", " << std::format("{}", z) << ")";
                    return;
                }

                if (x == 0 && y == 0 && z == 0)
                    buffer << "Vector3.zero";
                else if (x == 1 && y == 0 && z == 0)
                    buffer << "Vector3.xAxis";
                else if (x == -1 && y == 0 && z == 0)
                    buffer << "-Vector3.xAxis";
                else if (x == 0 && y == 1 && z == 0)
                    buffer << "Vector3.yAxis";
                else if (x == 0 && y == -1 && z == 0)
                    buffer << "-Vector3.yAxis";
                else if (x == 0 && y == 0 && z == 1)
                    buffer << "Vector3.zAxis";
                else if (x == 0 && y == 0 && z == -1)
                    buffer << "-Vector3.zAxis";
                else
                    buffer << "Vector3.new(" << std::format("{}", x) << ", " << std::format("{}", y) << ", " << std::format("{}", z) << ")";
            },
            lpNode->components
        );
    }
};
