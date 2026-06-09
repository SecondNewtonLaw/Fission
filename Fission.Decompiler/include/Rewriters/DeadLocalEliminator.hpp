//
// Created by Dottik on 8/6/2026.
//
// Removes `local X = <pure expr>` when X is never read again. Pure-only, so no effect is lost.
//

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <string>
#include <vector>

class DeadLocalEliminator : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        // back-to-front, since removing a local can leave an earlier one dead.
        for (size_t i = stmts.size(); i-- > 0;) {
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            std::string name;
            if (!decl || !SimpleLocalName(decl, name) || !IsPure(decl->value))
                continue;
            bool used = false;
            for (size_t j = i + 1; j < stmts.size() && !used; ++j)
                used = MentionsStatement(stmts[j], name);
            if (!used)
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

  private:
    static bool SimpleLocalName(const std::shared_ptr<VariableDeclarationNode> &decl, std::string &out) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier) {
            out = id->identifier->name;
            return !out.empty();
        }
        return false;
    }

    static bool IsPure(const std::shared_ptr<Expression> &e) {
        if (!e) // bare `local X`
            return true;
        if (std::dynamic_pointer_cast<NilLiteralNode>(e) || std::dynamic_pointer_cast<BooleanLiteralNode>(e) ||
            std::dynamic_pointer_cast<NumberLiteralNode>(e) || std::dynamic_pointer_cast<IntegerLiteralNode>(e) ||
            std::dynamic_pointer_cast<StringLiteralNode>(e) || std::dynamic_pointer_cast<VectorNode>(e) ||
            std::dynamic_pointer_cast<IdentifierExpressionNode>(e) || std::dynamic_pointer_cast<Identifier>(e))
            return true;
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e))
            return IsPure(un->operand);
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e))
            return IsPure(bin->left) && IsPure(bin->right);
        if (auto cbin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(e))
            return IsPure(cbin->left) && IsPure(cbin->right);
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (const auto &entry : tbl->expressions)
                if (!IsPure(entry))
                    return false;
            return true;
        }
        return false;
    }

    static bool MentionsBlock(const std::shared_ptr<BlockStatementNode> &block, const std::string &name) {
        if (!block)
            return false;
        for (const auto &s : block->body)
            if (MentionsStatement(s, name))
                return true;
        return false;
    }

    static bool MentionsExpression(const std::shared_ptr<Expression> &e, const std::string &name) {
        if (!e)
            return false;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e))
            return id->identifier && id->identifier->name == name;
        if (auto id = std::dynamic_pointer_cast<Identifier>(e))
            return id->name == name;
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e))
            return MentionsExpression(bin->left, name) || MentionsExpression(bin->right, name);
        // CompoundBinaryExpressionNode is not a BinaryExpressionNode subclass.
        if (auto cbin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(e))
            return MentionsExpression(cbin->left, name) || MentionsExpression(cbin->right, name);
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e))
            return MentionsExpression(un->operand, name);
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(e))
            return MentionsExpression(idx->left, name) || MentionsExpression(idx->right, name);
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(e))
            return MentionsExpression(mem->table, name) || MentionsExpression(mem->key, name);
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            if (MentionsExpression(call->callee, name))
                return true;
            for (const auto &a : call->arguments)
                if (MentionsExpression(a, name))
                    return true;
            for (const auto &r : call->rets)
                if (MentionsExpression(r, name))
                    return true;
            return false;
        }
        if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            if (MentionsExpression(nc->calledOn, name) || MentionsExpression(nc->callWhat, name))
                return true;
            for (const auto &a : nc->arguments)
                if (MentionsExpression(a, name))
                    return true;
            for (const auto &r : nc->rets)
                if (MentionsExpression(r, name))
                    return true;
            return false;
        }
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (const auto &entry : tbl->expressions)
                if (MentionsExpression(entry, name))
                    return true;
            return false;
        }
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(e))
            return fn->lpFunctionBody && MentionsBlock(fn->lpFunctionBody, name);
        return false;
    }

    static bool MentionsStatement(const std::shared_ptr<Statement> &s, const std::string &name) {
        if (!s)
            return false;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s))
            return MentionsExpression(decl->identifier, name) || MentionsExpression(decl->value, name) ||
                   (decl->type && MentionsExpression(*decl->type, name));
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s))
            return MentionsExpression(asn->left, name) || MentionsExpression(asn->right, name);
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s))
            return MentionsExpression(es->expression, name);
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (const auto &v : ret->returnValues)
                if (MentionsExpression(v, name))
                    return true;
            return false;
        }
        if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s))
            return MentionsExpression(iff->condition, name) || MentionsBlock(iff->thenBranch, name) || MentionsBlock(iff->elseBranch, name);
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s))
            return MentionsExpression(w->condition, name) || MentionsBlock(w->body, name);
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s))
            return MentionsExpression(r->condition, name) || MentionsBlock(r->body, name);
        if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s))
            return MentionsExpression(fn->loopVariable, name) || MentionsExpression(fn->startVariable, name) ||
                   MentionsExpression(fn->increaseBy, name) || MentionsExpression(fn->maxIncreased, name) || MentionsBlock(fn->lpLoopBody, name);
        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            for (const auto &v : fg->loopVariables)
                if (MentionsExpression(v, name))
                    return true;
            return MentionsExpression(fg->generator, name) || MentionsExpression(fg->state, name) || MentionsExpression(fg->index, name) ||
                   MentionsBlock(fg->body, name);
        }
        if (auto fd = std::dynamic_pointer_cast<FunctionDeclarationNode>(s))
            return fd->lpFunctionBody && MentionsBlock(fd->lpFunctionBody, name);
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s))
            return MentionsBlock(blk, name);
        if (auto e = std::dynamic_pointer_cast<Expression>(s))
            return MentionsExpression(e, name);
        return false;
    }
};
