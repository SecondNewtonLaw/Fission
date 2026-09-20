#pragma once
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include <chrono>
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100)
#endif
#include "Luau/Compiler.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
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
    // Omit informational comments while preserving warnings and the banner.
    OmitFissionComments = 1 << 9,
    // Capture CFG as in-memory Graphviz DOT.
    CaptureCFGGraph = 1 << 10,
    // Capture lifted AST as JSON.
    CaptureAST = 1 << 11
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
    // Capture fields remain empty unless requested and trail existing aggregate fields for compatibility.
    std::string cfgGraph{};
    std::string astJson{};
    std::string errorMessage{};
};
class Decompiler {
    Deserializer deserializer{};
    ControlFlowAnalyzer controlFlowAnalyzer{};
    SSABuilder ssaBuilder{};
    ASTLifter astLifter{};
    SourceGenerator sourceGenerator{};
    GraphVisualizer visualizer{};

    // Wall-clock limit for hostile or runaway CFG and lifting work.
    std::chrono::steady_clock::duration m_decompileBudget = std::chrono::seconds(120);

    DecompilationResult CommonDecompilerEntry(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags);
    // CommonDecompilerEntry converts pipeline exceptions into FailedToDecompile.
    DecompilationResult CommonDecompilerEntryImpl(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags);

  public:
    DecompilationResult
    DecompileTestCode(const std::string &testCode, DecompilerFlags flags = static_cast<DecompilerFlags>(0), const Luau::CompileOptions &compileOpts = {1, 2});
    DecompilationResult DecompileTestCodeFromFile(
        const std::string &fileName, DecompilerFlags flags = static_cast<DecompilerFlags>(0), const Luau::CompileOptions &compileOpts = {1, 2}
    );
    DecompilationResult DecompileRobloxBytecode(const std::string &bytecode, DecompilerFlags flags = static_cast<DecompilerFlags>(0));
    DecompilationResult DecompileRobloxBytecodeFromFile(const std::string &fileName, DecompilerFlags flags = static_cast<DecompilerFlags>(0));
    // Decompile non-Roblox Luau bytecode with the identity decoder.
    DecompilationResult DecompileVanillaBytecode(const std::string &bytecode, DecompilerFlags flags = static_cast<DecompilerFlags>(0));

    // Callers handling untrusted input can reduce the default 120-second budget.
    void SetDecompileBudget(std::chrono::steady_clock::duration budget) { m_decompileBudget = budget; }
};
