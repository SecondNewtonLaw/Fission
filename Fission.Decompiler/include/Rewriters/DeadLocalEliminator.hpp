//
// Created by Dottik on 8/6/2026.
//

// Removes unread local declarations only when their initializer cannot raise or have effects.

#pragma once
#include "FissionAllocator.hpp"
#include "Rewriters/ASTRewriter.hpp"

#include <algorithm>
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
            auto decl =
                stmts[i] && stmts[i]->nodeKind == ASTNodeKind::VariableDeclaration ? std::static_pointer_cast<VariableDeclarationNode>(stmts[i]) : nullptr;
            std::string name;
            const bool namedLocal = decl && SimpleLocalName(decl, name);
            if (namedLocal && IsPure(decl->value)) {
                const auto next = later.find(name);
                if (next == later.end() ? !tail.contains(name) : !next->second) {
                    keep[i] = false;
                    continue;
                }
            }
            const auto &references = CollectStatementReferences(stmts[i]);
            for (const auto &mentioned : references.names)
                later[mentioned] = true;
            if (namedLocal)
                later[name] = references.initializerReadsName;
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
    struct References {
        std::unordered_set<std::string> names;
        bool initializerReadsName = false;
    };
    std::unordered_map<const Statement *, References> m_collected;
    // `local a, b = f(...)` redeclares its results; each stays read only if the call itself reads it
    template <typename Call> void RecordCallShadows(const std::shared_ptr<Call> &call, std::unordered_map<std::string, bool> &later) {
        if (!call || call->inlineCall || !call->bIsLocalDeclaration || call->rets.empty())
            return;
        auto initializer = Fission::MakeShared<Call>(*call);
        initializer->rets.clear();
        std::unordered_set<std::string> initializerNames;
        bool collected = false;
        for (const auto &result : call->rets) {
            std::unordered_set<std::string> declared;
            CollectExpression(result, declared);
            for (const auto &name : declared) {
                if (!collected) {
                    CollectExpression(initializer, initializerNames);
                    collected = true;
                }
                later[name] = initializerNames.contains(name);
            }
        }
    }

    static bool SimpleLocalName(const std::shared_ptr<VariableDeclarationNode> &decl, std::string &out) {
        if (auto id = decl->identifier && decl->identifier->nodeKind == ASTNodeKind::IdentifierExpression
                          ? std::static_pointer_cast<IdentifierExpressionNode>(decl->identifier)
                          : nullptr;
            id && id->identifier) {
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
        if (AsLiteral<IntegerLiteralNode>(e))
            return Ty::Integer;
        if (AsLiteral<NumberLiteralNode>(e))
            return Ty::Number;
        if (AsLiteral<StringLiteralNode>(e))
            return Ty::String;
        if (AsLiteral<BooleanLiteralNode>(e))
            return Ty::Bool;
        if (AsLiteral<NilLiteralNode>(e))
            return Ty::Nil;
        if (AsLiteral<TableLiteralNode>(e))
            return Ty::Table;
        if (AsLiteral<VectorNode>(e))
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
        if (AsLiteral<NilLiteralNode>(e) || AsLiteral<BooleanLiteralNode>(e) || AsLiteral<NumberLiteralNode>(e) || AsLiteral<IntegerLiteralNode>(e) ||
            AsLiteral<StringLiteralNode>(e) || AsLiteral<VectorNode>(e))
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
            if (auto key = AsLiteral<NumberLiteralNode>(tentry->left);
                key && (std::bit_cast<uint64_t>(key->value) & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL)
                return false;
            const Ty kt = TypeOf(tentry->left);
            return (kt == Ty::Number || kt == Ty::String || kt == Ty::Bool) && IsPure(tentry->left) && IsPure(tentry->right);
        }
        if (auto bin = std::dynamic_pointer_cast<BinaryExpressionNode>(e))
            return IsPure(bin->left) && IsPure(bin->right) && OpIsThrowFree(bin->op, bin->left, bin->right);
        if (auto cbin = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(e))
            return IsPure(cbin->left) && IsPure(cbin->right) && OpIsThrowFree(cbin->op, cbin->left, cbin->right);
        if (auto tbl = AsLiteral<TableLiteralNode>(e)) {
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

    void CollectBlock(const std::shared_ptr<BlockStatementNode> &block, std::unordered_set<std::string> &out) {
        if (block)
            for (const auto &s : block->body)
                CollectStatement(s, out);
    }

    void CollectExpression(const std::shared_ptr<Expression> &e, std::unordered_set<std::string> &out) {
        if (auto id = e && e->nodeKind == ASTNodeKind::IdentifierExpression ? std::static_pointer_cast<IdentifierExpressionNode>(e) : nullptr) {
            if (id->identifier)
                out.insert(id->identifier->name);
        } else if (auto identifier = std::dynamic_pointer_cast<Identifier>(e)) {
            out.insert(identifier->name);
        } else if (auto fn = e && e->nodeKind == ASTNodeKind::FunctionDeclarationNode ? std::static_pointer_cast<FunctionDeclarationNode>(e) : nullptr) {
            CollectBlock(fn->lpFunctionBody, out);
        } else {
            ForEachSubExpression(e, [&](const std::shared_ptr<Expression> &child) { CollectExpression(child, out); });
        }
    }

    const References &CollectStatementReferences(const std::shared_ptr<Statement> &s) {
        static const References kEmpty;
        if (!s)
            return kEmpty;
        if (const auto it = m_collected.find(s.get()); it != m_collected.end())
            return it->second;
        References references;
        std::string name;
        if (auto declaration = s->nodeKind == ASTNodeKind::VariableDeclaration ? std::static_pointer_cast<VariableDeclarationNode>(s) : nullptr;
            declaration && SimpleLocalName(declaration, name)) {
            CollectExpression(declaration->value, references.names);
            references.initializerReadsName = references.names.contains(name);
            CollectExpression(declaration->identifier, references.names);
            if (declaration->type)
                CollectExpression(*declaration->type, references.names);
        } else {
            CollectStatementUncached(s, references.names);
        }
        return m_collected.emplace(s.get(), std::move(references)).first->second;
    }

    const std::unordered_set<std::string> &CollectStatement(const std::shared_ptr<Statement> &s) { return CollectStatementReferences(s).names; }

    void CollectStatement(const std::shared_ptr<Statement> &s, std::unordered_set<std::string> &out) {
        const auto &names = CollectStatement(s);
        out.insert(names.begin(), names.end());
    }

    void CollectStatementUncached(const std::shared_ptr<Statement> &s, std::unordered_set<std::string> &out) {
        if (!s)
            return;
        if (auto fd = std::dynamic_pointer_cast<FunctionDeclarationNode>(s)) {
            if (const size_t receiverEnd = fd->functionName.find_first_of(".:"); receiverEnd != std::string::npos)
                out.insert(fd->functionName.substr(0, receiverEnd));
            CollectBlock(fd->lpFunctionBody, out);
            return;
        }
        if (auto cls = std::dynamic_pointer_cast<ClassDeclarationNode>(s)) {
            CollectExpression(cls->superclass, out);
            for (const auto &method : cls->methods)
                CollectExpression(method, out);
            return;
        }
        const auto collect = [&](const std::shared_ptr<Expression> &e) { CollectExpression(e, out); };
        ForEachStatementExpression(s, collect);
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(s); decl && decl->type)
            collect(*decl->type);
        else if (auto fn = std::dynamic_pointer_cast<ForNumericNode>(s))
            collect(fn->loopVariable);
        else if (auto fg = std::dynamic_pointer_cast<ForGeneralNode>(s))
            std::ranges::for_each(fg->loopVariables, collect);
        else if (auto e = std::dynamic_pointer_cast<Expression>(s))
            collect(e);
        ForEachChildBlock(s, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &child : body)
                CollectStatement(child, out);
        });
    }
};
