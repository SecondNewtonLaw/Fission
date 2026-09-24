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
        CompileLevels(int optimizationLevel, int debugLevel)
            : optimization(std::exchange(fuzz::optimizationLevel, optimizationLevel)), debug(std::exchange(fuzz::debugLevel, debugLevel)) {}
        ~CompileLevels() {
            fuzz::optimizationLevel = optimization;
            fuzz::debugLevel = debug;
        }
    };

    void CheckSemanticParity(const std::string &source, int optimizationLevel = fuzz::kOpt, int debugLevel = fuzz::kDebug);
} // namespace replay_divergence
