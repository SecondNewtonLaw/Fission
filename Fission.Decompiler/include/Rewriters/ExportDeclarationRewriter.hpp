//
// Created by Dottik on 25/9/2026.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class ExportDeclarationRewriter {
  public:
    static void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        if (statements.empty())
            return;
        const auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(statements.back());
        if (!ret || ret->returnValues.size() != 1)
            return;
        const auto call = std::dynamic_pointer_cast<CallExpressionNode>(ret->returnValues.front());
        const auto freeze = call ? std::dynamic_pointer_cast<MemberExpressionNode>(call->callee) : nullptr;
        if (!call || !freeze || !IsName(freeze->table, "table") || !IsKey(freeze->key, "freeze") || call->arguments.size() != 1)
            return;
        const auto tableId = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->arguments.front());
        if (!tableId || !tableId->identifier)
            return;
        const std::string tableName = tableId->identifier->name;

        size_t tableIndex = statements.size();
        std::shared_ptr<TableLiteralNode> table;
        for (size_t i = 0; i + 1 < statements.size(); ++i) {
            const auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(statements[i]);
            if (decl && IsName(decl->identifier, tableName)) {
                if (tableIndex != statements.size())
                    return;
                tableIndex = i;
                table = std::dynamic_pointer_cast<TableLiteralNode>(decl->value);
            }
        }
        if (!table)
            return;

        std::unordered_map<std::string, std::shared_ptr<Expression>> entries;
        std::vector<std::string> entryOrder;
        for (const auto &entry : table->expressions) {
            const auto pair = std::dynamic_pointer_cast<BinaryExpressionNode>(entry);
            const auto id = pair ? std::dynamic_pointer_cast<IdentifierExpressionNode>(pair->left) : nullptr;
            if (!pair || pair->op != "=" || !id || !id->identifier || !ValidName(id->identifier->name) || !pair->right ||
                !entries.emplace(id->identifier->name, pair->right).second)
                return;
            entryOrder.push_back(id->identifier->name);
        }

        std::unordered_map<std::string, size_t> functionSources;
        std::unordered_map<std::string, std::string> functionNames;
        std::unordered_set<std::string> keys;
        for (const auto &name : entryOrder)
            keys.insert(name);
        for (size_t i = tableIndex + 1; i + 1 < statements.size(); ++i) {
            if (std::dynamic_pointer_cast<ClassDeclarationNode>(statements[i]))
                return;
            if (const auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements[i]); fn &&
                fn->functionName.starts_with(tableName + ".")) {
                const std::string key = fn->functionName.substr(tableName.size() + 1);
                if (!ValidName(key) || !functionSources.emplace(key, i).second)
                    return;
                keys.insert(key);
            }
            const auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(statements[i]);
            const auto member = assignment ? std::dynamic_pointer_cast<MemberExpressionNode>(assignment->left) : nullptr;
            if (!member || !IsName(member->table, tableName))
                continue;
            const auto key = std::dynamic_pointer_cast<StringLiteralNode>(member->key);
            if (!key || !ValidName(key->value))
                return;
            keys.insert(key->value);
            const auto rhs = std::dynamic_pointer_cast<IdentifierExpressionNode>(assignment->right);
            if (!rhs || !rhs->identifier || functionSources.contains(key->value))
                continue;
            for (size_t j = 0; j < i; ++j) {
                const auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements[j]);
                if (fn && fn->bIsLocalDeclaration && fn->functionName == rhs->identifier->name) {
                    functionSources.emplace(key->value, j);
                    functionNames.emplace(key->value, rhs->identifier->name);
                    break;
                }
            }
        }
        if (keys.empty())
            return;
        for (size_t i = 0; i + 1 < statements.size(); ++i)
            if (i != tableIndex && !WalkStatement(statements[i], tableName, keys, false))
                return;

        std::unordered_set<size_t> deadLocals;
        for (size_t i = 0; i < tableIndex; ++i) {
            const auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(statements[i]);
            const auto id = decl ? std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier) : nullptr;
            if (!id || !id->identifier || !entries.contains(id->identifier->name) || functionSources.contains(id->identifier->name) ||
                !(std::dynamic_pointer_cast<NumberLiteralNode>(decl->value) || std::dynamic_pointer_cast<IntegerLiteralNode>(decl->value) ||
                  std::dynamic_pointer_cast<StringLiteralNode>(decl->value) || std::dynamic_pointer_cast<BooleanLiteralNode>(decl->value) ||
                  std::dynamic_pointer_cast<NilLiteralNode>(decl->value)))
                continue;
            bool used = false;
            for (size_t j = 0; j + 1 < statements.size() && !used; ++j) {
                if (j == i || j == tableIndex)
                    continue;
                std::unordered_set<std::string> names;
                ScopeAwareRenamer::CollectIdentifierNames(statements[j], names);
                used = names.contains(id->identifier->name);
            }
            if (!used)
                deadLocals.insert(i);
        }

        std::unordered_map<std::string, std::shared_ptr<Expression>> foldedInitializers;
        std::unordered_set<size_t> foldedAssignments;
        for (size_t i = tableIndex + 1; i + 1 < statements.size(); ++i) {
            if (std::dynamic_pointer_cast<CommentNode>(statements[i]))
                continue;
            const auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(statements[i]);
            const auto member = assignment ? std::dynamic_pointer_cast<MemberExpressionNode>(assignment->left) : nullptr;
            const auto key = member && IsName(member->table, tableName) ? std::dynamic_pointer_cast<StringLiteralNode>(member->key) : nullptr;
            if (!key || !entries.contains(key->value) || functionSources.contains(key->value) ||
                !std::dynamic_pointer_cast<NilLiteralNode>(entries.at(key->value)) || foldedInitializers.contains(key->value))
                break;
            foldedInitializers.emplace(key->value, assignment->right);
            foldedAssignments.insert(i);
        }

        for (const auto &[key, oldName] : functionNames)
            if (oldName != key)
                for (size_t i = functionSources.at(key); i + 1 < statements.size(); ++i)
                    ScopeAwareRenamer::RenameInStatement(statements[i], oldName, key);

        std::vector<std::shared_ptr<Statement>> result;
        result.reserve(statements.size() + entryOrder.size());
        std::unordered_set<std::string> declared;
        for (size_t i = 0; i + 1 < statements.size(); ++i) {
            if (deadLocals.contains(i))
                continue;
            if (foldedAssignments.contains(i))
                continue;
            if (i == tableIndex) {
                for (const auto &key : entryOrder) {
                    if (functionSources.contains(key))
                        continue;
                    auto decl = std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(key)),
                                                                          foldedInitializers.contains(key) ? foldedInitializers.at(key) : entries.at(key));
                    decl->bExported = true;
                    WalkExpression(decl->value, tableName, keys, true);
                    result.push_back(std::move(decl));
                    declared.insert(key);
                }
                continue;
            }
            auto stmt = statements[i];
            if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
                for (const auto &[key, source] : functionSources)
                    if (source == i) {
                        fn->functionName = key;
                        fn->bExported = true;
                        fn->bIsLocalDeclaration = false;
                        declared.insert(key);
                        break;
                    }
            }
            const auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt);
            const auto member = assignment ? std::dynamic_pointer_cast<MemberExpressionNode>(assignment->left) : nullptr;
            if (member && IsName(member->table, tableName)) {
                const auto key = std::dynamic_pointer_cast<StringLiteralNode>(member->key);
                if (key && functionNames.contains(key->value) && IsName(assignment->right, key->value))
                    continue;
                if (key && !declared.contains(key->value)) {
                    auto decl = std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(key->value)),
                                                                          assignment->right);
                    decl->bExported = true;
                    stmt = std::move(decl);
                    declared.insert(key->value);
                }
            }
            if (auto comment = std::dynamic_pointer_cast<CommentNode>(stmt); comment && comment->bIsInformational &&
                comment->comment.find(tableName) != std::string::npos)
                continue;
            WalkStatement(stmt, tableName, keys, true);
            result.push_back(std::move(stmt));
        }
        statements = std::move(result);
    }

  private:
    static bool IsName(const std::shared_ptr<Expression> &expr, const std::string &name) {
        const auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr);
        return id && id->identifier && id->identifier->name == name;
    }

    static bool IsKey(const std::shared_ptr<Expression> &expr, const std::string &key) {
        const auto str = std::dynamic_pointer_cast<StringLiteralNode>(expr);
        return str && str->value == key;
    }

    static bool ValidName(const std::string &name) {
        static const std::unordered_set<std::string> reserved = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
                                                                "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
        return ScopeAwareRenamer::IsBareIdentifier(name) && !reserved.contains(name);
    }

    static bool WalkExpression(std::shared_ptr<Expression> &expr, const std::string &table, const std::unordered_set<std::string> &keys, bool rewrite) {
        if (!expr)
            return true;
        if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
            if (IsName(member->table, table)) {
                const auto key = std::dynamic_pointer_cast<StringLiteralNode>(member->key);
                if (!key || !keys.contains(key->value))
                    return false;
                if (rewrite)
                    expr = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(key->value));
                return true;
            }
            return WalkExpression(member->table, table, keys, rewrite) && WalkExpression(member->key, table, keys, rewrite);
        }
        if (IsName(expr, table))
            return false;
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
            for (const auto &[_, arg] : fn->argumentsNames) {
                const auto id = arg ? std::dynamic_pointer_cast<IdentifierExpressionNode>(arg->argumentName) : nullptr;
                if (id && id->identifier && (id->identifier->name == table || keys.contains(id->identifier->name)))
                    return false;
            }
            return fn->lpFunctionBody && WalkBlock(fn->lpFunctionBody->body, table, keys, rewrite, true);
        }
        if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
            if (!WalkExpression(call->callee, table, keys, rewrite))
                return false;
            for (auto &arg : call->arguments)
                if (!WalkExpression(arg, table, keys, rewrite))
                    return false;
        } else if (auto call = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
            if (!WalkExpression(call->calledOn, table, keys, rewrite))
                return false;
            for (auto &arg : call->arguments)
                if (!WalkExpression(arg, table, keys, rewrite))
                    return false;
        } else if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
            return WalkExpression(binary->left, table, keys, rewrite) && WalkExpression(binary->right, table, keys, rewrite);
        } else if (auto binary = std::dynamic_pointer_cast<CompoundBinaryExpressionNode>(expr)) {
            return WalkExpression(binary->left, table, keys, rewrite) && WalkExpression(binary->right, table, keys, rewrite);
        } else if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
            return WalkExpression(index->left, table, keys, rewrite) && WalkExpression(index->right, table, keys, rewrite);
        } else if (auto conditional = std::dynamic_pointer_cast<IfExpressionNode>(expr)) {
            return WalkExpression(conditional->condition, table, keys, rewrite) && WalkExpression(conditional->thenExpr, table, keys, rewrite) &&
                   WalkExpression(conditional->elseExpr, table, keys, rewrite);
        } else if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
            return WalkExpression(unary->operand, table, keys, rewrite);
        } else if (auto literal = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
            for (auto &entry : literal->expressions)
                if (!WalkExpression(entry, table, keys, rewrite))
                    return false;
        }
        return true;
    }

    static bool WalkBlock(std::vector<std::shared_ptr<Statement>> &stmts, const std::string &table, const std::unordered_set<std::string> &keys,
                          bool rewrite, bool inFunction = false) {
        for (auto &stmt : stmts)
            if (!WalkStatement(stmt, table, keys, rewrite, inFunction))
                return false;
        return true;
    }

    static bool WalkStatement(std::shared_ptr<Statement> &stmt, const std::string &table, const std::unordered_set<std::string> &keys, bool rewrite,
                              bool inFunction = false) {
        if (std::dynamic_pointer_cast<CommentNode>(stmt) || std::dynamic_pointer_cast<BreakStatementNode>(stmt) ||
            std::dynamic_pointer_cast<ContinueStatementNode>(stmt))
            return true;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            const auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
            return !IsName(decl->identifier, table) && !(inFunction && id && id->identifier && keys.contains(id->identifier->name)) &&
                   WalkExpression(decl->value, table, keys, rewrite);
        }
        if (auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
            return WalkExpression(assignment->left, table, keys, rewrite) && WalkExpression(assignment->right, table, keys, rewrite);
        if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (auto &value : ret->returnValues)
                if (!WalkExpression(value, table, keys, rewrite))
                    return false;
            return true;
        }
        if (auto block = std::dynamic_pointer_cast<BlockStatementNode>(stmt))
            return WalkBlock(block->body, table, keys, rewrite, inFunction);
        if (auto branch = std::dynamic_pointer_cast<IfStatementNode>(stmt))
            return WalkExpression(branch->condition, table, keys, rewrite) &&
                   (!branch->thenBranch || WalkBlock(branch->thenBranch->body, table, keys, rewrite, inFunction)) &&
                   (!branch->elseBranch || WalkBlock(branch->elseBranch->body, table, keys, rewrite, inFunction));
        if (auto loop = std::dynamic_pointer_cast<WhileStatementNode>(stmt))
            return WalkExpression(loop->condition, table, keys, rewrite) && (!loop->body || WalkBlock(loop->body->body, table, keys, rewrite, inFunction));
        if (auto loop = std::dynamic_pointer_cast<RepeatStatementNode>(stmt))
            return (!loop->body || WalkBlock(loop->body->body, table, keys, rewrite, inFunction)) && WalkExpression(loop->condition, table, keys, rewrite);
        if (auto loop = std::dynamic_pointer_cast<ForNumericNode>(stmt))
            return WalkExpression(loop->startVariable, table, keys, rewrite) && WalkExpression(loop->maxIncreased, table, keys, rewrite) &&
                   WalkExpression(loop->increaseBy, table, keys, rewrite) &&
                   (!loop->lpLoopBody || WalkBlock(loop->lpLoopBody->body, table, keys, rewrite, inFunction));
        if (auto loop = std::dynamic_pointer_cast<ForGeneralNode>(stmt))
            return WalkExpression(loop->generator, table, keys, rewrite) && WalkExpression(loop->state, table, keys, rewrite) &&
                   WalkExpression(loop->index, table, keys, rewrite) && (!loop->body || WalkBlock(loop->body->body, table, keys, rewrite, inFunction));
        if (auto expr = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt))
            return WalkExpression(expr->expression, table, keys, rewrite);
        if (auto expr = std::dynamic_pointer_cast<Expression>(stmt)) {
            auto copy = expr;
            return WalkExpression(copy, table, keys, rewrite);
        }
        return false;
    }
};
