//
// Floating-point classification that survives fast-math.
//

#pragma once

#include <bit>
#include <cstdint>

namespace Fission {
    // `std::isnan` and `std::isfinite` are folded to constants under -ffast-math (-ffinite-math-only), and
    // the compiler is then free to assume non-finite values never reach arithmetic in the first place.
    // Classifying the IEEE-754 bit pattern keeps these checks exact in every build, and keeps a NaN out of
    // the undefined double-to-integer conversions they guard.
    inline bool IsNaNDouble(double value) { return (std::bit_cast<uint64_t>(value) & 0x7fffffffffffffffULL) > 0x7ff0000000000000ULL; }

    inline bool IsFiniteDouble(double value) { return (std::bit_cast<uint64_t>(value) & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
} // namespace Fission
