//
// Created by Pixeluted on 29/11/2025.
//
#pragma once

#include "InstructionDecoder.hpp"

#include <libassert/assert.hpp>
#include <optional>
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
    RETURN,
    JUMP,
    JUMPIF,
    JUMPIFNOT,
    JUMPIFEQ,
    JUMPIFLE,
    JUMPIFLT,
    JUMPIFNOTEQ,
    JUMPIFNOTLE,
    JUMPIFNOTLT,
    ADD,
    SUB,
    MUL,
    DIV,
    MOD,
    POW,
    ADDK,
    SUBK,
    MULK,
    DIVK,
    MODK,
    POWK,
    AND,
    OR,
    ANDK,
    ORK,
    CONCAT,
    NOT,
    MINUS,
    LENGTH,
    NEWTABLE,
    DUPTABLE,
    SETLIST,
    FORNPREP,
    FORNLOOP,
    FORGLOOP,
    FORGPREP_INEXT,
    FORGPREP_NEXT,
    FASTCALL3,
    GETVARARGS,
    DUPCLOSURE,
    PREPVARARGS,
    FASTCALL,
    CAPTURE,
    SUBRK,
    DIVRK,
    FASTCALL1,
    FASTCALL2,
    FASTCALL2K,
    FORGPREP,
    JUMPXEQK,
    IDIV,
    IDIVK,

    PHI // phi node on SSA.
};

enum class LiftedOperandType : uint8_t { Register, ImmediateNil, ImmediateInteger, ImmediateBool, ImmediateConstant, ImmediateAux };

struct LiftedOperand {
    LiftedOperandType type;

    union {
        uint8_t reg;

        union {
            int32_t n;
            bool b;
            int32_t k;
            uint32_t u;
        } imm;
    } value;

    int32_t ssaVersion = -1;
};

struct LiftedInstruction {
    LiftedOperation operation;
    int32_t instructionIndex;
    std::vector<LiftedOperand> operands{};
    std::optional<std::string> comment = std::nullopt;
};

struct LiftedFunction {
    std::vector<LiftedInstruction> instructions;
    std::vector<LiftedFunction> subfunctions;
    std::string name;
    uint8_t numparams;
    DeserializedFunction *lpDeserialized;
};

class BytecodeLifter {
    uint64_t functionCounter = 0;

    LiftedFunction LiftFunctionBytecodeInternal(const DeserializedFunction *function, bool bIsMain = false);

    Fission::InstructionDecoder *lpDecoder = nullptr;

  public:
    BytecodeLifter(Fission::InstructionDecoder *decoder) {
        lpDecoder = decoder;
        ASSERT(lpDecoder != nullptr, "Instruction decoder cannot be nullptr", lpDecoder);
    }

    LiftedFunction LiftDeserializedBytecode(const DeserializedBytecode &bytecode);
};

std::string_view OperationToString(LiftedOperation operation);