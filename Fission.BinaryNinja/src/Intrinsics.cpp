//
// Intrinsic operations used by the Luau bytecode architecture.
//

#include "Intrinsics.hpp"

#include <array>

namespace fission::bn {

    namespace {

        constexpr std::array<const char *, static_cast<size_t>(Intrinsic::Count)> kNames = {
            "luau_getglobal",   "luau_setglobal",  "luau_getupvalue", "luau_setupvalue",
            "luau_getimport",   "luau_gettable",   "luau_settable",   "luau_gettableks",
            "luau_settableks",  "luau_gettablen",  "luau_settablen",  "luau_newtable",
            "luau_duptable",    "luau_setlist",    "luau_concat",     "luau_length",
            "luau_and",         "luau_or",         "luau_idiv",       "luau_mod",
            "luau_pow",         "luau_newclosure", "luau_dupclosure", "luau_capture",
            "luau_call",        "luau_namecall",   "luau_vararg",     "luau_prepvararg",
            "luau_iterprep",    "luau_iternext",   "luau_newclass",   "luau_newclassmember",
            "luau_arith",       "pcall",           "xpcall",          "coroutine.resume",
            "coroutine.create",
        };

        static_assert(kNames.size() == static_cast<size_t>(Intrinsic::Count));

    } // namespace

    const char *IntrinsicName(uint32_t intrinsic) { return intrinsic < kNames.size() ? kNames[intrinsic] : "luau_unknown"; }

    uint32_t IntrinsicCount() { return static_cast<uint32_t>(kNames.size()); }

} // namespace fission::bn
