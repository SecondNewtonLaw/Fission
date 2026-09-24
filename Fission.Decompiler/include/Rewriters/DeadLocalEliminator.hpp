//
// Created by Dottik on 8/6/2026.
//

// Removes unread local declarations only when their initializer cannot raise or have effects.

#pragma once
#include "Rewriters/ASTRewriter.hpp"

#include <bit>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class DeadLocalEliminator : public ASTRewriter {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        m_collected.clear();
        ASTRewriter::Run(statements);
    }

  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        // Walking backwards, `later` holds what the nearest later statement does with each name: reads it, or redeclares it
        // (reading it only in the new initializer counts as a read). Removing a local can make an earlier declaration dead.
        std::unordered_map<std::string, bool> later;
        std::unordered_set<std::string> tail;
        std::vector<bool> keep(stmts.size(), true);
        // a repeat's until-cond is in body scope; a read there is a real use
        if (m_tailScopeExpr)
            CollectExpression(m_tailScopeExpr, tail);
        for (size_t i = stmts.size(); i-- > 0;) {
            auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts[i]);
            std::string name;
            if (decl && SimpleLocalName(decl, name) && IsPure(decl->value)) {
                const auto next = later.find(name);
                if (next == later.end() ? !tail.contains(name) : !next->second) {
                    keep[i] = false;
                    continue;
                }
            }
            std::unordered_set<std::string> names;
            CollectStatement(stmts[i], names);
            for (const auto &mentioned : names)
                later[mentioned] = true;
            if (decl && SimpleLocalName(decl, name))
                later[name] = MentionsExpression(decl->value, name);
            else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[i])) {
                RecordCallShadows(std::dynamic_pointer_cast<CallExpressionNode>(es->expression), later);
                RecordCallShadows(std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression), later);
            }
        }
        size_t write = 0;
        for (size_t read = 0; read < stmts.size(); ++read)
            if (keep[read])
                stmts[write++] = std::move(stmts[read]);
        stmts.resize(write);
    }

  private:
    std::unordered_map<const Statement *, std::unordered_set<std::string>> m_collected;
    // `local a, b = f(...)` redeclares its results; each stays read only if the call itself reads it
    template <typename Call> void RecordCallShadows(const std::shared_ptr<Call> &call, std::unordered_map<std::string, bool> &later) {
        if (!call || call->inlineCall || !call->bIsLocalDeclaration)
            return;
        auto initializer = std::make_shared<Call>(*call);
        initializer->rets.clear();
        for (const auto &result : call->rets) {
            std::unordered_set<std::string> declared;
            CollectExpression(result, declared);
            for (const auto &name : declared)
                later[name] = MentionsExpression(initializer, name);
        }
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

    // "Pure" here means evaluation can never raise OR have effects. Reading locals and
    // creating closures/tables is effect-free, but arithmetic, comparison, concat, and length all
    // raise on wrong runtime types (or run metamethods), so they only pass with operand types that
    // are statically known safe. When in doubt, keep the statement.
    static bool IsPure(const std::shared_ptr<Expression> &e) {
        if (!e) // bare `local X`
            return true;
        if (std::dynamic_pointer_cast<NilLiteralNode>(e) || std::dynamic_pointer_cast<BooleanLiteralNode>(e) ||
            std::dynamic_pointer_cast<NumberLiteralNode>(e) || std::dynamic_pointer_cast<IntegerLiteralNode>(e) ||
            std::dynamic_pointer_cast<StringLiteralNode>(e) || std::dynamic_pointer_cast<VectorNode>(e))
            return true;
        if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e))
            return id->identifier && !id->identifier->bIsGlobal;
        if (auto id = std::dynamic_pointer_cast<Identifier>(e))
            return !id->bIsGlobal;
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
            if (auto key = std::dynamic_pointer_cast<NumberLiteralNode>(tentry->left);
                key && (std::bit_cast<uint64_t>(key->value) & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL)
                return false;
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

    // Every name MentionsExpression / MentionsStatement would find.
    void CollectBlock(const std::shared_ptr<BlockStatementNode> &block, std::unordered_set<std::string> &out) {
        if (block)
            for (const auto &s : block->body)
                CollectStatement(s, out);
    }

    void CollectExpression(const std::shared_ptr<Expression> &e, std::unordered_set<std::string> &out) {
        if (!e)
            return;
        if (auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(e)) {
            CollectExpression(conditional->condition, out);
            CollectExpression(conditional->thenExpr, out);
            CollectExpression(conditional->elseExpr, out);
        } else if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(e)) {
            if (id->identifier)
                out.insert(id->identifier->name);
        } else if (auto identifier = std::dynamic_pointer_cast<Identifier>(e)) {
            out.insert(identifier->name);
        } else if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e)) {
            CollectExpression(bin->left, out);
            CollectExpression(bin->right, out);
        } else if (auto cbin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(e)) {
            CollectExpression(cbin->left, out);
            CollectExpression(cbin->right, out);
        } else if (auto un = std::dynamic_pointer_cast<UnaryExpressionNode>(e)) {
            CollectExpression(un->operand, out);
        } else if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(e)) {
            CollectExpression(idx->left, out);
            CollectExpression(idx->right, out);
        } else if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(e)) {
            CollectExpression(mem->table, out);
            CollectExpression(mem->key, out);
        } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(e)) {
            CollectExpression(call->callee, out);
            for (const auto &a : call->arguments)
                CollectExpression(a, out);
            for (const auto &r : call->rets)
                CollectExpression(r, out);
        } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(e)) {
            CollectExpression(nc->calledOn, out);
            CollectExpression(nc->callWhat, out);
            for (const auto &a : nc->arguments)
                CollectExpression(a, out);
            for (const auto &r : nc->rets)
                CollectExpression(r, out);
        } else if (auto tbl = std::dynamic_pointer_cast<TableLiteralNode>(e)) {
            for (const auto &entry : tbl->expressions)
                CollectExpression(entry, out);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(e)) {
            CollectBlock(fn->lpFunctionBody, out);
        }
    }

    void CollectStatement(const std::shared_ptr<Statement> &s, std::unordered_set<std::string> &out) {
        if (!s)
            return;
        if (const auto it = m_collected.find(s.get()); it != m_collected.end()) {
            out.insert(it->second.begin(), it->second.end());
            return;
        }
        std::unordered_set<std::string> names;
        CollectStatementUncached(s, names);
        out.insert(names.begin(), names.end());
        m_collected.emplace(s.get(), std::move(names));
    }

    void CollectStatementUncached(const std::shared_ptr<Statement> &s, std::unordered_set<std::string> &out) {
        if (!s)
            return;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s)) {
            CollectExpression(decl->identifier, out);
            CollectExpression(decl->value, out);
            if (decl->type)
                CollectExpression(*decl->type, out);
        } else if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(s)) {
            CollectExpression(asn->left, out);
            CollectExpression(asn->right, out);
        } else if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(s)) {
            CollectExpression(es->expression, out);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(s)) {
            for (const auto &v : ret->returnValues)
                CollectExpression(v, out);
        } else if (auto iff = std::dynamic_pointer_cast<IfStatementNode>(s)) {
            CollectExpression(iff->condition, out);
            CollectBlock(iff->thenBranch, out);
            CollectBlock(iff->elseBranch, out);
        } else if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(s)) {
            CollectExpression(w->condition, out);
            CollectBlock(w->body, out);
        } else if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(s)) {
            CollectExpression(r->condition, out);
            CollectBlock(r->body, out);
        } else if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s)) {
            CollectExpression(fn->loopVariable, out);
            CollectExpression(fn->startVariable, out);
            CollectExpression(fn->increaseBy, out);
            CollectExpression(fn->maxIncreased, out);
            CollectBlock(fn->lpLoopBody, out);
        } else if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s)) {
            for (const auto &v : fg->loopVariables)
                CollectExpression(v, out);
            CollectExpression(fg->generator, out);
            CollectExpression(fg->state, out);
            CollectExpression(fg->index, out);
            CollectBlock(fg->body, out);
        } else if (auto fd = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            if (const size_t receiverEnd = fd->functionName.find_first_of(".:"); receiverEnd != std::string::npos)
                out.insert(fd->functionName.substr(0, receiverEnd));
            CollectBlock(fd->lpFunctionBody, out);
        } else if (auto cls = std::dynamic_pointer_cast<ClassDeclarationNode>(s)) {
            CollectExpression(cls->superclass, out);
            for (const auto &method : cls->methods)
                CollectExpression(method, out);
        } else if (auto blk = std::dynamic_pointer_cast<BlockStatementNode>(s)) {
            CollectBlock(blk, out);
        } else if (auto e = std::dynamic_pointer_cast<Expression>(s)) {
            CollectExpression(e, out);
        }
    }
};
