//
// Minimal Luau source -> bytecode compiler used to produce real test binaries.
//

#include "Luau/BytecodeBuilder.h"
#include "Luau/Compiler.h"

#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

    int ParseLevel(const std::string &value, int fallback) {
        int parsed = fallback;
        const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{})
            return fallback;
        return parsed;
    }

    std::string ReadFile(const std::string &path, bool &ok) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            ok = false;
            return {};
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        ok = true;
        return buffer.str();
    }

    bool WriteFile(const std::string &path, const std::string &contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        if (!stream)
            return false;
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return static_cast<bool>(stream);
    }

    void PrintUsage(const char *program) { std::cerr << "usage: " << program << " <input.luau> <output.luac> [-O0|-O1|-O2] [-g0|-g1|-g2]\n"; }

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }

    const std::string inputPath = argv[1];
    const std::string outputPath = argv[2];

    Luau::CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 2;

    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument.size() >= 3 && argument[0] == '-' && (argument[1] == 'O' || argument[1] == 'g')) {
            const int level = ParseLevel(argument.substr(2), argument[1] == 'O' ? 1 : 2);
            if (argument[1] == 'O')
                options.optimizationLevel = level;
            else
                options.debugLevel = level;
            continue;
        }
        std::cerr << "error: unknown argument '" << argument << "'\n";
        PrintUsage(argv[0]);
        return 1;
    }

    bool readOk = false;
    const std::string source = ReadFile(inputPath, readOk);
    if (!readOk) {
        std::cerr << "error: could not read '" << inputPath << "'\n";
        return 1;
    }

    try {
        Luau::BytecodeBuilder builder;
        Luau::compileOrThrow(builder, source, options);
        const std::string &bytecode = builder.getBytecode();
        if (bytecode.empty()) {
            std::cerr << "error: compiler produced empty bytecode\n";
            return 1;
        }
        if (!WriteFile(outputPath, bytecode)) {
            std::cerr << "error: could not write '" << outputPath << "'\n";
            return 1;
        }
        std::cerr << "wrote " << bytecode.size() << " bytes (bytecode v" << static_cast<int>(builder.getVersion()) << ", types v"
                  << static_cast<int>(builder.getTypeEncodingVersion()) << ") to " << outputPath << "\n";
    } catch (const Luau::CompileError &error) {
        std::cerr << "compile error: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
