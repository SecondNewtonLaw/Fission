//
// Created by Dottik on 5/10/2025.
//

#include "Deserializer.hpp"
#include "libassert/assert.hpp"
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <ostream>
#include <print>
#include <sstream>
#include <string>
#include <vector>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "DenominatorAnalysis.hpp"
#include "Luau/Compiler.h"
#include "SSABuilder.hpp"

#include <filesystem>
#pragma clang diagnostic pop

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
            auto instructionIndex = std::distance(rawFunc->instructions.data(), currentInst);

            stream << GetIndentation(indentationLevel + 6) << "_" << instructionIndex << ": " << OperationToString(currentInst->operation) << " ";

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

            if (currentInst->comment)
                stream << " /* " << *currentInst->comment << " */";

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

std::optional<std::string> readfile(std::filesystem::path path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file.is_open())
        return std::nullopt;

    std::streamsize size = file.tellg();

    file.seekg(0, std::ios::beg);

    std::string buffer(size, '\0');

    if (file.read(&buffer[0], size))
        return buffer;

    return buffer;
}

int main() {
    Luau::CompileOptions compileOpts{0, 2};
    auto hack = readfile("text.txt");
    if (!hack.has_value()) {
        std::println("Failed to read text.txt");
        return 1;
    }

    auto bytecode = Luau::compile(*hack, compileOpts);

    Deserializer deserializer{};
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    const auto deserializationResultOptional = deserializer.Deserialize(bytecode);
    std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    Fission::InstructionDecoder decoder{};
    // ASSERT(deserializationResultOptional.has_value(), "deserialization failed.");

    auto bytecodeLifter = BytecodeLifter{&decoder};

    std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();
    auto liftedIR = bytecodeLifter.LiftDeserializedBytecode(*deserializationResultOptional);
    std::chrono::steady_clock::time_point t3 = std::chrono::steady_clock::now();

    ControlFlowAnalyzer analyzer{};
    std::chrono::steady_clock::time_point t4 = std::chrono::steady_clock::now();
    auto analyzedFunction = analyzer.DetermineBasicBlocks(&liftedIR);
    std::chrono::steady_clock::time_point t5 = std::chrono::steady_clock::now();

    SSABuilder ssa;
    std::chrono::steady_clock::time_point t6 = std::chrono::steady_clock::now();
    ssa.Build(analyzedFunction);
    std::chrono::steady_clock::time_point t7 = std::chrono::steady_clock::now();

    std::chrono::steady_clock::time_point t8 = std::chrono::steady_clock::now();
    auto ir = FormatAnalyzedIR(analyzedFunction);
    std::chrono::steady_clock::time_point t9 = std::chrono::steady_clock::now();

    writefile(std::filesystem::path{"ir_out.txt"}, ir);
    std::cout << ir << std::endl;
    std::chrono::steady_clock::time_point t10 = std::chrono::steady_clock::now();
    std::string dotContent = GraphVisualizer::GenerateDotGraph(analyzedFunction);
    std::chrono::steady_clock::time_point t11 = std::chrono::steady_clock::now();
    writefile("cfg.dot", dotContent);

    std::cout << "Generated ir_out.txt and cfg.dot" << std::endl;
    std::cout << "you can open cfg.dot in https://edotor.net/ or a Graphviz viewer to enjoy a fucking readable output." << std::endl;

    std::println(
        "Decompilation Breakdown:\n\tDeserializing Bytecode: {}s\n\tLifting into IR: {}s\n\tControl Flow Analysis: {}s\n\tIR -> SSA Form: {}s\n\tOptimization: "
        "{}s\n\tFormatting: "
        "{}s\n\tGraph View Generation: {}s\n\tOutput generated in {}s.",
        std::chrono::duration<float>(t1 - t0).count(), std::chrono::duration<float>(t3 - t2).count(), std::chrono::duration<float>(t5 - t4).count(),
        std::chrono::duration<float>(t7 - t6), 0, std::chrono::duration<float>(t9 - t8).count(), std::chrono::duration<float>(t11 - t10).count(),
        std::chrono::duration<float>(t11 - t0).count()
    );
    return 0;
}