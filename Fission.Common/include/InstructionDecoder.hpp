//
// Created by Dottik on 5/10/2025.
//

#pragma once
#include <string_view>
#include <string>


namespace Fission {
    class InstructionDecoder {
    public:
        virtual uint32_t DecodeInstruction(uint32_t instruction) {
            return instruction;
        }
    };

    class RobloxClientDecoder : public InstructionDecoder {
    public:
        virtual uint32_t DecodeInstruction(uint32_t instruction) {
            auto abc = (instruction & 0xFFFFFF00);
            constexpr int32_t MULTIPLICATION_MAGIC = 203u;
            uint8_t opcode = static_cast<uint8_t>(instruction & 0xFF) * MULTIPLICATION_MAGIC;
            return abc | opcode;
        }
    };
}