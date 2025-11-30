//
// Created by Pixeluted on 29/11/2025.
//
#include "BytecodeLifter.hpp"

#include "Deserializer.hpp"

#include <libassert/assert.hpp>
#include <sstream>

std::string GetLuauBuiltinName(LuauBuiltinFunction id) {
    switch (id) {
    case LBF_NONE:
        return "none";
    case LBF_ASSERT:
        return "assert";
    case LBF_MATH_ABS:
        return "math.abs";
    case LBF_MATH_ACOS:
        return "math.acos";
    case LBF_MATH_ASIN:
        return "math.asin";
    case LBF_MATH_ATAN2:
        return "math.atan2";
    case LBF_MATH_ATAN:
        return "math.atan";
    case LBF_MATH_CEIL:
        return "math.ceil";
    case LBF_MATH_COSH:
        return "math.cosh";
    case LBF_MATH_COS:
        return "math.cos";
    case LBF_MATH_DEG:
        return "math.deg";
    case LBF_MATH_EXP:
        return "math.exp";
    case LBF_MATH_FLOOR:
        return "math.floor";
    case LBF_MATH_FMOD:
        return "math.fmod";
    case LBF_MATH_FREXP:
        return "math.frexp";
    case LBF_MATH_LDEXP:
        return "math.ldexp";
    case LBF_MATH_LOG10:
        return "math.log10";
    case LBF_MATH_LOG:
        return "math.log";
    case LBF_MATH_MAX:
        return "math.max";
    case LBF_MATH_MIN:
        return "math.min";
    case LBF_MATH_MODF:
        return "math.modf";
    case LBF_MATH_POW:
        return "math.pow";
    case LBF_MATH_RAD:
        return "math.rad";
    case LBF_MATH_SINH:
        return "math.sinh";
    case LBF_MATH_SIN:
        return "math.sin";
    case LBF_MATH_SQRT:
        return "math.sqrt";
    case LBF_MATH_TANH:
        return "math.tanh";
    case LBF_MATH_TAN:
        return "math.tan";
    case LBF_BIT32_ARSHIFT:
        return "bit32.arshift";
    case LBF_BIT32_BAND:
        return "bit32.band";
    case LBF_BIT32_BNOT:
        return "bit32.bnot";
    case LBF_BIT32_BOR:
        return "bit32.bor";
    case LBF_BIT32_BXOR:
        return "bit32.bxor";
    case LBF_BIT32_BTEST:
        return "bit32.btest";
    case LBF_BIT32_EXTRACT:
        return "bit32.extract";
    case LBF_BIT32_LROTATE:
        return "bit32.lrotate";
    case LBF_BIT32_LSHIFT:
        return "bit32.lshift";
    case LBF_BIT32_REPLACE:
        return "bit32.replace";
    case LBF_BIT32_RROTATE:
        return "bit32.rrotate";
    case LBF_BIT32_RSHIFT:
        return "bit32.rshift";
    case LBF_TYPE:
        return "type";
    case LBF_STRING_BYTE:
        return "string.byte";
    case LBF_STRING_CHAR:
        return "string.char";
    case LBF_STRING_LEN:
        return "string.len";
    case LBF_TYPEOF:
        return "typeof";
    case LBF_STRING_SUB:
        return "string.sub";
    case LBF_MATH_CLAMP:
        return "math.clamp";
    case LBF_MATH_SIGN:
        return "math.sign";
    case LBF_MATH_ROUND:
        return "math.round";
    case LBF_RAWSET:
        return "rawset";
    case LBF_RAWGET:
        return "rawget";
    case LBF_RAWEQUAL:
        return "rawequal";
    case LBF_TABLE_INSERT:
        return "table.insert";
    case LBF_TABLE_UNPACK:
        return "table.unpack";
    case LBF_VECTOR:
        return "Vector3.new";
    case LBF_BIT32_COUNTLZ:
        return "bit32.countlz";
    case LBF_BIT32_COUNTRZ:
        return "bit32.countrz";
    case LBF_SELECT_VARARG:
        return "select";
    case LBF_RAWLEN:
        return "rawlen";
    case LBF_BIT32_EXTRACTK:
        return "bit32.extract"; // extractk maps to extract
    case LBF_GETMETATABLE:
        return "getmetatable";
    case LBF_SETMETATABLE:
        return "setmetatable";
    case LBF_TONUMBER:
        return "tonumber";
    case LBF_TOSTRING:
        return "tostring";
    case LBF_BIT32_BYTESWAP:
        return "bit32.byteswap";
    case LBF_BUFFER_READI8:
        return "buffer.readi8";
    case LBF_BUFFER_READU8:
        return "buffer.readu8";
    case LBF_BUFFER_WRITEU8:
        return "buffer.writeu8";
    case LBF_BUFFER_READI16:
        return "buffer.readi16";
    case LBF_BUFFER_READU16:
        return "buffer.readu16";
    case LBF_BUFFER_WRITEU16:
        return "buffer.writeu16";
    case LBF_BUFFER_READI32:
        return "buffer.readi32";
    case LBF_BUFFER_READU32:
        return "buffer.readu32";
    case LBF_BUFFER_WRITEU32:
        return "buffer.writeu32";
    case LBF_BUFFER_READF32:
        return "buffer.readf32";
    case LBF_BUFFER_WRITEF32:
        return "buffer.writef32";
    case LBF_BUFFER_READF64:
        return "buffer.readf64";
    case LBF_BUFFER_WRITEF64:
        return "buffer.writef64";
    case LBF_VECTOR_MAGNITUDE:
        return "vector.magnitude";
    case LBF_VECTOR_NORMALIZE:
        return "vector.normalize";
    case LBF_VECTOR_CROSS:
        return "vector.cross";
    case LBF_VECTOR_DOT:
        return "vector.dot";
    case LBF_VECTOR_FLOOR:
        return "vector.floor";
    case LBF_VECTOR_CEIL:
        return "vector.ceil";
    case LBF_VECTOR_ABS:
        return "vector.abs";
    case LBF_VECTOR_SIGN:
        return "vector.sign";
    case LBF_VECTOR_CLAMP:
        return "vector.clamp";
    case LBF_VECTOR_MIN:
        return "vector.min";
    case LBF_VECTOR_MAX:
        return "vector.max";
    case LBF_MATH_LERP:
        return "math.lerp";
    case LBF_VECTOR_LERP:
        return "vector.lerp";
    case LBF_MATH_ISNAN:
        return "math.isnan";
    case LBF_MATH_ISINF:
        return "math.isinf";
    case LBF_MATH_ISFINITE:
        return "math.isfinite";
    default:
        return std::format("Unknown_BFID_{}", static_cast<int>(id));
    }
}

LiftedFunction BytecodeLifter::LiftFunctionBytecodeInternal(const DeserializedFunction *function, bool bIsMain) {
    LiftedFunction liftedFunction{};

    if (function->debugName)
        liftedFunction.name = *function->debugName;
    else {
        if (bIsMain)
            liftedFunction.name = "_start";
        else
            liftedFunction.name = std::format("f{}", functionCounter++);
    }

    for (size_t currentIndex = 0; currentIndex < function->instructions.size();) {
        const auto &instruction = LuauInstruction{lpDecoder->DecodeInstruction(function->instructions.at(currentIndex).instruction)};
        const uint8_t opCode = (uint8_t)instruction.GetOpCode();

        switch (opCode) {
        case LOP_NOP:
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
            break;
        case LOP_BREAK:
            liftedFunction.instructions.emplace_back(LiftedOperation::BREAK);
            break;
        case LOP_LOADNIL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateNil;
            break;
        }
        case LOP_LOADB: {
            const auto jumpOffset = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            auto &instr = liftedFunction.instructions.emplace_back(jumpOffset != 0 ? LiftedOperation::LOADNJUMP : LiftedOperation::LOAD);
            instr.operands.resize(jumpOffset != 0 ? 3 : 2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateBool;
            instr.operands[1].value.imm.b = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            if (jumpOffset != 0) {
                instr.operands[2].type = LiftedOperandType::ImmediateInteger;
                instr.operands[2].value.imm.n = jumpOffset;
            }
            break;
        }
        case LOP_LOADN: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_LOADKX:
        case LOP_LOADK: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            if (opCode == LOP_LOADK) {
                instr.operands[1].value.imm.k = instruction.GetD();
            } else {
                instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
                liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment =
                    "INFO: padding due to the original instruction requiring an auxiliary.";
            }

            std::stringstream finalComment;
            auto &k0 = function->constants.at(instr.operands[1].value.imm.k);

            switch (k0.kType) {
            case LUA_TSTRING:
                finalComment << "INFO: Loading Constant '" << std::get<std::string>(k0.constantData) << "'.";
                break;
            case LUA_TNUMBER:
                finalComment << "INFO: Loading Constant '" << std::get<LuauNumber>(k0.constantData) << "'.";
                break;
            case LUA_TVECTOR: {
                auto vec = std::get<LuauVector>(k0.constantData);
                finalComment << "INFO: Loads Vector3 with components; x = " << vec.x << " y = " << vec.y << " z = " << vec.z << " w = " << vec.w;
                break;
            }
            case LUA_TTABLE: {
                finalComment << "INFO: Loads Table Preset.";
                break;
            }
            default: {
                finalComment << "WARNING: Obtaining a non-representable constant which has not been handled. This should never happen unless the bytecode is "
                                "absolutely toasted or the kTable is corrupted";
                break;
            }
            }

            instr.comment = finalComment.str();
            break;
        }
        case LOP_MOVE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::MOVE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_GETGLOBAL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETGLOBAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            std::stringstream finalComment;
            auto &k0 = function->constants.at(instr.operands[1].value.imm.k);

            if (k0.kType == LUA_TSTRING) {
                finalComment << "INFO: Obtaining Global " << std::get<std::string>(k0.constantData);
            } else {
                finalComment << "WARNING: Obtaining a non-representable global. This should never happen unless the bytecode is absolutely toasted or the "
                                "kTable is corrupted";
            }
            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_SETGLOBAL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETGLOBAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            std::stringstream finalComment;
            auto &k0 = function->constants.at(instr.operands[1].value.imm.k);

            if (k0.kType == LUA_TSTRING) {
                finalComment << "INFO: Setting Global " << std::get<std::string>(k0.constantData);
            } else {
                finalComment << "WARNING: Setting a non-representable global. This should never happen unless the bytecode is absolutely toasted or the kTable "
                                "is corrupted";
            }

            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_GETUPVAL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETUPVAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);

            instr.comment =
                std::format("INFO: Loading upvalue at index 0x{:X} of the function into R{}.", instr.operands[1].value.imm.u, instr.operands[0].value.reg);
            break;
        }
        case LOP_SETUPVAL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETUPVAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_GETIMPORT: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETIMPORT);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant; // destination to ktable if loaded properly
            instr.operands[1].value.imm.k = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateAux; // index chain
            instr.operands[2].value.imm.u = function->instructions.at(currentIndex + 1).instruction;

            // taken from luaV_getimport, thank you Luau, why the fuck isn't this in a fucking macro?
            int id0 = int(instr.operands[2].value.imm.u >> 20) & 1023;
            int id1 = int(instr.operands[2].value.imm.u >> 10) & 1023;
            int id2 = int(instr.operands[2].value.imm.u) & 1023;

            int indexCount = instr.operands[2].value.imm.u >> 30;
            std::stringstream finalComment;
            finalComment << "INFO: Index Chain '";
            if (indexCount == 0) {
                instr.comment = std::format("WARNING: Auxiliary corrupted? No GETIMPORT indexes found, cannot estimate path.");
                break;
            }
            if (indexCount-- > 0) {
                auto &k0 = function->constants.at(id0);
                if (k0.kType == LUA_TSTRING)
                    finalComment << std::get<std::string>(k0.constantData);
            }
            if (indexCount-- > 0) {
                auto &k1 = function->constants.at(id1);
                if (k1.kType == LUA_TSTRING)
                    finalComment << "." << std::get<std::string>(k1.constantData);
            }
            if (indexCount-- > 0) {
                auto &k2 = function->constants.at(id2);
                if (k2.kType == LUA_TSTRING)
                    finalComment << "." << std::get<std::string>(k2.constantData);
            }

            finalComment << "'";

            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_GETTABLE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLE);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_SETTABLE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLE);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_GETTABLEKS: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLEKS);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            std::stringstream finalComment;
            auto &k0 = function->constants.at(instr.operands[2].value.imm.k);

            if (k0.kType == LUA_TSTRING) {
                finalComment << "INFO: Getting Table Index (Constant with known hash) '" << std::get<std::string>(k0.constantData) << "'";
            } else {
                finalComment << "WARNING: Getting a table index which cannot be represented. This should never happen unless the bytecode is absolutely "
                                "toasted or the kTable is corrupted";
            }

            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_SETTABLEKS: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLEKS);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            std::stringstream finalComment;
            auto &k0 = function->constants.at(instr.operands[2].value.imm.k);

            if (k0.kType == LUA_TSTRING) {
                finalComment << "INFO: Setting Table Index (Constant with known hash) '" << std::get<std::string>(k0.constantData) << "'";
            } else {
                finalComment << "WARNING: The index of the table cannot be represented. This should never happen unless the bytecode is absolutely toasted or "
                                "the kTable is corrupted";
            }

            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_GETTABLEN: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLEN);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.comment = std::format("INFO: Get Table index {}", instr.operands[2].value.imm.n);
            break;
        }
        case LOP_SETTABLEN: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLEN);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.comment = std::format("INFO: Set Table at index {}", instr.operands[2].value.imm.n);
            break;
        }
        case LOP_NEWCLOSURE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::NEWCLOSURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_NAMECALL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::NAMECALL);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            std::stringstream finalComment;
            const auto &constant = function->constants.at(instr.operands[2].value.imm.k);

            if (constant.kType == LUA_TSTRING) {
                finalComment << "INFO: Setting up namecall: '" << std::get<std::string>(constant.constantData) << "'";
            } else {
                finalComment << "WARNING: The constant used in this namecall cannnot be represented.";
            }

            instr.comment = finalComment.str();
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_CALL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::CALL);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);

            bool bIsVarargCall = instr.operands[1].value.imm.n == 0;
            bool bIsVarargReturn = instr.operands[2].value.imm.n == 0;

            std::string argsText;
            if (bIsVarargCall) {
                argsText = std::format("variadic arguments from register {} up to the top of the luau stack.", instr.operands[1].value.imm.n);
            } else {
                if (instr.operands[1].value.imm.n - 1 > 1)
                    argsText = std::format(
                        "{} arguments (R{} to R{})", instr.operands[1].value.imm.n - 1, instr.operands[0].value.reg + 1,
                        instr.operands[0].value.reg + 1 + (instr.operands[1].value.imm.n - 1) // args are after the function, just like lua_call/lua_pcall in C.
                    );
                else
                    argsText = std::format(
                        "1 argument (R{})",
                        instr.operands[0].value.reg + 1 // args are after the function, just like lua_call/lua_pcall in C.
                    );
            }

            std::string retsText;
            if (bIsVarargReturn) {
                retsText = "a variadic return";
            } else {
                if (instr.operands[2].value.imm.n - 1 == 0) {
                    retsText = std::format("no arguments as a return.");
                } else {
                    if (instr.operands[2].value.imm.n - 1 == 1) {
                        retsText = std::format(
                            "1 argument as a return (R{})", instr.operands[0].value.reg // rets replace the call on stack, like lua call and pcall rets on C.
                        );
                    } else {
                        retsText = std::format(
                            "{} arguments as a return (stored from R{} to R{})", instr.operands[2].value.imm.n - 1, instr.operands[0].value.reg,
                            instr.operands[0].value.reg +
                                (instr.operands[2].value.imm.n - 1) // rets replace the call on stack, like lua call and pcall rets on C.
                        );
                    }
                }
            }

            instr.comment = std::format("INFO: Calling function at R{} with {}, expecting {}", instr.operands[0].value.reg, argsText, retsText);

            break;
        }
        case LOP_RETURN: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::RETURN);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_JUMPX:
        case LOP_JUMPBACK:
        case LOP_JUMP: {
            if ((opCode != LOP_JUMPX && instruction.GetD() == 0) || instruction.GetE() == 0) {
                auto &ins = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                ins.comment = "WARNING: Op Code simplified, ignored by interpreter (JUMPX/JUMPBACK/JUMP)";
                break;
            }

            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::JUMP);
            instr.operands.resize(1);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            if (opCode == LOP_JUMPX) {
                instr.operands[0].value.imm.n = instruction.GetE();
            } else {
                instr.operands[0].value.imm.n = instruction.GetD();
            }

            if (instr.operands[0].value.imm.n < 0) {
                instr.comment = "INFO: JUMP instruction jumps into a negative offset, likely involved in a loop!";
            }

            break;
        }
        case LOP_JUMPIF:
        case LOP_JUMPIFNOT: {
            if (instruction.GetD() == 0) {
                auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                instr.comment = "WARNING: Op Code simplified, ignored by interpreter (JUMPIF/JUMPIFNOT)";
                break;
            }
            auto &instr = liftedFunction.instructions.emplace_back(opCode == LOP_JUMPIF ? LiftedOperation::JUMPIF : LiftedOperation::JUMPIFNOT);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();

            break;
        }
        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT: {
            LiftedOperation liftedOpCode;
            switch (opCode) {
            case LOP_JUMPIFEQ:
                liftedOpCode = LiftedOperation::JUMPIFEQ;
                break;
            case LOP_JUMPIFLE:
                liftedOpCode = LiftedOperation::JUMPIFLE;
                break;
            case LOP_JUMPIFLT:
                liftedOpCode = LiftedOperation::JUMPIFLT;
                break;
            case LOP_JUMPIFNOTEQ:
                liftedOpCode = LiftedOperation::JUMPIFNOTEQ;
                break;
            case LOP_JUMPIFNOTLE:
                liftedOpCode = LiftedOperation::JUMPIFNOTLE;
                break;
            case LOP_JUMPIFNOTLT:
                liftedOpCode = LiftedOperation::JUMPIFNOTLT;
                break;

            default:
                ASSERT(false, "how?");
            }

            if (instruction.GetD() == 1) {
                auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                instr.comment = "WARNING: Op Code simplified, ignored by interpreter (JUMPIFEQ/JUMPIFLE/JUMPIFLT/JUMPIFNOTEQ/JUMPIFNOTLE/JUMPIFNOTLT)";
                liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment =
                    "INFO: padding due to the original instruction requiring an auxiliary.";
                break;
            }

            auto &instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = function->instructions.at(currentIndex + 1).instruction;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_ADD:
        case LOP_SUB:
        case LOP_MUL:
        case LOP_DIV:
        case LOP_MOD:
        case LOP_POW: {
            LiftedOperation liftedOpCode;
            switch (opCode) {
            case LOP_ADD:
                liftedOpCode = LiftedOperation::ADD;
                break;
            case LOP_SUB:
                liftedOpCode = LiftedOperation::SUB;
                break;
            case LOP_MUL:
                liftedOpCode = LiftedOperation::MUL;
                break;
            case LOP_DIV:
                liftedOpCode = LiftedOperation::DIV;
                break;
            case LOP_MOD:
                liftedOpCode = LiftedOperation::MOD;
                break;
            case LOP_POW:
                liftedOpCode = LiftedOperation::POW;
                break;

            default:
                ASSERT(false, "how?");
            }

            auto &instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_ADDK:
        case LOP_SUBK:
        case LOP_MULK:
        case LOP_DIVK:
        case LOP_MODK:
        case LOP_POWK: {
            LiftedOperation liftedOpCode;
            switch (opCode) {
            case LOP_ADDK:
                liftedOpCode = LiftedOperation::ADDK;
                break;
            case LOP_SUBK:
                liftedOpCode = LiftedOperation::SUBK;
                break;
            case LOP_MULK:
                liftedOpCode = LiftedOperation::MULK;
                break;
            case LOP_DIVK:
                liftedOpCode = LiftedOperation::DIVK;
                break;
            case LOP_MODK:
                liftedOpCode = LiftedOperation::MODK;
                break;
            case LOP_POWK:
                liftedOpCode = LiftedOperation::POWK;
                break;

            default:
                ASSERT(false, "how?");
            }

            auto &instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_AND:
        case LOP_OR: {
            auto &instr = liftedFunction.instructions.emplace_back(opCode == LOP_AND ? LiftedOperation::AND : LiftedOperation::OR);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_ANDK:
        case LOP_ORK: {
            auto &instr = liftedFunction.instructions.emplace_back(opCode == LOP_ANDK ? LiftedOperation::ANDK : LiftedOperation::ORK);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_CONCAT: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::CONCAT);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_NOT:
        case LOP_MINUS:
        case LOP_LENGTH: {
            LiftedOperation liftedOpCode;
            switch (opCode) {
            case LOP_NOT:
                liftedOpCode = LiftedOperation::NOT;
                break;
            case LOP_MINUS:
                liftedOpCode = LiftedOperation::MINUS;
                break;
            case LOP_LENGTH:
                liftedOpCode = LiftedOperation::LENGTH;
                break;

            default:
                ASSERT(false, "how?");
            }

            auto &instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_NEWTABLE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::NEWTABLE);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_DUPTABLE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::DUPTABLE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_SETLIST: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETLIST);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::ImmediateInteger;
            instr.operands[3].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_FORNPREP: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORNPREP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FORNLOOP: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORNLOOP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FORGLOOP: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGLOOP);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_FORGPREP_INEXT: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGPREP_INEXT);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FASTCALL3: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL3);
            instr.operands.resize(5);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            const auto aux = function->instructions.at(currentIndex + 1).instruction;
            instr.operands[3].type = LiftedOperandType::Register;
            instr.operands[3].value.imm.n = LUAU_INSN_AUX_A(aux);
            instr.operands[4].type = LiftedOperandType::Register;
            instr.operands[4].value.imm.n = LUAU_INSN_AUX_B(aux);

            instr.comment = std::format("INFO: Perform FastCall3 of '{}'", GetLuauBuiltinName(static_cast<LuauBuiltinFunction>(instr.operands[0].value.imm.n)));
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_FORGPREP_NEXT: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGPREP_NEXT);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_GETVARARGS: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETVARARGS);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_DUPCLOSURE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::DUPCLOSURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_PREPVARARGS: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::PREPVARARGS);
            instr.operands.resize(1);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            break;
        }
        case LOP_FASTCALL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);

            instr.comment = std::format("INFO: Perform FastCall of '{}'", GetLuauBuiltinName(static_cast<LuauBuiltinFunction>(instr.operands[0].value.imm.n)));
            break;
        }
        case LOP_COVERAGE: {
            auto &ins = liftedFunction.instructions.emplace_back(LiftedOperation::NOP); // Unlikely to be seen in the wild, but we need it
            ins.comment = "WARNING: Op Code simplified, ignored by interpreter (COVERAGE)";
            break;
        }
        case LOP_CAPTURE: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::CAPTURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_SUBRK:
        case LOP_DIVRK: {
            auto &instr = liftedFunction.instructions.emplace_back(opCode == LOP_SUBRK ? LiftedOperation::SUBRK : LiftedOperation::DIVRK);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_FASTCALL1: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL1);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);

            instr.comment = std::format("INFO: Perform FastCall1 of '{}'", GetLuauBuiltinName(static_cast<LuauBuiltinFunction>(instr.operands[0].value.imm.n)));
            break;
        }
        case LOP_FASTCALL2: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL2);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::Register;
            instr.operands[3].value.reg = LUAU_INSN_AUX_A(function->instructions.at(currentIndex + 1).instruction);

            instr.comment = std::format("INFO: Perform FastCall2 of '{}'", GetLuauBuiltinName(static_cast<LuauBuiltinFunction>(instr.operands[0].value.imm.n)));
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_FASTCALL2K: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL2K);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::ImmediateConstant;
            instr.operands[3].value.imm.k = function->instructions.at(currentIndex + 1).instruction;

            instr.comment =
                std::format("INFO: Perform FastCall2K of '{}'", GetLuauBuiltinName(static_cast<LuauBuiltinFunction>(instr.operands[0].value.imm.n)));
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }
        case LOP_FORGPREP: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGPREP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_IDIV: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::IDIV);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_IDIVK: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::IDIVK);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB: {
            if (instruction.GetD() == 1u) {
                // instruction doesn't contribute to CFlow, eliminate straight up.
                auto &ins = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                ins.comment = "WARNING: Op Code simplified, the jump target pointed to pc++ (JUMPXEQXX)";
                break;
            }
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::JUMPXEQK);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            if (opCode == LOP_JUMPXEQKB) {
                instr.operands[1].type = LiftedOperandType::ImmediateBool;
                instr.operands[1].value.imm.n = LUAU_INSN_AUX_KB(function->instructions.at(currentIndex + 1).instruction);
            } else {
                instr.operands[1].type = LiftedOperandType::ImmediateNil;
            }
            instr.operands[2].type = LiftedOperandType::ImmediateBool; // NOT flag
            instr.operands[2].value.imm.b = LUAU_INSN_AUX_NOT(function->instructions.at(currentIndex + 1).instruction) != 0;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }

        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS: {
            if (instruction.GetD() == 1u) {
                // instruction doesn't contribute to CFlow, eliminate straight up.
                auto &ins = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
                ins.comment = "WARNING: Op Code simplified, the jump target pointed to pc++ (JUMPXEQXX)";
                break;
            }
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::JUMPXEQK);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::Register; // src
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger; // jmp off
            instr.operands[1].value.imm.n = instruction.GetD();
            instr.operands[1].type = LiftedOperandType::ImmediateConstant; // ktable idx
            instr.operands[1].value.imm.k = LUAU_INSN_AUX_KV(function->instructions.at(currentIndex + 1).instruction);
            instr.operands[2].type = LiftedOperandType::ImmediateBool; // NOT flag
            instr.operands[2].value.imm.b = LUAU_INSN_AUX_NOT(function->instructions.at(currentIndex + 1).instruction) != 0;
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP).comment = "INFO: padding due to the original instruction requiring an auxiliary.";
            break;
        }

        case LOP_CLOSEUPVALS: {
            auto &ins = liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
            ins.comment = std::format(
                "WARNING: Op Code simplified, it was previously CLOSEUPVALS, but in decompilation, this doesn't really provide significant information. This "
                "opcode migrates upvalues from the function's uv list to the heap from register R{} downward.",
                instruction.GetABCOperand(LuauInstruction::LuauOperand::A)
            );
            break;
        }

        default:
            if (opCode < LOP_NOP || opCode > LOP__COUNT)
                ASSERT(false, "malformed instruction, potentially mis-sized?", (LuauOpcode)opCode);

            ASSERT(false, "unhandled opcode.", (LuauOpcode)opCode);
            break;
        }

        currentIndex += instruction.GetOpCodeSize();
    }

    for (const auto subFunction : function->subfunctions) {
        liftedFunction.subfunctions.push_back(LiftFunctionBytecodeInternal(subFunction));
    }

    return liftedFunction;
}

LiftedFunction BytecodeLifter::LiftDeserializedBytecode(const DeserializedBytecode &bytecode) {
    return LiftFunctionBytecodeInternal(bytecode.lpMainFunction, true);
}

std::string_view OperationToString(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::NOP:
        return "NOP";
    case LiftedOperation::BREAK:
        return "DEBUGBREAK";
    case LiftedOperation::LOAD:
        return "LOAD";
    case LiftedOperation::LOADNJUMP:
        return "LOAD_AND_JUMP";
    case LiftedOperation::MOVE:
        return "MOVE";
    case LiftedOperation::GETGLOBAL:
        return "GETGLOBAL";
    case LiftedOperation::SETGLOBAL:
        return "SETGLOBAL";
    case LiftedOperation::GETUPVAL:
        return "GETUPVAL";
    case LiftedOperation::SETUPVAL:
        return "SETUPVAL";
    case LiftedOperation::GETIMPORT:
        return "GETIMPORT";
    case LiftedOperation::GETTABLE:
        return "GETTABLE";
    case LiftedOperation::SETTABLE:
        return "SETTABLE";
    case LiftedOperation::GETTABLEKS:
        return "GETTABLEKS";
    case LiftedOperation::SETTABLEKS:
        return "SETTABLEKS";
    case LiftedOperation::GETTABLEN:
        return "GETTABLEN";
    case LiftedOperation::SETTABLEN:
        return "SETTABLEN";
    case LiftedOperation::NEWTABLE:
        return "NEWTABLE";
    case LiftedOperation::DUPTABLE:
        return "DUPTABLE";
    case LiftedOperation::SETLIST:
        return "SETLIST";
    case LiftedOperation::NEWCLOSURE:
        return "NEWCLOSURE";
    case LiftedOperation::DUPCLOSURE:
        return "DUPCLOSURE";
    case LiftedOperation::NAMECALL:
        return "NAMECALL";
    case LiftedOperation::CALL:
        return "CALL";
    case LiftedOperation::RETURN:
        return "RETURN";
    case LiftedOperation::GETVARARGS:
        return "GETVARARGS";
    case LiftedOperation::PREPVARARGS:
        return "PREPVARARGS";
    case LiftedOperation::CAPTURE:
        return "CAPTURE";
    case LiftedOperation::FASTCALL:
        return "FASTCALL";
    case LiftedOperation::FASTCALL1:
        return "FASTCALL1";
    case LiftedOperation::FASTCALL2:
        return "FASTCALL2";
    case LiftedOperation::FASTCALL2K:
        return "FASTCALL2K";
    case LiftedOperation::FASTCALL3:
        return "FASTCALL3";
    case LiftedOperation::JUMP:
        return "JUMP";
    case LiftedOperation::JUMPIF:
        return "JUMPIF";
    case LiftedOperation::JUMPIFNOT:
        return "JUMPIFNOT";
    case LiftedOperation::JUMPIFEQ:
        return "JUMPIFEQ";
    case LiftedOperation::JUMPIFLE:
        return "JUMPIFLE";
    case LiftedOperation::JUMPIFLT:
        return "JUMPIFLT";
    case LiftedOperation::JUMPIFNOTEQ:
        return "JUMPIFNOTEQ";
    case LiftedOperation::JUMPIFNOTLE:
        return "JUMPIFNOTLE";
    case LiftedOperation::JUMPIFNOTLT:
        return "JUMPIFNOTLT";
    case LiftedOperation::FORNPREP:
        return "FORNPREP";
    case LiftedOperation::FORNLOOP:
        return "FORNLOOP";
    case LiftedOperation::FORGLOOP:
        return "FORGLOOP";
    case LiftedOperation::FORGPREP:
        return "FORGPREP";
    case LiftedOperation::FORGPREP_INEXT:
        return "FORGPREP_INEXT";
    case LiftedOperation::FORGPREP_NEXT:
        return "FORGPREP_NEXT";
    case LiftedOperation::ADD:
        return "ADD";
    case LiftedOperation::SUB:
        return "SUB";
    case LiftedOperation::MUL:
        return "MUL";
    case LiftedOperation::DIV:
        return "DIV";
    case LiftedOperation::IDIV:
        return "IDIV";
    case LiftedOperation::MOD:
        return "MOD";
    case LiftedOperation::POW:
        return "POW";
    case LiftedOperation::ADDK:
        return "ADDK";
    case LiftedOperation::SUBK:
        return "SUBK";
    case LiftedOperation::MULK:
        return "MULK";
    case LiftedOperation::DIVK:
        return "DIVK";
    case LiftedOperation::IDIVK:
        return "IDIVK";
    case LiftedOperation::MODK:
        return "MODK";
    case LiftedOperation::POWK:
        return "POWK";
    case LiftedOperation::SUBRK:
        return "SUBRK";
    case LiftedOperation::DIVRK:
        return "DIVRK";
    case LiftedOperation::AND:
        return "AND";
    case LiftedOperation::OR:
        return "OR";
    case LiftedOperation::ANDK:
        return "ANDK";
    case LiftedOperation::ORK:
        return "ORK";
    case LiftedOperation::CONCAT:
        return "CONCAT";
    case LiftedOperation::NOT:
        return "NOT";
    case LiftedOperation::MINUS:
        return "MINUS";
    case LiftedOperation::LENGTH:
        return "LENGTH";
    case LiftedOperation::JUMPXEQK:
        return "JMPXIFKEQ";
    default:
        ASSERT(false, "unhandled operation. No string representation available, so we panic.");
        return "UNKNOWN";
    }
}