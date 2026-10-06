//
// Created by Dottik on 1/10/2026.
//

#include "Decompiler.hpp"

#include <boost/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

static constexpr int kCompileLevelCount = 3;
static constexpr int kMaximumCompileLevel = 2;
static constexpr int kDefaultBudgetMs = 120000;
static constexpr int kMinimumBudgetMs = 1;
static constexpr int kMaximumBudgetMs = 2147483647;
static constexpr size_t kFlushRecordCount = 32;
static constexpr size_t kFlushByteCount = 65536;
static constexpr unsigned int kFreshCliSeed = 1;
static constexpr size_t kBytecodeVersionSize = 1;
static constexpr int kWorkerArgumentCount = 2;
static constexpr int kFirstArgumentIndex = 1;

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
        "       Fission.Batch --worker\n"
        "  --worker                                  persistent source requests on stdin, JSONL events on stdout\n"
        "    request: requestId, kind=source, budgetMs, reportProgress?, inputs[{id,path,source}], "
        "profiles[{id,optimizationLevel,debugLevel,flags,inputIds?}]\n"
        "    events: record (existing fields + requestId/id/profileId), complete (records/failures), error\n"
        "    optional stderr event: record-start (requestId/profileId/id/path)\n"
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

static bool ApplyFlag(Options &options, std::string_view flag) {
    if (flag == "--ast")
        options.flags |= DecompilerFlags::CaptureAST;
    else if (flag == "--cfg")
        options.flags |= DecompilerFlags::CaptureCFGGraph;
    else if (flag == "--notes")
        options.flags |= DecompilerFlags::FissionDebugNotes;
    else if (flag == "--recover-inline")
        options.flags |= DecompilerFlags::RecoverInline;
    else if (flag == "--no-comments")
        options.flags |= DecompilerFlags::OmitFissionComments;
    else if (flag == "--no-names")
        options.flags &= ~DecompilerFlags::AutoNameVariables;
    else if (flag == "--types")
        options.flags |= DecompilerFlags::InferTypes;
    else if (flag == "--roblox-types")
        options.flags |= DecompilerFlags::InferRobloxTypes;
    else if (flag == "--no-ir")
        options.includeIR = false;
    else
        return false;
    return true;
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
        } else if (ApplyFlag(options, arg)) {
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

static boost::json::object DecompileBytes(
    boost::json::object record, const std::string &bytecode, const Options &options, double compileSeconds, std::chrono::steady_clock::time_point total
) {
    Decompiler decompiler;
    decompiler.SetDecompileBudget(options.budget);
    const auto start = std::chrono::steady_clock::now();
    const bool roblox = options.kind == InputKind::Roblox || options.kind == InputKind::RobloxBase64;
    const auto flags = options.includeIR ? options.flags : options.flags | DecompilerFlags::OmitIR;
    const auto result = roblox ? decompiler.DecompileRobloxBytecode(bytecode, flags) : decompiler.DecompileVanillaBytecode(bytecode, flags);
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
    return DecompileBytes(std::move(record), bytecode, options, compileSeconds, total);
}

static void ValidateFields(const boost::json::object &object, std::initializer_list<std::string_view> fields) {
    for (const auto &field : object)
        if (std::ranges::find(fields, std::string_view{field.key()}) == fields.end())
            throw std::invalid_argument("unknown field: " + std::string{field.key()});
}

static std::string_view JsonString(const boost::json::value &value) {
    const auto &string = value.as_string();
    return {string.data(), string.size()};
}

static int JsonInteger(const boost::json::value &value, int minimum, int maximum) {
    if (!value.is_int64() || value.as_int64() < minimum || value.as_int64() > maximum)
        throw std::invalid_argument("integer outside allowed range " + std::to_string(minimum) + ".." + std::to_string(maximum));
    return static_cast<int>(value.as_int64());
}

struct WorkerProfile {
    std::string id;
    Options options;
    std::vector<size_t> inputs;
};

struct CompiledSource {
    std::string bytecode;
    std::string error;
};

static std::unordered_map<std::string_view, size_t> ValidateWorkerInputs(const boost::json::array &inputs) {
    std::unordered_map<std::string_view, size_t> inputIndices;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto &input = inputs[i].as_object();
        ValidateFields(input, {"id", "path", "source"});
        const auto id = JsonString(input.at("id"));
        if (id.empty() || !inputIndices.emplace(id, i).second)
            throw std::invalid_argument("input IDs must be nonempty and unique");
        JsonString(input.at("path"));
        JsonString(input.at("source"));
    }
    return inputIndices;
}

static WorkerProfile ParseWorkerProfile(
    const boost::json::object &profile, const boost::json::array &inputs, const std::unordered_map<std::string_view, size_t> &inputIndices, int budgetMs
) {
    ValidateFields(profile, {"id", "optimizationLevel", "debugLevel", "flags", "inputIds"});
    WorkerProfile validated;
    validated.id = JsonString(profile.at("id"));
    validated.options.budget = std::chrono::milliseconds{budgetMs};
    validated.options.optimizationLevel = JsonInteger(profile.at("optimizationLevel"), 0, kMaximumCompileLevel);
    validated.options.debugLevel = JsonInteger(profile.at("debugLevel"), 0, kMaximumCompileLevel);
    if (const auto flags = profile.if_contains("flags"))
        for (const auto &flag : flags->as_array())
            if (!ApplyFlag(validated.options, JsonString(flag)))
                throw std::invalid_argument("unsupported profile flag: " + std::string{JsonString(flag)});
    if (const auto selected = profile.if_contains("inputIds")) {
        std::unordered_set<size_t> seen;
        for (const auto &inputId : selected->as_array()) {
            const auto found = inputIndices.find(JsonString(inputId));
            if (found == inputIndices.end() || !seen.insert(found->second).second)
                throw std::invalid_argument("inputIds must contain unique known IDs");
            validated.inputs.push_back(found->second);
        }
    } else {
        for (size_t i = 0; i < inputs.size(); ++i)
            validated.inputs.push_back(i);
    }
    std::ranges::stable_sort(validated.inputs, [&](size_t lhs, size_t rhs) {
        return JsonString(inputs[lhs].as_object().at("path")) < JsonString(inputs[rhs].as_object().at("path"));
    });
    return validated;
}

static std::vector<WorkerProfile> ParseWorkerProfiles(const boost::json::object &request) {
    ValidateFields(request, {"requestId", "kind", "budgetMs", "inputs", "profiles", "reportProgress"});
    if (const auto progress = request.if_contains("reportProgress"); progress && !progress->is_bool())
        throw std::invalid_argument("reportProgress must be boolean");
    if (JsonString(request.at("requestId")).empty() || JsonString(request.at("kind")) != "source")
        throw std::invalid_argument("nonempty requestId and kind=source required");
    const auto budget = request.if_contains("budgetMs");
    const auto budgetMs = budget ? JsonInteger(*budget, kMinimumBudgetMs, kMaximumBudgetMs) : kDefaultBudgetMs;
    const auto &inputs = request.at("inputs").as_array();
    const auto &profiles = request.at("profiles").as_array();
    if (inputs.empty() || profiles.empty())
        throw std::invalid_argument("inputs and profiles must be nonempty");
    const auto inputIndices = ValidateWorkerInputs(inputs);
    std::vector<WorkerProfile> validatedProfiles;
    std::unordered_set<std::string_view> profileIds;
    for (const auto &value : profiles) {
        const auto &profile = value.as_object();
        const auto id = JsonString(profile.at("id"));
        if (id.empty() || !profileIds.insert(id).second)
            throw std::invalid_argument("profile IDs must be nonempty and unique");
        validatedProfiles.push_back(ParseWorkerProfile(profile, inputs, inputIndices, budgetMs));
    }
    return validatedProfiles;
}

using WorkerCompileCache = std::unordered_map<std::string_view, std::array<std::optional<CompiledSource>, kCompileLevelCount * kCompileLevelCount>>;

static CompiledSource CompileWorkerSource(std::string_view source, const Options &options) {
    CompiledSource compiled;
    try {
        Luau::CompileOptions compileOptions{};
        compileOptions.optimizationLevel = options.optimizationLevel;
        compileOptions.debugLevel = options.debugLevel;
        compiled.bytecode = Luau::compile(std::string{source}, compileOptions);
        if (!compiled.bytecode.empty() && compiled.bytecode.front() == '\0')
            compiled.error = compiled.bytecode.substr(kBytecodeVersionSize);
    } catch (const std::exception &error) {
        compiled.error = error.what();
    }
    return compiled;
}

static boost::json::object
DecompileWorkerInput(const boost::json::object &input, const WorkerProfile &profile, std::string_view requestId, WorkerCompileCache &compiled) {
    const auto total = std::chrono::steady_clock::now();
    const auto &options = profile.options;
    const auto source = JsonString(input.at("source"));
    auto &cached = compiled[source].at(options.optimizationLevel * kCompileLevelCount + options.debugLevel);
    double compileSeconds = 0;
    const bool compileReused = cached.has_value();
    if (!cached) {
        const auto start = std::chrono::steady_clock::now();
        cached = CompileWorkerSource(source, options);
        compileSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    boost::json::object record{
        {"event", "record"},
        {"requestId", requestId},
        {"id", input.at("id")},
        {"profileId", profile.id},
        {"path", input.at("path")},
        {"input", input.at("source")},
        {"optimizationLevel", options.optimizationLevel},
        {"debugLevel", options.debugLevel},
        {"compileReused", compileReused}
    };
    if (cached->error.empty())
        return DecompileBytes(std::move(record), cached->bytecode, options, compileSeconds, total);
    record["status"] = "failed_to_compile";
    record["error"] = cached->error;
    record["bytecodeSize"] = cached->bytecode.size();
    record["output"] = "";
    record["timingBreakdown"] = "";
    record["timings"] = {
        {"compileSeconds", compileSeconds},
        {"decompileSeconds", 0.0},
        {"totalSeconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - total).count()}
    };
    if (options.includeIR)
        record["ir"] = "";
    return record;
}

static void RunWorkerRequest(const boost::json::object &request) {
    const auto validatedProfiles = ParseWorkerProfiles(request);
    const auto requestId = JsonString(request.at("requestId"));
    const auto &inputs = request.at("inputs").as_array();
    const auto progress = request.if_contains("reportProgress");
    const bool reportProgress = progress && progress->as_bool();
    WorkerCompileCache compiled;
    size_t records = 0, failures = 0, bufferedBytes = 0;
    for (const auto &profile : validatedProfiles) {
        // CLI-compatible anonymous names. NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc51-cpp)
        std::srand(kFreshCliSeed);
        for (const auto index : profile.inputs) {
            const auto &input = inputs[index].as_object();
            if (reportProgress) {
                std::cerr << boost::json::serialize(
                                 boost::json::object{
                                     {"event", "record-start"},
                                     {"requestId", requestId},
                                     {"profileId", profile.id},
                                     {"id", input.at("id")},
                                     {"path", input.at("path")}
                                 }
                             )
                          << '\n';
                std::cerr.flush();
            }
            const auto record = DecompileWorkerInput(input, profile, requestId, compiled);
            failures += record.at("status").as_string() != "success";
            ++records;
            const auto line = boost::json::serialize(record);
            std::cout << line << '\n';
            bufferedBytes += line.size() + sizeof('\n');
            if (records % kFlushRecordCount == 0 || bufferedBytes >= kFlushByteCount) {
                std::cout.flush();
                bufferedBytes = 0;
            }
        }
    }
    std::cout << boost::json::serialize(boost::json::object{{"event", "complete"}, {"requestId", requestId}, {"records", records}, {"failures", failures}})
              << '\n';
    std::cout.flush();
}

static int RunWorker() {
    std::string line;
    while (std::getline(std::cin, line)) {
        boost::json::value request;
        try {
            request = boost::json::parse(line);
            RunWorkerRequest(request.as_object());
        } catch (const std::exception &error) {
            boost::json::object event{{"event", "error"}, {"error", error.what()}};
            if (request.is_object())
                if (const auto id = request.as_object().if_contains("requestId"); id && id->is_string())
                    event["requestId"] = *id;
            std::cout << boost::json::serialize(event) << '\n';
            std::cout.flush();
        }
        if (!std::cout)
            return EXIT_FAILURE;
    }
    return std::cin.bad() ? EXIT_FAILURE : EXIT_SUCCESS;
}

static void EnableLuauFlags() {
    // integer constants deserialize only with the experimental Luau flags on
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

int main(int argc, char **argv) {
    EnableLuauFlags();
    if (argc == kWorkerArgumentCount && std::strcmp(argv[kFirstArgumentIndex], "--worker") == 0)
        return RunWorker();
    const auto options = ParseArguments(argc, argv);
    if (!options) {
        PrintUsage();
        return 2;
    }

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
