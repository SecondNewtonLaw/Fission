//
// Created by Dottik on 2/10/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

// Emits `const` for initialized locals no statement writes again.
class ConstLocalMarker {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        m_scopes.emplace_back();
        WalkBlock(statements);
        m_scopes.pop_back();
        for (const auto &[flag, written] : m_candidates)
            *flag = !written;
    }

  private:
    // a name's binding: the `bConst` flag of the declaration that may become const, or null for parameters, loop variables and the like
    std::vector<std::unordered_map<std::string, bool *>> m_scopes;
    std::unordered_map<bool *, bool> m_candidates;

    static const std::string *NameOf(const std::shared_ptr<Expression> &expression) {
        const auto id =
            expression && expression->nodeKind == ASTNodeKind::IdentifierExpression ? std::static_pointer_cast<IdentifierExpressionNode>(expression) : nullptr;
        return id && id->identifier && !id->identifier->bIsGlobal ? &id->identifier->name : nullptr;
    }

    void Bind(const std::shared_ptr<Expression> &expression, bool *flag = nullptr) {
        if (const auto *name = NameOf(expression))
            m_scopes.back()[*name] = flag;
        else if (flag)
            m_candidates[flag] = true;
    }

    // every name a declaration binds shares its one keyword
    void Declare(const std::vector<std::shared_ptr<Expression>> &names, bool *flag) {
        m_candidates.emplace(flag, false);
        for (const auto &name : names)
            Bind(name, flag);
    }

    void Write(const std::string &name) {
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope)
            if (const auto binding = scope->find(name); binding != scope->end()) {
                if (binding->second)
                    m_candidates[binding->second] = true;
                return;
            }
    }

    void WriteTargets(const std::shared_ptr<Expression> &target) {
        if (!target || target->nodeKind == ASTNodeKind::MemberExpression || target->nodeKind == ASTNodeKind::IndexExpression)
            return;
        if (const auto *name = NameOf(target)) {
            Write(*name);
            return;
        }
        ForEachSubExpression(target, [&](const std::shared_ptr<Expression> &child) { WriteTargets(child); });
    }

    void WalkFunction(FunctionDeclarationNode &function) {
        m_scopes.emplace_back();
        for (const auto &[_, argument] : function.argumentsNames)
            if (argument)
                Bind(argument->argumentName);
        if (function.lpFunctionBody)
            WalkBlock(function.lpFunctionBody->body);
        m_scopes.pop_back();
    }

    void WalkExpression(const std::shared_ptr<Expression> &expression) {
        if (!expression)
            return;
        if (expression->nodeKind == ASTNodeKind::FunctionDeclarationNode) {
            const auto function = std::static_pointer_cast<FunctionDeclarationNode>(expression);
            WalkFunction(*function);
            return;
        }
        ForEachSubExpression(expression, [&](const std::shared_ptr<Expression> &child) { WalkExpression(child); });
    }

    void WalkScopedBlock(const std::shared_ptr<BlockStatementNode> &block, const std::vector<std::shared_ptr<Expression>> &bindings = {}) {
        m_scopes.emplace_back();
        for (const auto &binding : bindings)
            Bind(binding);
        if (block)
            WalkBlock(block->body);
        m_scopes.pop_back();
    }

    void WalkBlock(const std::vector<std::shared_ptr<Statement>> &statements) {
        for (const auto &statement : statements)
            WalkStatement(statement);
    }

    template <typename Call> void WalkCallStatement(Call &call) {
        for (const auto &argument : call.arguments)
            WalkExpression(argument);
        if constexpr (std::is_same_v<Call, CallExpressionNode>)
            WalkExpression(call.callee);
        else
            WalkExpression(call.calledOn);
        if (call.rets.empty())
            return;
        if (call.bIsLocalDeclaration) {
            Declare(call.rets, &call.bConst);
        } else {
            for (const auto &ret : call.rets) {
                WalkExpression(ret);
                WriteTargets(ret);
            }
        }
    }

    void WalkStatement(const std::shared_ptr<Statement> &statement) {
        if (!statement)
            return;
        if (const auto wrapped =
                statement->nodeKind == ASTNodeKind::ExpressionStatement ? std::static_pointer_cast<ExpressionStatementNode>(statement) : nullptr) {
            WalkStatement(wrapped->expression);
        } else if (
            const auto declaration =
                statement->nodeKind == ASTNodeKind::VariableDeclaration ? std::static_pointer_cast<VariableDeclarationNode>(statement) : nullptr
        ) {
            WalkExpression(declaration->value);
            if (declaration->value && !declaration->bExported) {
                Declare({declaration->identifier}, &declaration->bConst);
            } else if (NameOf(declaration->identifier)) {
                Bind(declaration->identifier);
            } else {
                WriteTargets(declaration->identifier);
            }
        } else if (
            const auto assignment =
                statement->nodeKind == ASTNodeKind::AssignmentStatement ? std::static_pointer_cast<AssignmentStatementNode>(statement) : nullptr
        ) {
            WalkExpression(assignment->right);
            WalkExpression(assignment->left);
            WriteTargets(assignment->left);
        } else if (
            const auto compound =
                statement->nodeKind == ASTNodeKind::CompoundAssignment ? std::static_pointer_cast<CompoundBinaryExpressionNode>(statement) : nullptr
        ) {
            WalkExpression(compound->right);
            WalkExpression(compound->left);
            WriteTargets(compound->left);
        } else if (
            const auto function =
                statement->nodeKind == ASTNodeKind::FunctionDeclarationNode ? std::static_pointer_cast<FunctionDeclarationNode>(statement) : nullptr
        ) {
            // `local function f` sees itself; `function f` assigns an existing binding
            if (function->bIsLocalDeclaration)
                m_scopes.back()[function->functionName] = nullptr;
            else if (!function->bAnonymousInline)
                Write(function->functionName);
            WalkFunction(*function);
        } else if (const auto call = statement->nodeKind == ASTNodeKind::CallExpression ? std::static_pointer_cast<CallExpressionNode>(statement) : nullptr) {
            WalkCallStatement(*call);
        } else if (
            const auto nameCall =
                statement->nodeKind == ASTNodeKind::MethodCallExpression ? std::static_pointer_cast<NameCallExpressionNode>(statement) : nullptr
        ) {
            WalkCallStatement(*nameCall);
        } else if (
            const auto repeat = statement->nodeKind == ASTNodeKind::RepeatStatement ? std::static_pointer_cast<RepeatStatementNode>(statement) : nullptr
        ) {
            // the condition sees the body's locals
            m_scopes.emplace_back();
            if (repeat->body)
                WalkBlock(repeat->body->body);
            WalkExpression(repeat->condition);
            m_scopes.pop_back();
        } else if (const auto numeric = statement->nodeKind == ASTNodeKind::ForNumeric ? std::static_pointer_cast<ForNumericNode>(statement) : nullptr) {
            WalkExpression(numeric->startVariable);
            WalkExpression(numeric->maxIncreased);
            WalkExpression(numeric->increaseBy);
            WalkScopedBlock(numeric->lpLoopBody, {numeric->loopVariable});
        } else if (const auto generic = statement->nodeKind == ASTNodeKind::ForGeneral ? std::static_pointer_cast<ForGeneralNode>(statement) : nullptr) {
            WalkExpression(generic->generator);
            WalkExpression(generic->state);
            WalkExpression(generic->index);
            WalkScopedBlock(generic->body, generic->loopVariables);
        } else if (const auto branch = statement->nodeKind == ASTNodeKind::IfStatement ? std::static_pointer_cast<IfStatementNode>(statement) : nullptr) {
            WalkExpression(branch->condition);
            WalkScopedBlock(branch->thenBranch);
            WalkScopedBlock(branch->elseBranch);
        } else if (const auto loop = statement->nodeKind == ASTNodeKind::WhileStatement ? std::static_pointer_cast<WhileStatementNode>(statement) : nullptr) {
            WalkExpression(loop->condition);
            WalkScopedBlock(loop->body);
        } else if (const auto block = statement->nodeKind == ASTNodeKind::BlockStatement ? std::static_pointer_cast<BlockStatementNode>(statement) : nullptr) {
            WalkScopedBlock(block);
        } else if (
            const auto classDeclaration =
                statement->nodeKind == ASTNodeKind::ClassDeclaration ? std::static_pointer_cast<ClassDeclarationNode>(statement) : nullptr
        ) {
            WalkExpression(classDeclaration->superclass);
            for (const auto &method : classDeclaration->methods)
                if (method)
                    WalkFunction(*method);
        } else {
            ForEachStatementExpression(statement, [&](const std::shared_ptr<Expression> &expression) { WalkExpression(expression); });
            if (const auto expression = AsExpression(statement))
                WalkExpression(expression);
        }
    }
};
