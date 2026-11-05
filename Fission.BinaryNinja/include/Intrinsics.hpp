//
// Intrinsic operations used by the Luau bytecode architecture.
//

#pragma once

#include <cstdint>
#include <string>

namespace fission::bn {

    // Intrinsic ids are indices into the table returned by IntrinsicName().
    enum class Intrinsic : uint32_t {
        GetGlobal = 0,
        SetGlobal,
        GetUpvalue,
        SetUpvalue,
        GetImport,
        GetTable,
        SetTable,
        GetTableKeyed,
        SetTableKeyed,
        GetTableIndexed,
        SetTableIndexed,
        NewTable,
        DupTable,
        SetList,
        Concat,
        Length,
        LogicalAnd,
        LogicalOr,
        IntegerDivide,
        Modulo,
        Power,
        NewClosure,
        DupClosure,
        Capture,
        Call,
        NameCall,
        Vararg,
        PrepareVararg,
        IterPrepare,
        IterNext,
        NewClass,
        NewClassMember,
        Arithmetic,
        Pcall,
        Xpcall,
        Resume,
        CreateThread,
        Count
    };

    [[nodiscard]] inline constexpr uint32_t IntrinsicId(Intrinsic intrinsic) { return static_cast<uint32_t>(intrinsic); }

    [[nodiscard]] const char *IntrinsicName(uint32_t intrinsic);
    [[nodiscard]] uint32_t IntrinsicCount();

} // namespace fission::bn
