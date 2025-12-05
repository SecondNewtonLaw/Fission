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

    std::string GetIndentation() { return std::string(this->dwIndentationLevel * kIndentationSpaceCount, ' '); } // NOLINT(*-return-braced-init-list)

    void NextLine() { buffer << "\n"; }
    void IncreaseIndentation() { this->dwIndentationLevel++; }
    void DecreaseIndentation() {
        ASSERT(this->dwIndentationLevel - 1 >= 0, "indentation out of range. Overpopped");
        this->dwIndentationLevel--;
    }

    void Visit(NoExpressionNode *lpNode) override { (void)lpNode; }

    void Visit(RootNode *lpNode) override {
        (void)lpNode;
        for (const auto &body : lpNode->programBody)
            body->Accept(this);
    }

    void Visit(Identifier *lpNode) override { buffer << lpNode->name; }

    void Visit(FunctionDeclarationNode *lpNode) override {
        (void)lpNode;

        buffer << this->GetIndentation();
        if (lpNode->bIsLocalDeclaration)
            buffer << "local ";
        buffer << std::format("function {}(", lpNode->functionName);

        for (int32_t i = 0; i < lpNode->argumentCount; i++) {
            buffer << lpNode->argumentsNames.at(i);
            if (i < lpNode->argumentCount - 1)
                buffer << ", ";
        }

        buffer << ")";
        this->NextLine();

        this->IncreaseIndentation();
        lpNode->lpFunctionBody->Accept(this);
        this->DecreaseIndentation();
        buffer << this->GetIndentation() << "end";
        this->NextLine();
    }

    void Visit(CommentNode *lpNode) override {
        (void)lpNode;
        buffer << this->GetIndentation() << "--[=[" << lpNode->comment << "]=]";
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

            if (lpNode->bIsVariadicCall)
                buffer << ", ...";
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
            if (lpNode->bIsVariadicCall)
                buffer << ", ...";
            buffer << ")";
        }
    }

    void Visit(UnaryExpressionNode *lpNode) override { (void)lpNode; }

    void Visit(IndexExpressionNode *lpNode) override {
        (void)lpNode;
        lpNode->left->Accept(this);
        buffer << ".";
        lpNode->right->Accept(this);
    }

    void Visit(MemberExpressionNode *lpNode) override {
        (void)lpNode;
        lpNode->table->Accept(this);
        buffer << "." << lpNode->keyName;
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
            buffer << this->GetIndentation() << "--[[ Fission: Control Flow Analysis Failed ]]";
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
        lpNode->left->Accept(this);
        buffer << " = ";
        lpNode->right->Accept(this);
        this->NextLine();
    }

    void Visit(BinaryExpressionNode *lpNode) override {
        (void)lpNode;
        lpNode->left->Accept(this);
        buffer << " " << lpNode->op << " ";
        lpNode->right->Accept(this);
    }

    void Visit(StringLiteralNode *lpNode) override {
        (void)lpNode;
        buffer << "\"" << lpNode->value << "\"";
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
        buffer << num;
    }

    void Visit(BooleanLiteralNode *lpNode) override {
        (void)lpNode;
        buffer << (lpNode->value ? "true" : "false");
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
        buffer << "nil";
    }

    void Visit(TableLiteralNode *lpNode) override {
        buffer << " { ";
        for (size_t i = 0; i < lpNode->expressions.size(); i++) {
            lpNode->expressions.at(i)->Accept(this);
            if (i < lpNode->expressions.size() - 1)
                buffer << ", ";
        }
        buffer << " }";
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

            if (lpNode->bIsVariadicCall)
                buffer << ", ...";
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

        if (lpNode->bIsVariadicCall)
            buffer << ", ...";
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
};
