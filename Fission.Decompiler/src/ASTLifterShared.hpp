//
// Created by Dottik on 23/9/2026.
//

#pragma once

#include "ASTLifter.hpp"
#include "SSABuilder.hpp"

#include <variant>

inline constexpr uint32_t InvalidBlockId = static_cast<uint32_t>(-1);

inline std::shared_ptr<Identifier> GlobalIdentifier(std::string name) {
    auto identifier = std::make_shared<Identifier>(std::move(name));
    identifier->bIsGlobal = true;
    return identifier;
}

inline const char *BinaryOperatorSymbol(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::ADD:
    case LiftedOperation::ADDK:
        return "+";
    case LiftedOperation::SUB:
    case LiftedOperation::SUBK:
        return "-";
    case LiftedOperation::MUL:
    case LiftedOperation::MULK:
        return "*";
    case LiftedOperation::DIV:
    case LiftedOperation::DIVK:
        return "/";
    case LiftedOperation::IDIV:
    case LiftedOperation::IDIVK:
        return "//";
    case LiftedOperation::MOD:
    case LiftedOperation::MODK:
        return "%";
    case LiftedOperation::POW:
    case LiftedOperation::POWK:
        return "^";
    case LiftedOperation::AND:
    case LiftedOperation::ANDK:
        return "and";
    case LiftedOperation::OR:
    case LiftedOperation::ORK:
        return "or";
    default:
        return nullptr;
    }
}

inline bool HasConstantRightOperand(LiftedOperation operation) {
    return operation == LiftedOperation::ADDK || operation == LiftedOperation::SUBK || operation == LiftedOperation::MULK ||
           operation == LiftedOperation::DIVK || operation == LiftedOperation::IDIVK || operation == LiftedOperation::MODK ||
           operation == LiftedOperation::POWK || operation == LiftedOperation::ANDK || operation == LiftedOperation::ORK;
}

// The register `inst` writes below one it reads, or -1: an assignment to a declared local (`x = y + 1`) has this shape, as
// does an expression result whose operands were staged above it; a call's results start at its callee slot instead.
inline int32_t AssignedLocal(const LiftedInstruction &inst) {
    if (inst.operands.size() < 2 || inst.operands[0].type != LiftedOperandType::Register || inst.operation == LiftedOperation::CALL ||
        inst.operation == LiftedOperation::CALLFB || inst.operation == LiftedOperation::NAMECALL || inst.operation == LiftedOperation::NAMECALLUDATA ||
        SSABuilder::GetRegisterAccess(inst, 0) != AccessType::Write)
        return -1;
    const int32_t target = inst.operands[0].value.reg;
    for (size_t i = 1; i < inst.operands.size(); ++i)
        if (inst.operands[i].type == LiftedOperandType::Register && inst.operands[i].value.reg > target && SSABuilder::GetRegisterAccess(inst, i) == AccessType::Read)
            return target;
    return -1;
}

inline bool IsFastCall(LiftedOperation op) {
    return op == LiftedOperation::FASTCALL || op == LiftedOperation::FASTCALL1 || op == LiftedOperation::FASTCALL2 || op == LiftedOperation::FASTCALL2K ||
           op == LiftedOperation::FASTCALL3;
}

inline std::shared_ptr<VectorNode> LiftVectorConstant(const LuauConstant &constant) {
    const auto &vector = std::get<LuauVectorConstant>(constant.constantData);
    return std::visit([](const auto &components) { return std::make_shared<VectorNode>(components); }, vector.components);
}
