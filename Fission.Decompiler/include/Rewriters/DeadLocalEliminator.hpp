//
// Created by Dottik on 8/6/2026.
//

// Removes unread local declarations only when their initializer cannot raise or have effects.

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <memory>
#include <string>
#include <vector>

class DeadLocalEliminator : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        // Removing a local can make an earlier declaration dead.
        for (size_t i = stmts.size(); i-- > 0;) {
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            std::string name;
            if (!decl || !SimpleLocalName(decl, name) || !IsPure(decl->value))
                continue;
            bool used = false;
            bool shadowed = false;
            for (size_t j = i + 1; j < stmts.size() && !used; ++j) {
                std::string shadowName;
                if (auto next = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[j]); next && SimpleLocalName(next, shadowName) && shadowName == name) {
                    used = MentionsExpression(next->value, name);
                    shadowed = true;
                    break;
                }
                if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[j]);
                    es && (CallShadows(std::dynamic_pointer_cast<CallExpressionNode>(es->expression), name, used) ||
                           CallShadows(std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression), name, used))) {
                    shadowed = true;
                    break;
                }
                used = MentionsStatement(stmts[j], name);
            }
            // a repeat's until-cond is in body scope; a read there is a real use
            if (!used && !shadowed && m_tailScopeExpr)
                used = MentionsExpression(m_tailScopeExpr, name);
            if (!used)
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }

  private:
    template <typename Call> static bool CallShadows(const std::shared_ptr<Call> &call, const std::string &name, bool &used) {
        if (!call || call->inlineCall || !call->bIsLocalDeclaration)
            return false;
        for (const auto &result : call->rets) {
            if (!MentionsExpression(result, name))
                continue;
            auto initializer = std::make_shared<Call>(*call);
            initializer->rets.clear();
            used = MentionsExpression(initializer, name);
            return true;
        }
        return false;
    }

    static bool SimpleLocalName(const std::shared_ptr<VariableDeclarationNode> &decl, std::string &out) {
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier) {
            out = id->identifier->name;
            return !out.empty();
        }
        return false;
    }

    // static type of an expression, where knowable; drives the can-this-raise decision below.
    // Integer is distinct from Number: this Luau's experimental `Ni` integers raise on unm and most
    // arithmetic (e.g. `- -184i` errors), so they are never throw-free operands.
    enum class Ty { Number, Integer, String, Bool, Nil, Table, Function, Vector, Unknown };

    static Ty TypeOf(const std::shared_ptr<Expression> &e) {
        if (std::dynamic_pointer_cast<IntegerLiteralNode>(e))
            return Ty::Integer;
        if (std::dynamic_pointer_cast<NumberLiteralNode>(e))
            return Ty::Number;
        if (std::dynamic_pointer_cast<StringLiteralNode>(e))
            return Ty::String;
        if (std::dynamic_pointer_cast<BooleanLiteralNode>(e))
            return Ty::Bool;
        if (std::dynamic_pointer_cast<NilLiteralNode>(e))
            return Ty::Nil;
        if (std::dynamic_pointer_cast<TableLiteralNode>(e))
            return Ty::Table;
        if (std::dynamic_pointer_cast<VectorNode>(e))
            return Ty::Vector;
        if (std::dynamic_pointer_cast<FunctionDeclarationNode>(e))
            return Ty::Function;
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e); un && un->op == "not ") // emitters use the trailing-space form
            return Ty::Bool;
        return Ty::Unknown;
    }

    // "Pure" here means evaluation can never raise OR have effects. Reading locals/globals and
    // creating closures/tables is effect-free, but arithmetic, comparison, concat, and length all
    // raise on wrong runtime types (or run metamethods), so they only pass with operand types that
    // are statically known safe. When in doubt, keep the statement.
    static bool IsPure(const std::shared_ptr<Expression> &e) {
        if (!e) // bare `local X`
            return true;
        if (std::dynamic_pointer_cast<NilLiteralNode>(e) || std::dynamic_pointer_cast<BooleanLiteralNode>(e) ||
            std::dynamic_pointer_cast<NumberLiteralNode>(e) || std::dynamic_pointer_cast<IntegerLiteralNode>(e) ||
            std::dynamic_pointer_cast<StringLiteralNode>(e) || std::dynamic_pointer_cast<VectorNode>(e) ||
            std::dynamic_pointer_cast<IdentifierExpressionNode>(e) || std::dynamic_pointer_cast<Identifier>(e))
            return true;
        if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e)) {
            if (!IsPure(un->operand))
                return false;
            const Ty t = TypeOf(un->operand);
            if (un->op == "not ") // emitters use the trailing-space form
                return true;
            if (un->op == "-")
                return t == Ty::Number || t == Ty::Vector;
            if (un->op == "#")
                return t == Ty::String || t == Ty::Table;
            return false;
        }
        // table constructor key=value entry: the store into a fresh table cannot run metamethods,
        // but a nil (or unknown-and-possibly-nil) key raises "table index is nil".
        if (auto tentry = std::dynamic_pointer_cast<TableBinaryExpressionNode>(e)) {
            const Ty kt = TypeOf(tentry->left);
            return (kt == Ty::Number || kt == Ty::String || kt == Ty::Bool) && IsPure(tentry->left) && IsPure(tentry->right);
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e))
            return IsPure(bin->left) && IsPure(bin->right) && OpIsThrowFree(bin->op, bin->left, bin->right);
        if (auto cbin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(e))
            return IsPure(cbin->left) && IsPure(cbin->right) && OpIsThrowFree(cbin->op, cbin->left, cbin->right);
        if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (const auto &entry : tbl->expressions)
                if (!IsPure(entry))
                    return false;
            return true;
        }
        if (std::dynamic_pointer_cast<FunctionDeclarationNode>(e))
            return true; // closure creation
        return false;
    }

    static bool OpIsThrowFree(const std::string &op, const std::shared_ptr<Expression> &l, const std::shared_ptr<Expression> &r) {
        if (op == "and" || op == "or")
            return true;
        const Ty lt = TypeOf(l), rt = TypeOf(r);
        if (op == "==" || op == "~=") {
            // __eq only fires (and can only raise) when both operands are tables/userdata; a fresh
            // table literal has no metatable, so any known-type side makes the comparison safe.
            return lt != Ty::Unknown || rt != Ty::Unknown;
        }
        if (op == "<" || op == "<=" || op == ">" || op == ">=")
            return (lt == Ty::Number && rt == Ty::Number) || (lt == Ty::String && rt == Ty::String);
        if (op == "..")
            return (lt == Ty::Number || lt == Ty::String) && (rt == Ty::Number || rt == Ty::String);
        if (op == "+" || op == "-" || op == "*" || op == "/" || op == "//" || op == "%" || op == "^")
            return (lt == Ty::Number && rt == Ty::Number) || (lt == Ty::Vector && rt == Ty::Vector && op != "%" && op != "^");
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
        if (auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(e))
            return MentionsExpression(conditional->condition, name) || MentionsExpression(conditional->thenExpr, name) ||
                   MentionsExpression(conditional->elseExpr, name);
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
            return MentionsExpression(decl->identifier, name) || MentionsExpression(decl->value, name) || (decl->type && MentionsExpression(*decl->type, name));
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
            return MentionsExpression(fn->loopVariable, name) || MentionsExpression(fn->startVariable, name) || MentionsExpression(fn->increaseBy, name) ||
                   MentionsExpression(fn->maxIncreased, name) || MentionsBlock(fn->lpLoopBody, name);
        if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            for (const auto &v : fg->loopVariables)
                if (MentionsExpression(v, name))
                    return true;
            return MentionsExpression(fg->generator, name) || MentionsExpression(fg->state, name) || MentionsExpression(fg->index, name) ||
                   MentionsBlock(fg->body, name);
        }
        if (auto fd = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            const size_t receiverEnd = fd->functionName.find_first_of(".:");
            if (receiverEnd != std::string::npos && fd->functionName.substr(0, receiverEnd) == name)
                return true;
            return fd->lpFunctionBody && MentionsBlock(fd->lpFunctionBody, name);
        }
        if (auto cls = std::dynamic_pointer_cast<ClassDeclarationNode>(s)) {
            if (MentionsExpression(cls->superclass, name))
                return true;
            for (const auto &method : cls->methods)
                if (MentionsExpression(method, name))
                    return true;
        }
        if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s))
            return MentionsBlock(blk, name);
        if (auto e = std::dynamic_pointer_cast<Expression>(s))
            return MentionsExpression(e, name);
        return false;
    }
};
