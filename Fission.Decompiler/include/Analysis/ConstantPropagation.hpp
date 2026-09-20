#pragma once

#include "ControlFlowAnalyzer.hpp"

#include <cstddef>

namespace Fission {

    struct ConstantPropagationStats {
        size_t foldedInstructions = 0;
        size_t prunedBranches = 0;

        [[nodiscard]] bool Changed() const { return foldedInstructions != 0 || prunedBranches != 0; }
    };

    class ConstantPropagation {
      public:
        ConstantPropagationStats Run(AnalyzedFunction &function);
    };

} // namespace Fission
