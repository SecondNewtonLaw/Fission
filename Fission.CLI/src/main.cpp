//
// Created by Dottik on 5/10/2025.
//

#include "Decompiler.hpp"
#include "libassert/assert.hpp"

int main() {
    Decompiler decompiler{};
    ASSERT(
        decompiler.DecompileRobloxBytecodeFromFile(
            "bytecode_raw.txt",
            DecompilerFlags::PrintTimingBreakdown | DecompilerFlags::WriteIRToFile | DecompilerFlags::GenerateSSAIRGraph | DecompilerFlags::GenerateIRGraph
        ) == DecompileResult::Success,
        "Decompilation failed."
    );
    return 0;
}