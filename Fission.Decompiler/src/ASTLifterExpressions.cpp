//
// Created by Dottik on 23/9/2026.
//

#include "ASTLifter.hpp"
#include "ASTLifterShared.hpp"
#include "SafetyGuard.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

#ifndef NDEBUG
#if defined(__clang__)
#pragma clang optimize off
#endif
#endif

// Luau's builtin metadata (Compiler/src/Builtins.h is internal); the layout mirrors Luau's and resolves against Luau.Compiler.
namespace Luau::Compile {
    struct BuiltinInfo {
        int params;
        int results;
        unsigned int flags;
    };
    BuiltinInfo getBuiltinInfo(int bfid);
} // namespace Luau::Compile

std::shared_ptr<Expression> ASTLifter::ConstantLiteral(int32_t index) const {
    const auto &k = ConstantAt(index);
    switch (k.kType) {
    case LUA_TNIL:
        return std::make_shared<NilLiteralNode>();
    case LUA_TBOOLEAN:
        return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
    case LUA_TNUMBER:
        return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
    case LUA_TINTEGER:
        return std::make_shared<IntegerLiteralNode>(std::get<int64_t>(k.constantData));
    case LUA_TSTRING:
        return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
    case LUA_TVECTOR:
        return LiftVectorConstant(k);
    default:
        return std::make_shared<NilLiteralNode>();
    }
}

std::shared_ptr<Expression> ASTLifter::LiftExpression(const LiftedOperand &__operand, bool forceExpression) {
    // hostile def graphs would overflow the stack; real expressions nest shallowly
    constexpr int kMaxExpressionDepth = 64;
    if (m_expressionDepth >= kMaxExpressionDepth)
        throw Fission::DecompilerError("malformed bytecode: expression nesting too deep");
    struct DepthGuard {
        int &d;
        ~DepthGuard() { --d; }
    } depthGuard{++m_expressionDepth};

    // walk MOVE chains iteratively; deep copy fans would recurse one frame per hop
    LiftedOperand operand = __operand;
    while (true) {
        if (operand.type != LiftedOperandType::Register)
            break;
        if (const auto overridden = m_valueTermOverrides.find(SSARef{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion});
            overridden != m_valueTermOverrides.end())
            return overridden->second;
        if (m_pinnedRegisters.contains({operand.value.reg, operand.ssaVersion}))
            return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
        SSARef closureRef{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
        if (m_inlineableClosures.contains(closureRef))
            break;
        const auto *moveDef = m_currentFunction->GetDefinition(operand);
        if (!moveDef || moveDef->operation != LiftedOperation::MOVE)
            break;
        if (m_processedInstructions.contains(moveDef->instructionIndex))
            break;
        if (!forceExpression && !ShouldInline(moveDef) && !m_deferToConditionInline.contains(moveDef))
            break;
        operand = moveDef->operands[1];
    }

    if (operand.type == LiftedOperandType::Register && m_pinnedRegisters.contains({operand.value.reg, operand.ssaVersion})) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }

    // a closure parked for its single argument use renders once; erasing it keeps a second use from duplicating the literal
    if (operand.type == LiftedOperandType::Register) {
        SSARef ref{static_cast<uint8_t>(operand.value.reg), operand.ssaVersion};
        auto it = m_inlineableClosures.find(ref);
        if (it != m_inlineableClosures.end()) {
            auto closureNode = it->second;
            m_inlineableClosures.erase(it);
            return closureNode;
        }
    }

    if (operand.type == LiftedOperandType::ImmediateNil)
        return std::make_shared<NilLiteralNode>();
    if (operand.type == LiftedOperandType::ImmediateBool)
        return std::make_shared<BooleanLiteralNode>(operand.value.imm.b);
    if (operand.type == LiftedOperandType::ImmediateInteger)
        return std::make_shared<NumberLiteralNode>(operand.value.imm.n);
    if (operand.type == LiftedOperandType::ImmediateConstant)
        return ConstantLiteral(operand.value.imm.k);

    const auto *def = m_currentFunction->GetDefinition(operand);

    if (def && m_processedInstructions.contains(def->instructionIndex)) {
        // a processed def was either declared (reference its name) or consumed by an inline; a tail-duplicated
        // region lifts it again, and only pure reads may be materialized twice
        const bool consumedByInline = m_inlineConsumedDefs.contains(def->instructionIndex) &&
                                      (def->operation == LiftedOperation::LOAD || def->operation == LiftedOperation::GETIMPORT ||
                                       def->operation == LiftedOperation::GETGLOBAL || def->operation == LiftedOperation::GETUPVAL);
        if (!consumedByInline)
            return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }

    if (def && def->operation == LiftedOperation::GETVARARGS) {
        auto vararg = std::make_shared<VarArgExpression>();
        vararg->bAdjustToOne = def->operands.size() > 1 && def->operands[1].value.imm.n == 2;
        return vararg;
    }

    if (!def || (!forceExpression && !ShouldInline(def) && !m_deferToConditionInline.contains(def))) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }

    if (def->operation == LiftedOperation::LOADNJUMP && def->operands.size() >= 2 && def->operands[1].type == LiftedOperandType::ImmediateBool)
        return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
    if (def->operation == LiftedOperation::LOAD) {
        if (def->operands[1].type == LiftedOperandType::ImmediateNil)
            return std::make_shared<NilLiteralNode>();
        if (def->operands[1].type == LiftedOperandType::ImmediateBool)
            return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
        if (def->operands[1].type == LiftedOperandType::ImmediateInteger)
            return std::make_shared<NumberLiteralNode>(def->operands[1].value.imm.n);
        if (def->operands[1].type == LiftedOperandType::ImmediateConstant)
            return ConstantLiteral(def->operands[1].value.imm.k);
    }

    // left-leaning binary spines (`a+b+c+...`) are walked iteratively, then folded up
    if (BinaryOperatorSymbol(def->operation) != nullptr) {
        std::vector<std::pair<const char *, std::shared_ptr<Expression>>> rights;
        const LiftedInstruction *curDef = def;
        LiftedOperand leftLeafOperand{};
        bool leftLeafSet = false;
        while (true) {
            const char *sym = BinaryOperatorSymbol(curDef->operation);
            std::shared_ptr<Expression> rightExpr;
            if (HasConstantRightOperand(curDef->operation))
                rightExpr = ConstantLiteral(curDef->operands[2].value.imm.k);
            else
                rightExpr = LiftExpression(curDef->operands[2]);
            rights.emplace_back(sym, rightExpr);

            const auto &leftOp = curDef->operands[1];
            if (leftOp.type != LiftedOperandType::Register) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            if (m_pinnedRegisters.contains({leftOp.value.reg, leftOp.ssaVersion})) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            SSARef leftRef{static_cast<uint8_t>(leftOp.value.reg), leftOp.ssaVersion};
            if (m_inlineableClosures.contains(leftRef)) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            const auto *leftDef = m_currentFunction->GetDefinition(leftOp);
            if (!leftDef || m_processedInstructions.contains(leftDef->instructionIndex) || !ShouldInline(leftDef) || BinaryOperatorSymbol(leftDef->operation) == nullptr) {
                leftLeafOperand = leftOp;
                leftLeafSet = true;
                break;
            }
            curDef = leftDef;
        }
        std::shared_ptr<Expression> expr = leftLeafSet ? LiftExpression(leftLeafOperand) : std::make_shared<NilLiteralNode>();
        for (auto it = rights.rbegin(); it != rights.rend(); ++it)
            expr = std::make_shared<BinaryExpressionNode>(it->first, expr, it->second);
        return expr;
    }

    switch (def->operation) {

    case LiftedOperation::SUBRK:
    case LiftedOperation::DIVRK: {
        // `k - r` / `k / r`: the constant index sits in operand 1
        auto left = ConstantLiteral(def->operands[1].value.imm.k);
        auto right = LiftExpression(def->operands[2]);
        return std::make_shared<BinaryExpressionNode>(def->operation == LiftedOperation::SUBRK ? "-" : "/", left, right);
    }
    case LiftedOperation::GETTABLEN: {
        // the immediate is index - 1; a plain number literal recompiles back to GETTABLEN
        auto base = LiftExpression(def->operands[1]);
        auto index = std::make_shared<NumberLiteralNode>(static_cast<double>(def->operands[2].value.imm.n) + 1.0);
        return std::make_shared<IndexExpressionNode>(base, index);
    }

    case LiftedOperation::NOT:
        return std::make_shared<UnaryExpressionNode>("not " /* not is extra space. */, LiftExpression(def->operands[1]));
    case LiftedOperation::MINUS:
        return std::make_shared<UnaryExpressionNode>("-", LiftExpression(def->operands[1]));
    case LiftedOperation::LENGTH:
        return std::make_shared<UnaryExpressionNode>("#", LiftExpression(def->operands[1]));

    case LiftedOperation::MOVE:
        return LiftExpression(def->operands[1], forceExpression);
    case LiftedOperation::GETVARARGS:
        return std::make_shared<VarArgExpression>();

    case LiftedOperation::CONCAT: {
        int startReg = def->operands[1].value.reg;
        int endReg = def->operands[2].value.reg;
        std::vector<LiftedOperand> operands;

        bool implicitCoversAll = false;
        if (m_currentFunction->implicitUses.contains(def)) {
            const auto &vers = m_currentFunction->implicitUses.at(def);
            if (vers.size() == (size_t)(endReg - startReg + 1)) {
                implicitCoversAll = true;
                for (size_t i = 0; i < vers.size(); ++i) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = startReg + i;
                    op.ssaVersion = vers[i];
                    operands.push_back(op);
                }
            }
        }

        if (!implicitCoversAll) {
            operands.push_back(def->operands[1]);

            if (m_currentFunction->implicitUses.contains(def)) {
                const auto &vers = m_currentFunction->implicitUses.at(def);
                for (size_t i = 0; i < vers.size(); ++i) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = startReg + 1 + i;
                    op.ssaVersion = vers[i];
                    operands.push_back(op);
                }
            } else {
                for (int r = startReg + 1; r < endReg; ++r) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = r;
                    op.ssaVersion = -1;
                    operands.push_back(op);
                }
            }

            if (endReg > startReg) {
                operands.push_back(def->operands[2]);
            }
        }

        // one CONCAT over a range groups like the source it came from: `a .. (b .. c)`, `..` being right-associative
        std::vector<std::shared_ptr<Expression>> parts;
        parts.reserve(operands.size());
        for (const auto &op : operands)
            parts.push_back(LiftExpression(op));
        std::shared_ptr<Expression> expr = nullptr;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part)
            expr = expr ? std::make_shared<BinaryExpressionNode>("..", *part, expr) : *part;
        return expr ? expr : std::make_shared<StringLiteralNode>("");
    }

    case LiftedOperation::DUPTABLE:
    case LiftedOperation::NEWTABLE:
        return LiftTableLiteral(*def);

    case LiftedOperation::CALL:
    case LiftedOperation::CALLFB:
    case LiftedOperation::NAMECALL:
        return LiftCall(*def, def->instructionIndex, true);

    case LiftedOperation::GETGLOBAL: {
        const auto &k = ConstantAt(def->operands[1].value.imm.k);
        return std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(std::get<std::string>(k.constantData)));
    }
    case LiftedOperation::GETUPVAL: {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->m_currentFunction->GetUpvalueName(def->operands[1].value.imm.n)));
    }
    case LiftedOperation::GETIMPORT: {
        uint32_t importData = def->operands[2].value.imm.u;
        int count = importData >> 30;
        int id0 = int(importData >> 20) & 1023;
        int id1 = int(importData >> 10) & 1023;
        int id2 = int(importData) & 1023;

        std::vector<std::string> parts;
        auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants;
        if (count >= 1)
            parts.push_back(std::get<std::string>(constants.at(id0).constantData));
        if (count >= 2)
            parts.push_back(std::get<std::string>(constants.at(id1).constantData));
        if (count >= 3)
            parts.push_back(std::get<std::string>(constants.at(id2).constantData));

        if (parts.empty())
            return std::make_shared<NilLiteralNode>();

        std::shared_ptr<Expression> curr = std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(parts[0]));
        for (size_t i = 1; i < parts.size(); ++i)
            curr = std::make_shared<MemberExpressionNode>(curr, parts[i]);
        return curr;
    }
    case LiftedOperation::GETTABLE:
    case LiftedOperation::GETTABLEKS: {
        // member chains `a.b.c.d` are walked iteratively, one hop per GETTABLE(KS)
        struct Hop {
            bool isKeyed;
            std::shared_ptr<Expression> indexExpr;
            std::string memberName;
        };
        std::vector<Hop> hops;
        const LiftedInstruction *curDef = def;
        LiftedOperand leftLeafOp{};
        bool leftLeafSet = false;
        while (true) {
            Hop hop{};
            if (curDef->operation == LiftedOperation::GETTABLEKS) {
                hop.isKeyed = false;
                const auto &k = ConstantAt(curDef->operands[2].value.imm.k);
                hop.memberName = std::get<std::string>(k.constantData);
            } else {
                hop.isKeyed = true;
                hop.indexExpr = LiftExpression(curDef->operands[2]);
            }
            hops.push_back(std::move(hop));

            const auto &leftOp = curDef->operands[1];
            if (leftOp.type != LiftedOperandType::Register) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            if (m_pinnedRegisters.contains({leftOp.value.reg, leftOp.ssaVersion})) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            SSARef lr{static_cast<uint8_t>(leftOp.value.reg), leftOp.ssaVersion};
            if (m_inlineableClosures.contains(lr)) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            const auto *leftDef = m_currentFunction->GetDefinition(leftOp);
            if (!leftDef || m_processedInstructions.contains(leftDef->instructionIndex) || !ShouldInline(leftDef) ||
                (leftDef->operation != LiftedOperation::GETTABLE && leftDef->operation != LiftedOperation::GETTABLEKS)) {
                leftLeafOp = leftOp;
                leftLeafSet = true;
                break;
            }
            curDef = leftDef;
        }
        std::shared_ptr<Expression> expr = leftLeafSet ? LiftExpression(leftLeafOp) : std::make_shared<NilLiteralNode>();
        for (auto it = hops.rbegin(); it != hops.rend(); ++it) {
            if (it->isKeyed)
                expr = std::make_shared<IndexExpressionNode>(expr, it->indexExpr);
            else
                expr = std::make_shared<MemberExpressionNode>(expr, it->memberName);
        }
        return expr;
    }

    default:
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand, false)));
    }
}

bool ASTLifter::IsMultretCall(const LiftedInstruction &callDef, int32_t callDefIndex) const {
    if (callDef.operation == LiftedOperation::NAMECALL)
        return true; // a method call is never a fast builtin -> always multret-capable
    if (callDef.operation != LiftedOperation::CALL && callDef.operation != LiftedOperation::CALLFB)
        return true;

    // a fast builtin's FASTCALL sits a few instructions before its CALL; an earlier call in between means a general call
    const auto &instrs = m_currentFunction->lpLiftedFunction->instructions;
    constexpr int32_t kScanWindow = 32;
    const int32_t lo = std::max(0, callDefIndex - kScanWindow);
    for (int32_t j = callDefIndex - 1; j >= lo; --j) {
        switch (instrs[j].operation) {
        case LiftedOperation::FASTPCALL:
            continue; // A selects pcall/xpcall, not a Luau builtin metadata id.
        case LiftedOperation::FASTCALL:
        case LiftedOperation::FASTCALL1:
        case LiftedOperation::FASTCALL2:
        case LiftedOperation::FASTCALL2K:
        case LiftedOperation::FASTCALL3:
            if (instrs[j].operands.empty())
                return true;
            // a builtin spreads unless it declares exactly one result (math.modf returns 2, select is variadic)
            return Luau::Compile::getBuiltinInfo(instrs[j].operands[0].value.imm.n).results != 1;
        case LiftedOperation::CALL:
        case LiftedOperation::CALLFB:
        case LiftedOperation::NAMECALL:
            return true;
        default:
            break;
        }
    }
    return true;
}

std::shared_ptr<Expression> ASTLifter::LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested) {
    // a NAMECALL two slots before a CALL on the same base is the real call site
    const LiftedInstruction *curInst = &inst;
    int32_t curIdx = instructionIndex;
    while ((curInst->operation == LiftedOperation::CALL || curInst->operation == LiftedOperation::CALLFB) && curIdx >= 2) {
        if (curInst->operands.empty())
            break;
        const auto &prev = m_currentFunction->lpLiftedFunction->instructions[curIdx - 2];
        if (prev.operation != LiftedOperation::NAMECALL || prev.operands.empty() || prev.operands[0].value.reg != curInst->operands[0].value.reg)
            break;
        curInst = &prev;
        curIdx -= 2;
    }
    const LiftedInstruction &resolvedInst = *curInst;
    const int32_t resolvedIdx = curIdx;

    bool isNameCall = (resolvedInst.operation == LiftedOperation::NAMECALL);
    int32_t callInfoIndex = isNameCall ? resolvedIdx + 2 : resolvedIdx;

    if (static_cast<size_t>(callInfoIndex) >= m_currentFunction->lpLiftedFunction->instructions.size())
        return std::make_shared<NilLiteralNode>();

    const auto &callInfoInst = m_currentFunction->lpLiftedFunction->instructions[callInfoIndex];
    if (callInfoInst.operands.empty() || resolvedInst.operands.size() < (isNameCall ? 2u : 1u))
        return std::make_shared<NilLiteralNode>();
    int regFunc = callInfoInst.operands[0].value.reg;

    std::vector<std::shared_ptr<Expression>> args;
    std::shared_ptr<Expression> callee;
    bool isVararg = false;
    // a call in the final argument slot truncated to one value; rendered bare it would spread on recompile
    const LiftedInstruction *adjustArgCallDef = nullptr;

    if (isNameCall)
        callee = LiftExpression(resolvedInst.operands[1], false);
    else
        callee = LiftExpression(resolvedInst.operands[0], false);

    if (m_currentFunction->implicitUses.contains(&callInfoInst)) {
        const auto &argVersions = m_currentFunction->implicitUses.at(&callInfoInst);
        int startOffset = isNameCall ? 1 : 0;
        for (size_t k = startOffset; k < argVersions.size(); ++k) {
            LiftedOperand op;
            op.type = LiftedOperandType::Register;
            op.value.reg = regFunc + 1 + k;
            op.ssaVersion = argVersions[k];

            auto def = m_currentFunction->GetDefinition(op);
            if (def && !m_processedInstructions.contains(def->instructionIndex) &&
                (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB || def->operation == LiftedOperation::NAMECALL)) {
                int32_t actualCallIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                const auto &defInstrs = m_currentFunction->lpLiftedFunction->instructions;
                if (actualCallIdx >= 0 && static_cast<size_t>(actualCallIdx) < defInstrs.size() && defInstrs[actualCallIdx].operands.size() > 2 &&
                    defInstrs[actualCallIdx].operands[2].value.imm.n == 0) {
                    m_processedInstructions.insert(def->instructionIndex);
                    if (def->operation == LiftedOperation::NAMECALL) {
                        m_processedInstructions.insert(def->instructionIndex + 1);
                        m_processedInstructions.insert(def->instructionIndex + 2);
                    }
                    args.push_back(LiftCall(*def, def->instructionIndex, true));
                    adjustArgCallDef = nullptr; // spread tail
                    break;
                }
            }
            if (def && def->operation == LiftedOperation::GETVARARGS && !m_processedInstructions.contains(def->instructionIndex)) {
                isVararg = true;
                auto vararg = std::make_shared<VarArgExpression>();
                vararg->bAdjustToOne = def->operands.size() > 1 && def->operands[1].value.imm.n == 2 && k + 1 == argVersions.size();
                args.push_back(vararg);
                adjustArgCallDef = nullptr;
                continue;
            }
            args.push_back(LiftExpression(op, false));
            if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB || def->operation == LiftedOperation::NAMECALL)) {
                adjustArgCallDef = def;
            } else {
                adjustArgCallDef = nullptr;
            }
        }
    }

    int32_t prevIdx = resolvedIdx - 1;
    if (prevIdx >= 0) {
        const auto &prevInst = m_currentFunction->lpLiftedFunction->instructions[prevIdx];
        if ((prevInst.operation == LiftedOperation::CALL || prevInst.operation == LiftedOperation::CALLFB) && prevInst.operands.size() > 2 &&
            prevInst.operands[2].value.imm.n == 0) {
            if (!m_processedInstructions.contains(prevIdx)) {
                m_processedInstructions.insert(prevIdx);
                args.push_back(LiftCall(prevInst, prevIdx, true));
                adjustArgCallDef = nullptr; // spread tail appended after the arg loop
            }
        }
    }

    // `f(a, (g()))`: single-return fast builtins never spread, so IsMultretCall spares them the parens
    if (adjustArgCallDef && !args.empty()) {
        if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(args.back()))
            c->bAdjustToOne = true;
        else if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(args.back()))
            n->bAdjustToOne = true;
    }

    std::vector<std::shared_ptr<Expression>> rets;
    if (isNameCall) {
        auto kIdx = resolvedInst.operands[2].value.imm.k;
        std::string method = std::get<std::string>(m_currentFunction->lpLiftedFunction->lpDeserialized->constants.at(kIdx).constantData);

        return std::make_shared<NameCallExpressionNode>(
            callee, std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(method)), args, rets, isVararg, isNested
        );
    } else {
        return std::make_shared<CallExpressionNode>(callee, args, rets, isVararg, isNested);
    }
}

static bool IsLegalLuauIdentifier(const std::string &str) {
    if (str.empty() || std::isdigit(static_cast<unsigned char>(str.front())))
        return false;
    if (!std::ranges::all_of(str, [](char c) {
            auto uc = static_cast<unsigned char>(c);
            return std::isalnum(uc) || uc == '_';
        }))
        return false;
    // a reserved keyword must be bracketed as a key (`["end"] = 1`)
    static const std::unordered_set<std::string> kReserved = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",  "function", "if",   "in",
                                                              "local", "nil",   "not", "or",   "repeat", "return", "then",  "true", "until",    "while"};
    return !kReserved.contains(str);
}

static std::shared_ptr<Expression> MakeTableKey(const std::string &keyStr) {
    if (IsLegalLuauIdentifier(keyStr))
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(keyStr));
    return std::make_shared<StringLiteralNode>(keyStr);
}

std::shared_ptr<Expression> ASTLifter::LiftSetListElement(const LiftedInstruction &setList, size_t k, bool forceComputed) {
    if (!m_currentFunction->implicitUses.contains(&setList))
        return std::make_shared<NilLiteralNode>();
    const auto &versions = m_currentFunction->implicitUses.at(&setList);
    if (k >= versions.size())
        return std::make_shared<NilLiteralNode>();

    const int startReg = setList.operands[1].value.reg;
    LiftedOperand itemOp{};
    itemOp.type = LiftedOperandType::Register;
    itemOp.value.reg = startReg + static_cast<int>(k);
    itemOp.ssaVersion = versions[k];

    std::shared_ptr<Expression> expr = nullptr;
    if (auto lpDef = m_currentFunction->GetDefinition(itemOp)) {
        if (lpDef->operation == LiftedOperation::LOAD) {
            expr = LiftExpression(itemOp, true);
            this->m_processedInstructions.insert(lpDef->instructionIndex);
            this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
        } else if (lpDef->operation == LiftedOperation::MOVE) {
            expr = LiftExpression(lpDef->operands[1], false);
            this->m_processedInstructions.insert(lpDef->instructionIndex);
            this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
        } else {
            // SETLIST counts its first element twice (operand + implicit use); an otherwise unused computed element
            // is force-inlined so it cannot leak as a local declared after the constructor
            const SSARef itemRef{static_cast<uint8_t>(itemOp.value.reg), itemOp.ssaVersion};
            const int selfUses = (itemOp.value.reg == startReg) ? 2 : 1;
            auto ucIt = m_currentFunction->useCounts.find(itemRef);
            const bool singleUse = ucIt == m_currentFunction->useCounts.end() || ucIt->second <= selfUses;
            // a nested constructor's use count is inflated by its own population; ShouldInline dedups it
            const bool inlineTable = (lpDef->operation == LiftedOperation::NEWTABLE || lpDef->operation == LiftedOperation::DUPTABLE) && ShouldInline(lpDef);
            const bool canForce = forceComputed && (lpDef->operation != LiftedOperation::PHI || !m_definedRegisters.contains(itemOp.value.reg));
            if (singleUse && forceComputed && !canForce && !ShouldInline(lpDef))
                ExplainKeep(lpDef, "constructor must reuse the materialized local", &setList);
            if ((singleUse && (canForce || ShouldInline(lpDef))) || inlineTable) {
                expr = LiftExpression(itemOp, true);
                this->m_processedInstructions.insert(lpDef->instructionIndex);
                this->m_inlineConsumedDefs.insert(lpDef->instructionIndex);
                ConsumeInlinedInputs(*lpDef);
            } else {
                expr = LiftExpression(itemOp, false);
            }
        }
    }

    // an unwritten register reads nil
    return expr ? expr : std::static_pointer_cast<Expression>(std::make_shared<NilLiteralNode>());
}

bool ASTLifter::RenderClosureInPlace(const LiftedInstruction &closure, const std::shared_ptr<FunctionDeclarationNode> &function, bool anonymous) {
    const SSARef ref{static_cast<uint8_t>(closure.operands[0].value.reg), closure.operands[0].ssaVersion};
    if (const auto slots = m_closureSlots.find(ref); slots != m_closureSlots.end()) {
        function->bAnonymousInline = true;
        function->bIsLocalDeclaration = false;
        for (const auto &slot : slots->second)
            *slot = *function;
        m_closureSlots.erase(slots);
        return true;
    }
    // a constructor that inlines at its reader renders after this handler and takes the parked closure
    if (const auto *store = SoleUser(ref); store && StoreTargetsFreshTable(store)) {
        const bool list = store->operation == LiftedOperation::SETLIST;
        const auto *table = m_currentFunction->GetDefinition(store->operands[list ? 0 : 1]);
        if ((list || (store->operands[0].value.reg == ref.regIndex && store->operands[0].ssaVersion == ref.version)) && table && ShouldInline(table)) {
            function->bAnonymousInline = true;
            function->bIsLocalDeclaration = false;
            m_inlineableClosures[ref] = function;
            return true;
        }
    }
    if (!anonymous)
        return false;
    // `(function(...) ... end)(args)`: a nameless closure called where it is made
    if (const auto *call = SoleUser(ref); call && (call->operation == LiftedOperation::CALL || call->operation == LiftedOperation::CALLFB) &&
                                          call->operands[0].value.reg == ref.regIndex && call->operands[0].ssaVersion == ref.version &&
                                          BlockOf(call) == BlockOf(&closure)) {
        function->bAnonymousInline = true;
        function->bIsLocalDeclaration = false;
        m_inlineableClosures[ref] = function;
        return true;
    }
    // `t[k] = function() ... end`: a nameless closure stored right after its captures
    if (const auto *store = SoleUser(ref); store &&
                                           (store->operation == LiftedOperation::SETTABLE || store->operation == LiftedOperation::SETTABLEKS ||
                                            store->operation == LiftedOperation::SETTABLEN) &&
                                           store->operands[0].value.reg == ref.regIndex && store->operands[0].ssaVersion == ref.version &&
                                           !BeginsDebugLocal(closure, ref.regIndex)) {
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        bool adjacent = BlockOf(store) == BlockOf(&closure);
        for (auto k = static_cast<size_t>(closure.instructionIndex) + 1; adjacent && k < static_cast<size_t>(store->instructionIndex); ++k)
            adjacent = instructions[k].operation == LiftedOperation::CAPTURE || instructions[k].operation == LiftedOperation::NOP;
        if (adjacent) {
            function->bAnonymousInline = true;
            function->bIsLocalDeclaration = false;
            m_inlineableClosures[ref] = function;
            return true;
        }
    }
    // `return function() ... end`: a nameless closure whose one use is its own block's return
    const auto users = m_currentFunction->users.find(ref);
    if (users == m_currentFunction->users.end() || users->second.size() != 1 || users->second.front()->operation != LiftedOperation::RETURN ||
        BlockOf(users->second.front()) != BlockOf(&closure))
        return false;
    function->bAnonymousInline = true;
    function->bIsLocalDeclaration = false;
    m_inlineableClosures[ref] = function;
    return true;
}

std::shared_ptr<Expression> ASTLifter::LiftStoreTarget(const LiftedInstruction &store) {
    switch (store.operation) {
    case LiftedOperation::SETGLOBAL:
        return std::make_shared<IdentifierExpressionNode>(GlobalIdentifier(std::get<std::string>(ConstantAt(store.operands[1].value.imm.k).constantData)));
    case LiftedOperation::SETUPVAL:
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(m_currentFunction->GetUpvalueName(store.operands[1].value.imm.n)));
    case LiftedOperation::SETTABLEKS: {
        auto table = LiftExpression(store.operands[1]);
        return std::make_shared<MemberExpressionNode>(table, std::get<std::string>(ConstantAt(store.operands[2].value.imm.k).constantData));
    }
    case LiftedOperation::SETTABLEN: {
        // the immediate is index - 1
        auto table = LiftExpression(store.operands[1]);
        return std::make_shared<IndexExpressionNode>(table, std::make_shared<NumberLiteralNode>(static_cast<double>(store.operands[2].value.imm.n) + 1.0));
    }
    default: {
        auto table = LiftExpression(store.operands[1]);
        return std::make_shared<IndexExpressionNode>(table, LiftExpression(store.operands[2]));
    }
    }
}

std::shared_ptr<TableLiteralNode> ASTLifter::LiftTableLiteral(const LiftedInstruction &inst, std::vector<int32_t> *plan) {
    const bool dryRun = plan != nullptr;
    std::vector<std::shared_ptr<Expression>> elements;
    std::vector<int32_t> candidatesIndexes;
    int32_t tableReg = inst.operands[0].value.reg;
    // a store to the same register at another version targets a different table
    int32_t tableVersion = inst.operands[0].ssaVersion;
    constexpr size_t scanLimit = 100;
    size_t maxIdx = m_currentFunction->lpLiftedFunction->instructions.size();
    const int tableBlock = BlockOf(&inst);
    size_t blockEnd = inst.instructionIndex + 1;
    if (tableBlock >= 0 && m_currentFunction->basicBlocks[tableBlock].lpTail)
        blockEnd = (std::min)(maxIdx, static_cast<size_t>(m_currentFunction->basicBlocks[tableBlock].lpTail->instructionIndex) + 1);
    size_t scanEnd = (std::min)(maxIdx, inst.instructionIndex + scanLimit);
    bool bFoundSetList = false;
    const LiftedInstruction *deferredSetList = nullptr;
    for (size_t i = inst.instructionIndex + 1; i < scanEnd; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
        if (candidate.operation == LiftedOperation::SETLIST && candidate.operands.size() > 1 &&
            candidate.operands[0].value.reg == tableReg && candidate.operands[0].ssaVersion == tableVersion) {
            deferredSetList = &candidate;
            break;
        }
    }
    if (!deferredSetList && scanEnd < blockEnd) {
        const LiftedInstruction *distantSetList = nullptr;
        bool hasFieldStore = false;
        for (size_t i = inst.instructionIndex + 1; i < blockEnd; ++i) {
            const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
            if (candidate.operation == LiftedOperation::SETLIST && candidate.operands.size() > 2 && candidate.operands[0].value.reg == tableReg &&
                candidate.operands[0].ssaVersion == tableVersion) {
                distantSetList = &candidate;
                break;
            }
            if ((candidate.operation == LiftedOperation::SETTABLE || candidate.operation == LiftedOperation::SETTABLEKS ||
                 candidate.operation == LiftedOperation::SETTABLEN) &&
                candidate.operands.size() > 1 && candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion == tableVersion)
                hasFieldStore = true;
        }

        const auto elements = distantSetList ? SetListElements(*distantSetList) : std::vector<LiftedOperand>{};
        const bool callElements = !elements.empty() && std::ranges::all_of(elements, [&](const LiftedOperand &element) {
            const auto *definition = m_currentFunction->GetDefinition(element);
            return definition && (definition->operation == LiftedOperation::CALL || definition->operation == LiftedOperation::CALLFB ||
                                  definition->operation == LiftedOperation::NAMECALL);
        });
        if (!hasFieldStore && callElements && distantSetList->operands[2].value.imm.n == 0) {
            deferredSetList = distantSetList;
            scanEnd = static_cast<size_t>(distantSetList->instructionIndex) + 1;
        }
    }

    if (inst.operation == LiftedOperation::DUPTABLE) {
        int constantIdx = inst.operands[1].value.imm.k;
        const auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants;
        if (constantIdx < 0 || static_cast<size_t>(constantIdx) >= constants.size())
            return dryRun ? nullptr : std::make_shared<TableLiteralNode>();

        const auto &constant = constants[constantIdx];
        if (constant.kType == LUA_TTABLE) {
            const auto &tableData = constant.GetValue<LuauTable>();
            for (size_t i = 0; i < tableData.keys.size() && i < tableData.valueConstantIndices.size(); i++) {
                const auto valIdx = tableData.valueConstantIndices[i];
                const bool hasValue = valIdx >= 0 && static_cast<size_t>(valIdx) < constants.size();
                elements.push_back(std::make_shared<BinaryExpressionNode>(
                    "=", MakeTableKey(tableData.keys[i]), hasValue ? ConstantLiteral(valIdx) : std::make_shared<NilLiteralNode>()
                ));
            }
        }
        if (!elements.empty())
            return dryRun ? nullptr : std::make_shared<TableLiteralNode>(elements);
    }

    std::unordered_set<SSARef, std::hash<SSARef>> closureVisited;
    std::function<bool(const LiftedOperand &)> dependsOnClosure = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (!closureVisited.insert(key).second)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false;
        if (def->operation == LiftedOperation::NEWCLOSURE || def->operation == LiftedOperation::DUPCLOSURE)
            return true;
        const size_t firstOperand = (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) ? 0 : 1;
        for (size_t i = firstOperand; i < def->operands.size(); ++i)
            if (dependsOnClosure(def->operands[i]))
                return true;
        return false;
    };

    std::unordered_set<SSARef, std::hash<SSARef>> diamondVisited;
    std::function<bool(const LiftedOperand &)> dependsOnMaterializedDiamond = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register || !diamondVisited.insert({static_cast<uint8_t>(op.value.reg), op.ssaVersion}).second)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false;
        if (IsDiamondBoolLoad(op))
            return true;
        for (size_t oi = 1; oi < def->operands.size(); ++oi)
            if (dependsOnMaterializedDiamond(def->operands[oi]))
                return true;
        return false;
    };

    // a value reading the table under construction (`t[3] = t[1] + t[2]`) cannot move into its literal;
    // walks every def, since a separate local reading the table still cannot precede it
    std::unordered_map<SSARef, bool, std::hash<SSARef>> readsTableMemo;
    std::function<bool(const LiftedOperand &)> readsTableReg = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        if (op.value.reg == tableReg && op.ssaVersion == tableVersion)
            return true;
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (auto it = readsTableMemo.find(key); it != readsTableMemo.end())
            return it->second;
        readsTableMemo.emplace(key, false); // seed false first so a self-referential cycle terminates
        const auto *def = m_currentFunction->GetDefinition(op);
        bool result = false;
        if (def && (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE)) {
            if (const auto users = m_currentFunction->users.find(key); users != m_currentFunction->users.end()) {
                for (const auto *user : users->second) {
                    if (!user)
                        continue;
                    if (user->operation == LiftedOperation::SETLIST && user->operands.size() > 1 && user->operands[0].value.reg == key.regIndex &&
                        user->operands[0].ssaVersion == key.version) {
                        result = std::ranges::any_of(SetListElements(*user), [&](const LiftedOperand &element) { return readsTableReg(element); });
                    } else if ((user->operation == LiftedOperation::SETTABLE || user->operation == LiftedOperation::SETTABLEKS ||
                                user->operation == LiftedOperation::SETTABLEN) &&
                               user->operands.size() > 1 && user->operands[1].value.reg == key.regIndex &&
                               user->operands[1].ssaVersion == key.version) {
                        result = readsTableReg(user->operands[0]) || (user->operands.size() > 2 && readsTableReg(user->operands[2]));
                    }
                    if (result)
                        break;
                }
            }
        }
        if (def)
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (readsTableReg(def->operands[oi])) {
                    result = true;
                    break;
                }
        // a call reads its callee and arguments as implicit uses
        if (!result && def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB) &&
            m_currentFunction->implicitUses.contains(def))
            result = readsTableReg(def->operands[0]) ||
                     std::ranges::any_of(CallArguments(*def), [&](const LiftedOperand &argument) { return readsTableReg(argument); });
        readsTableMemo[key] = result;
        return result;
    };

    // a local table renders at its NEWTABLE, before values declared after it; an inlined table renders at its
    // last use, where every contributor exists
    const int32_t tableIndex = inst.instructionIndex;
    // a plan is asked while deciding whether the table inlines, so it assumes it does
    const bool tableIsLocal = !dryRun && !ShouldInline(&inst);

    auto isLaterName = [&](const LiftedInstruction *def) -> bool {
        return BlockOf(def) == tableBlock && def->instructionIndex > tableIndex;
    };

    std::unordered_set<std::string> templateKeys;
    if (const auto &constants = m_currentFunction->lpLiftedFunction->lpDeserialized->constants; inst.operation == LiftedOperation::DUPTABLE &&
                                                                                                 inst.operands[1].value.imm.k >= 0 &&
                                                                                                 static_cast<size_t>(inst.operands[1].value.imm.k) < constants.size())
        if (const auto &constant = constants[inst.operands[1].value.imm.k]; constant.kType == LUA_TTABLE) {
            const auto shape = constant.GetValue<LuauTable>();
            templateKeys.insert(shape.keys.begin(), shape.keys.end());
        }

    // a closure built for this local table renders in its constructor; the closure's handler, which runs
    // after this declaration, fills the slot. It may not capture the table or a local declared after it.
    // Only a field the source constructor held qualifies: an all-record constructor is a DUPTABLE whose template
    // names the key; a mixed one has list items, flushed before its keyed items, and reserves exactly its keyed
    // fields in NEWTABLE's hash size; `local t = {}` has neither, its sizes predicted from later stores.
    size_t keyedFields = 0;
    const int32_t hashSizeLog = inst.operation == LiftedOperation::NEWTABLE ? inst.operands[1].value.imm.n : 0;
    const size_t hashCapacity = hashSizeLog > 0 && hashSizeLog < 32 ? size_t{1} << (hashSizeLog - 1) : 0;
    // an inlined table renders at its reader, after the closure's handler, which parked the closure for it
    auto closureField = [&](const LiftedOperand &value) -> bool {
        if (value.type != LiftedOperandType::Register ||
            (!tableIsLocal && !dryRun && !m_inlineableClosures.contains({static_cast<uint8_t>(value.value.reg), value.ssaVersion})))
            return false;
        const auto *def = m_currentFunction->GetDefinition(value);
        const auto *store = SoleUser({static_cast<uint8_t>(value.value.reg), value.ssaVersion});
        if (!def || (def->operation != LiftedOperation::NEWCLOSURE && def->operation != LiftedOperation::DUPCLOSURE) || !isLaterName(def) || !store)
            return false;
        if (store->operation == LiftedOperation::SETTABLEKS) {
            if (inst.operation == LiftedOperation::DUPTABLE ? !templateKeys.contains(std::get<std::string>(ConstantAt(store->operands[2].value.imm.k).constantData))
                                                            : !bFoundSetList || keyedFields >= hashCapacity)
                return false;
        } else if (store->operation != LiftedOperation::SETLIST && (!bFoundSetList || keyedFields >= hashCapacity)) {
            return false;
        }
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        for (auto k = static_cast<size_t>(def->instructionIndex) + 1; k < instructions.size() && instructions[k].operation == LiftedOperation::CAPTURE; ++k) {
            const auto &capture = instructions[k];
            if (capture.operands.size() < 2 || capture.operands[0].value.imm.n > 1)
                continue;
            const auto &captured = capture.operands[1];
            if (captured.value.reg == tableReg && captured.ssaVersion == tableVersion)
                return false;
            if (const auto *capturedDef = m_currentFunction->GetDefinition(captured); capturedDef && isLaterName(capturedDef))
                return false;
        }
        return true;
    };
    // a local declared before this local table is named in its constructor as-is
    auto earlierLocal = [&](const LiftedOperand &value) {
        if (!tableIsLocal || value.type != LiftedOperandType::Register || IsDiamondBoolLoad(value))
            return false;
        const auto *def = m_currentFunction->GetDefinition(value);
        return def && !ShouldInline(def) && !isLaterName(def) && def->operation != LiftedOperation::NEWCLOSURE && def->operation != LiftedOperation::DUPCLOSURE;
    };
    // a field folds only when its whole value inlines at the field site; a register it reads that stays a
    // statement would be named inside the constructor before its declaration
    std::unordered_map<SSARef, bool, std::hash<SSARef>> foldableMemo;
    std::function<bool(const LiftedOperand &)> valueFoldable = [&](const LiftedOperand &valOp) -> bool {
        if (valOp.type != LiftedOperandType::Register)
            return true;
        const auto *def = m_currentFunction->GetDefinition(valOp);
        if (!def)
            return true;
        const SSARef key{static_cast<uint8_t>(valOp.value.reg), valOp.ssaVersion};
        if (auto it = foldableMemo.find(key); it != foldableMemo.end())
            return it->second;
        foldableMemo.emplace(key, false); // seed false so a self-referential cycle terminates conservatively
        if (IsDiamondBoolLoad(valOp))
            return false;
        bool ok = ShouldInline(def);
        if (ok)
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && !valueFoldable(def->operands[oi]) && !earlierLocal(def->operands[oi])) {
                    ok = false;
                    break;
                }
        foldableMemo[key] = ok;
        return ok;
    };
    auto closureSlot = [&](const LiftedOperand &value) -> std::shared_ptr<Expression> {
        if (!tableIsLocal)
            return LiftExpression(value);
        auto slot =std::make_shared<FunctionDeclarationNode>("", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, nullptr, false);
        m_closureSlots[SSARef{static_cast<uint8_t>(value.value.reg), value.ssaVersion}].push_back(slot);
        return slot;
    };

    // does `op`, rendered by LiftExpression, name a statement-level value declared after this table?
    std::unordered_map<SSARef, bool, std::hash<SSARef>> laterMemo;
    std::function<bool(const LiftedOperand &)> readsLaterName = [&](const LiftedOperand &op) -> bool {
        if (op.type != LiftedOperandType::Register)
            return false;
        const auto *def = m_currentFunction->GetDefinition(op);
        if (!def)
            return false;
        const SSARef key{static_cast<uint8_t>(op.value.reg), op.ssaVersion};
        if (auto it = laterMemo.find(key); it != laterMemo.end())
            return it->second;
        laterMemo.emplace(key, false);
        bool res;
        if (ShouldInline(def)) {
            res = false;
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && readsLaterName(def->operands[oi])) {
                    res = true;
                    break;
                }
            if (!res && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB))
                res = readsLaterName(def->operands[0]) ||
                      std::ranges::any_of(CallArguments(*def), [&](const LiftedOperand &argument) { return readsLaterName(argument); });
        } else {
            res = isLaterName(def);
        }
        laterMemo[key] = res;
        return res;
    };

    // mirrors LiftSetListElement's force-inline choice for one element
    auto elementForwardRefs = [&](int startReg, const LiftedOperand &element) -> bool {
        const int reg = element.value.reg;
        const auto *def = m_currentFunction->GetDefinition(element);
        if (!def)
            return false;
        if ((def->operation == LiftedOperation::NEWCLOSURE || def->operation == LiftedOperation::DUPCLOSURE) && isLaterName(def))
            return !closureField(element);
        if (def->operation == LiftedOperation::LOAD)
            return false;
        if (def->operation == LiftedOperation::MOVE)
            return readsLaterName(def->operands[1]);
        const int selfUses = (reg == startReg) ? 2 : 1;
        auto ucIt = m_currentFunction->useCounts.find({static_cast<uint8_t>(reg), element.ssaVersion});
        const bool singleUse = ucIt == m_currentFunction->useCounts.end() || ucIt->second <= selfUses;
        const bool inlineTable = (def->operation == LiftedOperation::NEWTABLE || def->operation == LiftedOperation::DUPTABLE) && ShouldInline(def);
        const bool canForce = def->operation != LiftedOperation::PHI || !m_definedRegisters.contains(reg);
        if (inlineTable || (singleUse && (canForce || ShouldInline(def)))) {
            for (size_t oi = 1; oi < def->operands.size(); ++oi)
                if (def->operands[oi].type == LiftedOperandType::Register && readsLaterName(def->operands[oi]))
                    return true;
            const bool call = def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::CALLFB;
            return call && !def->operands.empty() &&
                   (readsLaterName(def->operands[0]) ||
                    std::ranges::any_of(CallArguments(*def), [&](const LiftedOperand &argument) { return readsLaterName(argument); }));
        }
        return isLaterName(def);
    };

    // A store into another table is an effect (and can raise) unless that table is a nested
    // constructor built inside this fold window.
    auto storesIntoForeignTable = [&](const LiftedInstruction &candidate, size_t tableOperand) {
        if (candidate.operands.size() <= tableOperand || candidate.operands[tableOperand].value.reg == tableReg)
            return false;
        const auto *target = m_currentFunction->GetDefinition(candidate.operands[tableOperand]);
        return !target || (target->operation != LiftedOperation::NEWTABLE && target->operation != LiftedOperation::DUPTABLE) ||
               target->instructionIndex <= inst.instructionIndex;
    };

    // An effect between the table and a store may only be skipped when it is rendered inside this constructor:
    // its value reaches a store into this table, possibly through a nested constructor built after it.
    auto feedsThisTable = [&](const LiftedInstruction &value) {
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        const LiftedInstruction *current = &value;
        if (current->operation == LiftedOperation::NAMECALL || current->operation == LiftedOperation::NAMECALLUDATA)
            for (auto next = static_cast<size_t>(current->instructionIndex) + 1; next < instructions.size(); ++next)
                if (instructions[next].operation != LiftedOperation::NOP) {
                    current = &instructions[next];
                    break;
                }
        const auto isStore = [](const LiftedInstruction &candidate) {
            return candidate.operation == LiftedOperation::SETLIST || candidate.operation == LiftedOperation::SETTABLE ||
                   candidate.operation == LiftedOperation::SETTABLEKS || candidate.operation == LiftedOperation::SETTABLEN;
        };
        for (int depth = 0; depth < 32; ++depth) {
            std::vector<SSARef> refs;
            if (const auto defs = m_defsByInstruction.find(current); defs != m_defsByInstruction.end())
                refs = defs->second;
            else if (!current->operands.empty() && current->operands[0].type == LiftedOperandType::Register)
                refs.push_back({static_cast<uint8_t>(current->operands[0].value.reg), current->operands[0].ssaVersion});
            // every value it defines must go to one consumer; a table's own population stores do not count
            const LiftedInstruction *user = nullptr;
            for (const auto &ref : refs) {
                const auto users = m_currentFunction->users.find(ref);
                if (users == m_currentFunction->users.end())
                    continue;
                for (const auto *candidate : users->second) {
                    const size_t tableIndex = candidate->operation == LiftedOperation::SETLIST ? 0 : 1;
                    if (isStore(*candidate) && candidate->operands.size() > tableIndex && candidate->operands[tableIndex].value.reg == ref.regIndex &&
                        candidate->operands[tableIndex].ssaVersion == ref.version)
                        continue;
                    if (user && user != candidate)
                        return false;
                    user = candidate;
                }
            }
            if (!user || user->instructionIndex <= current->instructionIndex)
                return false;
            const bool store = isStore(*user);
            if (!store) {
                current = user;
                continue;
            }
            if (user->operands.size() < 2)
                return false;
            const auto &table = user->operands[user->operation == LiftedOperation::SETLIST ? 0 : 1];
            if (table.value.reg == tableReg && table.ssaVersion == tableVersion)
                return true;
            const auto *nested = m_currentFunction->GetDefinition(table);
            if (!nested || (nested->operation != LiftedOperation::NEWTABLE && nested->operation != LiftedOperation::DUPTABLE) ||
                nested->instructionIndex <= inst.instructionIndex)
                return false;
            current = nested;
        }
        return false;
    };

    for (size_t i = inst.instructionIndex + 1; i < scanEnd; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];
        if ((candidate.operation == LiftedOperation::SETLIST && storesIntoForeignTable(candidate, 0)) ||
            ((candidate.operation == LiftedOperation::SETTABLE || candidate.operation == LiftedOperation::SETTABLEKS ||
              candidate.operation == LiftedOperation::SETTABLEN) &&
             storesIntoForeignTable(candidate, 1)))
            break;
        if (inst.operation == LiftedOperation::DUPTABLE) {
            if (tableBlock < 0 || i > static_cast<size_t>(m_currentFunction->basicBlocks[tableBlock].lpTail->instructionIndex))
                break;
            const bool fieldStore = candidate.operation == LiftedOperation::SETTABLEKS && candidate.operands[1].value.reg == tableReg &&
                                    candidate.operands[1].ssaVersion == tableVersion;
            const bool closure = (candidate.operation == LiftedOperation::NEWCLOSURE || candidate.operation == LiftedOperation::DUPCLOSURE) &&
                                 closureField(candidate.operands[0]);
            const size_t storedOperand = candidate.operation == LiftedOperation::SETLIST ? 0 : 1;
            const bool nestedStore = (candidate.operation == LiftedOperation::SETLIST || candidate.operation == LiftedOperation::SETTABLE ||
                                      candidate.operation == LiftedOperation::SETTABLEKS || candidate.operation == LiftedOperation::SETTABLEN) &&
                                     candidate.operands.size() > storedOperand && candidate.operands[storedOperand].value.reg != tableReg;
            if (!fieldStore && !closure && !nestedStore && candidate.operation != LiftedOperation::NOP && candidate.operation != LiftedOperation::CAPTURE &&
                !IsFastCall(candidate.operation)) {
                if (!ShouldInline(&candidate))
                    break;
                bool readsTable = false;
                for (size_t operand = 1; operand < candidate.operands.size(); ++operand)
                    if (candidate.operands[operand].type == LiftedOperandType::Register && candidate.operands[operand].value.reg == tableReg &&
                        candidate.operands[operand].ssaVersion == tableVersion)
                        readsTable = true;
                if (readsTable)
                    break;
            }
        }

        if (candidate.operation == LiftedOperation::JUMP || candidate.operation == LiftedOperation::JUMPIF ||
            candidate.operation == LiftedOperation::JUMPIFNOT || candidate.operation == LiftedOperation::JUMPIFEQ ||
            candidate.operation == LiftedOperation::JUMPIFNOTEQ || candidate.operation == LiftedOperation::JUMPIFLE ||
            candidate.operation == LiftedOperation::JUMPIFNOTLE || candidate.operation == LiftedOperation::JUMPIFLT ||
            candidate.operation == LiftedOperation::JUMPIFNOTLT || candidate.operation == LiftedOperation::JUMPXEQK ||
            candidate.operation == LiftedOperation::RETURN || candidate.operation == LiftedOperation::BREAK ||
            candidate.operation == LiftedOperation::FORNPREP || candidate.operation == LiftedOperation::FORNLOOP ||
            candidate.operation == LiftedOperation::FORGLOOP || candidate.operation == LiftedOperation::FORGPREP ||
            candidate.operation == LiftedOperation::FORGPREP_INEXT || candidate.operation == LiftedOperation::FORGPREP_NEXT)
            break;

        if (candidate.operation == LiftedOperation::SETLIST) {
            if (candidate.operands[0].value.reg == tableReg && candidate.operands[0].ssaVersion != tableVersion)
                break;
            if (candidate.operands[0].value.reg == tableReg) {
                if (const auto setListElements = SetListElements(candidate); !setListElements.empty()) {
                    const int startReg = candidate.operands[1].value.reg;
                    // a local table renders before later-declared values; elements naming them, or collapsed
                    // comparisons, stay `t[i] = elem` statements after the constructor
                    if (tableIsLocal && std::ranges::any_of(setListElements, [&](const LiftedOperand &element) {
                            diamondVisited.clear();
                            return elementForwardRefs(startReg, element) || dependsOnMaterializedDiamond(element);
                        }))
                        break;

                    for (size_t k = 0; k < setListElements.size() && !dryRun; k++)
                        elements.push_back(closureField(setListElements[k]) ? closureSlot(setListElements[k]) : LiftSetListElement(candidate, k, true));

                    // a fixed-count SETLIST truncated its last element to one value: `{a, (g())}`
                    if (!dryRun && candidate.operands.size() > 2 && candidate.operands[2].value.imm.n != 0 && !elements.empty()) {
                        const auto *ldef = m_currentFunction->GetDefinition(setListElements.back());
                        if (ldef &&
                            (ldef->operation == LiftedOperation::CALL || ldef->operation == LiftedOperation::CALLFB ||
                             ldef->operation == LiftedOperation::NAMECALL) &&
                            IsMultretCall(*ldef, ldef->instructionIndex)) {
                            if (auto c = std::dynamic_pointer_cast<CallExpressionNode>(elements.back()))
                                c->bAdjustToOne = true;
                            else if (auto n = std::dynamic_pointer_cast<NameCallExpressionNode>(elements.back()))
                                n->bAdjustToOne = true;
                        }
                    }
                }
                bFoundSetList = true;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEKS) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                const bool closure = closureField(candidate.operands[0]);
                if (!closure && (readsTableReg(candidate.operands[0]) || !(valueFoldable(candidate.operands[0]) || earlierLocal(candidate.operands[0])) ||
                                 (tableIsLocal && readsLaterName(candidate.operands[0]))))
                    break;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
                ++keyedFields;
                if (dryRun)
                    continue;
                const auto &k = ConstantAt(candidate.operands[2].value.imm.k);
                elements.push_back(std::make_shared<BinaryExpressionNode>(
                    "=", MakeTableKey(std::get<std::string>(k.constantData)),
                    closure ? closureSlot(candidate.operands[0]) : LiftExpression(candidate.operands[0])
                ));
                ConsumeInlinedDefs(candidate.operands[0]);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEN) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                const bool closure = closureField(candidate.operands[0]);
                if (!closure && (readsTableReg(candidate.operands[0]) || !(valueFoldable(candidate.operands[0]) || earlierLocal(candidate.operands[0])) ||
                                 (tableIsLocal && readsLaterName(candidate.operands[0]))))
                    break;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
                ++keyedFields;
                if (dryRun)
                    continue;
                // the immediate is index - 1
                const int idx = candidate.operands[2].value.imm.n + 1;
                elements.push_back(std::make_shared<BinaryExpressionNode>(
                    "=", std::make_shared<MemberExpressionNode>(std::make_shared<NumberLiteralNode>(idx)),
                    closure ? closureSlot(candidate.operands[0]) : LiftExpression(candidate.operands[0])
                ));
                ConsumeInlinedDefs(candidate.operands[0]);
            }
        } else if (candidate.operation == LiftedOperation::SETTABLE) {
            if (candidate.operands[1].value.reg == tableReg && candidate.operands[1].ssaVersion != tableVersion)
                break;
            if (candidate.operands[1].value.reg == tableReg) {
                if (m_setListKeySnapshots.contains(&candidate))
                    break;
                const bool closure = closureField(candidate.operands[0]);
                if ((!closure && readsTableReg(candidate.operands[0])) || readsTableReg(candidate.operands[2]))
                    break;
                if (tableIsLocal && ((!closure && readsLaterName(candidate.operands[0])) || readsLaterName(candidate.operands[2])))
                    break;
                diamondVisited.clear();
                if (!closure && dependsOnMaterializedDiamond(candidate.operands[0]))
                    break;
                diamondVisited.clear();
                if (dependsOnMaterializedDiamond(candidate.operands[2]))
                    break;
                if (!closure && !valueFoldable(candidate.operands[0]) && !earlierLocal(candidate.operands[0]))
                    break;
                // a closure key is declared by its own statement after the constructor; computed keys fold fine
                closureVisited.clear();
                if (dependsOnClosure(candidate.operands[2]))
                    break;
                candidatesIndexes.emplace_back(candidate.instructionIndex);
                ++keyedFields;
                if (dryRun)
                    continue;
                auto keyExpr = LiftExpression(candidate.operands[2]);
                auto valExpr = closure ? closureSlot(candidate.operands[0]) : LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<TableBinaryExpressionNode>("=", keyExpr, valExpr));
                ConsumeInlinedDefs(candidate.operands[0]);
                ConsumeInlinedDefs(candidate.operands[2]);
            }
        } else if ((CanOperationRaise(candidate.operation) || StaysAsStatement(&candidate)) && !feedsThisTable(candidate)) {
            break;
        }
    }

    if (dryRun) {
        *plan = std::move(candidatesIndexes);
        return nullptr;
    }
    if (bFoundSetList || !elements.empty())
        for (const auto &ins : candidatesIndexes)
            m_processedInstructions.insert(ins);

    return elements.empty() ? std::make_shared<TableLiteralNode>() : std::make_shared<TableLiteralNode>(elements);
}
