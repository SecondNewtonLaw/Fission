//
// Created by Pixeluted on 29/11/2025.
//
#include "IRLifter.hpp"

#include "Deserializer.hpp"

LiftedFunction LiftFunctionBytecodeInternal(const DeserializedFunction *function) {
    LiftedFunction liftedFunction{};

    for (size_t currentIndex = 0; currentIndex < function->instructions.size(); currentIndex += function->instructions.at(currentIndex).GetOpCodeSize()) {
        const auto& instruction = function->instructions.at(currentIndex);
        const auto opCode = instruction.GetOpCode();

        switch (opCode) {
        case LOP_NOP:
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
            break;
        case LOP_BREAK:
            liftedFunction.instructions.emplace_back(LiftedOperation::BREAK);
            break;
        case LOP_LOADNIL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateNil;
            break;
        }
        case LOP_LOADB: {
            const auto jumpOffset = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            auto& instr = liftedFunction.instructions.emplace_back(jumpOffset != 0 ? LiftedOperation::LOADNJUMP : LiftedOperation::LOAD);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_LOADK: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::LOAD);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_MOVE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::MOVE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_GETGLOBAL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETGLOBAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_SETGLOBAL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETGLOBAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
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

LiftedFunction LiftDeserializedBytecode(const DeserializedBytecode &bytecode) {
    return LiftFunctionBytecodeInternal(bytecode.lpMainFunction);
}