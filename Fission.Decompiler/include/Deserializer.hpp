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

struct DeserializedBytecode;
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
typedef int64_t LuauInteger;

struct LuauVector {
    float x, y, z, w;
};

struct LuauTable {
    std::vector<std::string> keys{};
    std::vector<int32_t> valueConstantIndices{};
};

struct LuauConstant {
    lua_Type kType{};
    std::variant<LuauTable, LuauString, LuauVector, LuauNumber, LuauBoolean, LuauProto, LuauInteger> constantData{};

    LuauConstant() : kType(LUA_TNIL) {}

    LuauConstant(const lua_Type kType) { this->kType = kType; }

    template <typename T> T GetValue() const {
        static_assert(
            typeid(T) == typeid(LuauTable) || typeid(T) == typeid(LuauString) || typeid(T) == typeid(LuauVector) || typeid(T) == typeid(LuauNumber) ||
                typeid(T) == typeid(LuauBoolean) || typeid(T) == typeid(LuauProto),
            "invalid templated typename T!"
        );
        return std::get<T>(constantData);
    }
};

struct DeserializedFunction {
    std::uint8_t uTypeVersion;
    std::uint8_t uBytecodeVersion;

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
    static std::string GetBytecodeTypeName(uint8_t typeByte) {
        uint8_t baseType = typeByte & ~LBC_TYPE_OPTIONAL_BIT;
        bool isOptional = (typeByte & LBC_TYPE_OPTIONAL_BIT) != 0;

        std::string typeName;

        if (baseType >= LBC_TYPE_TAGGED_USERDATA_BASE && baseType < LBC_TYPE_TAGGED_USERDATA_END) {
            // typeName = typeMap[baseType - LBC_TYPE_TAGGED_USERDATA_BASE];
            typeName = "any /* userdata, unmapped */";
        } else {
            switch (baseType) {
            case LBC_TYPE_NIL:
                typeName = "nil";
                break;
            case LBC_TYPE_BOOLEAN:
                typeName = "boolean";
                break;
            case LBC_TYPE_NUMBER:
                typeName = "number";
                break;
            case LBC_TYPE_STRING:
                typeName = "string";
                break;
            case LBC_TYPE_TABLE:
                typeName = "table";
                break;
            case LBC_TYPE_FUNCTION:
                typeName = "function";
                break;
            case LBC_TYPE_THREAD:
                typeName = "thread";
                break;
            case LBC_TYPE_USERDATA:
                typeName = "userdata";
                break;
            case LBC_TYPE_VECTOR:
                typeName = "vector";
                break;
            case LBC_TYPE_BUFFER:
                typeName = "buffer";
                break;
            case LBC_TYPE_ANY:
                typeName = "any";
                break;
            default:
                typeName = "unknown";
                break;
            }
        }

        if (isOptional) {
            typeName += "?";
        }

        return typeName;
    }

    static std::optional<std::string> TryGetTypeName(DeserializedFunction *lpFunc, uint8_t arg) {
        if (lpFunc == nullptr || lpFunc->typeinfo.empty() || lpFunc->uTypeVersion == 1)
            return std::nullopt;

        BinaryReader reader{lpFunc->typeinfo.data(), lpFunc->typeinfo.size()};

        auto typeSize = reader.ReadVariableInteger32();
        reader.ReadVariableInteger32();
        reader.ReadVariableInteger32();

        if (typeSize == 0 || typeSize < 2 + static_cast<uint32_t>(arg) + 1)
            return std::nullopt;

        if (static_cast<size_t>(reader.GetEndPosition() - reader.GetCurrentReaderPosition()) < typeSize)
            return std::nullopt;

        auto types = reader.GetCurrentReaderPosition();
        return GetBytecodeTypeName(types[2 + arg]);
    }

    static std::string GetTypeName(DeserializedFunction *lpFunc, uint8_t arg) { return TryGetTypeName(lpFunc, arg).value_or("any"); }

    std::optional<DeserializedBytecode> Deserialize(const std::string &bytecode);
};
