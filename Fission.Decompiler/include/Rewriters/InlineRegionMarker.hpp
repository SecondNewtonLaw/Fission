//
// Created by Dottik on 1/10/2026.
//

// Marks each run of statements the compiler inlined from one function. A run is wrapped in `do ... end` only when nothing after it reads a
// name it declares; otherwise it is only commented, leaving scoping to the hoister and ScopeBlockIntroducer.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"
#include "FissionAllocator.hpp"
#include "InlineCallRecovery.hpp"
#include "Rewriters/DeclarationHoister.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class InlineRegionMarker {
  public:
    explicit InlineRegionMarker(std::shared_ptr<const InlineSourceMap> sources) : m_sources(std::move(sources)) {}

    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        if (m_sources)
            Mark(statements, -1, nullptr);
    }

  private:
    std::shared_ptr<const InlineSourceMap> m_sources;

    static bool AnyNested(const std::shared_ptr<Statement> &statement, const std::function<bool(const std::shared_ptr<Statement> &)> &predicate) {
        bool found = false;
        ForEachChildBlock(statement, [&](const std::vector<std::shared_ptr<Statement>> &body) {
            for (const auto &child : body)
                found = found || predicate(child) || AnyNested(child, predicate);
        });
        return found;
    }

    void MarkFunctions(const std::shared_ptr<Expression> &expression) {
        if (!expression)
            return;
        if (expression->nodeKind == ASTNodeKind::FunctionDeclarationNode) {
            const auto function = std::static_pointer_cast<FunctionDeclarationNode>(expression);
            if (function->lpFunctionBody)
                Mark(function->lpFunctionBody->body, -1, nullptr);
            else
                ForEachSubExpression(expression, [&](const std::shared_ptr<Expression> &child) { MarkFunctions(child); });
        } else
            ForEachSubExpression(expression, [&](const std::shared_ptr<Expression> &child) { MarkFunctions(child); });
    }

    static std::vector<std::string> DeclaredNames(const std::shared_ptr<Statement> &statement) {
        const auto nameOf = [](const std::shared_ptr<Expression> &target) {
            const auto identifier = std::dynamic_pointer_cast<IdentifierExpressionNode>(target);
            return identifier && identifier->identifier ? identifier->identifier->name : std::string{};
        };
        std::vector<std::string> names;
        if (const auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(statement))
            names.push_back(nameOf(declaration->identifier));
        else if (const auto function = std::dynamic_pointer_cast<FunctionDeclarationNode>(statement); function && function->bIsLocalDeclaration)
            names.push_back(function->functionName);
        else if (const auto expression = std::dynamic_pointer_cast<ExpressionStatementNode>(statement)) {
            if (const auto call = std::dynamic_pointer_cast<CallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration)
                for (const auto &ret : call->rets)
                    names.push_back(nameOf(ret));
            if (const auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(expression->expression); call && call->bIsLocalDeclaration)
                for (const auto &ret : call->rets)
                    names.push_back(nameOf(ret));
        }
        return names;
    }

    void Mark(std::vector<std::shared_ptr<Statement>> &statements, int32_t enclosing, const std::shared_ptr<Expression> &readAfter) {
        constexpr int32_t kCaller = -1;
        std::vector<int32_t> origins(statements.size());
        for (size_t i = 0; i < statements.size(); ++i) {
            const auto &statement = statements[i];
            if (!statement)
                continue;
            if (const auto function = std::dynamic_pointer_cast<FunctionDeclarationNode>(statement); function && function->lpFunctionBody)
                Mark(function->lpFunctionBody->body, -1, nullptr);
            ForEachStatementExpression(statement, [&](const std::shared_ptr<Expression> &expression) { MarkFunctions(expression); });
            origins[i] = statement->inlineSource;
            // a test the compiler merged with an inlined return still guards caller code
            if (origins[i] >= 0 && AnyNested(statement, [](const auto &child) { return child && child->inlineSource == kCaller; }))
                origins[i] = kCaller;
            const int32_t inner = origins[i] >= 0 ? origins[i] : enclosing;
            // a repeat's `until` reads the body's locals
            const auto repeat = std::dynamic_pointer_cast<RepeatStatementNode>(statement);
            ForEachChildBlock(statement, [&](std::vector<std::shared_ptr<Statement>> &body) { Mark(body, inner, repeat ? repeat->condition : nullptr); });
        }

        std::vector<std::shared_ptr<Statement>> rebuilt;
        rebuilt.reserve(statements.size());
        for (size_t i = 0; i < statements.size();) {
            const int32_t origin = origins[i];
            if (origin < 0 || origin == enclosing || static_cast<size_t>(origin) >= m_sources->sources.size()) {
                rebuilt.push_back(std::move(statements[i++]));
                continue;
            }
            const size_t start = i;
            while (i < statements.size() && origins[i] == origin)
                ++i;
            const auto &source = m_sources->sources[origin];
            auto comment = Fission::MakeShared<CommentNode>(
                std::format(
                    "Fission: INFO: inlined call to {} (defined at line {}, bytecode ID {})",
                    source.function->debugName ? std::format("'{}'", *source.function->debugName) : std::string("an anonymous function"), source.firstLine,
                    source.function->bytecodeId
                ),
                true, true
            );
            const auto readLater = [&](const std::string &name) {
                return std::any_of(
                           statements.begin() + static_cast<std::ptrdiff_t>(i), statements.end(),
                           [&](const auto &later) { return later && DeclarationHoister::Mentions(later, name); }
                       ) ||
                       (readAfter && DeclarationHoister::ExpressionMentions(readAfter, name));
            };
            // bare `local x` heading the run evaluate nothing and may open before the block
            size_t body = start;
            while (body < i) {
                const auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(statements[body]);
                if (!declaration || declaration->value)
                    break;
                ++body;
            }
            const bool scoped = std::all_of(
                statements.begin() + static_cast<std::ptrdiff_t>(body), statements.begin() + static_cast<std::ptrdiff_t>(i), [&](const auto &statement) {
                    return std::ranges::none_of(DeclaredNames(statement), [&](const std::string &name) { return !name.empty() && readLater(name); });
                }
            );
            if (!scoped || body == i) {
                rebuilt.push_back(comment);
                for (size_t k = start; k < i; ++k)
                    rebuilt.push_back(std::move(statements[k]));
                continue;
            }
            for (size_t k = start; k < body; ++k)
                rebuilt.push_back(std::move(statements[k]));
            auto region = Fission::MakeShared<BlockStatementNode>();
            region->bEmitAsDoBlock = true;
            region->body.push_back(comment);
            for (size_t k = body; k < i; ++k)
                region->body.push_back(std::move(statements[k]));
            rebuilt.push_back(std::move(region));
        }
        statements = std::move(rebuilt);
    }
};
