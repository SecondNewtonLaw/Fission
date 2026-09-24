//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#pragma once

#include "Decompiler.hpp"
#include <cstddef>
#include <string>
#include <string_view>

namespace lifting_semantics_test {
    void EnableLuauFFlagsOnce();
    std::string DecompileOrFail(const std::string &source, int optLevel = 1, DecompilerFlags flags = static_cast<DecompilerFlags>(0));
    std::string DecompileVanillaOrFail(const std::string &bytecode);
    std::string ExtractFirstNumericForLoopVar(const std::string &source);
    bool Contains(const std::string &haystack, const std::string &needle);
    std::string DecodeBase64(std::string_view input);
    std::string DecompileItemSpawnOrFail();
    size_t LineIndentContaining(const std::string &source, std::string_view needle);
    bool CompilesOk(const std::string &source);
    void CheckSameTrace(const std::string &source, const std::string &decompiled, int optLevel = 0);
    void CheckSameTraceWithPrelude(const std::string &source, const std::string &preludeSource);
    size_t CountOccurrences(const std::string &haystack, const std::string &needle);
} // namespace lifting_semantics_test
