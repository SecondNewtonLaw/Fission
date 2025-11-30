//
// Created by Pixeluted on 29/11/2025.
//
#include "BytecodeLifter.hpp"

#include "Deserializer.hpp"

#include <libassert/assert.hpp>

LiftedFunction BytecodeLifter::LiftFunctionBytecodeInternal(const DeserializedFunction *function) {
    LiftedFunction liftedFunction { };

    if (function->debugName)
        liftedFunction.name = *function->debugName;
    else {
        liftedFunction.name = std::format("f{}", functionCounter++);
    }

    for (size_t currentIndex = 0; currentIndex < function->instructions.size(); currentIndex += function->instructions.at(currentIndex).GetOpCodeSize()) {
        const auto &instruction = function->instructions.at(currentIndex);
        const auto opCode = instruction.GetOpCode();

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
        case LOP_LOADK: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.n = instruction.GetD();
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
            break;
        }
        case LOP_SETGLOBAL: {
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETGLOBAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_GETUPVAL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETUPVAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_SETUPVAL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETUPVAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_GETIMPORT: {
            // TODO: im too stupid for this, do it ditto - pixeluted
            break;
        }
        case LOP_GETTABLE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLE);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLE);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLEKS);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_SETTABLEKS: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLEKS);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_GETTABLEN: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETTABLEN);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_SETTABLEN: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETTABLEN);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_NEWCLOSURE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::NEWCLOSURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_NAMECALL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::NAMECALL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_CALL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::CALL);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_RETURN: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::RETURN);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }

        default:
            break;
        }
    }

    for (const auto subFunction : function->subfunctions) {
        liftedFunction.subfunctions.push_back(LiftFunctionBytecodeInternal(subFunction));
    }

    return liftedFunction;
}

LiftedFunction BytecodeLifter::LiftDeserializedBytecode(const DeserializedBytecode &bytecode) {
    return LiftFunctionBytecodeInternal(bytecode.lpMainFunction);
}

std::string_view OperationToString(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::NOP:        return "NOP";
    case LiftedOperation::BREAK:      return "DEBUGBREAK";
    case LiftedOperation::LOAD:       return "LOAD";
    case LiftedOperation::LOADNJUMP:  return "LOAD_AND_JUMP";
    case LiftedOperation::MOVE:       return "MOVE";
    case LiftedOperation::GETGLOBAL:  return "GETGLOBAL";
    case LiftedOperation::SETGLOBAL:  return "SETGLOBAL";
    case LiftedOperation::GETUPVAL:   return "GETUPVAL";
    case LiftedOperation::SETUPVAL:   return "SETUPVAL";
    case LiftedOperation::GETIMPORT:  return "GETIMPORT";
    case LiftedOperation::GETTABLE:   return "GETTABLE";
    case LiftedOperation::SETTABLE:   return "SETTABLE";
    case LiftedOperation::GETTABLEKS: return "GETTABLEKS";
    case LiftedOperation::SETTABLEKS: return "SETTABLEKS";
    case LiftedOperation::GETTABLEN:  return "GETTABLEN";
    case LiftedOperation::SETTABLEN:  return "SETTABLEN";
    case LiftedOperation::NEWCLOSURE: return "NEWCLOSURE";
    case LiftedOperation::NAMECALL:   return "NAMECALL";
    case LiftedOperation::CALL:       return "CALL";
    case LiftedOperation::RETURN:     return "RETURN";
    default:
        ASSERT(false, "unhandled operation. No string representation available, so we panic.");
        return "UNKNOWN";
    }
}