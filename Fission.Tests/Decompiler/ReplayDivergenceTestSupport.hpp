//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#pragma once

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include <string>
#include <utility>

namespace replay_divergence {
    struct CompileLevels {
        int optimization, debug;
        DecompilerFlags flags;
        CompileLevels(int optimizationLevel, int debugLevel, DecompilerFlags decompileFlags = fuzz::decompileFlags)
            : optimization(std::exchange(fuzz::optimizationLevel, optimizationLevel)), debug(std::exchange(fuzz::debugLevel, debugLevel)),
              flags(std::exchange(fuzz::decompileFlags, decompileFlags)) {}
        ~CompileLevels() {
            fuzz::optimizationLevel = optimization;
            fuzz::debugLevel = debug;
            fuzz::decompileFlags = flags;
        }
    };

    void CheckSemanticParity(const std::string &source, int optimizationLevel = fuzz::kOpt, int debugLevel = fuzz::kDebug);
} // namespace replay_divergence
