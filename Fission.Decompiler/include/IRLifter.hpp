//
// Created by Pixeluted on 29/11/2025.
//
#pragma once

#include <boost/container/small_vector.hpp>
#include <vector>

struct DeserializedFunction;
struct DeserializedBytecode;
enum class LiftedOperation : uint32_t {
    NOP,
    BREAK,
    LOAD,
    LOADNJUMP,
    MOVE,
    GETGLOBAL,
    SETGLOBAL
};

enum class LiftedOperandType : uint8_t {
    Register,
    Immediate,
    ImmediateNil,
    ImmediateInteger,
    ImmediateBool,
    ImmediateConstant
};

struct LiftedOperand {
    LiftedOperandType type;
    union {
        uint8_t reg;
        union {
            int16_t n;
            bool b;
            int32_t k;
        } imm;
    } value;
};

struct LiftedInstruction {
    LiftedOperation operation;
    std::vector<LiftedOperand> operands{};
};

struct LiftedFunction {
    std::vector<LiftedInstruction> instructions;
    std::vector<LiftedFunction> subfunctions;
};

LiftedFunction LiftDeserializedBytecode(const DeserializedBytecode& bytecode);
