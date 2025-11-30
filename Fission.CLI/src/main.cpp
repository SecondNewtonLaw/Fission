//
// Created by Dottik on 5/10/2025.
//

#include "Deserializer.hpp"
#include "libassert/assert.hpp"
#include <sstream>
#include <format>
#include <cstdio>
#include <ostream>
#include <iostream>
#include <fstream>

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "BytecodeLifter.hpp"
#include "Luau/Compiler.h"

#include <filesystem>
#pragma clang diagnostic pop

std::string GetIndentation(int indentationLevel) {
    std::string indent(indentationLevel, ' ');
    return indent;
}

void PrintFunctionOntoStream(std::stringstream &stream, int indentationLevel, const LiftedFunction &func) {
    stream << GetIndentation(indentationLevel) << "/* Function Name: '" << func.name << "' */\n";

    stream << GetIndentation(indentationLevel) << "/* Function IR Instructions (size: " << func.instructions.size() << ") */" << "\n";
    for (const auto &insn : func.instructions) {
        stream << GetIndentation(indentationLevel + 4) << OperationToString(insn.operation) << " ";
        for (std::size_t i = 0; i < insn.operands.size(); i++) {
            const auto &operand = insn.operands[i];
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
            if (i + 1 != insn.operands.size())
                stream << ", ";
        }

        if (insn.comment)
            stream << " /* " << *insn.comment << " */";

        stream << "\n";
    }

    stream << GetIndentation(indentationLevel) << "/* Functions inside of Function (size: " << func.subfunctions.size() << ") */" << "\n";

    for (const auto &subfunction : func.subfunctions)
        PrintFunctionOntoStream(stream, indentationLevel + 4, subfunction);

}

std::string FormatIR(LiftedFunction &func) {
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
    // Luau::CompileOptions compileOpts {1, 2};
    // auto hack = readfile("text.txt");
    // auto bytecode = Luau::compile(*hack, compileOpts);

    auto bypassFE = readfile("bytecode_raw.txt");
    Deserializer deserializer { };
    const auto deserializationResultOptional = deserializer.Deserialize(*bypassFE);

    Fission::RobloxClientDecoder decoder { };
    // ASSERT(deserializationResultOptional.has_value(), "deserialization failed.");
    auto &deserializationResult = deserializationResultOptional.value();
    auto bytecodeLifter = BytecodeLifter {&decoder};
    auto liftedIR = bytecodeLifter.LiftDeserializedBytecode(deserializationResult);

    auto ir = FormatIR(liftedIR);

    writefile(std::filesystem::path {"ir_out.txt"}, ir);
    std::cout << ir << std::endl;
    return 0;
}