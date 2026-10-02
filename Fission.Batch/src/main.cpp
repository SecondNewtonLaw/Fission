//
// Created by Dottik on 1/10/2026.
//

#include "Decompiler.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

enum class InputKind { Source, Bytecode, Roblox, RobloxBase64 };

struct Options {
    InputKind kind = InputKind::Source;
    int optimizationLevel = 1;
    int debugLevel = 1;
    DecompilerFlags flags = DecompilerFlags::PrintTimingBreakdown | DecompilerFlags::AutoNameVariables;
    std::chrono::milliseconds budget{120000};
    std::string extension;
    std::string outPath;
    bool includeIR = true;
    std::vector<fs::path> inputs;
};

static void PrintUsage() {
    std::fputs(
        "usage: Fission.Batch [options] <file|dir>...\n"
        "Decompiles every input and writes one JSON object per line (stdout or --out).\n"
        "  --kind source|bytecode|roblox|roblox-b64   input format (default source)\n"
        "  --opt N / --debug N                        Luau compile levels for --kind source (default 1 / 1)\n"
        "  --ext .lua                                 only take files with this extension from directories\n"
        "  --out FILE                                 write JSONL here instead of stdout\n"
        "  --budget-ms N                              per-input decompile wall-clock budget (default 120000)\n"
        "  --ast --cfg --notes                        also capture AST JSON, CFG DOT, debug notes\n"
        "  --recover-inline --no-comments --no-names --types --roblox-types --no-ir\n",
        stderr
    );
}

static std::optional<Options> ParseArguments(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&]() -> std::optional<std::string> {
            if (i + 1 >= argc)
                return std::nullopt;
            return std::string{argv[++i]};
        };
        if (arg == "--kind") {
            const auto value = next();
            if (!value)
                return std::nullopt;
            if (*value == "source")
                options.kind = InputKind::Source;
            else if (*value == "bytecode")
                options.kind = InputKind::Bytecode;
            else if (*value == "roblox")
                options.kind = InputKind::Roblox;
            else if (*value == "roblox-b64")
                options.kind = InputKind::RobloxBase64;
            else
                return std::nullopt;
        } else if (arg == "--opt" || arg == "--debug" || arg == "--budget-ms") {
            const auto value = next();
            if (!value)
                return std::nullopt;
            const int number = std::atoi(value->c_str());
            if (arg == "--opt")
                options.optimizationLevel = std::clamp(number, 0, 2);
            else if (arg == "--debug")
                options.debugLevel = std::clamp(number, 0, 2);
            else
                options.budget = std::chrono::milliseconds{(std::max)(number, 1)};
        } else if (arg == "--ext" || arg == "--out") {
            const auto value = next();
            if (!value)
                return std::nullopt;
            (arg == "--ext" ? options.extension : options.outPath) = *value;
        } else if (arg == "--ast") {
            options.flags |= DecompilerFlags::CaptureAST;
        } else if (arg == "--cfg") {
            options.flags |= DecompilerFlags::CaptureCFGGraph;
        } else if (arg == "--notes") {
            options.flags |= DecompilerFlags::FissionDebugNotes;
        } else if (arg == "--recover-inline") {
            options.flags |= DecompilerFlags::RecoverInline;
        } else if (arg == "--no-comments") {
            options.flags |= DecompilerFlags::OmitFissionComments;
        } else if (arg == "--no-names") {
            options.flags &= ~DecompilerFlags::AutoNameVariables;
        } else if (arg == "--types") {
            options.flags |= DecompilerFlags::InferTypes;
        } else if (arg == "--roblox-types") {
            options.flags |= DecompilerFlags::InferRobloxTypes;
        } else if (arg == "--no-ir") {
            options.includeIR = false;
        } else if (arg == "-h" || arg == "--help" || arg.starts_with("--")) {
            return std::nullopt;
        } else {
            options.inputs.emplace_back(arg);
        }
    }
    if (options.inputs.empty())
        return std::nullopt;
    return options;
}

static std::optional<std::string> ReadBinary(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

static std::optional<std::string> DecodeBase64(const std::string &text) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    decoded.reserve(text.size() * 3 / 4);
    uint32_t bits = 0;
    int count = 0;
    for (const char c : text) {
        if (c == '=')
            break;
        if (std::isspace(static_cast<unsigned char>(c)))
            continue;
        const auto value = alphabet.find(c);
        if (value == std::string_view::npos)
            return std::nullopt;
        bits = (bits << 6) | static_cast<uint32_t>(value);
        if ((count += 6) >= 8) {
            count -= 8;
            decoded.push_back(static_cast<char>((bits >> count) & 0xFF));
        }
    }
    return decoded;
}

static const char *StatusName(DecompileResult code) {
    switch (code) {
    case DecompileResult::Success:
        return "success";
    case DecompileResult::FailedToReadFile:
        return "failed_to_read_file";
    case DecompileResult::FailedToDeserialize:
        return "failed_to_deserialize";
    case DecompileResult::FailedToDecompile:
        return "failed_to_decompile";
    }
    return "unknown";
}

static boost::json::object DecompileOne(const fs::path &path, const Options &options) {
    boost::json::object record;
    record["path"] = path.generic_string();

    const auto total = std::chrono::steady_clock::now();
    const auto input = ReadBinary(path);
    if (!input) {
        record["status"] = StatusName(DecompileResult::FailedToReadFile);
        return record;
    }

    std::string bytecode;
    double compileSeconds = 0;
    if (options.kind == InputKind::Source) {
        record["input"] = *input;
        Luau::CompileOptions compileOptions{};
        compileOptions.optimizationLevel = options.optimizationLevel;
        compileOptions.debugLevel = options.debugLevel;
        const auto start = std::chrono::steady_clock::now();
        bytecode = Luau::compile(*input, compileOptions);
        compileSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        // Luau encodes a compile error as a zero version byte followed by the message
        if (!bytecode.empty() && bytecode.front() == '\0') {
            record["status"] = "failed_to_compile";
            record["error"] = bytecode.substr(1);
            return record;
        }
    } else if (options.kind == InputKind::RobloxBase64) {
        const auto decoded = DecodeBase64(*input);
        if (!decoded) {
            record["status"] = StatusName(DecompileResult::FailedToDeserialize);
            record["error"] = "invalid base64";
            return record;
        }
        bytecode = *decoded;
    } else {
        bytecode = *input;
    }

    Decompiler decompiler;
    decompiler.SetDecompileBudget(options.budget);
    const auto start = std::chrono::steady_clock::now();
    const bool roblox = options.kind == InputKind::Roblox || options.kind == InputKind::RobloxBase64;
    const auto result = roblox ? decompiler.DecompileRobloxBytecode(bytecode, options.flags) : decompiler.DecompileVanillaBytecode(bytecode, options.flags);
    const auto end = std::chrono::steady_clock::now();

    record["status"] = StatusName(result.resultCode);
    if (options.kind == InputKind::Source) {
        record["optimizationLevel"] = options.optimizationLevel;
        record["debugLevel"] = options.debugLevel;
    }
    record["bytecodeSize"] = bytecode.size();
    record["output"] = result.decompilationOutput;
    if (options.includeIR)
        record["ir"] = result.irOutput;
    record["timingBreakdown"] = result.timingStatistics;
    record["timings"] = {
        {"compileSeconds", compileSeconds},
        {"decompileSeconds", std::chrono::duration<double>(end - start).count()},
        {"totalSeconds", std::chrono::duration<double>(end - total).count()},
    };
    if (!result.errorMessage.empty())
        record["error"] = result.errorMessage;
    if (!result.astJson.empty())
        record["ast"] = result.astJson;
    if (!result.cfgGraph.empty())
        record["cfg"] = result.cfgGraph;
    if (!result.debugNotes.empty())
        record["debugNotes"] = result.debugNotes;
    return record;
}

int main(int argc, char **argv) {
    const auto options = ParseArguments(argc, argv);
    if (!options) {
        PrintUsage();
        return 2;
    }

    // integer constants deserialize only with the experimental Luau flags on
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;

    std::vector<fs::path> files;
    for (const auto &input : options->inputs) {
        std::error_code error;
        if (fs::is_directory(input, error)) {
            for (const auto &entry : fs::recursive_directory_iterator(input, fs::directory_options::skip_permission_denied, error))
                if (entry.is_regular_file() && (options->extension.empty() || entry.path().extension() == options->extension))
                    files.push_back(entry.path());
        } else {
            files.push_back(input);
        }
    }
    std::ranges::sort(files);

    std::ofstream file;
    if (!options->outPath.empty()) {
        file.open(options->outPath, std::ios::binary);
        if (!file) {
            std::fprintf(stderr, "cannot open %s\n", options->outPath.c_str());
            return 1;
        }
    }
    std::ostream &out = options->outPath.empty() ? std::cout : file;

    size_t failures = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto record = DecompileOne(files[i], *options);
        failures += record.at("status").as_string() != "success";
        out << boost::json::serialize(record) << '\n';
        out.flush();
        std::fprintf(stderr, "[%zu/%zu] %s %s\n", i + 1, files.size(), record.at("status").as_string().c_str(), files[i].generic_string().c_str());
    }
    std::fprintf(stderr, "%zu inputs, %zu failed\n", files.size(), failures);
    return failures == 0 ? 0 : 1;
}
