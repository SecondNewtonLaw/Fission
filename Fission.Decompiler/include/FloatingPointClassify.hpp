//
// Floating-point classification for fast-math builds.
//

#pragma once

#include <bit>
#include <cstdint>

namespace Fission {
    // -ffast-math (-ffinite-math-only) folds std::isnan to false. Test the IEEE-754 bits instead.
    inline bool IsNaNDouble(double value) { return (std::bit_cast<uint64_t>(value) & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL; }

    // -ffast-math (-ffinite-math-only) folds std::isfinite to true. Test the exponent bits instead.
    inline bool IsFiniteDouble(double value) { return (std::bit_cast<uint64_t>(value) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
} // namespace Fission
