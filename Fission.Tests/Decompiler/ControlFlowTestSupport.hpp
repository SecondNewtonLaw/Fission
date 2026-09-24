//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#pragma once

#include <cstddef>
#include <regex>
#include <string>

namespace control_flow_test {
    void EnableLuauFFlagsOnce();
    std::string DecompileOrFail(const std::string &source, int optLevel = 1);
    bool Contains(const std::string &haystack, const std::string &needle);
    bool ContainsRegex(const std::string &haystack, const std::regex &pattern);
    bool Recompiles(const std::string &src);
    std::string FirstBetween(const std::string &haystack, const std::string &begin, const std::string &end);
    size_t CountOccurrences(const std::string &haystack, const std::string &needle);
} // namespace control_flow_test
