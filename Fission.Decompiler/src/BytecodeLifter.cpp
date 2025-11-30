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
            }
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
            auto &instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETUPVAL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
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
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
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
            if (instruction.GetD() == 0) {
                liftedFunction.instructions.emplace_back(LiftedOperation::NOP);
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
            break;
        }
        case LOP_JUMPIF:
        case LOP_JUMPIFNOT: {
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

            auto& instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            instr.operands[2].type = LiftedOperandType::Register;
            instr.operands[2].value.reg = function->instructions.at(currentIndex + 1).instruction;
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

            auto& instr = liftedFunction.instructions.emplace_back(liftedOpCode);
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

            auto& instr = liftedFunction.instructions.emplace_back(liftedOpCode);
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
            auto& instr = liftedFunction.instructions.emplace_back(opCode == LOP_AND ? LiftedOperation::AND : LiftedOperation::OR);
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
            auto& instr = liftedFunction.instructions.emplace_back(opCode == LOP_ANDK ? LiftedOperation::ANDK : LiftedOperation::ORK);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::CONCAT);
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

            auto& instr = liftedFunction.instructions.emplace_back(liftedOpCode);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_NEWTABLE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::NEWTABLE);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_DUPTABLE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::DUPTABLE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_SETLIST: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::SETLIST);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::ImmediateInteger;
            instr.operands[3].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_FORNPREP: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORNPREP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FORNLOOP: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORNLOOP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FORGLOOP: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGLOOP);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_FORGPREP_INEXT: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGPREP_INEXT);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_FASTCALL3: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL3);
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
            break;
        }
        case LOP_FORGPREP_NEXT: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGREP_NEXT);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_GETVARARGS: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::GETVARARGS);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_DUPCLOSURE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::DUPCLOSURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateConstant;
            instr.operands[1].value.imm.k = instruction.GetD();
            break;
        }
        case LOP_PREPVARARGS: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::PREPVARARGS);
            instr.operands.resize(1);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            break;
        }
        case LOP_FASTCALL: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_COVERAGE: {
            liftedFunction.instructions.emplace_back(LiftedOperation::NOP); // Unlikely to be seen in the wild, but we need it
            break;
        }
        case LOP_CAPTURE: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::CAPTURE);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            break;
        }
        case LOP_SUBRK:
        case LOP_DIVRK: {
            auto& instr = liftedFunction.instructions.emplace_back(opCode == LOP_SUBRK ? LiftedOperation::SUBRK : LiftedOperation::DIVRK);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL1);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        case LOP_FASTCALL2: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL2);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::Register;
            instr.operands[3].value.reg = LUAU_INSN_AUX_A(function->instructions.at(currentIndex + 1).instruction);
            break;
        }
        case LOP_FASTCALL2K: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FASTCALL2K);
            instr.operands.resize(4);
            instr.operands[0].type = LiftedOperandType::ImmediateInteger;
            instr.operands[0].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateInteger;
            instr.operands[2].value.imm.n = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            instr.operands[3].type = LiftedOperandType::ImmediateConstant;
            instr.operands[3].value.imm.k = function->instructions.at(currentIndex + 1).instruction;
            break;
        }
        case LOP_FORGPREP: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::FORGPREP);
            instr.operands.resize(2);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::ImmediateInteger;
            instr.operands[1].value.imm.n = instruction.GetD();
            break;
        }
        case LOP_IDIV: {
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::IDIV);
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
            auto& instr = liftedFunction.instructions.emplace_back(LiftedOperation::IDIVK);
            instr.operands.resize(3);
            instr.operands[0].type = LiftedOperandType::Register;
            instr.operands[0].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::A);
            instr.operands[1].type = LiftedOperandType::Register;
            instr.operands[1].value.reg = instruction.GetABCOperand(LuauInstruction::LuauOperand::B);
            instr.operands[2].type = LiftedOperandType::ImmediateConstant;
            instr.operands[2].value.imm.k = instruction.GetABCOperand(LuauInstruction::LuauOperand::C);
            break;
        }
        // TODO: LOP_JUMPXEQKNIL, LOP_JUMPXEQKB, LOP_JUMPXEQKN, LOP_JUMPXEQKS for you ditto :heart:

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
    case LiftedOperation::NOP:            return "NOP";
    case LiftedOperation::BREAK:          return "DEBUGBREAK";
    case LiftedOperation::LOAD:           return "LOAD";
    case LiftedOperation::LOADNJUMP:      return "LOAD_AND_JUMP";
    case LiftedOperation::MOVE:           return "MOVE";
    case LiftedOperation::GETGLOBAL:      return "GETGLOBAL";
    case LiftedOperation::SETGLOBAL:      return "SETGLOBAL";
    case LiftedOperation::GETUPVAL:       return "GETUPVAL";
    case LiftedOperation::SETUPVAL:       return "SETUPVAL";
    case LiftedOperation::GETIMPORT:      return "GETIMPORT";
    case LiftedOperation::GETTABLE:       return "GETTABLE";
    case LiftedOperation::SETTABLE:       return "SETTABLE";
    case LiftedOperation::GETTABLEKS:     return "GETTABLEKS";
    case LiftedOperation::SETTABLEKS:     return "SETTABLEKS";
    case LiftedOperation::GETTABLEN:      return "GETTABLEN";
    case LiftedOperation::SETTABLEN:      return "SETTABLEN";
    case LiftedOperation::NEWTABLE:       return "NEWTABLE";
    case LiftedOperation::DUPTABLE:       return "DUPTABLE";
    case LiftedOperation::SETLIST:        return "SETLIST";
    case LiftedOperation::NEWCLOSURE:     return "NEWCLOSURE";
    case LiftedOperation::DUPCLOSURE:     return "DUPCLOSURE";
    case LiftedOperation::NAMECALL:       return "NAMECALL";
    case LiftedOperation::CALL:           return "CALL";
    case LiftedOperation::RETURN:         return "RETURN";
    case LiftedOperation::GETVARARGS:     return "GETVARARGS";
    case LiftedOperation::PREPVARARGS:    return "PREPVARARGS";
    case LiftedOperation::CAPTURE:        return "CAPTURE";
    case LiftedOperation::FASTCALL:       return "FASTCALL";
    case LiftedOperation::FASTCALL1:      return "FASTCALL1";
    case LiftedOperation::FASTCALL2:      return "FASTCALL2";
    case LiftedOperation::FASTCALL2K:     return "FASTCALL2K";
    case LiftedOperation::FASTCALL3:      return "FASTCALL3";
    case LiftedOperation::JUMP:           return "JUMP";
    case LiftedOperation::JUMPIF:         return "JUMPIF";
    case LiftedOperation::JUMPIFNOT:      return "JUMPIFNOT";
    case LiftedOperation::JUMPIFEQ:       return "JUMPIFEQ";
    case LiftedOperation::JUMPIFLE:       return "JUMPIFLE";
    case LiftedOperation::JUMPIFLT:       return "JUMPIFLT";
    case LiftedOperation::JUMPIFNOTEQ:    return "JUMPIFNOTEQ";
    case LiftedOperation::JUMPIFNOTLE:    return "JUMPIFNOTLE";
    case LiftedOperation::JUMPIFNOTLT:    return "JUMPIFNOTLT";
    case LiftedOperation::FORNPREP:       return "FORNPREP";
    case LiftedOperation::FORNLOOP:       return "FORNLOOP";
    case LiftedOperation::FORGLOOP:       return "FORGLOOP";
    case LiftedOperation::FORGPREP:       return "FORGPREP";
    case LiftedOperation::FORGPREP_INEXT: return "FORGPREP_INEXT";
    case LiftedOperation::FORGREP_NEXT:   return "FORGREP_NEXT";
    case LiftedOperation::ADD:            return "ADD";
    case LiftedOperation::SUB:            return "SUB";
    case LiftedOperation::MUL:            return "MUL";
    case LiftedOperation::DIV:            return "DIV";
    case LiftedOperation::IDIV:           return "IDIV";
    case LiftedOperation::MOD:            return "MOD";
    case LiftedOperation::POW:            return "POW";
    case LiftedOperation::ADDK:           return "ADDK";
    case LiftedOperation::SUBK:           return "SUBK";
    case LiftedOperation::MULK:           return "MULK";
    case LiftedOperation::DIVK:           return "DIVK";
    case LiftedOperation::IDIVK:          return "IDIVK";
    case LiftedOperation::MODK:           return "MODK";
    case LiftedOperation::POWK:           return "POWK";
    case LiftedOperation::SUBRK:          return "SUBRK";
    case LiftedOperation::DIVRK:          return "DIVRK";
    case LiftedOperation::AND:            return "AND";
    case LiftedOperation::OR:             return "OR";
    case LiftedOperation::ANDK:           return "ANDK";
    case LiftedOperation::ORK:            return "ORK";
    case LiftedOperation::CONCAT:         return "CONCAT";
    case LiftedOperation::NOT:            return "NOT";
    case LiftedOperation::MINUS:          return "MINUS";
    case LiftedOperation::LENGTH:         return "LENGTH";
    default:
        ASSERT(false, "unhandled operation. No string representation available, so we panic.");
        return "UNKNOWN";
    }
}
