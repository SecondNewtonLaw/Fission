//
// Created by Dottik on 5/10/2025.
//

#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "SSABuilder.hpp"
#include "libassert/assert.hpp"
#include "luacode.h"

#include <Luau/Ast.h>
#include <Luau/Compiler.h>
#include <Luau/Parser.h>

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

static void CliFailureHandler(const libassert::assertion_info &info) {
    libassert::enable_virtual_terminal_processing_if_needed();
    auto msg = info.to_string(0, libassert::color_scheme::blank);
    std::fprintf(stderr, "\n*** LIBASSERT ASSERTION FAILED ***\n%s\n", msg.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

struct InitCliHandler {
    InitCliHandler() { libassert::set_failure_handler(CliFailureHandler); }
};
static InitCliHandler g_cliHandlerInit;

static void PrintDecompileError(const DecompilationResult &result) {
    if (!result.errorMessage.empty())
        std::fprintf(stderr, "%s\n", result.errorMessage.c_str());
}

#pragma comment(lib, "crypt32.lib")

std::string DecodeBase64FileToBinary(const std::wstring &filepath) {
    std::ifstream file(filepath.c_str(), std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open Base64 file");

    std::stringstream ss;
    ss << file.rdbuf();
    std::string rawContent = ss.str();

    std::string base64Content;
    base64Content.reserve(rawContent.size());

    for (unsigned char c : rawContent) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            base64Content.push_back(c);
        }
    }

    if (base64Content.empty())
        throw std::runtime_error("Base64 content is empty after sanitization");

    while (base64Content.size() % 4 != 0) {
        base64Content.push_back('=');
    }

    DWORD decodedLength = 0;
    if (!CryptStringToBinaryA(
            base64Content.c_str(), static_cast<DWORD>(base64Content.size()), CRYPT_STRING_BASE64, nullptr, &decodedLength, nullptr, nullptr
        )) {
        throw std::runtime_error("CryptStringToBinaryA (Size) failed with error: " + std::to_string(GetLastError()));
    }

    std::vector<BYTE> decodedData(decodedLength);
    if (!CryptStringToBinaryA(
            base64Content.c_str(), static_cast<DWORD>(base64Content.size()), CRYPT_STRING_BASE64, decodedData.data(), &decodedLength, nullptr, nullptr
        )) {
        throw std::runtime_error("CryptStringToBinaryA (Decode) failed with error: " + std::to_string(GetLastError()));
    }

    decodedData.resize(decodedLength);
    return std::string(reinterpret_cast<const char *>(decodedData.data()), decodedData.size());
}

int main(int argc, char **argv) {
    Decompiler decompiler{};
    const bool omitComments = std::any_of(argv + 1, argv + argc, [](const char *arg) { return std::strcmp(arg, "--omit-comments") == 0; });
    const bool optimizeIR = std::any_of(argv + 1, argv + argc, [](const char *arg) { return std::strcmp(arg, "--optimize-ir") == 0; });
    const bool debugNotes = std::any_of(argv + 1, argv + argc, [](const char *arg) { return std::strcmp(arg, "--debug-notes") == 0; });
    auto outputFlags = static_cast<DecompilerFlags>(0);
    if (omitComments)
        outputFlags |= DecompilerFlags::OmitFissionComments;
    if (optimizeIR)
        outputFlags |= DecompilerFlags::OptimizeIR;
    if (debugNotes)
        outputFlags |= DecompilerFlags::FissionDebugNotes;
    int inputExitCode = 0;
    bool handledInputs = false;

    // Decompile raw or base64-encoded Roblox bytecode with an optional time budget.
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string arg = argv[i];
        // Compile source and print SSA IR with register versions and access modes.
        if (arg == "--ssa-dump") {
            std::ifstream sin(argv[i + 1], std::ios::binary);
            if (!sin) {
                std::fprintf(stderr, "open failed: %s\n", argv[i + 1]);
                return 2;
            }
            std::stringstream sss;
            sss << sin.rdbuf();
            const std::string ssrc = sss.str();
            for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (strncmp(flag->name, "Luau", 4) == 0)
                    flag->value = true;
            Luau::CompileOptions sopts{};
            sopts.optimizationLevel = 1;
            sopts.debugLevel = 1;
            const std::string bc = Luau::compile(ssrc, sopts);
            if (bc.empty() || bc[0] == '\0') {
                std::fprintf(stderr, "[ssa-dump] compile failed\n");
                return 1;
            }
            Fission::InstructionDecoder decoder{};
            Deserializer des{};
            auto d = des.Deserialize(bc);
            if (!d || d->functions.empty()) {
                std::fprintf(stderr, "[ssa-dump] deserialize failed\n");
                return 1;
            }
            BytecodeLifter lifter{&decoder};
            LiftedFunction lifted = lifter.LiftDeserializedBytecode(*d);
            ControlFlowAnalyzer cfa{};
            AnalyzedFunction fn = cfa.DetermineBasicBlocks(&lifted);
            cfa.OptimizeGraph(fn);
            cfa.PruneUnreachable(fn);
            cfa.IdentifyStructures(fn);
            SSABuilder ssa{};
            ssa.Build(fn);

            std::function<void(const AnalyzedFunction &, int)> dump = [&](const AnalyzedFunction &f, int depth) {
                const std::string pad(depth * 2, ' ');
                std::printf(
                    "%s== function (params=%d) blocks=%zu ==\n", pad.c_str(), f.lpLiftedFunction ? f.lpLiftedFunction->numparams : 0, f.basicBlocks.size()
                );
                auto opnd = [&](const LiftedInstruction &in, size_t k) {
                    const auto &o = in.operands[k];
                    if (o.type != LiftedOperandType::Register)
                        return std::string("#");
                    const AccessType a = SSABuilder::GetRegisterAccess(in, k);
                    const char *acc = a == AccessType::Write ? "w" : a == AccessType::ReadWrite ? "rw" : a == AccessType::Read ? "r" : "-";
                    return std::format("R{}:v{}{}", o.value.reg, o.ssaVersion, acc);
                };
                for (const auto &b : f.basicBlocks) {
                    std::printf(
                        "%sBB%u [%s] preds=%zu succs=%zu\n", pad.c_str(), b.dwBlockId, BlockTypeToString(b.bType).c_str(), b.predecessors.size(),
                        b.successors.size()
                    );
                    for (const auto &phi : b.phiNodes)
                        if (!phi.operands.empty())
                            std::printf("%s    PHI R%d:v%d\n", pad.c_str(), phi.operands[0].value.reg, phi.operands[0].ssaVersion);
                    if (!b.lpHead)
                        continue;
                    for (const LiftedInstruction *p = b.lpHead; p && p <= b.lpTail; ++p) {
                        std::string ops;
                        for (size_t k = 0; k < p->operands.size(); ++k)
                            ops += (k ? ", " : "") + opnd(*p, k);
                        std::printf("%s    _%d %s %s\n", pad.c_str(), p->instructionIndex, std::string(OperationToString(p->operation)).c_str(), ops.c_str());
                    }
                }
                for (const auto &inner : f.innerFunctions)
                    dump(inner, depth + 1);
            };
            dump(fn, 0);
            return 0;
        }
        // Parse without compiling, avoiding Luau's 200-local compiler limit.
        if (arg == "--parse-check") {
            std::ifstream pin(argv[i + 1], std::ios::binary);
            if (!pin) {
                std::fprintf(stderr, "open failed: %s\n", argv[i + 1]);
                return 2;
            }
            std::stringstream pss;
            pss << pin.rdbuf();
            const std::string psrc = pss.str();
            for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (strncmp(flag->name, "Luau", 4) == 0)
                    flag->value = true;
            Luau::Allocator allocator;
            Luau::AstNameTable names(allocator);
            Luau::ParseResult res = Luau::Parser::parse(psrc.data(), psrc.size(), names, allocator, Luau::ParseOptions{});
            if (res.errors.empty()) {
                std::fprintf(stderr, "[parse-check] OK: %s (%zu bytes)\n", argv[i + 1], psrc.size());
                handledInputs = true;
                ++i;
                continue;
            }
            std::fprintf(stderr, "[parse-check] FAIL: %zu error(s) in %s\n", res.errors.size(), argv[i + 1]);
            size_t shown = 0;
            for (const auto &e : res.errors) {
                if (shown++ >= 100) {
                    std::fprintf(stderr, "  ... %zu more\n", res.errors.size() - 100);
                    break;
                }
                const Luau::Location loc = e.getLocation();
                std::fprintf(stderr, "  %d:%d: %s\n", static_cast<int>(loc.begin.line) + 1, static_cast<int>(loc.begin.column) + 1, e.getMessage().c_str());
            }
            inputExitCode = 1;
            handledInputs = true;
            ++i;
            continue;
        }
        // Decode Roblox bytecode and print its lifted AST as JSON.
        if (arg == "--roblox-ast") {
            const std::string p(argv[i + 1]);
            const std::string bytecode = DecodeBase64FileToBinary(std::wstring(p.begin(), p.end()));
            for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (strncmp(flag->name, "Luau", 4) == 0)
                    flag->value = true;
            decompiler.SetDecompileBudget(std::chrono::seconds(120));
            const auto r = decompiler.DecompileRobloxBytecode(bytecode, DecompilerFlags::CaptureAST | outputFlags);
            std::fprintf(stderr, "[roblox-ast] %s: code=%d\n", argv[i + 1], static_cast<int>(r.resultCode));
            PrintDecompileError(r);
            if (!r.debugNotes.empty())
                std::fprintf(stderr, "%s", r.debugNotes.c_str());
            std::cout << r.astJson << "\n";
            return r.resultCode == DecompileResult::Success ? 0 : 1;
        }
        // Compile source and print its post-rewrite AST as JSON.
        if (arg == "--decompile-test-ast") {
            std::ifstream din(argv[i + 1], std::ios::binary);
            if (!din) {
                std::fprintf(stderr, "open failed: %s\n", argv[i + 1]);
                return 2;
            }
            std::stringstream dss;
            dss << din.rdbuf();
            const std::string dsrc = dss.str();
            for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (strncmp(flag->name, "Luau", 4) == 0)
                    flag->value = true;
            decompiler.SetDecompileBudget(std::chrono::seconds(120));
            Luau::CompileOptions dopts{};
            dopts.optimizationLevel = 1;
            dopts.debugLevel = 2;
            const auto dr = decompiler.DecompileTestCode(dsrc, DecompilerFlags::AutoNameVariables | DecompilerFlags::CaptureAST | outputFlags, dopts);
            std::fprintf(stderr, "[decompile-test-ast] %s: code=%d\n", argv[i + 1], static_cast<int>(dr.resultCode));
            PrintDecompileError(dr);
            if (!dr.debugNotes.empty())
                std::fprintf(stderr, "%s", dr.debugNotes.c_str());
            std::cout << dr.astJson << "\n";
            return dr.resultCode == DecompileResult::Success ? 0 : 1;
        }
        // Compile and decompile source without generating a CFG graph.
        if (arg == "--decompile-test") {
            std::ifstream din(argv[i + 1], std::ios::binary);
            if (!din) {
                std::fprintf(stderr, "open failed: %s\n", argv[i + 1]);
                return 2;
            }
            std::stringstream dss;
            dss << din.rdbuf();
            const std::string dsrc = dss.str();
            for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (strncmp(flag->name, "Luau", 4) == 0)
                    flag->value = true;
            decompiler.SetDecompileBudget(std::chrono::seconds(120));
            Luau::CompileOptions dopts{};
            dopts.optimizationLevel = 1;
            dopts.debugLevel = 2;
            const auto dr = decompiler.DecompileTestCode(dsrc, DecompilerFlags::AutoNameVariables | DecompilerFlags::WriteIRToFile | outputFlags, dopts);
            std::fprintf(stderr, "[decompile-test] %s: code=%d (IR -> ir_out.txt)\n", argv[i + 1], static_cast<int>(dr.resultCode));
            PrintDecompileError(dr);
            if (!dr.debugNotes.empty())
                std::fprintf(stderr, "%s", dr.debugNotes.c_str());
            std::cout << "\n===SOURCE===\n" << dr.decompilationOutput << "\n===END===\n";
            return dr.resultCode == DecompileResult::Success ? 0 : 1;
        }
        const bool isBin = (arg == "--roblox-file");
        const bool isB64 = (arg == "--roblox-b64");
        if (!isBin && !isB64)
            continue;
        std::string bytecode;
        if (isBin) {
            std::ifstream in(argv[i + 1], std::ios::binary);
            if (!in) {
                std::fprintf(stderr, "open failed: %s\n", argv[i + 1]);
                return 2;
            }
            std::stringstream ss;
            ss << in.rdbuf();
            bytecode = ss.str();
        } else {
            const std::string p(argv[i + 1]);
            bytecode = DecodeBase64FileToBinary(std::wstring(p.begin(), p.end()));
        }
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
        decompiler.SetDecompileBudget(std::chrono::seconds(120));
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = decompiler.DecompileRobloxBytecode(bytecode, DecompilerFlags::PrintTimingBreakdown | outputFlags);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        const size_t lines = static_cast<size_t>(std::count(r.decompilationOutput.begin(), r.decompilationOutput.end(), '\n'));
        std::fprintf(
            stderr, "[roblox] %s: code=%d %lldms %zu lines %zu bytes\n", argv[i + 1], static_cast<int>(r.resultCode), static_cast<long long>(ms), lines,
            r.decompilationOutput.size()
        );
        PrintDecompileError(r);
        if (!r.debugNotes.empty())
            std::fprintf(stderr, "%s", r.debugNotes.c_str());
        if (!r.timingStatistics.empty())
            std::fprintf(stderr, "%s\n", r.timingStatistics.c_str());
        std::cout << "\n===SOURCE===\n" << r.decompilationOutput << "\n===END===\n";
        if (r.resultCode != DecompileResult::Success)
            inputExitCode = 1;
        handledInputs = true;
        ++i;
    }
    if (handledInputs)
        return inputExitCode;

    std::string s = "____";
    uintptr_t size = 0;
    auto sz = luau_compile(
        "print(\"hi\", 0, vector.create(1,1,1), { a = 2 }, nil, 0.2)", sizeof("print(\"hi\", 0, vector.create(1,1,1), { a = 2 }, nil, 0.2)"), nullptr, &size
    );
    s.resize(size);
    memcpy(s.data(), sz, size);
    for (unsigned char c : s) {
        std::cout << std::format("0x{:02x}, ", c);
    }

    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true; // enable all fflags, because integer is experimental.

    Luau::CompileOptions tmpOpts{};
    tmpOpts.optimizationLevel = 2;
    tmpOpts.debugLevel = 0;
    auto decompileResult = decompiler.DecompileTestCodeFromFile(
        "test.txt",
        DecompilerFlags::PrintTimingBreakdown | DecompilerFlags::WriteIRToFile | DecompilerFlags::GenerateSSAIRGraph | DecompilerFlags::GenerateIRGraph |
            DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes | DecompilerFlags::AutoNameVariables,
        tmpOpts
    );
    ASSERT(decompileResult.resultCode == DecompileResult::Success, "Decompilation failed.");

    std::cout << "\n===SOURCE===\n" << decompileResult.decompilationOutput << "\n===END===\n";

    if (!decompileResult.timingStatistics.empty())
        std::cout << decompileResult.timingStatistics << std::endl;

    system("graph_generator.bat");
}
