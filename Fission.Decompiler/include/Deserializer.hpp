#pragma once
#include <cstdint>
#include <string>
#include <string_view>

#include "BinaryReader.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "lua.h"
#include <array>
#include <cctype>
#include <libassert/assert.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#define USERDATA_TYPE_LIMIT (LBC_TYPE_TAGGED_USERDATA_END - LBC_TYPE_TAGGED_USERDATA_BASE)

using LuauUserdataTypeNames = std::array<std::string, USERDATA_TYPE_LIMIT>;

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

struct LuauVectorConstant {
    using Float = std::array<float, 4>;
    using Double = std::array<double, 4>;

    std::variant<Float, Double> components;

    LuauVectorConstant(float x, float y, float z, float w) : components(Float{x, y, z, w}) {}
    LuauVectorConstant(double x, double y, double z, double w) : components(Double{x, y, z, w}) {}
};

struct LuauTable {
    std::vector<std::string> keys{};
    std::vector<int32_t> valueConstantIndices{};
};

// V10 class shape names a class and its declared members; NEWCLASSMEMBER provides method values.
struct LuauClassShape {
    std::string className{};
    std::vector<std::string> propertyNames{};
    std::vector<std::string> methodNames{};
};

struct LuauConstant {
    lua_Type kType{};
    std::variant<LuauTable, LuauString, LuauVectorConstant, LuauNumber, LuauBoolean, LuauProto, LuauInteger, LuauClassShape> constantData{};

    LuauConstant() : kType(LUA_TNIL) {}

    LuauConstant(const lua_Type kType) { this->kType = kType; }

    // Class shapes use LUA_TNIL because they have no runtime Lua type.
    [[nodiscard]] bool IsClassShape() const { return std::holds_alternative<LuauClassShape>(constantData); }

    template <typename T> T GetValue() const {
        static_assert(
            typeid(T) == typeid(LuauTable) || typeid(T) == typeid(LuauString) || typeid(T) == typeid(LuauVectorConstant) || typeid(T) == typeid(LuauNumber) ||
                typeid(T) == typeid(LuauBoolean) || typeid(T) == typeid(LuauProto),
            "invalid templated typename T!"
        );
        return std::get<T>(constantData);
    }
};

struct DeserializedFunction {
    std::uint8_t uTypeVersion;
    std::uint8_t uBytecodeVersion;

    std::uint32_t bytecodeId{};
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
    // Offset avoids a pointer that would dangle when the enclosing function moves.
    std::size_t abslineinfoOffset = 0;
    std::vector<LuauLocalVar> locvars{};
    std::vector<std::string> upvalueNames;
    std::shared_ptr<const LuauUserdataTypeNames> userdataTypeNames;
};

struct DeserializedBytecode {
    std::uint8_t bytecodeVersion = 0;
    std::uint8_t typesVersion = 0;
    std::vector<std::string> stringTable{};
    std::vector<DeserializedFunction> functions{};
    std::array<uint32_t, USERDATA_TYPE_LIMIT> userdataMappings{};
    DeserializedFunction *lpMainFunction;

    std::optional<std::string> ReadFromStringTable(const std::uint32_t stringId) const {
        if (stringId == 0)
            return std::nullopt;

        // Malformed string references return an empty optional.
        if (stringId - 1 >= stringTable.size())
            return std::nullopt;

        return stringTable[stringId - 1];
    }
};

class Deserializer {

  public:
    static bool IsValidTypeName(std::string_view name) {
        bool segmentStart = true;
        for (const unsigned char c : name) {
            if (c == '.') {
                if (segmentStart)
                    return false;
                segmentStart = true;
            } else if ((segmentStart && (std::isalpha(c) || c == '_')) || (!segmentStart && (std::isalnum(c) || c == '_'))) {
                segmentStart = false;
            } else {
                return false;
            }
        }
        return !segmentStart;
    }

    static std::string GetBytecodeTypeName(uint8_t typeByte, const LuauUserdataTypeNames *userdataTypeNames = nullptr) {
        uint8_t baseType = typeByte & ~LBC_TYPE_OPTIONAL_BIT;
        bool isOptional = (typeByte & LBC_TYPE_OPTIONAL_BIT) != 0;

        std::string typeName;

        if (baseType >= LBC_TYPE_TAGGED_USERDATA_BASE && baseType < LBC_TYPE_TAGGED_USERDATA_END) {
            const auto index = static_cast<size_t>(baseType - LBC_TYPE_TAGGED_USERDATA_BASE);
            if (userdataTypeNames && IsValidTypeName((*userdataTypeNames)[index]))
                typeName = (*userdataTypeNames)[index];
            else
                typeName = "any --[[ userdata, unmapped ]]";
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
                typeName = "(...any) -> ...any";
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
            typeName = (baseType == LBC_TYPE_FUNCTION ? "(" + typeName + ")?" : typeName + "?");
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
        const auto typeByte = types[2 + arg];
        const auto baseType = typeByte & ~LBC_TYPE_OPTIONAL_BIT;
        if (baseType == LBC_TYPE_TABLE || baseType == LBC_TYPE_USERDATA)
            return std::nullopt;
        if (baseType >= LBC_TYPE_TAGGED_USERDATA_BASE && baseType < LBC_TYPE_TAGGED_USERDATA_END &&
            (!lpFunc->userdataTypeNames || !IsValidTypeName((*lpFunc->userdataTypeNames)[baseType - LBC_TYPE_TAGGED_USERDATA_BASE])))
            return std::nullopt;
        return GetBytecodeTypeName(typeByte, lpFunc->userdataTypeNames.get());
    }

    std::optional<DeserializedBytecode> Deserialize(const std::string &bytecode);
};
