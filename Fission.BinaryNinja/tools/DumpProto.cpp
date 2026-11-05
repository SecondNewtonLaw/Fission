//
// Diagnostic tool: decompile every proto in a Luau bytecode file in isolation.
// Used to check that per-function decompilation does not leak a nested proto's
// statements into its parent or vice versa.
//

#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "Image.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

    std::string ReadFile(const std::string &path) {
        std::ifstream stream(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        return buffer.str();
    }

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: DumpProto <file.luac> <protoId>\n";
        return 2;
    }

    const std::string bytes = ReadFile(argv[1]);
    if (bytes.empty()) {
        std::cerr << "failed to read " << argv[1] << "\n";
        return 1;
    }

    Deserializer deserializer;
    const auto bytecode = deserializer.Deserialize(bytes);
    if (!bytecode) {
        std::cerr << "failed to deserialize\n";
        return 1;
    }

    const uint32_t requested = static_cast<uint32_t>(std::stoul(argv[2]));
    for (const auto &function : bytecode->functions) {
        if (function.bytecodeId != requested)
            continue;
        std::cout << std::format("proto {} params={} nups={} isvararg={} instructions={}\n", function.bytecodeId, function.numparams, function.nups,
            function.isvararg, function.instructions.size());
        for (const auto &local : function.locvars)
            std::cout << std::format("  local {} reg={} startpc={} endpc={}\n", local.varname, local.reg, local.startpc, local.endpc);
        for (size_t i = 0; i < function.upvalueNames.size(); ++i)
            std::cout << std::format("  upvalue {} = {}\n", i, function.upvalueNames[i]);
    }

    Decompiler decompiler;
    const auto result = decompiler.DecompileVanillaFunction(bytes, requested);
    std::cout << "result=" << static_cast<int>(result.resultCode) << " error=" << result.errorMessage << "\n";
    std::cout << "---- begin ----\n" << result.decompilationOutput << "---- end ----\n";
    return result.resultCode == DecompileResult::Success ? 0 : 1;
}
