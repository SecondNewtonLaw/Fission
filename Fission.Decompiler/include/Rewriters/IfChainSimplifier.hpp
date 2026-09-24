//
// Created by Dottik on 2/6/2026.
//

// Flips inverted branches so nested else blocks render as elseif chains.

#pragma once
#include "Rewriters/ASTRewriter.hpp"
#include "SourceGenerator/Generator.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <string>

class IfChainSimplifier : public ASTRewriter {
  protected:
    void RewriteStatements(std::vector<std::shared_ptr<Statement>> &stmts) override {
        HoistSharedReturn(stmts);
        Simplify(stmts);
    }

  private:
    // Luau copies the return that follows an if chain into each arm, so the chain arrives as guards that return.
    // Where a block continues into `return R`, `if c then A; return R end; rest` is `if c then A else rest end`.
    // A lone `if c then return R end` guard clause is left alone unless an arm with work shows the split.
    void HoistSharedReturn(std::vector<std::shared_ptr<Statement>> &stmts) {
        if (stmts.size() < 2)
            return;
        std::shared_ptr<Statement> hoisted = stmts.back();
        std::vector<std::shared_ptr<Statement>> body(stmts.begin(), stmts.end() - 1);
        if (const auto last = std::dynamic_pointer_cast<IfStatementNode>(stmts.back())) {
            // an if/else closing the block returns the same value on every path
            if (!last->thenBranch || last->thenBranch->body.empty() || !std::dynamic_pointer_cast<ReturnStatementNode>(last->thenBranch->body.back()))
                return;
            hoisted = last->thenBranch->body.back();
            if (!ReturnsOnEveryPath(last, Render(hoisted)))
                return;
            body = stmts;
        } else if (!std::dynamic_pointer_cast<ReturnStatementNode>(hoisted)) {
            return;
        }
        // a bare `if c then ...; return end` is the usual guard clause and stays one
        if (std::static_pointer_cast<ReturnStatementNode>(hoisted)->returnValues.empty())
            return;
        const auto returned = Render(hoisted);
        const auto split = std::any_of(body.begin(), body.end(), [&](const std::shared_ptr<Statement> &stmt) {
            const auto guard = std::dynamic_pointer_cast<IfStatementNode>(stmt);
            return guard && !guard->elseBranch && guard->thenBranch && guard->thenBranch->body.size() > 1 && Render(guard->thenBranch->body.back()) == returned;
        });
        // statements from the first if on may move into an arm, scoping their locals to it; the hoisted return may not read one
        const auto moved = std::ranges::find_if(body, [](const std::shared_ptr<Statement> &stmt) { return std::dynamic_pointer_cast<IfStatementNode>(stmt) != nullptr; });
        if (!split || DeclaresNameIn({moved, body.end()}, returned))
            return;
        FoldIntoReturn(body, returned);
        body.push_back(hoisted);
        stmts = std::move(body);
    }

    static bool ReturnsOnEveryPath(const std::shared_ptr<IfStatementNode> &branch, const std::string &returned) {
        const auto arm = [&](const std::shared_ptr<BlockStatementNode> &block) {
            if (!block || block->body.empty())
                return false;
            if (const auto nested = std::dynamic_pointer_cast<IfStatementNode>(block->body.back()); nested && nested->elseBranch)
                return ReturnsOnEveryPath(nested, returned);
            return std::dynamic_pointer_cast<ReturnStatementNode>(block->body.back()) && Render(block->body.back()) == returned;
        };
        return branch->elseBranch && arm(branch->thenBranch) && arm(branch->elseBranch);
    }

    void FoldIntoReturn(std::vector<std::shared_ptr<Statement>> &block, const std::string &returned) {
        if (!block.empty() && std::dynamic_pointer_cast<ReturnStatementNode>(block.back()) && Render(block.back()) == returned)
            block.pop_back();
        for (size_t k = 0; k < block.size(); ++k) {
            const auto branch = std::dynamic_pointer_cast<IfStatementNode>(block[k]);
            if (!branch || !branch->thenBranch)
                continue;
            // a closing if flows into the return from both arms
            if (k + 1 != block.size()) {
                if (branch->elseBranch || branch->thenBranch->body.empty() || Render(branch->thenBranch->body.back()) != returned)
                    continue;
                auto rest = std::make_shared<BlockStatementNode>();
                rest->body.assign(block.begin() + static_cast<std::ptrdiff_t>(k + 1), block.end());
                block.resize(k + 1);
                branch->elseBranch = rest;
            }
            for (const auto &arm : {branch->thenBranch, branch->elseBranch})
                if (arm) {
                    FoldIntoReturn(arm->body, returned);
                    Simplify(arm->body);
                }
            return;
        }
    }

    static bool DeclaresNameIn(const std::vector<std::shared_ptr<Statement>> &block, const std::string &text) {
        const auto mentions = [&](const std::string &name) {
            for (auto at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
                const auto word = [&](size_t i) { return i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'); };
                if ((at == 0 || !word(at - 1)) && !word(at + name.size()))
                    return true;
            }
            return false;
        };
        for (const auto &stmt : block) {
            if (const auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
                SourceGenerator generator;
                decl->identifier->Accept(&generator);
                std::string names = generator.buffer.str();
                std::ranges::replace(names, ',', ' ');
                std::istringstream words(names);
                for (std::string name; words >> name;)
                    if (mentions(name))
                        return true;
            } else if (const auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->bIsLocalDeclaration && mentions(fn->functionName)) {
                return true;
            } else if (const auto branch = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
                if ((branch->thenBranch && DeclaresNameIn(branch->thenBranch->body, text)) || (branch->elseBranch && DeclaresNameIn(branch->elseBranch->body, text)))
                    return true;
            }
        }
        return false;
    }

    static std::string Render(const std::shared_ptr<Statement> &stmt) {
        SourceGenerator generator;
        stmt->Accept(&generator);
        return generator.buffer.str();
    }

    void Simplify(std::vector<std::shared_ptr<Statement>> &stmts) {
        // `if c then ...; continue else B end` is `if c then ...; continue end; B`; B's locals would outlive it unless nothing follows
        for (size_t i = 0; i < stmts.size(); ++i) {
            const auto branch = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
            if (!branch || !branch->thenBranch || branch->thenBranch->body.empty() || !branch->elseBranch || branch->elseBranch->body.empty())
                continue;
            const auto &exit = branch->thenBranch->body.back();
            if (!std::dynamic_pointer_cast<ContinueStatementNode>(exit) && !std::dynamic_pointer_cast<BreakStatementNode>(exit))
                continue;
            const bool declares = std::ranges::any_of(branch->elseBranch->body, [](const std::shared_ptr<Statement> &stmt) {
                const auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt);
                return std::dynamic_pointer_cast<VariableDeclarationNode>(stmt) || (fn && fn->bIsLocalDeclaration);
            });
            if (declares && i + 1 != stmts.size())
                continue;
            const auto rest = std::move(branch->elseBranch->body);
            branch->elseBranch.reset();
            stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1), rest.begin(), rest.end());
        }
        for (auto &stmt : stmts) {
            auto ifS = std::dynamic_pointer_cast<IfStatementNode>(stmt);
            if (!ifS)
                continue;
            if (ifS->thenBranch && ifS->thenBranch->body.empty() && ifS->elseBranch && !ifS->elseBranch->body.empty()) {
                ifS->condition = InvertCondition(ifS->condition);
                std::swap(ifS->thenBranch, ifS->elseBranch);
            }
            if (ifS->elseBranch && ifS->elseBranch->body.empty())
                ifS->elseBranch.reset();
            if (!ifS->thenBranch || ifS->thenBranch->body.empty() || !ifS->elseBranch || ifS->elseBranch->body.empty())
                continue;
            const bool lifterOrder = ifS->bFallthroughInElse;
            if (lifterOrder) {
                ifS->condition = InvertCondition(ifS->condition);
                std::swap(ifS->thenBranch, ifS->elseBranch);
                ifS->bFallthroughInElse = false;
            }

            bool condNegated = false;
            if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition); u && u->op == "not ")
                condNegated = true;
            if (auto b = std::dynamic_pointer_cast<BinaryExpressionNode>(ifS->condition); b && b->op == "~=")
                condNegated = true;

            const bool thenLeadsWithIf = std::dynamic_pointer_cast<IfStatementNode>(ifS->thenBranch->body.front()) != nullptr;
            const bool elseLeadsWithIf = std::dynamic_pointer_cast<IfStatementNode>(ifS->elseBranch->body.front()) != nullptr;
            // a relational test cannot invert, so it arrives as `not (a < b)` with the arms swapped
            const auto notTest = std::dynamic_pointer_cast<UnaryExpressionNode>(ifS->condition);
            if ((condNegated && thenLeadsWithIf && !elseLeadsWithIf) || (!lifterOrder && notTest && notTest->op == "not " && !elseLeadsWithIf)) {
                ifS->condition = InvertCondition(ifS->condition);
                std::swap(ifS->thenBranch, ifS->elseBranch);
            }
            while (ifS->elseBranch && ifS->elseBranch->body.size() == 1) {
                auto inner = std::dynamic_pointer_cast<IfStatementNode>(ifS->elseBranch->body.front());
                if (!inner || !SameBody(ifS->thenBranch, inner->thenBranch))
                    break;
                ifS->condition = std::make_shared<BinaryExpressionNode>("or", ifS->condition, inner->condition);
                ifS->elseBranch = inner->elseBranch;
            }
        }
        for (size_t i = 0; i + 1 < stmts.size();) {
            auto first = std::dynamic_pointer_cast<IfStatementNode>(stmts[i]);
            auto next = std::dynamic_pointer_cast<IfStatementNode>(stmts[i + 1]);
            if (first && next && !first->elseBranch && first->thenBranch && first->thenBranch->body.size() == 1 &&
                std::dynamic_pointer_cast<ReturnStatementNode>(first->thenBranch->body.front())) {
                if (SameBody(first->thenBranch, next->thenBranch)) {
                    first->condition = std::make_shared<BinaryExpressionNode>("or", first->condition, next->condition);
                    first->elseBranch = next->elseBranch;
                    stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    continue;
                }
                if (SameBody(first->thenBranch, next->elseBranch)) {
                    first->condition = std::make_shared<BinaryExpressionNode>("or", first->condition, InvertCondition(next->condition));
                    first->elseBranch = next->thenBranch;
                    stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i + 1));
                    continue;
                }
            }
            ++i;
        }
    }

    static std::string RenderBody(const std::shared_ptr<BlockStatementNode> &body) {
        SourceGenerator generator;
        if (body)
            for (const auto &stmt : body->body)
                if (stmt)
                    stmt->Accept(&generator);
        return generator.buffer.str();
    }

    static bool SameBody(const std::shared_ptr<BlockStatementNode> &lhs, const std::shared_ptr<BlockStatementNode> &rhs) {
        return lhs && rhs && !lhs->body.empty() && RenderBody(lhs) == RenderBody(rhs);
    }

    // `not X` -> X; comparisons flip; anything else is wrapped in `not (...)`.
    static std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond) {
        if (auto u = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); u && u->op == "not ")
            return u->operand;
        if (auto b = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
            std::string inv;
            if (b->op == "==")
                inv = "~=";
            else if (b->op == "~=")
                inv = "==";
            if (!inv.empty())
                return std::make_shared<BinaryExpressionNode>(inv, b->left, b->right);
        }
        return std::make_shared<UnaryExpressionNode>("not ", cond);
    }
};
