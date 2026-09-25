//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#pragma once

#include "AbstractSyntaxTree/ASTNode.hpp"
#include <cstddef>
#include <memory>
#include <regex>
#include <string>

namespace control_flow_regression {
    void EnableLuauFFlagsOnce();
    std::string DecompileOrFail(const std::string &source, int optLevel = 1, int debugLevel = 2);
    bool Contains(const std::string &haystack, const std::string &needle);
    bool ContainsRegex(const std::string &haystack, const std::regex &pattern);
    bool Recompiles(const std::string &src);
    std::string FirstBetween(const std::string &haystack, const std::string &begin, const std::string &end);
    size_t CountOccurrences(const std::string &haystack, const std::string &needle);
    bool NoForwardReference(const std::string &decompiled);
} // namespace control_flow_regression

namespace scope_regression {
    std::shared_ptr<VariableDeclarationNode> LocalDecl(const std::string &name, const std::string &valueName);
    std::shared_ptr<ExpressionStatementNode> UseStmt(const std::string &name);
    bool IsDoBlock(const std::shared_ptr<Statement> &s);
} // namespace scope_regression
