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

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "IRLifter.hpp"
#include "Luau/Compiler.h"
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
                stream << std::format("0x{:X}", operand.value.imm.n);
                break;
            case LiftedOperandType::ImmediateBool:
                stream << std::format("{}", operand.value.imm.b ? "true" : "false");
                break;
            case LiftedOperandType::ImmediateConstant:
                stream << "K" << std::to_string(operand.value.imm.k);
                break;
            default:
                ASSERT(false, "unhandled mapping of operand to text");
            }
            if (i + 1 != insn.operands.size())
                stream << ", ";
        }

        stream << "\n";
    }

    stream << GetIndentation(indentationLevel) << "/* Functions inside of Function (size: " << func.subfunctions.size() << ") */" << "\n";

    for (const auto &subfunction : func.subfunctions)
        PrintFunctionOntoStream(stream, indentationLevel + 4, subfunction);

}

void PrintIR(LiftedFunction &func) {
    std::stringstream sstream;

    sstream << "/* Fission IR Viewer */\n";
    sstream << "/* Created by Fission Contributors */\n";
    sstream << "\n";
    PrintFunctionOntoStream(sstream, 4, func);

    std::cout << sstream.str();
}

int main() {
    Luau::CompileOptions compileOpts {0, 2};
    auto bytecode = Luau::compile(
        R"(
function a()
    local a = false
    local b = a
    local c = 32000
    local d = 10000
    local q = "string"

    function b()
        local aa = false
        local bb = aa
        local cc = 32000
        local dd = 10000
        local q = "string"
    end

    b()
end

a()
)",
        compileOpts
    );

    Deserializer deserializer { };
    const auto deserializationResultOptional = deserializer.Deserialize(bytecode);

    // ASSERT(deserializationResultOptional.has_value(), "deserialization failed.");
    auto &deserializationResult = deserializationResultOptional.value();
    auto liftedIR = LiftDeserializedBytecode(deserializationResult);

    PrintIR(liftedIR);
    return 0;
}