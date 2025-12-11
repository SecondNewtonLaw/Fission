#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "BinaryReader.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "lua.h"
#include <array>
#include <libassert/assert.hpp>
#include <optional>
#include <span>
#include <variant>
#include <vector>

#define USERDATA_TYPE_LIMIT (LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE)

struct LuauLocalVar {
    std::string varname;
    int startpc;
    int endpc;
    uint8_t reg;
};

typedef uint32_t Instruction;
struct DeserializedFunction;

struct LuauInstruction {
    Instruction instruction;

    LuauInstruction() : instruction(0) {}

    LuauInstruction(const Instruction instruction) { this->instruction = instruction; }

    [[nodiscard]] bool HasAux() const { return Luau::getOpLength(this->GetOpCode()) == 2; }

    [[nodiscard]] int GetOpCodeSize() const { return Luau::getOpLength(this->GetOpCode()); }

    [[nodiscard]] LuauOpcode GetOpCode() const { return static_cast<LuauOpcode>(LUAU_INSN_OP(this->instruction)); }

    enum class LuauOperand : uint8_t { A, B, C, D, E };

    [[nodiscard]] uint8_t GetABCOperand(const LuauOperand operand) const {
        ASSERT(operand <= LuauOperand::C && operand >= LuauOperand::A, "malformed request for an ABC operand.");

        switch (operand) {
        case LuauOperand::A:
            return LUAU_INSN_A(instruction);
        case LuauOperand::B:
            return LUAU_INSN_B(instruction);
        case LuauOperand::C:
            return LUAU_INSN_C(instruction);

        default:
            ASSERT(false, "malformed request for an ABC operand, operand out of range.");
        }

        UNREACHABLE();
    }

    [[nodiscard]] int16_t GetD() const { return LUAU_INSN_D(instruction); }

    [[nodiscard]] int32_t GetE() const { return LUAU_INSN_E(instruction); }
};

typedef double LuauNumber;
typedef std::string LuauString;
typedef bool LuauBoolean;
typedef DeserializedFunction *LuauProto; // bytecode id?

struct LuauVector {
    float x, y, z, w;
};

struct LuauTable {
    std::vector<std::string> keys{};
};

struct LuauConstant {
    lua_Type kType{};
    std::variant<LuauTable, LuauString, LuauVector, LuauNumber, LuauBoolean, LuauProto> constantData{};

    LuauConstant() : kType(LUA_TNIL) {}

    LuauConstant(const lua_Type kType) { this->kType = kType; }
};

struct DeserializedFunction {
    std::uint8_t bytecodeId{};
    std::uint8_t maxstacksize{};
    std::uint8_t numparams{};
    std::uint8_t nups{};
    bool isvararg{};
    bool bIsMain{};
    std::uint8_t flags{};

    std::vector<uint8_t> typeinfo{};
    std::vector<LuauInstruction> instructions{};
    std::vector<LuauConstant> constants{};
    std::vector<DeserializedFunction *> subfunctions{};
    std::uint32_t lineDefined;
    std::optional<std::string> debugName;
    std::uint8_t linegaplog2;
    std::vector<std::uint8_t> lineinfo;
    std::int32_t *abslineinfo;
    std::vector<LuauLocalVar> locvars{};
    std::vector<std::string> upvalueNames;
};

struct DeserializedBytecode {
    std::uint8_t bytecodeVersion = 0;
    std::uint8_t typesVersion = 0;
    std::vector<std::string> stringTable{};
    std::vector<DeserializedFunction> functions{};
    std::array<uint8_t, USERDATA_TYPE_LIMIT> userdataMappings{};
    DeserializedFunction *lpMainFunction;

    std::optional<std::string> ReadFromStringTable(const std::uint32_t stringId) const {
        if (stringId == 0)
            return std::nullopt;

        return stringTable.at(stringId - 1);
    }
};

class Deserializer {

  public:
    std::optional<DeserializedBytecode> Deserialize(const std::string &bytecode);
};