//
// Created by Pixeluted on 01/12/2025.
//
#include "Decompiler.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ostream>
#include <print>

std::string GetIndentation(int indentationLevel) {
    std::string indent(indentationLevel, ' ');
    return indent;
}

std::string FormatIntList(const std::vector<std::uint32_t> &list) {
    if (list.empty())
        return "[]";
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < list.size(); ++i) {
        ss << list[i];
        if (i < list.size() - 1)
            ss << ", ";
    }
    ss << "]";
    return ss.str();
}

void PrintFunctionOntoStream(std::stringstream &stream, int indentationLevel, const AnalyzedFunction &analyzedFunc) {
    LiftedFunction *rawFunc = analyzedFunc.lpLiftedFunction;

    stream << GetIndentation(indentationLevel) << "/* Function Name: '" << rawFunc->name << "' */\n";
    stream << GetIndentation(indentationLevel) << "/* Basic Blocks: " << analyzedFunc.basicBlocks.size() << " */\n";

    for (const auto &block : analyzedFunc.basicBlocks) {
        stream << "\n";
        stream << GetIndentation(indentationLevel + 2) << "BLOCK_" << block.dwBlockId << ":\n";
        stream << GetIndentation(indentationLevel + 4) << "Type: " << BlockTypeToString(block.bType) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Terminator: " << BlockTerminatorToString(block.bTerminator) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Predecessors: " << FormatIntList(block.predecessors) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Successors:   " << FormatIntList(block.successors) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Code:\n";

        if (block.lpHead == nullptr || block.lpTail == nullptr) {
            stream << GetIndentation(indentationLevel + 6) << "<Empty/Error Block>\n";
            continue;
        }

        LiftedInstruction *currentInst = block.lpHead;
        while (true) {
            stream << GetIndentation(indentationLevel + 6) << "_" << currentInst->instructionIndex << ": " << OperationToString(currentInst->operation) << " ";

            for (std::size_t i = 0; i < currentInst->operands.size(); i++) {
                const auto &operand = currentInst->operands[i];
                switch (operand.type) {
                case LiftedOperandType::Register:
                    stream << "R" << std::to_string(operand.value.reg);
                    break;
                case LiftedOperandType::ImmediateNil:
                    stream << "nil";
                    break;
                case LiftedOperandType::ImmediateInteger:
                    stream << std::format("0x{:X}", static_cast<uint32_t>(operand.value.imm.n));
                    break;
                case LiftedOperandType::ImmediateBool:
                    stream << std::format("{}", operand.value.imm.b ? "true" : "false");
                    break;
                case LiftedOperandType::ImmediateConstant:
                    stream << "K" << std::to_string(operand.value.imm.k);
                    break;
                case LiftedOperandType::ImmediateAux:
                    stream << "AUXV_" << std::to_string(operand.value.imm.u);
                    break;
                default:
                    ASSERT(false, "unhandled mapping of operand to text");
                }
                if (i + 1 != currentInst->operands.size())
                    stream << ", ";
            }

            if (currentInst->instructionRemarks)
                stream << " /* " << *currentInst->instructionRemarks << " */";

            stream << "\n";

            if (currentInst == block.lpTail)
                break;

            currentInst++;
        }
    }

    if (analyzedFunc.innerFunctions.empty())
        stream << "\n" << GetIndentation(indentationLevel) << "/* There are no nested functions inside of this function. */" << "\n";
    else
        stream << "\n" << GetIndentation(indentationLevel) << "/* Functions inside of Function (size: " << analyzedFunc.innerFunctions.size() << ") */" << "\n";

    for (const auto &subfunction : analyzedFunc.innerFunctions)
        PrintFunctionOntoStream(stream, indentationLevel + 4, subfunction);
}

std::string FormatAnalyzedIR(const AnalyzedFunction &func) {
    std::stringstream sstream;

    sstream << "/* Fission IR Viewer */\n";
    sstream << "/* Created by Fission Contributors */\n";
    sstream << GetIndentation(4) << "/* Program Root (Main Procedure/Prototype/Function) */\n";
    PrintFunctionOntoStream(sstream, 4, func);

    return sstream.str();
}

void writefile(const std::filesystem::path &path, const std::string &content) {
    std::ofstream file(path);

    if (!file.is_open())
        return;

    file << content;
    file.close();
}

std::optional<std::string> readfile(const std::filesystem::path &path, const bool isBinary = false) {
    std::ifstream file(path, (isBinary ? std::ios::binary : 0) | std::ios::ate);

    if (!file.is_open())
        return std::nullopt;

    std::streamsize size = file.tellg();

    file.seekg(0, std::ios::beg);

    std::string buffer(size, '\0');

    if (file.read(&buffer[0], size))
        return buffer;

    return buffer;
}

DecompilationResult Decompiler::CommonDecompilerEntry(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags) {
    DecompilationResult res{};
    const auto deserializeStart = std::chrono::steady_clock::now();
    const auto deserializedBytecode = deserializer.Deserialize(bytecode);
    const auto deserializeEnd = std::chrono::steady_clock::now();
    if (!deserializedBytecode || deserializedBytecode->functions.empty())
        return {"", "", DecompileResult::FailedToDeserialize};

    auto bytecodeLifter = BytecodeLifter{decoder};
    const auto bytecodeLiftStart = std::chrono::steady_clock::now();
    auto liftedBytecode = bytecodeLifter.LiftDeserializedBytecode(*deserializedBytecode);
    const auto bytecodeLiftEnd = std::chrono::steady_clock::now();

    const auto controlFlowAnalyzeStart = std::chrono::steady_clock::now();
    const auto basicBlockIdentificationStart = std::chrono::steady_clock::now();
    auto controlFlowAnalyzedFunction = controlFlowAnalyzer.DetermineBasicBlocks(&liftedBytecode);
    const auto basicBlockIdentificationEnd = std::chrono::steady_clock::now();

    const auto optimizeGraphStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.OptimizeGraph(controlFlowAnalyzedFunction);
    const auto optimizeGraphEnd = std::chrono::steady_clock::now();

    const auto unreachablePruningStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.PruneUnreachable(controlFlowAnalyzedFunction);
    const auto unreachablePruningEnd = std::chrono::steady_clock::now();

    const auto identifyLoopStructuresStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.IdentifyStructures(controlFlowAnalyzedFunction);
    const auto controlFlowAnalyzeEnd = std::chrono::steady_clock::now();

    const auto ssaStart = std::chrono::steady_clock::now();
    SSABuilder.Build(controlFlowAnalyzedFunction);
    const auto ssaEnd = std::chrono::steady_clock::now();

    const auto astStart = std::chrono::steady_clock::now();
    const auto liftedAST = ASTLifter.Lift(controlFlowAnalyzedFunction);
    const auto astEnd = std::chrono::steady_clock::now();

    RootNode root{liftedAST.statements};

    const auto sgenStart = std::chrono::steady_clock::now();
    const auto generator = SourceGenerator.GenerateSource(&root);
    const auto sgenEnd = std::chrono::steady_clock::now();

    std::println("generated source code:\n{}", generator);

    const auto printIR = (flags & DecompilerFlags::PrintIR) == DecompilerFlags::PrintIR;
    const auto writeIR = (flags & DecompilerFlags::WriteIRToFile) == DecompilerFlags::WriteIRToFile;
    if (printIR || writeIR) {
        const auto formattedIR = FormatAnalyzedIR(controlFlowAnalyzedFunction);
        if (printIR) {
            std::println("{}", formattedIR);
        }

        if (writeIR) {
            writefile(std::filesystem::path{"ir_out.txt"}, formattedIR);
            std::println("Generated ir_out.txt");
        }
    }

    const auto generateIRGraph = (flags & DecompilerFlags::GenerateIRGraph) == DecompilerFlags::GenerateIRGraph;
    const auto generateSSAGraph = (flags & DecompilerFlags::GenerateSSAIRGraph) == DecompilerFlags::GenerateSSAIRGraph;
    if (generateIRGraph || generateSSAGraph) {
        const auto dotGraph = visualizer.GenerateDotGraph(
            controlFlowAnalyzedFunction, generateIRGraph && generateSSAGraph ? GraphContent::Both
                                         : generateIRGraph == true           ? GraphContent::IROnly
                                                                             : GraphContent::SSAOnly
        );

        writefile("cfg.dot", dotGraph);
        std::println("Generated cfg.dot, use graph_generator to turn it into viewable .svg");
    }

    if ((flags & DecompilerFlags::PrintTimingBreakdown) == DecompilerFlags::PrintTimingBreakdown) {
        res.timingStatistics = std::format(
            "Decompilation Breakdown:\n\t"
            "Deserializing Bytecode: {}\n\t"
            "Lifting into IR: {}\n\t"
            "Control Flow Analysis: {}\n\t"
            "\tDetermining Basic Blocks: {}\n\t"
            "\tGraph Optimization: {}\n\t"
            "\tBlock Pruning: {}\n\t"
            "\tStructure Identification: {}\n\t"
            "IR -> SSA Form: {}\n\t"
            "IR (SSA) -> AST: {}\n\t"
            "AST -> Source Code: {}\n\t"
            "Optimization: {}s",
            std::chrono::duration<float>(deserializeEnd - deserializeStart), std::chrono::duration<float>(bytecodeLiftEnd - bytecodeLiftStart),
            std::chrono::duration<float>(controlFlowAnalyzeEnd - controlFlowAnalyzeStart),
            std::chrono::duration<float>(basicBlockIdentificationEnd - basicBlockIdentificationStart),
            std::chrono::duration<float>(optimizeGraphEnd - optimizeGraphStart), std::chrono::duration<float>(unreachablePruningEnd - unreachablePruningStart),
            std::chrono::duration<float>(controlFlowAnalyzeEnd - identifyLoopStructuresStart), std::chrono::duration<float>(ssaEnd - ssaStart),
            std::chrono::duration<float>(astEnd - astStart), std::chrono::duration<float>(sgenEnd - sgenStart), 0
        );
    }

    res.resultCode = DecompileResult::Success;
    res.decompilationOutput = std::move(generator);
    return res;
}

DecompilationResult Decompiler::DecompileTestCode(const std::string &testCode, const DecompilerFlags flags, const Luau::CompileOptions &compileOpts) {
    const auto compiledBytecode = Luau::compile(testCode, compileOpts);
    auto normalDecoder = Fission::InstructionDecoder{};
    return CommonDecompilerEntry(compiledBytecode, &normalDecoder, flags);
}

DecompilationResult Decompiler::DecompileTestCodeFromFile(const std::string &fileName, const DecompilerFlags flags, const Luau::CompileOptions &compileOpts) {
    const auto readFile = readfile(std::filesystem::path(fileName));
    if (!readFile)
        return {"", "", DecompileResult::FailedToReadFile};

    return DecompileTestCode(*readFile, flags, compileOpts);
}

DecompilationResult Decompiler::DecompileRobloxBytecode(const std::string &bytecode, DecompilerFlags flags) {
    auto robloxDecoder = Fission::RobloxClientDecoder{};
    return CommonDecompilerEntry(bytecode, &robloxDecoder, flags);
}

DecompilationResult Decompiler::DecompileRobloxBytecodeFromFile(const std::string &fileName, DecompilerFlags flags) {
    const auto readFile = readfile(std::filesystem::path(fileName), true);
    if (!readFile)
        return {"", "", DecompileResult::FailedToReadFile};

    return DecompileRobloxBytecode(*readFile, flags);
}