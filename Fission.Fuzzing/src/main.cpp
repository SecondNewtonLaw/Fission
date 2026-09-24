// Fuzz generated source, Fission ASTs, and mutated bytecode with per-stage crash attribution.
#include "FissionAstGenerator.hpp"
#include "FuzzOracle.hpp"
#include "LuauAstGenerator.hpp"
#include "SSAOracle.hpp"
#include "SemanticOracle.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {
    // Make crashes die straight to stderr instead of popping a modal dialog, so the fuzzer can run
    // unattended: a debug-CRT assert (e.g. MSVC's checked operator[]), a failed C assert, and abort()
    // all otherwise block on a message box. Route all three to the console and disable Windows Error
    // Reporting's crash popup. The process still terminates; the breadcrumb identifies the input.
    void SuppressCrashDialogs() {
#ifdef _WIN32
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        for ([[maybe_unused]] int report : {_CRT_ASSERT, _CRT_ERROR, _CRT_WARN}) { // _Crt* are no-ops under NDEBUG
            _CrtSetReportMode(report, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(report, _CRTDBG_FILE_STDERR);
        }
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
    }

    void Write(const fs::path &p, const std::string &content) { std::ofstream(p, std::ios::binary) << content; }

    // In-memory breadcrumb: each worker keeps a tiny rolling list of its recent in-flight samples (no
    // per-sample disk IO; that was the throughput bottleneck). If a worker thread hard-faults (segfault
    // etc., not a catchable throw), the unhandled-exception filter runs on that thread and writes its
    // last entry out, so the culprit survives the crash.
    thread_local std::vector<std::string> g_breadcrumbTrail;
    std::string g_breadcrumbDir; // set before workers start

#ifdef _WIN32
    LONG WINAPI HardFaultBreadcrumb(EXCEPTION_POINTERS *) {
        if (!g_breadcrumbTrail.empty() && !g_breadcrumbDir.empty()) {
            std::ofstream(g_breadcrumbDir + "/_HARD_FAULT_" + std::to_string(::GetCurrentThreadId()) + ".lua", std::ios::binary) << g_breadcrumbTrail.back();
        }
        return EXCEPTION_EXECUTE_HANDLER; // terminate after the breadcrumb is on disk
    }
#endif

    struct Counters {
        std::map<std::string, int> buckets;       // outcome -> count
        std::map<std::string, int> crashStages;   // stage -> count
        std::map<std::string, int> savedExamples; // bucket -> dumped count (cap dumps)
    };

    void WriteBin(const fs::path &p, const std::string &content) {
        std::ofstream(p, std::ios::binary).write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    std::string ReadBin(const fs::path &p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }

    uint64_t HashBytes(const std::string &bytes) {
        uint64_t hash = 1469598103934665603ull;
        for (const unsigned char byte : bytes) {
            hash ^= byte;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    struct LiftedIR {
        std::vector<LiftedOperation> operations;
        std::string signature;
    };

    std::optional<LiftedIR> LiftBytecodeIR(const std::string &bytecode, Fission::InstructionDecoder &decoder) {
        try {
            Deserializer deserializer{};
            auto deserialized = deserializer.Deserialize(bytecode);
            if (!deserialized || deserialized->functions.empty())
                return std::nullopt;
            BytecodeLifter lifter{&decoder};
            const auto lifted = lifter.LiftDeserializedBytecode(*deserialized);
            LiftedIR ir;
            std::ostringstream signature;
            std::function<void(const LiftedFunction &)> walk = [&](const LiftedFunction &function) {
                signature << "F " << static_cast<unsigned>(function.numparams) << ' ' << function.subfunctions.size() << '\n';
                for (const auto &instruction : function.instructions) {
                    ir.operations.push_back(instruction.operation);
                    signature << static_cast<uint32_t>(instruction.operation);
                    for (const auto &operand : instruction.operands) {
                        signature << ' ' << static_cast<unsigned>(operand.type) << ':';
                        switch (operand.type) {
                        case LiftedOperandType::Register:
                            signature << static_cast<unsigned>(operand.value.reg) << ':' << operand.ssaVersion;
                            break;
                        case LiftedOperandType::ImmediateNil:
                            signature << '0';
                            break;
                        case LiftedOperandType::ImmediateInteger:
                            signature << operand.value.imm.n;
                            break;
                        case LiftedOperandType::ImmediateBool:
                            signature << operand.value.imm.b;
                            break;
                        case LiftedOperandType::ImmediateConstant:
                            signature << operand.value.imm.k;
                            break;
                        case LiftedOperandType::ImmediateAux:
                            signature << operand.value.imm.u;
                            break;
                        }
                    }
                    if (instruction.instructionRemarks)
                        signature << ' ' << instruction.instructionRemarks->size() << ':' << *instruction.instructionRemarks;
                    signature << '\n';
                }
                for (const auto &child : function.subfunctions)
                    walk(child);
                signature << "E\n";
            };
            walk(lifted);
            ir.signature = signature.str();
            return ir;
        } catch (...) {
            return std::nullopt;
        }
    }

    std::tuple<size_t, int, int, int> DescribeMismatch(const std::vector<LiftedOperation> &original, const std::vector<LiftedOperation> &roundtrip) {
        const size_t common = (std::min)(original.size(), roundtrip.size());
        size_t index = 0;
        while (index < common && original[index] == roundtrip[index])
            ++index;
        const int originalOperation = index < original.size() ? static_cast<int>(original[index]) : -1;
        const int roundtripOperation = index < roundtrip.size() ? static_cast<int>(roundtrip[index]) : -1;
        const int lengthOrder = original.size() < roundtrip.size() ? -1 : original.size() > roundtrip.size() ? 1 : 0;
        return {index, originalOperation, roundtripOperation, lengthOrder};
    }

    bool MinimizeFinding(const std::string &source, const fs::path &prefix, int budget) {
        std::string baselineBc, outputBc;
        if (!fuzz::LuauCompiles(source, &baselineBc))
            return false;
        const auto baseline = fuzz::FullDecompile(source);
        if (baseline.code != DecompileResult::Success || !fuzz::LuauCompiles(baseline.output, &outputBc))
            return false;
        const auto preludes = fuzz::CompilePreludes([](const std::string &s, std::string *out) { return fuzz::LuauCompiles(s, out); });
        if (preludes.empty())
            return false;
        const auto verdict = fuzz::CompareSemantics(baselineBc, outputBc, preludes);
        std::vector<fuzz::SemTrace> originalTraces;
        for (const auto &prelude : preludes)
            originalTraces.push_back(fuzz::RunLuauTrace(baselineBc, prelude));
        const bool semantic = verdict.kind == fuzz::SemVerdict::Kind::Diverge;
        const auto loss = LuauAstGenerator::RecoveryLoss(source, baseline.output, verdict.kind == fuzz::SemVerdict::Kind::Match, fuzz::optimizationLevel >= 2);
        if (!semantic && (verdict.kind != fuzz::SemVerdict::Kind::Match || !loss || loss->empty()))
            return false;
        const std::string firstLine = loss ? loss->substr(0, loss->find('\n')) : "";
        const std::string site = semantic ? "" : firstLine.substr(0, firstLine.rfind(": "));
        std::string reducedOutput = baseline.output, evidence;
        const auto interesting = [&](const std::string &candidate) {
            std::string bc, decompiledBc;
            if (!fuzz::LuauCompiles(candidate, &bc))
                return false;
            const auto decompiled = fuzz::FullDecompile(candidate);
            if (decompiled.code != DecompileResult::Success || !fuzz::LuauCompiles(decompiled.output, &decompiledBc))
                return false;
            const auto current = fuzz::CompareSemantics(bc, decompiledBc, preludes);
            if (semantic) {
                if (current.kind != fuzz::SemVerdict::Kind::Diverge || current.fixture != verdict.fixture ||
                    current.original.status != verdict.original.status || current.decompiled.status != verdict.decompiled.status ||
                    current.original.error != verdict.original.error || current.decompiled.error != verdict.decompiled.error)
                    return false;
                evidence = "fixture=" + std::to_string(current.fixture) + "\n---- original ----\n" + current.original.trace + "---- decompiled ----\n" +
                           current.decompiled.trace;
            } else {
                if (current.kind != fuzz::SemVerdict::Kind::Match)
                    return false;
                const auto remaining = LuauAstGenerator::RecoveryLoss(candidate, decompiled.output, true, fuzz::optimizationLevel >= 2);
                const std::string marker = site + ": ";
                if (!remaining || (!remaining->starts_with(marker) && remaining->find("\n" + marker) == std::string::npos))
                    return false;
                evidence = *remaining;
            }
            for (size_t i = 0; i < preludes.size(); ++i) {
                const auto original = fuzz::RunLuauTrace(bc, preludes[i]);
                if (!originalTraces[i].comparable || !original.comparable || original.status != originalTraces[i].status ||
                    original.status == fuzz::SemTrace::Status::Timeout || original.status == fuzz::SemTrace::Status::LoadFailed ||
                    original.trace != originalTraces[i].trace)
                    return false;
            }
            reducedOutput = decompiled.output;
            return true;
        };
        int attempts = 0;
        const std::string reduced = LuauAstGenerator::Minimize(source, interesting, budget, attempts);
        if (!interesting(reduced))
            return false;
        Write(prefix.string() + ".min.lua", reduced);
        Write(prefix.string() + ".min.out.lua", reducedOutput);
        Write(
            prefix.string() + ".min.txt", std::string("kind=") + (semantic ? "semantic" : "structural") + "\nsite=" + site +
                                              "\nattempts=" + std::to_string(attempts) + "\nbytes=" + std::to_string(source.size()) + " -> " +
                                              std::to_string(reduced.size()) + "\noriginal-observed-traces=preserved\n" + evidence
        );
        return reduced.size() < source.size();
    }

    // tiny deterministic PRNG (xorshift32). deterministic so a campaign is reproducible from its seed
    // and a crashing sample can be regenerated; never seeded from the clock.
    inline uint32_t XorShift(uint32_t &s) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s ? s : (s = 0x1234567u);
    }

    // Build one fuzz candidate of raw bytecode bytes. Three strategies keep coverage broad:
    //  0: mutate real bytecode (flip/replace bytes, maybe truncate/extend); exercises deep paths that
    //     only a near-valid chunk reaches; 1: a valid version header + random tail; gets past the
    //     version gate then stresses the body parser; 2: pure random bytes; the shallow paths.
    std::string MakeDeserCandidate(int strategy, uint32_t &rng, const std::vector<std::string> &bases) {
        std::string cand;
        if (strategy == 0 && !bases.empty()) {
            cand = bases[XorShift(rng) % bases.size()];
            const int ops = 1 + static_cast<int>(XorShift(rng) % 8);
            for (int k = 0; k < ops && !cand.empty(); ++k)
                cand[XorShift(rng) % cand.size()] = static_cast<char>(XorShift(rng) & 0xFF);
            if ((XorShift(rng) % 4) == 0 && cand.size() > 1)
                cand.resize(1 + XorShift(rng) % cand.size()); // truncate
            if ((XorShift(rng) % 4) == 0) {
                const int n = static_cast<int>(XorShift(rng) % 48);
                for (int k = 0; k < n; ++k)
                    cand.push_back(static_cast<char>(XorShift(rng) & 0xFF)); // extend
            }
        } else if (strategy == 1) {
            const uint8_t ver = static_cast<uint8_t>(LBC_VERSION_MIN + XorShift(rng) % (LBC_VERSION_MAX - LBC_VERSION_MIN + 1));
            cand.push_back(static_cast<char>(ver));
            if (ver >= 4) {
                const uint8_t tv = static_cast<uint8_t>(LBC_TYPE_VERSION_MIN + XorShift(rng) % (LBC_TYPE_VERSION_MAX - LBC_TYPE_VERSION_MIN + 1));
                cand.push_back(static_cast<char>(tv));
            }
            const int n = static_cast<int>(XorShift(rng) % 256);
            for (int k = 0; k < n; ++k)
                cand.push_back(static_cast<char>(XorShift(rng) & 0xFF));
        } else {
            const int n = static_cast<int>(XorShift(rng) % 512);
            for (int k = 0; k < n; ++k)
                cand.push_back(static_cast<char>(XorShift(rng) & 0xFF));
        }
        return cand;
    }
} // namespace

int main(int argc, char **argv) {
    SuppressCrashDialogs(); // run unattended: crashes go to stderr, no modal dialog to dismiss
    uint32_t seed = 1;
    int count = 2000;
    int maxCorpus = 300;
    bool sugarOnly = false;
    int minimizeBudget = 64;
    std::string minimizeFile;
    std::string sugarSource, sugarOutput;
    int threads = 1;       // --threads N: parallelize the source-gen campaign across N workers
    bool doMutate = false; // off by default: mutated bytecode can hard-fault (see below), killing the run
    bool doDeser = false;  // --deser: deserialization-level fuzzing (raw bytes -> Deserialize + pipeline)
    std::string out = std::string(FISSION_FUZZING_SOURCE_DIR);
    std::string singleFile; // --file <path>: decompile one source through the raw pipeline (crash repro)
    std::string deserFile;  // --deser-file <path>: re-run one raw bytecode sample (crash breadcrumb)
    std::string robloxFile; // --roblox-file <path>: decompile a binary Roblox-bytecode sample (regress guard)
    std::string robloxCompare;
    bool robloxRecompileOnly = false;
    std::string robloxCorpus;
    int corpusStart = 0;
    int corpusLimit = 0;
    std::string mutateFile; // --repro-mutate <path> <seed>: compile src, flip bytes by seed, decompile (single-shot)
    std::string semFile;    // --sem-file <path>: decompile one source, run both in the Luau VM, diff traces
    std::string replayDir;  // --replay-dir <path>: validate every saved source finding in one run
    uint32_t mutateSeed = 0;
    bool ssaOracle = false; // --ssa-oracle: compare SSA reaching definitions against VM dataflow (generated or --replay-dir)

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seed" && i + 1 < argc)
            seed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--count" && i + 1 < argc)
            count = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc)
            out = argv[++i];
        else if (a == "--max-corpus" && i + 1 < argc)
            maxCorpus = std::atoi(argv[++i]);
        else if (a == "--mutate")
            doMutate = true;
        else if (a == "--sugar-only")
            sugarOnly = true;
        else if (a == "--minimize-file" && i + 1 < argc)
            minimizeFile = argv[++i];
        else if (a == "--check-sugar" && i + 2 < argc) {
            sugarSource = argv[++i];
            sugarOutput = argv[++i];
        } else if (a == "--minimize-budget" && i + 1 < argc)
            minimizeBudget = (std::max)(0, std::atoi(argv[++i]));
        else if (a == "--deser")
            doDeser = true;
        else if (a == "--file" && i + 1 < argc)
            singleFile = argv[++i];
        else if (a == "--deser-file" && i + 1 < argc)
            deserFile = argv[++i];
        else if (a == "--roblox-file" && i + 1 < argc)
            robloxFile = argv[++i];
        else if (a == "--roblox-compare" && i + 1 < argc)
            robloxCompare = argv[++i];
        else if (a == "--roblox-recompile" && i + 1 < argc) {
            robloxCompare = argv[++i];
            robloxRecompileOnly = true;
        } else if (a == "--roblox-corpus" && i + 1 < argc)
            robloxCorpus = argv[++i];
        else if (a == "--corpus-start" && i + 1 < argc)
            corpusStart = (std::max)(0, std::atoi(argv[++i]));
        else if (a == "--corpus-limit" && i + 1 < argc)
            corpusLimit = (std::max)(0, std::atoi(argv[++i]));
        else if (a == "--sem-file" && i + 1 < argc)
            semFile = argv[++i];
        else if (a == "--replay-dir" && i + 1 < argc)
            replayDir = argv[++i];
        else if (a == "--ssa-oracle")
            ssaOracle = true;
        else if (a == "--opt" && i + 1 < argc)
            fuzz::optimizationLevel = std::clamp(std::atoi(argv[++i]), 0, 2);
        else if (a == "--debug" && i + 1 < argc)
            fuzz::debugLevel = std::clamp(std::atoi(argv[++i]), 0, 2);
        else if (a == "--repro-mutate" && i + 2 < argc) {
            mutateFile = argv[++i];
            mutateSeed = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (a == "--threads" && i + 1 < argc) {
            const int tn = std::atoi(argv[++i]);
            threads = tn < 1 ? 1 : tn;
        }
    }
    // --mutate feeds corrupted bytecode to the deserializer. The decompiler's safety boundary
    // turns libassert aborts into graceful FailedToDecompile, but a raw memory fault or stack
    // overflow on hostile bytecode is NOT a C++ exception and will hard-crash this process. That is
    // itself a finding (a gap in the hostile-input contract); the breadcrumb names the culprit.
    if (doMutate)
        std::fprintf(stderr, "[fuzzing] --mutate on: a hostile-bytecode hard-fault will crash the run; the breadcrumb identifies it.\n");

    fuzz::EnableLuauFlags();
    // route every libassert failure (incl. our out-of-boundary direct stage calls) through a throw
    // so one bad sample is attributed and skipped, not an _Exit that kills the whole campaign.
    Fission::ScopedThrowingAssertHandler guard;

    if (ssaOracle) {
        const fs::path findingDir = fs::path(out) / "ssa-oracle";
        fs::create_directories(findingDir);
        std::map<std::string, size_t> signatures;
        std::map<std::string, size_t> saved;
        size_t samples = 0, dirty = 0, failed = 0, functions = 0, reads = 0, skipped = 0;
        auto check = [&](const std::string &source, const std::string &label) {
            std::string bc;
            if (!fuzz::LuauCompiles(source, &bc))
                return;
            ++samples;
            fuzz::SSAOracleReport report;
            try {
                Fission::InstructionDecoder decoder{};
                Deserializer deserializer{};
                auto deserialized = deserializer.Deserialize(bc);
                if (!deserialized || deserialized->functions.empty()) {
                    ++failed;
                    return;
                }
                BytecodeLifter lifter{&decoder};
                auto lifted = lifter.LiftDeserializedBytecode(*deserialized);
                ControlFlowAnalyzer cfa{};
                auto analyzed = cfa.DetermineBasicBlocks(&lifted);
                cfa.OptimizeGraph(analyzed);
                cfa.PruneUnreachable(analyzed);
                cfa.IdentifyStructures(analyzed);
                SSABuilder{}.Build(analyzed);
                report = fuzz::CheckSSAAgainstVM(analyzed);
            } catch (...) {
                ++failed;
                return;
            }
            functions += report.functions;
            reads += report.readsCompared;
            skipped += report.functionsSkipped;
            if (report.mismatches.empty())
                return;
            ++dirty;
            std::set<std::string> sampleSignatures;
            std::string evidence;
            for (const auto &m : report.mismatches) {
                sampleSignatures.insert(m.kind + ":" + m.operation);
                evidence += std::format("-- {} {} pc={} r{} {} {}\n", m.kind, m.function, m.pc, m.reg, m.operation, m.detail);
            }
            for (const auto &signature : sampleSignatures) {
                ++signatures[signature];
                if (saved[signature]++ < 3) {
                    std::string file = signature;
                    std::ranges::replace(file, ':', '_');
                    Write(findingDir / std::format("{}_{}.lua", file, saved[signature]), "-- " + label + "\n" + evidence + source + "\n" + report.dumps);
                }
            }
        };

        if (!replayDir.empty()) {
            for (const auto &entry : fs::recursive_directory_iterator(replayDir, fs::directory_options::skip_permission_denied)) {
                const std::string name = entry.path().filename().string();
                if (entry.is_regular_file() && entry.path().extension() == ".lua" && !name.contains(".out.lua") && !name.contains(".min.out.lua"))
                    check(ReadBin(entry.path()), entry.path().string());
            }
        } else {
            LuauAstGenerator luauGen{seed + 1u};
            FissionAstGenerator fissionGen{(seed ^ 0x5bd1e995u) + 1u};
            for (int i = 0; i < count; ++i) {
                std::string source;
                try {
                    source = (i % 2) == 0 ? luauGen.Generate(sugarOnly) : fissionGen.Generate();
                } catch (...) {
                    continue;
                }
                check(source, std::format("seed={} idx={}", seed, i));
                if ((i + 1) % 1000 == 0)
                    std::fprintf(stderr, "[ssa-oracle] %d/%d dirty=%zu\n", i + 1, count, dirty);
            }
        }

        std::fprintf(stderr, "\n================ FISSION.FUZZING SSA ORACLE ================\n");
        std::fprintf(
            stderr, "samples=%zu dirty=%zu pipeline-failed=%zu functions=%zu skipped=%zu reads=%zu\n", samples, dirty, failed, functions, skipped, reads
        );
        for (const auto &[signature, n] : signatures)
            std::fprintf(stderr, "  %-40s %zu\n", signature.c_str(), n);
        std::fprintf(stderr, "findings: %s\n", findingDir.string().c_str());
        return dirty == 0 ? 0 : 1;
    }

    if (!replayDir.empty()) {
        std::vector<fs::path> paths;
        for (const auto &entry : fs::recursive_directory_iterator(replayDir, fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".lua")
                continue;
            const std::string name = entry.path().filename().string();
            if (name.contains(".out.lua") || name.contains(".min.lua"))
                continue;
            paths.push_back(entry.path());
        }
        std::ranges::sort(paths);
        const auto preludes = fuzz::CompilePreludes([](const std::string &s, std::string *o) { return fuzz::LuauCompiles(s, o); });
        if (preludes.empty()) {
            std::fprintf(stderr, "[replay] semantic preludes failed to compile\n");
            return 2;
        }

        std::map<std::string, size_t> outcomes;
        std::map<std::string, size_t> forwardClasses;
        std::map<uint64_t, size_t> semanticClusters;
        std::map<uint64_t, fs::path> semanticExamples;
        std::set<std::string> seenSources;
        size_t sourceCount = 0;
        for (const auto &path : paths) {
            const std::string source = ReadBin(path);
            if (!seenSources.insert(source).second)
                continue;
            ++sourceCount;
            std::string originalBc;
            if (!fuzz::LuauCompiles(source, &originalBc)) {
                ++outcomes["INVALID_INPUT"];
                std::fprintf(stderr, "[replay] INVALID_INPUT %016llx %s\n", static_cast<unsigned long long>(HashBytes(source)), path.string().c_str());
                continue;
            }
            const auto decompiled = fuzz::FullDecompile(source);
            if (decompiled.code != DecompileResult::Success) {
                ++outcomes["DECOMPILE_FAILED"];
                std::fprintf(stderr, "[replay] DECOMPILE_FAILED %016llx %s\n", static_cast<unsigned long long>(HashBytes(source)), path.string().c_str());
                continue;
            }
            std::string decompiledBc;
            if (!fuzz::LuauCompiles(decompiled.output, &decompiledBc)) {
                ++outcomes["INVALID_RECOMPILE"];
                std::fprintf(stderr, "[replay] INVALID_RECOMPILE %016llx %s %s\n", static_cast<unsigned long long>(HashBytes(source)), path.string().c_str(),
                             decompiledBc.empty() ? "" : decompiledBc.c_str() + 1);
                continue;
            }
            if (fuzz::UsesGeneratedLocalBeforeDeclared(decompiled.output, &source)) {
                ++outcomes["FORWARD_REFERENCE"];
                std::string culprit;
                const std::string forwardClass = fuzz::ClassifyForwardRef(decompiled.output, &source, &culprit);
                ++forwardClasses[forwardClass];
                std::fprintf(
                    stderr, "[replay] FORWARD_REFERENCE %016llx %s %s:%s\n", static_cast<unsigned long long>(HashBytes(source)), path.string().c_str(),
                    forwardClass.c_str(), culprit.c_str()
                );
                continue;
            }

            const auto semantics = fuzz::CompareSemantics(originalBc, decompiledBc, preludes);
            if (semantics.kind == fuzz::SemVerdict::Kind::Diverge) {
                ++outcomes["SEM_DIVERGE"];
                const std::string semanticSignature = std::format(
                    "{}\n{}\n{}\n{}", static_cast<int>(semantics.original.status), semantics.original.trace,
                    static_cast<int>(semantics.decompiled.status), semantics.decompiled.trace
                );
                const uint64_t signature = HashBytes(semanticSignature);
                ++semanticClusters[signature];
                semanticExamples.try_emplace(signature, path);
                std::fprintf(
                    stderr, "[replay] SEM_DIVERGE %016llx fixture=%zu cluster=%016llx %s\n", static_cast<unsigned long long>(HashBytes(source)),
                    semantics.fixture, static_cast<unsigned long long>(signature), path.string().c_str()
                );
            } else if (semantics.kind == fuzz::SemVerdict::Kind::Unrunnable) {
                ++outcomes["SEM_UNCHECKED"];
            } else {
                ++outcomes["PASS"];
                const auto originalOps = fuzz::LiftOpcodes(source);
                const auto reconstructedOps = fuzz::LiftOpcodes(decompiled.output);
                if (originalOps && reconstructedOps && *originalOps != *reconstructedOps)
                    ++outcomes["IR_DIFFERENT"];
            }
        }

        std::fprintf(stderr, "\n================ FISSION.FUZZING REPLAY ================\n");
        std::fprintf(stderr, "sources: %zu\n", sourceCount);
        for (const auto &[name, count] : outcomes)
            std::fprintf(stderr, "  %-22s %zu\n", name.c_str(), count);
        for (const auto &[name, count] : forwardClasses)
            std::fprintf(stderr, "  forward:%-14s %zu\n", name.c_str(), count);
        for (const auto &[signature, count] : semanticClusters)
            std::fprintf(
                stderr, "  semantic:%016llx %zu example=%s\n", static_cast<unsigned long long>(signature), count,
                semanticExamples.at(signature).string().c_str()
            );
        std::fprintf(stderr, "=========================================================\n");
        const size_t failures = outcomes["INVALID_INPUT"] + outcomes["DECOMPILE_FAILED"] + outcomes["INVALID_RECOMPILE"] +
                                outcomes["FORWARD_REFERENCE"] + outcomes["SEM_DIVERGE"];
        return failures == 0 ? 0 : 1;
    }
    if (!sugarSource.empty()) {
        const auto source = ReadBin(sugarSource), output = ReadBin(sugarOutput);
        std::string before, after;
        if (!fuzz::LuauCompiles(source, &before) || !fuzz::LuauCompiles(output, &after))
            return 2;
        const auto preludes = fuzz::CompilePreludes([](const std::string &s, std::string *out) { return fuzz::LuauCompiles(s, out); });
        if (preludes.empty())
            return 2;
        const auto verdict = fuzz::CompareSemantics(before, after, preludes);
        const auto loss = LuauAstGenerator::RecoveryLoss(source, output, verdict.kind == fuzz::SemVerdict::Kind::Match, fuzz::optimizationLevel >= 2);
        if (!loss)
            return 2;
        std::fprintf(
            stderr, "[sugar] semantics=%s\n%s",
            verdict.kind == fuzz::SemVerdict::Kind::Match     ? "match"
            : verdict.kind == fuzz::SemVerdict::Kind::Diverge ? "diverge"
                                                              : "unchecked",
            loss->c_str()
        );
        if (verdict.kind == fuzz::SemVerdict::Kind::Unrunnable)
            return 2;
        return loss->empty() && verdict.kind == fuzz::SemVerdict::Kind::Match ? 0 : 1;
    }
    if (!minimizeFile.empty()) {
        fs::create_directories(out);
        const auto prefix = fs::path(out) / "reproducer";
        const bool shrunk = MinimizeFinding(ReadBin(minimizeFile), prefix, minimizeBudget);
        std::fprintf(stderr, "[minimize] %s: %s\n", shrunk ? "reduced" : "not reduced", prefix.string().c_str());
        return shrunk ? 0 : 1;
    }

    if (!robloxCompare.empty()) {
        const auto bytecode = ReadBin(robloxCompare);
        Decompiler decompiler{};
        const auto result = decompiler.DecompileRobloxBytecode(bytecode, static_cast<DecompilerFlags>(0));
        if (result.resultCode != DecompileResult::Success) {
            std::fprintf(stderr, "[roblox-compare] decompile failed: %d\n", static_cast<int>(result.resultCode));
            return 2;
        }
        if (!robloxRecompileOnly) {
            std::fputs("SOURCE_BEGIN\n", stdout);
            std::fputs(result.decompilationOutput.c_str(), stdout);
            std::fputs("\nSOURCE_END\n", stdout);
        }
        std::string recompiled;
        if (!fuzz::LuauCompiles(result.decompilationOutput, &recompiled)) {
            std::fprintf(stderr, "[roblox-compare] recompile failed: %s\n", recompiled.empty() ? "(empty)" : recompiled.substr(1).c_str());
            return 2;
        }
        std::string forwardReference;
        if (fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &forwardReference)) {
            std::fprintf(stderr, "[roblox-recompile] generated binding failure: %s\n", forwardReference.c_str());
            return 2;
        }
        if (robloxRecompileOnly) {
            const auto secondGeneration = fuzz::FullDecompile(result.decompilationOutput);
            std::string secondGenerationBytecode;
            if (secondGeneration.code != DecompileResult::Success || !fuzz::LuauCompiles(secondGeneration.output, &secondGenerationBytecode)) {
                std::fprintf(stderr, "[roblox-recompile] second-generation decompile/recompile failed\n");
                return 2;
            }
            if (fuzz::UsesGeneratedLocalBeforeDeclared(secondGeneration.output, &forwardReference)) {
                std::fprintf(stderr, "[roblox-recompile] second-generation binding failure: %s\n", forwardReference.c_str());
                return 2;
            }
            std::fprintf(
                stderr, "[roblox-recompile] ok: %s (%zu/%zu source bytes, %zu/%zu bytecode bytes)\n", robloxCompare.c_str(), result.decompilationOutput.size(),
                secondGeneration.output.size(), recompiled.size(), secondGenerationBytecode.size()
            );
            return 0;
        }
        Fission::RobloxClientDecoder robloxDecoder{};
        Fission::InstructionDecoder vanillaDecoder{};
        const auto original = LiftBytecodeIR(bytecode, robloxDecoder);
        const auto roundtrip = LiftBytecodeIR(recompiled, vanillaDecoder);
        if (!original || !roundtrip) {
            std::fprintf(stderr, "[roblox-compare] IR lift failed\n");
            return 2;
        }
        const auto secondGeneration = fuzz::FullDecompile(result.decompilationOutput);
        std::string secondGenerationBytecode;
        fuzz::SemVerdict semantic{};
        if (secondGeneration.code == DecompileResult::Success && fuzz::LuauCompiles(secondGeneration.output, &secondGenerationBytecode)) {
            const auto preludes = fuzz::CompilePreludes([](const std::string &source, std::string *output) { return fuzz::LuauCompiles(source, output); });
            if (!preludes.empty())
                semantic = fuzz::CompareSemantics(recompiled, secondGenerationBytecode, preludes);
        }
        const char *semanticStatus = semantic.kind == fuzz::SemVerdict::Kind::Match     ? "MATCH"
                                     : semantic.kind == fuzz::SemVerdict::Kind::Diverge ? "DIVERGE"
                                                                                        : "UNRUNNABLE";
        std::fprintf(stderr, "SEMANTIC_SOURCE_ROUNDTRIP=%s\n", semanticStatus);
        if (semantic.kind == fuzz::SemVerdict::Kind::Diverge) {
            std::fprintf(
                stderr, "SEMANTIC_FIXTURE=%zu\nORIGINAL_TRACE_BEGIN\n%sORIGINAL_TRACE_END\nROUNDTRIP_TRACE_BEGIN\n%sROUNDTRIP_TRACE_END\n", semantic.fixture,
                semantic.original.trace.c_str(), semantic.decompiled.trace.c_str()
            );
        }
        std::fputs("ORIGINAL_IR_BEGIN\n", stdout);
        std::fputs(original->signature.c_str(), stdout);
        std::fputs("ORIGINAL_IR_END\nROUNDTRIP_IR_BEGIN\n", stdout);
        std::fputs(roundtrip->signature.c_str(), stdout);
        std::fputs("ROUNDTRIP_IR_END\n", stdout);
        std::fprintf(
            stderr, "[roblox-compare] original=%zu roundtrip=%zu equal=%s\n", original->operations.size(), roundtrip->operations.size(),
            original->signature == roundtrip->signature ? "yes" : "no"
        );
        return original->signature == roundtrip->signature ? 0 : 1;
    }

    if (!robloxCorpus.empty()) {
        std::vector<fs::path> paths;
        std::error_code error;
        for (fs::recursive_directory_iterator it(robloxCorpus, error), end; it != end; it.increment(error)) {
            if (error) {
                error.clear();
                continue;
            }
            if (it->is_regular_file() && it->path().extension() == ".lbc")
                paths.push_back(it->path());
        }
        std::ranges::sort(paths);
        paths.erase(paths.begin(), paths.begin() + (std::min)(paths.size(), static_cast<size_t>(corpusStart)));
        if (corpusLimit > 0 && paths.size() > static_cast<size_t>(corpusLimit))
            paths.resize(static_cast<size_t>(corpusLimit));

        std::set<std::pair<uint64_t, size_t>> seen;
        std::set<std::tuple<int, int, int>> mismatchSignatures;
        std::mutex stateMutex;
        std::atomic<size_t> next{0};
        size_t checked = 0, completed = 0, duplicates = 0, matches = 0, findings = 0, reported = 0;
        const auto worker = [&] {
            while (true) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= paths.size())
                    return;
                const auto &path = paths[index];
                const auto bytecode = ReadBin(path);
                const auto hash = HashBytes(bytecode);
                {
                    const std::scoped_lock lock{stateMutex};
                    if (!seen.emplace(hash, bytecode.size()).second) {
                        ++duplicates;
                        continue;
                    }
                    ++checked;
                }

                Decompiler decompiler{};
                decompiler.SetDecompileBudget(std::chrono::seconds(30));
                const auto result = decompiler.DecompileRobloxBytecode(bytecode, static_cast<DecompilerFlags>(0));
                std::string line;
                bool match = false;
                std::optional<std::tuple<int, int, int>> mismatchSignature;
                if (result.resultCode != DecompileResult::Success) {
                    line = std::format("DECOMPILE\t{:016x}\t{}\n", hash, path.string());
                } else {
                    std::string recompiled;
                    if (!fuzz::LuauCompiles(result.decompilationOutput, &recompiled)) {
                        line = std::format("RECOMPILE\t{:016x}\t{}\n", hash, path.string());
                    } else if (fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput)) {
                        line = std::format("FORWARD_REF\t{:016x}\t{}\n", hash, path.string());
                    } else {
                        Fission::RobloxClientDecoder robloxDecoder{};
                        Fission::InstructionDecoder vanillaDecoder{};
                        const auto original = LiftBytecodeIR(bytecode, robloxDecoder);
                        const auto roundtrip = LiftBytecodeIR(recompiled, vanillaDecoder);
                        if (!original || !roundtrip) {
                            line = std::format("IR_LIFT\t{:016x}\t{}\n", hash, path.string());
                        } else if (original->signature != roundtrip->signature) {
                            const auto [first, originalOperation, roundtripOperation, lengthOrder] =
                                DescribeMismatch(original->operations, roundtrip->operations);
                            mismatchSignature = std::tuple{originalOperation, roundtripOperation, lengthOrder};
                            line = std::format(
                                "IR_MISMATCH\t{:016x}\t{}\t{}\t{}\t{}\t{}\t{}\n", hash, first, originalOperation, roundtripOperation,
                                original->operations.size(), roundtrip->operations.size(), path.string()
                            );
                        } else {
                            match = true;
                        }
                    }
                }

                const std::scoped_lock lock{stateMutex};
                ++completed;
                if (match) {
                    ++matches;
                } else {
                    ++findings;
                    if (!mismatchSignature || mismatchSignatures.insert(*mismatchSignature).second) {
                        std::fputs(line.c_str(), stdout);
                        ++reported;
                    }
                }
                if (completed % 100 == 0)
                    std::fprintf(
                        stderr, "[roblox-corpus] checked=%zu duplicate=%zu match=%zu finding=%zu reported=%zu\n", checked, duplicates, matches, findings,
                        reported
                    );
            }
        };
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(threads));
        for (int i = 0; i < threads; ++i)
            workers.emplace_back(worker);
        for (auto &workerThread : workers)
            workerThread.join();
        std::fprintf(
            stderr, "[roblox-corpus] checked=%zu duplicate=%zu match=%zu finding=%zu reported=%zu\n", checked, duplicates, matches, findings, reported
        );
        return findings == 0 ? 0 : 1;
    }

    // --roblox-file: decompile a binary Roblox-bytecode sample through the public boundary with a budget.
    // regression guard for the protected Samples/EncodedRoblox/* (must not hang/crash/OOM after CFA edits).
    if (!robloxFile.empty()) {
        std::ifstream in(robloxFile, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string bc = ss.str();
        Decompiler decompiler{};
        decompiler.SetDecompileBudget(std::chrono::seconds(120));
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = decompiler.DecompileRobloxBytecode(bc, static_cast<DecompilerFlags>(0));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const size_t lines = (size_t)std::count(r.decompilationOutput.begin(), r.decompilationOutput.end(), '\n');
        std::fprintf(
            stderr, "[roblox] %s: code=%d %lldms %zu lines %zu bytes\n", robloxFile.c_str(), static_cast<int>(r.resultCode), (long long)ms, lines,
            r.decompilationOutput.size()
        );
        return r.resultCode == DecompileResult::Success ? 0 : 1;
    }

    // --repro-mutate <src> <mutationSeed>: compile the source, apply ProbeMutatedBytecode's exact byte
    // flips for the given seed, then decompile the corrupted bytecode raw (no catch) so a hostile-input
    // hard fault reproduces in isolation -- run under a debugger / ASan to localize it.
    if (!mutateFile.empty()) {
        std::ifstream in(mutateFile, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        std::string bc;
        if (!fuzz::LuauCompiles(ss.str(), &bc) || bc.size() < 2) {
            std::fprintf(stderr, "[repro-mutate] source does not compile\n");
            return 2;
        }
        uint32_t s = mutateSeed * 2654435761u + 1u;
        const int flips = 1 + (s % 6);
        for (int i = 0; i < flips; ++i) {
            s = s * 1103515245u + 12345u;
            bc[s % bc.size()] = static_cast<char>((s >> 8) & 0xFF);
        }
        std::fprintf(stderr, "[repro-mutate] %d byte flips applied; running stages on corrupted %zu-byte bytecode...\n", flips, bc.size());
        auto step = [](const char *s) {
            std::fprintf(stderr, "[repro-mutate] >> %s\n", s);
            std::fflush(stderr);
        };
        Fission::InstructionDecoder decoder{};
        step("Deserialize");
        Deserializer des{};
        auto d = des.Deserialize(bc);
        if (!d || d->functions.empty()) {
            std::fprintf(stderr, "[repro-mutate] deser refused (graceful)\n");
            return 0;
        }
        step("BytecodeLifter");
        BytecodeLifter lifter{&decoder};
        auto lifted = lifter.LiftDeserializedBytecode(*d);
        step("DetermineBasicBlocks");
        ControlFlowAnalyzer cfa{};
        auto cf = cfa.DetermineBasicBlocks(&lifted);
        step("OptimizeGraph");
        cfa.OptimizeGraph(cf);
        step("PruneUnreachable");
        cfa.PruneUnreachable(cf);
        step("IdentifyStructures");
        cfa.IdentifyStructures(cf);
        step("SSABuilder.Build");
        SSABuilder ssa{};
        ssa.Build(cf);
        step("ASTLifter.Lift");
        ASTLifter astLifter{};
        (void)astLifter.Lift(cf);
        std::fprintf(stderr, "[repro-mutate] all stages survived\n");
        return 0;
    }

    // --sem-file: ground-truth repro tool. Compile the source, decompile it, run both programs in a
    // sandboxed Luau VM, and diff the execution traces. Exit 0 on Match, 1 on Diverge, 2 on harness
    // failure (does not compile / original unrunnable).
    if (!semFile.empty()) {
        std::ifstream in(semFile, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "[sem-file] cannot open source: %s\n", semFile.c_str());
            return 2;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string bc;
        if (!fuzz::LuauCompiles(ss.str(), &bc)) {
            std::fprintf(stderr, "[sem-file] source does not compile\n");
            return 2;
        }
        const auto dec = fuzz::FullDecompile(ss.str());
        if (dec.code != DecompileResult::Success) {
            std::fprintf(stderr, "[sem-file] decompile failed: code=%d\n", static_cast<int>(dec.code));
            return 2;
        }
        std::string outBc;
        if (!fuzz::LuauCompiles(dec.output, &outBc)) {
            std::fprintf(stderr, "[sem-file] decompiled output does not recompile\n");
            return 1;
        }
        const auto preludes = fuzz::CompilePreludes([](const std::string &s, std::string *o) { return fuzz::LuauCompiles(s, o); });
        if (preludes.empty()) {
            std::fprintf(stderr, "[sem-file] semantic preludes failed to compile\n");
            return 2;
        }
        const auto v = fuzz::CompareSemantics(bc, outBc, preludes);
        const char *kind = v.kind == fuzz::SemVerdict::Kind::Match ? "MATCH" : v.kind == fuzz::SemVerdict::Kind::Diverge ? "DIVERGE" : "UNRUNNABLE";
        std::fprintf(
            stderr, "[sem-file] verdict=%s fixture=%zu original(status=%d) decompiled(status=%d)\n", kind, v.fixture, static_cast<int>(v.original.status),
            static_cast<int>(v.decompiled.status)
        );
        std::fprintf(stderr, "---- original trace ----\n%s---- decompiled trace ----\n%s----\n", v.original.trace.c_str(), v.decompiled.trace.c_str());
        return v.kind == fuzz::SemVerdict::Kind::Match ? 0 : v.kind == fuzz::SemVerdict::Kind::Diverge ? 1 : 2;
    }

    // --file exposes stage progress for debugger-assisted crash localization.
    if (!singleFile.empty()) {
        std::ifstream in(singleFile, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        const std::string src = ss.str();
        std::string bc;
        if (!fuzz::LuauCompiles(src, &bc)) {
            std::fprintf(stderr, "[file] does not compile\n");
            return 2;
        }
        std::fprintf(stderr, "[file] compiled %zu bytes; running attributed stages...\n", bc.size());
        // CFG dump: block id/type/flags/successors + loop fields, for loop/break structure debugging.
        try {
            Fission::InstructionDecoder decoder{};
            Deserializer des{};
            auto d = des.Deserialize(bc);
            if (d && !d->functions.empty()) {
                BytecodeLifter lifter{&decoder};
                auto lifted = lifter.LiftDeserializedBytecode(*d);
                FissionDebugNotes debugNotes;
                debugNotes.Reset(true);
                ControlFlowAnalyzer cfa{&debugNotes};
                auto cf = cfa.DetermineBasicBlocks(&lifted);
                cfa.OptimizeGraph(cf);
                cfa.PruneUnreachable(cf);
                cfa.IdentifyStructures(cf);
                SSABuilder ssa{};
                ssa.SetDebugNotes(&debugNotes);
                ssa.Build(cf);
                std::function<void(const AnalyzedFunction &, int)> dump = [&](const AnalyzedFunction &fn, int dep) {
                    std::fprintf(stderr, "[cfg] %*sfn blocks=%zu\n", dep * 2, "", fn.basicBlocks.size());
                    for (const auto &b : fn.basicBlocks) {
                        std::fprintf(
                            stderr, "[cfg] %*s  B%u type=%d term=%d flags=0x%x pred=[", dep * 2, "", b.dwBlockId, (int)b.bType, (int)b.bTerminator,
                            b.dwBlockFlags
                        );
                        for (auto p : b.predecessors)
                            std::fprintf(stderr, "%u ", p);
                        std::fprintf(stderr, "] succ=[");
                        for (auto s : b.successors)
                            std::fprintf(stderr, "%u ", s);
                        std::fprintf(
                            stderr, "] latch=%d exit=%d header=%d head=%d tail=%d\n", b.loopLatch.has_value() ? (int)b.loopLatch.value() : -1,
                            b.loopExit.has_value() ? (int)b.loopExit.value() : -1, b.loopHeader.has_value() ? (int)b.loopHeader.value() : -1,
                            b.lpHead ? (int)b.lpHead->operation : -1,
                            b.lpTail ? (int)b.lpTail->operation : -1
                        );
                        for (const auto &note : b.analysisNotes)
                            std::fprintf(stderr, "[cfg] %*s    why: %s\n", dep * 2, "", note.c_str());
                        if (b.lpHead)
                            for (const auto *instruction = b.lpHead; instruction <= b.lpTail; ++instruction)
                                if (const auto uses = fn.implicitUses.find(instruction); uses != fn.implicitUses.end()) {
                                    std::fprintf(stderr, "[cfg] %*s    _%d implicit=[", dep * 2, "", instruction->instructionIndex);
                                    for (const int version : uses->second)
                                        std::fprintf(stderr, "v%d ", version);
                                    std::fprintf(stderr, "]\n");
                                }
                    }
                    for (const auto &inner : fn.innerFunctions)
                        dump(inner, dep + 1);
                };
                dump(cf, 0);
            }
        } catch (...) {
            std::fprintf(stderr, "[cfg] (dump threw)\n");
        }
        const char *stage = fuzz::RunStagesAttributed(bc);
        std::fprintf(stderr, "[file] attributed stages: %s\n", stage ? stage : "all completed");
        const auto dec = fuzz::FullDecompile(src);
        std::fprintf(stderr, "[file] FullDecompile result code: %d\n", static_cast<int>(dec.code));
        if (dec.code == DecompileResult::Success) {
            std::string outBc;
            const bool ok = fuzz::LuauCompiles(dec.output, &outBc);
            std::fprintf(stderr, "[file] output recompiles: %s\n", ok ? "yes" : "NO");
            if (!ok)
                std::fprintf(stderr, "[file] recompile error: %s\n", outBc.empty() ? "(empty)" : outBc.substr(1).c_str());
            std::fprintf(stdout, "%s\n", dec.output.c_str());
        }
        return 0;
    }

    // --deser-file: re-run one raw bytecode sample (e.g. a crash breadcrumb) through Deserialize + the
    // attributed pipeline to reproduce/localize a deserialization-level fault under a debugger.
    if (!deserFile.empty()) {
        const std::string bytes = ReadBin(deserFile);
        std::fprintf(stderr, "[deser-file] %zu bytes\n", bytes.size());
        // run the pipeline stage-by-stage with a flushed print before each, so an uncatchable hard
        // fault (e.g. a debug-STL operator[] abort) leaves the last printed stage as the culprit.
        Fission::ScopedDecompileBudget budget(std::chrono::seconds(15));
        auto step = [](const char *s) {
            std::fprintf(stderr, "[stage] %s\n", s);
            std::fflush(stderr);
        };
        try {
            Fission::InstructionDecoder decoder{};
            Deserializer des{};
            step("Deserialize");
            auto d = des.Deserialize(bytes);
            std::fprintf(stderr, "[deser-file] Deserialize: %s\n", d ? (d->functions.empty() ? "ok (empty)" : "ok") : "nullopt");
            std::fflush(stderr);
            if (d && !d->functions.empty()) {
                BytecodeLifter lifter{&decoder};
                step("BytecodeLifter");
                auto lifted = lifter.LiftDeserializedBytecode(*d);
                ControlFlowAnalyzer cfa{};
                step("DetermineBasicBlocks");
                auto cf = cfa.DetermineBasicBlocks(&lifted);
                step("OptimizeGraph");
                cfa.OptimizeGraph(cf);
                step("PruneUnreachable");
                cfa.PruneUnreachable(cf);
                step("IdentifyStructures");
                cfa.IdentifyStructures(cf);
                step("SSABuilder");
                SSABuilder ssa{};
                ssa.Build(cf);
                step("ASTLifter");
                ASTLifter astl{};
                auto ast = astl.Lift(cf);
                step("SourceGenerator");
                RootNode root{ast.statements};
                SourceGenerator sg{};
                (void)sg.GenerateSource(&root);
                step("all completed");
            }
        } catch (const std::exception &e) {
            std::fprintf(stderr, "[deser-file] threw: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "[deser-file] threw (non-std)\n");
        }
        return 0;
    }

    // --deser: deserialization-level fuzzing. Mutated/structured/random bytes go straight to the
    // deserializer and the full pipeline; both must fail gracefully (nullopt / caught throw), never
    // hard-fault. A segfault/stack-overflow would kill the process; the breadcrumb names the bytes.
    if (doDeser) {
        const fs::path dCrash = fs::path(out) / "deser-crashes";
        const fs::path dBread = fs::path(out) / "_current_deser_sample.bin";
        fs::create_directories(dCrash);

        // real bytecode to mutate (strategy 0): diverse shapes reach more parser paths.
        static const char *kSeeds[] = {
            "return 1",
            "local function f(a, b) if a then return a + b end return b end return f(1, 2)",
            "local t = { 1, 2, 3, a = 'x', ['y'] = 4 } for k, v in pairs(t) do print(k, v) end return t",
            "local s = 0 for i = 1, 10 do s = s + i end while s > 0 do s = s - 1 end return s",
            "return function(...) local x = { ... } return #x, select('#', ...) end",
        };
        std::vector<std::string> bases;
        for (const char *s : kSeeds) {
            std::string bc;
            if (fuzz::LuauCompiles(s, &bc))
                bases.push_back(bc);
        }

        Counters c;
        int dumped = 0;
        for (int i = 0; i < count; ++i) {
            if (i % 2000 == 0)
                std::fprintf(stderr, "[deser-fuzz] %d/%d\n", i, count);
            uint32_t rng = (static_cast<uint32_t>(i) * 2654435761u) ^ seed ^ 0x9e3779b9u;
            if (!rng)
                rng = 1;
            std::string cand = MakeDeserCandidate(i % 3, rng, bases);

            // breadcrumb: a hard fault (not a catchable throw) leaves this file naming the exact bytes.
            WriteBin(dBread, cand);

            // (1) the deserializer in isolation: must return nullopt or a value, never crash.
            try {
                Deserializer des{};
                auto d = des.Deserialize(cand);
                c.buckets[d ? "DESER_VALUE" : "DESER_NULLOPT"]++;
            } catch (...) {
                c.buckets["DESER_THREW"]++; // graceful (caught); still tracked
            }

            // (2) the whole pipeline on the same bytes: a deserialized-but-malformed chunk must not
            // crash downstream either. A short budget bounds any runaway lifter/CFA loop.
            {
                Fission::ScopedDecompileBudget budget(std::chrono::seconds(10));
                if (const char *stage = fuzz::RunStagesAttributed(cand)) {
                    c.buckets["PIPELINE_THREW"]++; // caught throw (graceful refusal), attributed below
                    c.crashStages[stage]++;
                    if (dumped < 100)
                        WriteBin(dCrash / ("threw_" + std::string(stage) + "_" + std::to_string(dumped++) + ".bin"), cand);
                } else {
                    c.buckets["PIPELINE_OK"]++;
                }
            }
        }
        fs::remove(dBread); // clean exit: nothing in flight to blame

        std::printf("\n================ FISSION.FUZZING (DESER) REPORT ================\n");
        std::printf("samples: %d   seed: %u\n", count, seed);
        std::printf("---- outcomes ----\n");
        for (const auto &[k, v] : c.buckets)
            std::printf("  %-20s %d\n", k.c_str(), v);
        if (!c.crashStages.empty()) {
            std::printf("---- pipeline throw attribution (graceful, caught) ----\n");
            for (const auto &[k, v] : c.crashStages)
                std::printf("  %-26s %d\n", k.c_str(), v);
        }
        std::printf("===============================================================\n");
        return 0; // reaching here means no hard fault occurred across the campaign
    }

    const fs::path corpusDir = fs::path(out) / "corpus";
    const fs::path crashDir = fs::path(out) / "crashes";
    fs::create_directories(corpusDir);
    fs::create_directories(crashDir);

    g_breadcrumbDir = crashDir.string();
#ifdef _WIN32
    SetUnhandledExceptionFilter(HardFaultBreadcrumb); // dump a faulting worker's in-flight sample before it dies
#endif

    std::atomic<long> progress{0};   // total samples processed across all workers (for the progress line)
    std::atomic<int> corpusSaved{0}; // shared corpus-size cap
    std::atomic<int> minimizations[2]{};

    // One worker over index range [startIdx, endIdx). Each worker owns its generators (seeded per
    // thread so streams differ and stay reproducible), its Counters, its breadcrumb, and tags every
    // dump with its thread id so concurrent writes never collide. Threads share only the global libassert
    // handler (set once, read-only here) and the atomics; the decompile budget is thread_local.
    auto runWorker = [&](int tid, int startIdx, int endIdx) -> Counters {
        Counters c;
        LuauAstGenerator luauGen{seed + static_cast<uint32_t>(tid) * 2654435761u + 1u};
        FissionAstGenerator fissionGen{(seed ^ 0x5bd1e995u) + static_cast<uint32_t>(tid) * 40503u + 1u};
        const std::string tag = "t" + std::to_string(tid) + "_";
        g_breadcrumbTrail.clear();

        auto dumpCrash = [&](const std::string &stage, const std::string &source, const std::string &kind) {
            const fs::path d = crashDir / stage;
            fs::create_directories(d);
            if (c.savedExamples[stage]++ < 50)
                Write(d / (kind + "_" + tag + std::to_string(c.savedExamples[stage]) + ".lua"), source);
        };

        for (int i = startIdx; i < endIdx; ++i) {
            const long done = progress.fetch_add(1, std::memory_order_relaxed);
            if (done % 5000 == 0)
                std::fprintf(stderr, "[fuzzing] %ld/%d  corpus=%d\n", done, count, corpusSaved.load(std::memory_order_relaxed));

            const bool useLuau = sugarOnly || (i % 2) == 0;
            const char *front = useLuau ? "luau-ast" : "fission-ast";
            std::string source;
            try {
                source = useLuau ? luauGen.Generate(sugarOnly) : fissionGen.Generate();
            } catch (...) {
                c.buckets["GENERATOR_THREW"]++;
                dumpCrash(std::string("generator-") + front, "(generator threw before producing source)", "gen");
                continue;
            }

            // in-memory breadcrumb: push the in-flight sample to a tiny rolling trail (no disk IO). If this
            // thread hard-faults, HardFaultBreadcrumb dumps the last entry. Keep only the last few.
            if (g_breadcrumbTrail.size() >= 4)
                g_breadcrumbTrail.erase(g_breadcrumbTrail.begin());
            g_breadcrumbTrail.push_back(std::string("-- front=") + front + " seed=" + std::to_string(seed) + " idx=" + std::to_string(i) + "\n" + source);

            const std::string family = source.starts_with("-- fuzz-case: ") ? source.substr(14, source.find('\n') - 14) : "generic";
            c.buckets["family:" + family + ":generated"]++;
            std::string bc;
            if (!fuzz::LuauCompiles(source, &bc)) {
                // a well-formed AST that yields non-compiling source: a transpiler bug (luau-ast) or,
                // more interestingly, a SourceGenerator bug (fission-ast); same class as the campaign fixes.
                const std::string b = useLuau ? "GEN_NONCOMPILE" : "SOURCEGEN_INVALID";
                c.buckets[b]++;
                if (c.savedExamples[b]++ < 50) {
                    const std::string id = tag + std::to_string(c.savedExamples[b]);
                    Write(crashDir / (b + "_" + id + ".lua"), source);
                    Write(crashDir / (b + "_" + id + ".err"), bc.empty() ? "empty bytecode" : bc.substr(1)); // payload after byte0==0 is the error
                }
                continue;
            }

            // per-stage attributed crash tracking (direct calls, outside the safety boundary).
            if (const char *stage = fuzz::RunStagesAttributed(bc)) {
                c.buckets["CRASH"]++;
                c.crashStages[stage]++;
                dumpCrash(stage, source, "crash");
                continue;
            }

            // mutated/garbage bytecode hardening (opt-in; every few samples to keep throughput up).
            if (doMutate && (i % 4) == 0) {
                if (const char *stage = fuzz::ProbeMutatedBytecode(bc, static_cast<uint32_t>(i) ^ seed)) {
                    c.buckets["MUTATED_CRASH"]++;
                    c.crashStages[stage]++;
                    dumpCrash("mutated", source, "mutated");
                }
            }

            // Behavioral comparison decides success; opcode equality is diagnostic.
            const auto dec = fuzz::FullDecompile(source);
            if (dec.code != DecompileResult::Success) {
                c.buckets["FAILED_DECOMPILE"]++; // graceful refusal (unsupported/hostile); not a crash
                continue;
            }
            std::string outBc;
            if (!fuzz::LuauCompiles(dec.output, &outBc)) {
                c.buckets["INVALID_RECOMPILE"]++;
                if (c.savedExamples["INVALID_RECOMPILE"]++ < 50) {
                    const std::string id = tag + std::to_string(c.savedExamples["INVALID_RECOMPILE"]);
                    Write(crashDir / ("INVALID_RECOMPILE_" + id + ".lua"), source);
                    Write(crashDir / ("INVALID_RECOMPILE_" + id + ".out.lua"), dec.output);
                    Write(
                        crashDir / ("INVALID_RECOMPILE_" + id + ".err"), outBc.empty() ? "empty bytecode" : outBc.substr(1)
                    ); // payload after byte0==0 is the error
                }
                continue;
            }
            const auto before = fuzz::LiftOpcodes(source);
            const auto inputSugar = LuauAstGenerator::MeasureSugar(source);
            const auto outputSugar = LuauAstGenerator::MeasureSugar(dec.output);
            std::string sugarLoss;
            if (!inputSugar || !outputSugar) {
                c.buckets["SUGAR_PARSE_ERROR"]++;
            } else {
                for (const auto &[kind, amount] : *inputSugar) {
                    const auto found = outputSugar->find(kind);
                    const int recovered = found == outputSugar->end() ? 0 : found->second;
                    c.buckets["sugar:" + kind + ":input"] += amount;
                    c.buckets["sugar:" + kind + ":retained"] += (std::min)(amount, recovered);
                    if (recovered < amount) {
                        c.buckets["sugar:" + kind + ":reduced-cases"]++;
                        sugarLoss += kind + ": " + std::to_string(amount) + " -> " + std::to_string(recovered) + "\n";
                    }
                }
                if (!sugarLoss.empty())
                    c.buckets["SUGAR_COUNT_REDUCED"]++;
            }
            const auto after = fuzz::LiftOpcodes(dec.output);
            const bool irSame = before && after && *before == *after;
            static const std::vector<std::string> preludeBcs =
                fuzz::CompilePreludes([](const std::string &s, std::string *o) { return fuzz::LuauCompiles(s, o); });
            if (preludeBcs.empty()) {
                c.buckets["SEM_ORACLE_ERROR"]++;
                continue;
            }
            const auto v = fuzz::CompareSemantics(bc, outBc, preludeBcs);
            const auto recovery = LuauAstGenerator::RecoveryLoss(source, dec.output, v.kind == fuzz::SemVerdict::Kind::Match, fuzz::optimizationLevel >= 2);
            if (!recovery)
                c.buckets["SUGAR_PARSE_ERROR"]++;
            sugarLoss = recovery.value_or("");
            if (!sugarLoss.empty())
                c.buckets["SUGAR_REDUCED"]++;
            if (minimizeBudget > 0 && (v.kind == fuzz::SemVerdict::Kind::Diverge || (v.kind == fuzz::SemVerdict::Kind::Match && !sugarLoss.empty()))) {
                const bool semantic = v.kind == fuzz::SemVerdict::Kind::Diverge;
                const std::string signature =
                    semantic ? "semantic:" + std::to_string(v.fixture) + ":" + v.original.error.value_or("") + ":" + v.decompiled.error.value_or("")
                             : sugarLoss.substr(0, sugarLoss.find('\n'));
                if (c.savedExamples["minimize:" + signature]++ == 0 && minimizations[semantic ? 0 : 1].fetch_add(1) < 3) {
                    const auto prefix = crashDir / ("finding_" + tag + std::to_string(i));
                    Write(prefix.string() + ".lua", source);
                    c.buckets[MinimizeFinding(source, prefix, minimizeBudget) ? "MINIMIZED" : "MINIMIZE_UNCHANGED"]++;
                }
            }
            if (!sugarLoss.empty() && c.savedExamples["SUGAR_REDUCED"]++ < 100) {
                const std::string id = "SUGAR_REDUCED_" + tag + std::to_string(i);
                Write(crashDir / (id + ".lua"), source);
                Write(crashDir / (id + ".out.lua"), dec.output);
                Write(
                    crashDir / (id + ".sugar.txt"), "seed=" + std::to_string(seed) + " idx=" + std::to_string(i) + " family=" + family + " semantics=" +
                                                        (v.kind == fuzz::SemVerdict::Kind::Match     ? "match"
                                                         : v.kind == fuzz::SemVerdict::Kind::Diverge ? "diverge"
                                                                                                     : "unchecked") +
                                                        "\n" + sugarLoss
                );
            }
            c.buckets
                ["family:" + family +
                 (v.kind == fuzz::SemVerdict::Kind::Match     ? ":match"
                  : v.kind == fuzz::SemVerdict::Kind::Diverge ? ":diverge"
                                                              : ":unchecked")]++;

            if (fuzz::UsesGeneratedLocalBeforeDeclared(dec.output, &source)) {
                // Name matching can flag unrelated scopes; the VM verdict determines divergence.
                const std::string b = irSame ? "FORWARDREF_IR_STABLE" : "FORWARDREF_IR_DIVERGE";
                c.buckets[b]++;
                if (!irSame)
                    c.buckets["  fwd:" + fuzz::ClassifyForwardRef(dec.output, &source)]++;
                const char *semKind = v.kind == fuzz::SemVerdict::Kind::Match ? "MATCH" : v.kind == fuzz::SemVerdict::Kind::Diverge ? "DIVERGE" : "UNCHECKED";
                c.buckets[std::string("  fwd-sem:") + semKind]++;
                if (v.kind == fuzz::SemVerdict::Kind::Diverge)
                    c.buckets["FORWARDREF_SEM_DIVERGE"]++;
                if (c.savedExamples[b]++ < 50) {
                    const std::string id = tag + std::to_string(c.savedExamples[b]);
                    Write(crashDir / (b + "_" + id + ".lua"), source);
                    Write(crashDir / (b + "_" + id + ".out.lua"), dec.output);
                    if (v.kind == fuzz::SemVerdict::Kind::Diverge)
                        Write(
                            crashDir / (b + "_" + id + ".trace"),
                            "front=" + std::string(front) + " seed=" + std::to_string(seed) + " idx=" + std::to_string(i) +
                                " fixture=" + std::to_string(v.fixture) + " original_status=" + std::to_string(static_cast<int>(v.original.status)) +
                                " decompiled_status=" + std::to_string(static_cast<int>(v.decompiled.status)) + "\n---- original ----\n" + v.original.trace +
                                "---- decompiled ----\n" + v.decompiled.trace
                        );
                }
                continue;
            }

            if (v.kind == fuzz::SemVerdict::Kind::Match && irSame) {
                c.buckets["OK_IR_STABLE"]++;
            } else if (v.kind == fuzz::SemVerdict::Kind::Match) {
                c.buckets["OK_SEM_MATCH"]++;
            } else {
                const std::string b = v.kind == fuzz::SemVerdict::Kind::Diverge ? "SEM_DIVERGE" : "SEM_UNCHECKED";
                c.buckets[b]++;
                if (v.kind == fuzz::SemVerdict::Kind::Diverge && c.savedExamples[b]++ < 50) {
                    const std::string id = tag + std::to_string(c.savedExamples[b]);
                    Write(crashDir / ("SEM_DIVERGE_" + id + ".lua"), source);
                    Write(crashDir / ("SEM_DIVERGE_" + id + ".out.lua"), dec.output);
                    Write(
                        crashDir / ("SEM_DIVERGE_" + id + ".trace"),
                        "front=" + std::string(front) + " seed=" + std::to_string(seed) + " idx=" + std::to_string(i) +
                            " fixture=" + std::to_string(v.fixture) + " original_status=" + std::to_string(static_cast<int>(v.original.status)) +
                            " decompiled_status=" + std::to_string(static_cast<int>(v.decompiled.status)) + "\n---- original ----\n" + v.original.trace +
                            "---- decompiled ----\n" + v.decompiled.trace
                    );
                }
            }

            if (v.kind == fuzz::SemVerdict::Kind::Match && corpusSaved.load(std::memory_order_relaxed) < maxCorpus) {
                const int slot = corpusSaved.fetch_add(1, std::memory_order_relaxed);
                if (slot < maxCorpus)
                    Write(corpusDir / (std::string(front) + "_" + std::to_string(i) + ".lua"), source);
            }
        }

        g_breadcrumbTrail.clear(); // clean exit for this worker; no in-flight sample to blame
        return c;
    };

    // partition [0, count) across `threads` workers; each runs runWorker on its slice.
    Counters c;
    if (threads <= 1) {
        c = runWorker(0, 0, count);
    } else {
        const int chunk = (count + threads - 1) / threads;
        std::vector<Counters> results(threads);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t) {
            const int s = t * chunk;
            const int e = (s + chunk < count) ? (s + chunk) : count;
            if (s >= e)
                break;
            pool.emplace_back([&, t, s, e]() { results[t] = runWorker(t, s, e); });
        }
        for (auto &th : pool)
            th.join();
        for (const auto &r : results) {
            for (const auto &[k, v] : r.buckets)
                c.buckets[k] += v;
            for (const auto &[k, v] : r.crashStages)
                c.crashStages[k] += v;
        }
    }

    std::printf("\n==================== FISSION.FUZZING REPORT ====================\n");
    std::printf("samples: %d   seed: %u   threads: %d   corpus saved: %d -> %s\n", count, seed, threads, corpusSaved.load(), corpusDir.string().c_str());
    std::printf("---- outcomes ----\n");
    for (const auto &[k, v] : c.buckets)
        std::printf("  %-22s %d\n", k.c_str(), v);
    if (!c.crashStages.empty()) {
        std::printf("---- crash attribution (stage) ----\n");
        for (const auto &[k, v] : c.crashStages)
            std::printf("  %-26s %d\n", k.c_str(), v);
    }
    std::printf("crash/malformation dumps: %s\n", crashDir.string().c_str());
    std::printf("===============================================================\n");

    // nonzero exit if any hard fault surfaced, so CI / scripts can gate on it.
    const int hard = c.buckets["CRASH"] + c.buckets["MUTATED_CRASH"] + c.buckets["INVALID_RECOMPILE"] + c.buckets["SOURCEGEN_INVALID"] +
                     c.buckets["GENERATOR_THREW"] + c.buckets["GEN_NONCOMPILE"] + c.buckets["SEM_DIVERGE"] + c.buckets["FORWARDREF_SEM_DIVERGE"] +
                     c.buckets["SEM_ORACLE_ERROR"] + c.buckets["SUGAR_PARSE_ERROR"];
    return hard > 0 ? 1 : 0;
}
