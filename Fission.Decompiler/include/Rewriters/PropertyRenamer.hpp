// Names locals from their sole property-read value source.

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"

#include <cctype>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class PropertyRenamer {
  public:
    void Run(std::vector<std::shared_ptr<Statement>> &statements) {
        ScopeAwareRenamer::Run(statements, [](const std::vector<std::shared_ptr<Statement>> &scope) {
            // Count every value source per name within this scope (descending
            // blocks/loops/ifs but not nested functions), remembering the leaf
            // when the source is a property read.
            std::unordered_map<std::string, int> srcCount;
            std::unordered_map<std::string, std::pair<bool, std::string>> srcInfo; // name -> (isPropertyRead, leaf)
            for (const auto &s : scope)
                ScanSources(s, srcCount, srcInfo);

            std::vector<std::pair<std::string, std::string>> candidates;
            for (const auto &[name, count] : srcCount) {
                if (count != 1)
                    continue; // reassigned / re-bound -> name would mislead
                auto it = srcInfo.find(name);
                if (it == srcInfo.end() || !it->second.first || it->second.second.empty())
                    continue;
                candidates.emplace_back(name, it->second.second);
            }
            return candidates;
        });
    }

  private:
    static std::string Decapitalize(const std::string &s) {
        if (s.empty())
            return s;
        std::string r = s;
        r[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[0])));
        return r;
    }

    // (isPropertyRead, decapitalized leaf) for a value expression.
    static std::pair<bool, std::string> PropLeaf(const std::shared_ptr<Expression> &value) {
        if (auto mem = std::dynamic_pointer_cast<MemberExpressionNode>(value)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(mem->key))
                return {true, Decapitalize(key->value)};
        }
        if (auto idx = std::dynamic_pointer_cast<IndexExpressionNode>(value)) {
            if (auto key = std::dynamic_pointer_cast<StringLiteralNode>(idx->right))
                return {true, Decapitalize(key->value)};
        }
        return {false, ""};
    }

    static void Bump(
        const std::string &name, const std::pair<bool, std::string> &info, std::unordered_map<std::string, int> &srcCount,
        std::unordered_map<std::string, std::pair<bool, std::string>> &srcInfo
    ) {
        srcCount[name]++;
        srcInfo[name] = info;
    }

    static void ScanSources(
        const std::shared_ptr<Statement> &stmt, std::unordered_map<std::string, int> &srcCount,
        std::unordered_map<std::string, std::pair<bool, std::string>> &srcInfo
    ) {
        if (!stmt)
            return;
        if (auto es = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            // call-result locals are value sources too (never property reads).
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(es->expression); call && call->bIsLocalDeclaration) {
                for (const auto &r : call->rets)
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(r); id && id->identifier)
                        Bump(id->identifier->name, {false, ""}, srcCount, srcInfo);
            } else if (auto nc = std::dynamic_pointer_cast<NameCallExpressionNode>(es->expression); nc && nc->bIsLocalDeclaration) {
                for (const auto &r : nc->rets)
                    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(r); id && id->identifier)
                        Bump(id->identifier->name, {false, ""}, srcCount, srcInfo);
            }
            return;
        }
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (decl->value)
                if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier); id && id->identifier)
                    Bump(id->identifier->name, PropLeaf(decl->value), srcCount, srcInfo);
            return;
        }
        if (auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left); id && id->identifier)
                Bump(id->identifier->name, PropLeaf(asn->right), srcCount, srcInfo);
            return;
        }
        if (auto b = std::dynamic_pointer_cast<BlockStatementNode>(stmt)) {
            for (const auto &s : b->body)
                ScanSources(s, srcCount, srcInfo);
            return;
        }
        if (auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            ScanSources(ifS->thenBranch, srcCount, srcInfo);
            ScanSources(ifS->elseBranch, srcCount, srcInfo);
            return;
        }
        if (auto w = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            ScanSources(w->body, srcCount, srcInfo);
            return;
        }
        if (auto r = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            ScanSources(r->body, srcCount, srcInfo);
            return;
        }
        if (auto fnum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            ScanSources(fnum->lpLoopBody, srcCount, srcInfo);
            return;
        }
        if (auto fgen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            ScanSources(fgen->body, srcCount, srcInfo);
            return;
        }
        // FunctionDeclaration: a new scope, handled by the engine's recursion.
    }
};
