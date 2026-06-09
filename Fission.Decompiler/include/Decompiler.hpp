//
// Created by Pixeluted on 01/12/2025.
//
#pragma once
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include "ASTLifter.hpp"
#include "SSABuilder.hpp"
#include "SourceGenerator/Generator.hpp"

enum class DecompileResult : uint8_t {
    Success,
    FailedToReadFile,
    FailedToDeserialize,
    FailedToDecompile, // internal failure (malformed/hostile bytecode) caught by the safety boundary
};

enum class DecompilerFlags : uint16_t {
    PrintIR = 1 << 0,
    WriteIRToFile = 1 << 1,
    GenerateIRGraph = 1 << 2,
    GenerateSSAIRGraph = 1 << 3,
    PrintTimingBreakdown = 1 << 4,
    InferTypes = 1 << 5,
    OptimizeIR = 1 << 6,
    InferRobloxTypes = 1 << 7,
    AutoNameVariables = 1 << 8,
    // drop Fission's info comments (function info, capture/name notes). warnings + banner still emitted.
    OmitFissionComments = 1 << 9
};

constexpr DecompilerFlags operator|(DecompilerFlags lhs, DecompilerFlags rhs) {
    return static_cast<DecompilerFlags>(static_cast<uint16_t>(lhs) | static_cast<uint16_t>(rhs));
}

constexpr DecompilerFlags operator&(DecompilerFlags lhs, DecompilerFlags rhs) {
    return static_cast<DecompilerFlags>(static_cast<uint16_t>(lhs) & static_cast<uint16_t>(rhs));
}

constexpr DecompilerFlags operator~(DecompilerFlags flag) { return static_cast<DecompilerFlags>(~static_cast<uint16_t>(flag)); }

inline DecompilerFlags &operator|=(DecompilerFlags &lhs, DecompilerFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline DecompilerFlags &operator&=(DecompilerFlags &lhs, DecompilerFlags rhs) {
    lhs = lhs & rhs;
    return lhs;
}

struct DecompilationResult {
    std::string decompilationOutput;
    std::string irOutput;
    std::string timingStatistics;
    DecompileResult resultCode;
};
class Decompiler {
    Deserializer deserializer{};
    ControlFlowAnalyzer controlFlowAnalyzer{};
    SSABuilder SSABuilder{};
    ASTLifter ASTLifter{};
    SourceGenerator SourceGenerator{};
    GraphVisualizer visualizer{};

    DecompilationResult CommonDecompilerEntry(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags);
    // the actual pipeline; CommonDecompilerEntry wraps it in the safety boundary so throws become FailedToDecompile.
    DecompilationResult CommonDecompilerEntryImpl(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags);

  public:
    DecompilationResult
    DecompileTestCode(const std::string &testCode, DecompilerFlags flags = static_cast<DecompilerFlags>(0), const Luau::CompileOptions &compileOpts = {1, 2});
    DecompilationResult DecompileTestCodeFromFile(
        const std::string &fileName, DecompilerFlags flags = static_cast<DecompilerFlags>(0), const Luau::CompileOptions &compileOpts = {1, 2}
    );
    DecompilationResult DecompileRobloxBytecode(const std::string &bytecode, DecompilerFlags flags = static_cast<DecompilerFlags>(0));
    DecompilationResult DecompileRobloxBytecodeFromFile(const std::string &fileName, DecompilerFlags flags = static_cast<DecompilerFlags>(0));
};
