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
    SETGLOBAL,
    GETUPVAL,
    SETUPVAL,
    GETIMPORT,
    GETTABLE,
    SETTABLE,
    GETTABLEKS,
    SETTABLEKS,
    GETTABLEN,
    SETTABLEN,
    NEWCLOSURE,
    NAMECALL,
    CALL,
    RETURN
};

enum class LiftedOperandType : uint8_t {
    Register,
    ImmediateNil,
    ImmediateInteger,
    ImmediateBool,
    ImmediateConstant,
    ImmediateAux
};

struct LiftedOperand {
    LiftedOperandType type;

    union {
        uint8_t reg;

        union {
            int16_t n;
            bool b;
            int32_t k;
            uint32_t u;
        } imm;
    } value;
};

struct LiftedInstruction {
    LiftedOperation operation;
    std::vector<LiftedOperand> operands { };
};

struct LiftedFunction {
    std::vector<LiftedInstruction> instructions;
    std::vector<LiftedFunction> subfunctions;
    std::string name;
};

LiftedFunction LiftDeserializedBytecode(const DeserializedBytecode &bytecode);

std::string_view OperationToString(LiftedOperation operation);