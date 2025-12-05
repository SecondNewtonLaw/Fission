//
// Created by Pixeluted on 04/12/2025.
//
#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

#include "Deserializer.hpp"
#include <algorithm>
#include <format>
#include <queue>
#include <set>

static std::shared_ptr<BlockStatementNode> CreateBlock(const std::vector<std::shared_ptr<Statement>> &stmts) {
    auto block = std::make_shared<BlockStatementNode>();
    block->body = stmts;
    return block;
}

static int32_t FindMergeBlock(const AnalyzedFunction *func, uint32_t branchA, uint32_t branchB) {
    if (branchA == branchB)
        return (int32_t)branchA;

    std::set<uint32_t> reachableFromA;
    std::queue<uint32_t> q;

    q.push(branchA);
    reachableFromA.insert(branchA);
    while (!q.empty()) {
        uint32_t curr = q.front();
        q.pop();

        if (reachableFromA.size() > 2000u)
            break;

        const auto &block = func->basicBlocks[curr];
        for (uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (!reachableFromA.contains(succ)) {
                reachableFromA.insert(succ);
                q.push(succ);
            }
        }
    }

    std::queue<uint32_t> q2;
    q2.push(branchB);
    std::set<uint32_t> visitedB;
    visitedB.insert(branchB);

    if (reachableFromA.contains(branchB))
        return static_cast<int32_t>(branchB);

    while (!q2.empty()) {
        const uint32_t curr = q2.front();
        q2.pop();

        for (const auto &block = func->basicBlocks[curr]; uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (reachableFromA.contains(succ))
                return static_cast<int32_t>(succ); // found merge point

            if (!visitedB.contains(succ)) {
                visitedB.insert(succ);
                q2.push(succ);
            }
        }
    }

    return -1;
}

std::shared_ptr<Expression> ASTLifter::LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand) {
    if (operand.type == LiftedOperandType::Register && this->m_pinnedRegisters.contains(operand.value.reg))
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(operand)));

    const auto definitionInstruction = func->GetDefinition(operand);

    if (!definitionInstruction)
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(operand)));

    switch (definitionInstruction->operation) {
    case LiftedOperation::LOAD: {
        auto type = definitionInstruction->operands[1].type;
        if (type == LiftedOperandType::ImmediateNil) {
            return std::make_shared<NilLiteralNode>();
        } else if (type == LiftedOperandType::ImmediateBool) {
            return std::make_shared<BooleanLiteralNode>(definitionInstruction->operands[1].value.imm.b);
        } else if (type == LiftedOperandType::ImmediateInteger) {
            return std::make_shared<NumberLiteralNode>(definitionInstruction->operands[1].value.imm.n);
        } else if (type == LiftedOperandType::ImmediateConstant) {
            const auto &k = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[1].value.imm.k];
            switch (k.kType) {
            case LUA_TNIL:
                return std::make_shared<NilLiteralNode>();
            case LUA_TBOOLEAN:
                return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
            case LUA_TNUMBER:
                return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
            case LUA_TSTRING:
                return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
            default:
                return std::make_shared<NilLiteralNode>(); // fallback
            }
        }
        break;
    }
    case LiftedOperation::MOVE:
        return LiftExpression(func, definitionInstruction->operands[1]);

    // Arithmetic
    case LiftedOperation::ADD:
        return std::make_shared<BinaryExpressionNode>(
            "+", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::SUB:
        return std::make_shared<BinaryExpressionNode>(
            "-", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::MUL:
        return std::make_shared<BinaryExpressionNode>(
            "*", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::DIV:
        return std::make_shared<BinaryExpressionNode>(
            "/", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::MOD:
        return std::make_shared<BinaryExpressionNode>(
            "%", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::POW:
        return std::make_shared<BinaryExpressionNode>(
            "^", LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );

    case LiftedOperation::ADDK:
    case LiftedOperation::SUBK:
    case LiftedOperation::MULK:
    case LiftedOperation::DIVK:
    case LiftedOperation::MODK:
    case LiftedOperation::POWK: {
        std::string op = "+";
        if (definitionInstruction->operation == LiftedOperation::SUBK)
            op = "-";
        if (definitionInstruction->operation == LiftedOperation::MULK)
            op = "*";
        if (definitionInstruction->operation == LiftedOperation::DIVK)
            op = "/";
        if (definitionInstruction->operation == LiftedOperation::MODK)
            op = "%";
        if (definitionInstruction->operation == LiftedOperation::POWK)
            op = "^";

        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];

        ASSERT(constant.kType == LUA_TNUMBER, "we are cooked dev");
        std::shared_ptr<Expression> right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));

        return std::make_shared<BinaryExpressionNode>(op, left, right);
    }

    case LiftedOperation::NOT:
        return std::make_shared<UnaryExpressionNode>("not", LiftExpression(func, definitionInstruction->operands[1]));
    case LiftedOperation::MINUS:
        return std::make_shared<UnaryExpressionNode>("-", LiftExpression(func, definitionInstruction->operands[1]));
    case LiftedOperation::LENGTH:
        return std::make_shared<UnaryExpressionNode>("#", LiftExpression(func, definitionInstruction->operands[1]));

    case LiftedOperation::GETTABLE:
        return std::make_shared<IndexExpressionNode>(
            LiftExpression(func, definitionInstruction->operands[1]), LiftExpression(func, definitionInstruction->operands[2])
        );
    case LiftedOperation::GETTABLEKS: {
        auto table = LiftExpression(func, definitionInstruction->operands[1]);
        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        std::string key = (constant.kType == LUA_TSTRING) ? std::get<std::string>(constant.constantData) : "ERR";
        return std::make_shared<MemberExpressionNode>(table, key);
    }
    case LiftedOperation::GETGLOBAL: {
        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[1].value.imm.k];
        std::string name = (constant.kType == LUA_TSTRING) ? std::get<std::string>(constant.constantData) : "ERR";
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
    }
    case LiftedOperation::GETIMPORT: {
        uint32_t importData = definitionInstruction->operands[2].value.imm.u;

        int id0 = int(importData >> 20) & 1023;
        int id1 = int(importData >> 10) & 1023;
        int id2 = int(importData) & 1023;
        int count = importData >> 30;

        if (count == 0) {
            return std::make_shared<CommentNode>("Corrupted GETIMPORT (count 0)");
        }

        std::vector<std::string> parts;
        parts.reserve(3);

        auto &constants = func->lpLiftedFunction->lpDeserialized->constants;

        if (count >= 1) {
            auto &k = constants.at(id0);
            if (k.kType == LUA_TSTRING)
                parts.push_back(std::get<std::string>(k.constantData));
            else
                parts.emplace_back("ERR");
        }
        if (count >= 2) {
            auto &k = constants.at(id1);
            if (k.kType == LUA_TSTRING)
                parts.push_back(std::get<std::string>(k.constantData));
            else
                parts.emplace_back("ERR");
        }
        if (count >= 3) {
            auto &k = constants.at(id2);
            if (k.kType == LUA_TSTRING)
                parts.push_back(std::get<std::string>(k.constantData));
            else
                parts.emplace_back("ERR");
        }

        if (parts.empty())
            return std::make_shared<NilLiteralNode>();

        std::shared_ptr<Expression> currentExpr = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(parts[0]));

        for (size_t i = 1; i < parts.size(); ++i) {
            currentExpr = std::make_shared<MemberExpressionNode>(currentExpr, parts[i]);
        }

        return currentExpr;
    }
    case LiftedOperation::DUPCLOSURE: {
        const auto &duplicatedClosure = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[1].value.imm.k];
        if (duplicatedClosure.kType != LUA_TFUNCTION) {
            ASSERT(false, "dupclosure constant wasnt function"); // no way to really handle
        }

        const auto duplicatedFunction = std::get<LuauProto>(duplicatedClosure.constantData);

        // If we ever change the logic inside the bytecode lifter, this only needs to changed too, thx.
        auto functionName = duplicatedFunction->debugName ? duplicatedFunction->debugName.value() : std::format("f{}", duplicatedFunction->bytecodeId);
        auto functionArguments = duplicatedFunction->numparams;
        auto functionArgumentNames = std::unordered_map<int32_t, std::string>();
        for (int i = 0; i < functionArguments; i++) {
            functionArgumentNames[i] = std::format("v{}", i);
        }

        const AnalyzedFunction *lpFunc = nullptr;
        for (const auto &ffunc : func->innerFunctions) {
            if (ffunc.lpLiftedFunction->lpDeserialized->bytecodeId == duplicatedFunction->bytecodeId)
                lpFunc = &ffunc;
        }
        ASSERT(lpFunc != nullptr, "lpFunc == nullptr");
        std::set<uint32_t> visited;
        return std::make_shared<FunctionDeclarationNode>(
            functionName, functionArguments, functionArgumentNames, duplicatedFunction->isvararg,
            CreateBlock(this->LiftTree(const_cast<AnalyzedFunction *>(lpFunc), 0, -1, visited)), !duplicatedFunction->debugName.has_value()
        );
    }
    case LiftedOperation::NEWCLOSURE: {
        const auto proto = func->lpLiftedFunction->lpDeserialized->subfunctions[definitionInstruction->operands[1].value.imm.k];
        std::string name = proto->debugName.value_or(std::format("f{}", proto->bytecodeId));
        std::unordered_map<int32_t, std::string> args;
        for (int i = 0; i < proto->numparams; i++)
            args[i] = std::format("v{}", i);

        auto *lpFunc = &func->innerFunctions[definitionInstruction->operands[1].value.imm.k];
        std::set<uint32_t> visited;
        return std::make_shared<FunctionDeclarationNode>(
            name, proto->numparams, args, proto->isvararg, CreateBlock(this->LiftTree(const_cast<AnalyzedFunction *>(lpFunc), 0, -1, visited)),
            !proto->debugName.has_value() // NOLINT(*-pro-type-const-cast)
        );
    }

    case LiftedOperation::CALL: {
    case LiftedOperation::NAMECALL: {
        // namecall currently simply does v{} (rid), we'll just do the same.
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(operand)));
    }
    }

    default:
        break;
    }

    return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("reg_" + std::to_string(operand.value.reg)));
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftBlockInstructions(const AnalyzedFunction *func, const BasicBlock &block) {
    std::vector<std::shared_ptr<Statement>> statements;

    if (!block.lpHead || !block.lpTail)
        return statements;

    int32_t startIdx = block.lpHead->instructionIndex;
    int32_t endIdx = block.lpTail->instructionIndex;

    for (int i = startIdx; i <= endIdx; ++i) {
        const auto &inst = func->lpLiftedFunction->instructions[i];

        switch (inst.operation) {
            // control flow handled by the LiftTree structure

        case LiftedOperation::NEWTABLE: {
            auto nSize = inst.operands[2];
            if (nSize.value.imm.n == 0) {
                statements.push_back(
                    std::make_shared<VariableDeclarationNode>(
                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(inst.operands[0]))),
                        std::make_shared<TableLiteralNode>()
                    )
                );
                break;
            }

            int tableReg = inst.operands[0].value.reg;
            std::vector<std::shared_ptr<Expression>> setListWhat;

            int32_t setlistId = i;
            while (func->lpLiftedFunction->instructions[++setlistId].operation != LiftedOperation::SETLIST) {
                ASSERT(func->lpLiftedFunction->instructions.size() > setlistId);
            }

            if (func->implicitUses.contains(&func->lpLiftedFunction->instructions[setlistId])) {
                if (tableReg == func->lpLiftedFunction->instructions[setlistId].operands[0].value.imm.n) {
                    auto itemsIdx = func->lpLiftedFunction->instructions[setlistId].operands[1].value.imm.n;
                    size_t nItemsCount = inst.operands[2].value.imm.n;
                    const auto &versions = func->implicitUses.at(&func->lpLiftedFunction->instructions[setlistId]);
                    for (size_t k = 0; k < nItemsCount && k < versions.size(); ++k) {
                        LiftedOperand op;
                        op.type = LiftedOperandType::Register;
                        op.value.reg = itemsIdx + k;
                        op.ssaVersion = versions[k];
                        setListWhat.push_back(LiftExpression(func, op));
                    }
                } // different table, don't set.
            }

            statements.push_back(
                std::make_shared<VariableDeclarationNode>(
                    std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(inst.operands[0]))),
                    std::make_shared<TableLiteralNode>(setListWhat)
                )
            );
            break;
        }

        case LiftedOperation::JUMP:
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPXEQK:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::FORNPREP:
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
        case LiftedOperation::FORGPREP: {
            continue;
        }

            // stmts
        case LiftedOperation::CALL: {
            // TODO: Handle vararg properly later.
            int regFunc = inst.operands[0].value.reg;
            int32_t argCount = inst.operands[1].value.imm.n - 1;
            // int32_t retCount = inst.operands[2].value.imm.n;
            auto callee = LiftExpression(func, inst.operands[0]);

            std::vector<std::shared_ptr<Expression>> args;
            if (func->implicitUses.contains(&inst)) {
                const auto &versions = func->implicitUses.at(&inst);
                for (int32_t k = 0; k < static_cast<int32_t>(versions.size()); ++k) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = regFunc + 1 + k;
                    op.ssaVersion = versions[k];

                    if (auto def = func->GetDefinition(op); def != nullptr && def->operation == LiftedOperation::GETVARARGS)
                        break;
                    args.push_back(LiftExpression(func, op));
                }
            }

            std::vector<SSARef> refs;

            for (const auto &ret : func->definitionMap) {
                if (ret.second != func->lpLiftedFunction->instructions.data() + i)
                    continue;
                refs.emplace_back(ret.first);
            }

            std::ranges::sort(refs, [](const SSARef &a, const SSARef &b) { return a.regIndex < b.regIndex; });

            std::vector<std::shared_ptr<Expression>> rets;
            for (const auto &ref : refs)
                rets.push_back(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(ref.regIndex, ref.version))));

            statements.push_back(std::make_shared<ExpressionStatementNode>(std::make_shared<CallExpressionNode>(callee, args, rets, argCount == (int32_t)-1)));
            break;
        }
        case LiftedOperation::SETGLOBAL: {
            int kIndex = inst.operands[1].value.imm.k;
            const auto &k = func->lpLiftedFunction->lpDeserialized->constants[kIndex];
            std::string name = (k.kType == LUA_TSTRING) ? std::get<std::string>(k.constantData) : "ERR";

            auto right = LiftExpression(func, inst.operands[0]);
            if (func->GetDefinition(inst.operands[0]) == nullptr) {
                statements.push_back(
                    std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)), right)
                );
            } else {
                auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
                statements.push_back(std::make_shared<AssignmentStatementNode>(left, right));
            }
            break;
        }
        case LiftedOperation::SETTABLE: {
            auto table = LiftExpression(func, inst.operands[1]);
            auto key = LiftExpression(func, inst.operands[2]);
            auto val = LiftExpression(func, inst.operands[0]);

            auto idxExpr = std::make_shared<IndexExpressionNode>(table, key);
            statements.push_back(std::make_shared<AssignmentStatementNode>(idxExpr, val));
            break;
        }
        case LiftedOperation::SETTABLEKS: {
            int kIndex = inst.operands[2].value.imm.k;
            const auto &k = func->lpLiftedFunction->lpDeserialized->constants[kIndex];
            std::string name = (k.kType == LUA_TSTRING) ? std::get<std::string>(k.constantData) : "ERR";

            auto table = LiftExpression(func, inst.operands[1]);
            auto val = LiftExpression(func, inst.operands[0]);
            auto memberExpr = std::make_shared<MemberExpressionNode>(table, name);
            statements.push_back(std::make_shared<AssignmentStatementNode>(memberExpr, val));
            break;
        }
        case LiftedOperation::RETURN: {
            // TODO: vararg return, this pmos!
            int regStart = inst.operands[0].value.reg;
            size_t count = inst.operands[1].value.imm.n - 1;
            std::vector<std::shared_ptr<Expression>> rets;

            if (func->implicitUses.contains(&inst)) {
                const auto &versions = func->implicitUses.at(&inst);
                for (size_t k = 0; k < count && k < versions.size(); ++k) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = regStart + k;
                    op.ssaVersion = versions[k];
                    rets.push_back(LiftExpression(func, op));
                }
            }
            statements.push_back(std::make_shared<ReturnStatementNode>(rets));
            break;
        }
        case LiftedOperation::NAMECALL: {
            auto &callInsn = func->lpLiftedFunction->instructions[i + 2];
            int regFunc = callInsn.operands[0].value.reg;
            int32_t argCount = callInsn.operands[1].value.imm.n - 1;

            std::vector<std::shared_ptr<Expression>> args;
            if (func->implicitUses.contains(&callInsn)) {
                const auto &versions = func->implicitUses.at(&callInsn);
                for (int32_t k = 1; k < static_cast<int32_t>(versions.size()); ++k) { // skip arg0 (which is self).
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = regFunc + 1 + k;
                    op.ssaVersion = versions[k];

                    if (auto def = func->GetDefinition(op); def != nullptr && def->operation == LiftedOperation::GETVARARGS)
                        break;
                    args.push_back(LiftExpression(func, op));
                }
            }

            std::vector<SSARef> refs;

            for (const auto &ret : func->definitionMap) {
                if (ret.second != func->lpLiftedFunction->instructions.data() + i)
                    continue;
                refs.emplace_back(ret.first);
            }

            std::ranges::sort(refs, [](const SSARef &a, const SSARef &b) { return a.regIndex < b.regIndex; });

            std::vector<std::shared_ptr<Expression>> rets;
            for (const auto &ref : refs)
                rets.push_back(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(this->GetVarName(ref.regIndex, ref.version))));

            auto kIdx = func->lpLiftedFunction->lpDeserialized->constants.at(func->lpLiftedFunction->instructions[i].operands[2].value.imm.k);
            ASSERT(kIdx.kType == LUA_TSTRING, "ktt != LUA_TSTRING (NAMECALL)");
            auto fakeOp = LiftedOperand{};
            fakeOp.type = LiftedOperandType::Register;
            fakeOp.value.reg = func->lpLiftedFunction->instructions[i].operands[1].value.reg;
            fakeOp.ssaVersion = func->lpLiftedFunction->instructions[i].operands[1].ssaVersion;
            statements.push_back(
                std::make_shared<ExpressionStatementNode>(std::make_shared<NameCallExpressionNode>(
                    LiftExpression(func, fakeOp),
                    std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::get<std::string>(kIdx.constantData))), args, rets,
                    argCount == (int32_t)-1
                ))
            );
            i += 2; // skip NOP (aux) and CALL
            break;
        }
        case LiftedOperation::LOAD:
        case LiftedOperation::NOP:
        case LiftedOperation::GETVARARGS:
        case LiftedOperation::MOVE: { // register moves are handled at the SSA level, we are not to be concerned with moving the registers.
            break;
        }
        case LiftedOperation::GETIMPORT:
        case LiftedOperation::GETGLOBAL: {
            int32_t reg = inst.operands[0].value.reg;
            int32_t ver = inst.operands[0].ssaVersion;
            int uses = 0;
            if (func->useCounts.contains({reg, ver})) {
                uses = func->useCounts.at({reg, ver});
            }

            if (uses == 0 && !m_pinnedRegisters.contains(reg)) {
                auto expression = LiftExpression(func, inst.operands[0]);

                auto targetIdentifier = std::make_shared<Identifier>("_");
                auto targetExpression = std::make_shared<IdentifierExpressionNode>(targetIdentifier);

                statements.push_back(std::make_shared<VariableDeclarationNode>(targetExpression, expression));
                break; // emit an unused variable denoted by _
            }

            if (uses > 0 /* if we have more than 0 refs, then we don't care. We inline these */ && !m_pinnedRegisters.contains(reg))
                break;
        }
        default:
            if (nullptr == func->GetDefinition(inst.operands[0])) {
                // auto targetIdentifier = std::make_shared<Identifier>(std::format("v{}", inst.operands[0].value.reg));
                // statements.push_back(std::make_shared<VariableDeclarationNode>(targetIdentifier));
            } else {
                auto expression = LiftExpression(func, inst.operands[0]);

                auto targetIdentifier = std::make_shared<Identifier>(this->GetVarName(inst.operands[0]));
                auto targetExpression = std::make_shared<IdentifierExpressionNode>(targetIdentifier);

                statements.push_back(std::make_shared<AssignmentStatementNode>(targetExpression, expression));
            }
            break;
        }
    }
    return statements;
}

std::vector<std::shared_ptr<Statement>>
ASTLifter::LiftTree(AnalyzedFunction *func, uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited) {
    std::vector<std::shared_ptr<Statement>> nodes;

    if (currentBlockId == static_cast<uint32_t>(-1) || currentBlockId >= func->basicBlocks.size())
        return nodes;
    if (currentBlockId == stopBlockId)
        return nodes;

    if (visited.contains(currentBlockId))
        return nodes;
    visited.insert(currentBlockId);

    const auto &block = func->basicBlocks[currentBlockId];

    auto blockStatements = LiftBlockInstructions(func, block);
    nodes.insert(nodes.end(), blockStatements.begin(), blockStatements.end());

    switch (block.bType) {
    case BlockType::IfHeader: {
        if (block.ifStatementTrue.has_value() && block.ifStatementFalse.has_value()) {
            uint32_t trueIdx = block.ifStatementTrue.value();
            uint32_t falseIdx = block.ifStatementFalse.value();

            std::shared_ptr<Expression> condition = nullptr;
            if (block.lpTail) {
                switch (block.lpTail->operation) {
                case LiftedOperation::JUMPXEQK: {
                    auto kIdx = block.lpTail->operands[2].value.imm.k;
                    auto notFlag = block.lpTail->operands[3].value.imm.b;
                    std::shared_ptr<Expression> rhs;
                    const auto &k = func->lpLiftedFunction->lpDeserialized->constants[kIdx];
                    switch (k.kType) {
                    case LUA_TNIL:
                        rhs = std::make_shared<NilLiteralNode>();
                        break;
                    case LUA_TBOOLEAN:
                        rhs = std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
                        break;
                    case LUA_TNUMBER:
                        rhs = std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
                        break;
                    case LUA_TSTRING:
                        rhs = std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
                        break;
                    default:
                        rhs = std::make_shared<NilLiteralNode>(); // fallback
                        break;
                    }

                    if (notFlag)
                        condition = std::make_shared<BinaryExpressionNode>("==", this->LiftExpression(func, block.lpTail->operands[0]), rhs);
                    else
                        condition = std::make_shared<BinaryExpressionNode>("~=", this->LiftExpression(func, block.lpTail->operands[0]), rhs);
                    break;
                }
                case LiftedOperation::JUMPIFNOTEQ: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        "==", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                    );
                    break;
                }
                case LiftedOperation::JUMPIFEQ: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        "~=", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                    );
                    break;
                }
                case LiftedOperation::JUMPIFLE: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        "<=", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                    );
                    break;
                }
                case LiftedOperation::JUMPIFNOTLE: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        ">=", this->LiftExpression(func, block.lpTail->operands[2]), this->LiftExpression(func, block.lpTail->operands[0])
                    );
                    break;
                }
                case LiftedOperation::JUMPIFLT: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        "<", this->LiftExpression(func, block.lpTail->operands[2]), this->LiftExpression(func, block.lpTail->operands[0])
                    );
                    break;
                }
                case LiftedOperation::JUMPIFNOTLT: {
                    condition = std::make_shared<BinaryExpressionNode>(
                        ">", this->LiftExpression(func, block.lpTail->operands[2]), this->LiftExpression(func, block.lpTail->operands[0])
                    );
                    break;
                }
                default:
                    condition = LiftExpression(func, block.lpTail->operands[0]);
                    break;
                }
            }

            ASSERT(condition, "failed to determine conditional for if branch.");

            const uint32_t mergeIdx = FindMergeBlock(func, trueIdx, falseIdx);

            std::set<uint32_t> branchVisited = visited;
            auto thenStmts = LiftTree(func, trueIdx, mergeIdx, branchVisited);

            branchVisited = visited;
            auto elseStmts = LiftTree(func, falseIdx, mergeIdx, branchVisited);

            auto ifNode = std::make_shared<IfStatementNode>();
            ifNode->condition = condition;
            if (!thenStmts.empty()) {
                ifNode->thenBranch = CreateBlock(thenStmts);
            }
            if (!elseStmts.empty()) {
                ifNode->elseBranch = CreateBlock(elseStmts);
            }
            nodes.push_back(ifNode);

            // continue from merge point
            if (mergeIdx != static_cast<uint32_t>(-1) && mergeIdx != stopBlockId) {
                auto nextNodes = LiftTree(func, mergeIdx, stopBlockId, visited);
                nodes.insert(nodes.end(), nextNodes.begin(), nextNodes.end());
            }
        } else {
            nodes.push_back(std::make_shared<CommentNode>("Broken If Header"));
        }
        break;
    }

    case BlockType::LoopHeader: {
        if (block.loopLatch.has_value()) {
            uint32_t exitIdx = block.loopExit.value();

            if ((block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop) {
                if (exitIdx == static_cast<uint32_t>(-1))
                    exitIdx = block.loopLatch.value(
                    ); // in while loops, there may be no exit. Thus, if there's none, set it to the latch. This should get things going.

                // assume the first non-exit successor is the loop body start
                int32_t bodyIdx = -1;
                for (auto succ : block.successors) {
                    if (succ != exitIdx) {
                        bodyIdx = succ;
                        break;
                    }
                }
                auto whileNode = std::make_shared<WhileStatementNode>();

                // try to extract condition
                if (block.bTerminator == BlockTerminator::Conditional && block.lpTail) {
                    switch (block.lpTail->operation) {
                    case LiftedOperation::JUMPIFNOTEQ: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            "==", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIFEQ: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            "~=", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIFLT: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            "<", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIFNOTLT: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            ">", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIFLE: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            "<=", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIFNOTLE: {
                        whileNode->condition = std::make_shared<BinaryExpressionNode>(
                            ">=", this->LiftExpression(func, block.lpTail->operands[0]), this->LiftExpression(func, block.lpTail->operands[2])
                        );
                        break;
                    }
                    case LiftedOperation::JUMPIF: {
                    case LiftedOperation::JUMPIFNOT: {
                        whileNode->condition = this->LiftExpression(func, block.lpTail->operands[0]);
                        break;
                    }
                    }
                    case LiftedOperation::JUMPXEQK: {
                        auto kIdx = block.lpTail->operands[2].value.imm.k;
                        auto notFlag = block.lpTail->operands[3].value.imm.b;
                        std::shared_ptr<Expression> rhs;
                        const auto &k = func->lpLiftedFunction->lpDeserialized->constants[kIdx];
                        switch (k.kType) {
                        case LUA_TNIL:
                            rhs = std::make_shared<NilLiteralNode>();
                            break;
                        case LUA_TBOOLEAN:
                            rhs = std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
                            break;
                        case LUA_TNUMBER:
                            rhs = std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
                            break;
                        case LUA_TSTRING:
                            rhs = std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
                            break;
                        default:
                            rhs = std::make_shared<NilLiteralNode>(); // fallback
                            break;
                        }

                        if (notFlag)
                            whileNode->condition = std::make_shared<BinaryExpressionNode>("==", this->LiftExpression(func, block.lpTail->operands[0]), rhs);
                        else
                            whileNode->condition = std::make_shared<BinaryExpressionNode>("~=", this->LiftExpression(func, block.lpTail->operands[0]), rhs);
                        break;
                    }

                    default:
                        whileNode->condition = LiftExpression(func, block.lpTail->operands[0]);
                        break;
                    }
                } else {
                    whileNode->condition = std::make_shared<BooleanLiteralNode>(true);
                }

                ASSERT(bodyIdx != -1, "control flow analysis failed");
                std::set<uint32_t> loopVisited = visited;
                auto loopBody = LiftTree(func, bodyIdx, exitIdx, loopVisited);
                whileNode->body = CreateBlock(loopBody);

                nodes.push_back(whileNode);

                // continue after loop
                if (exitIdx != stopBlockId) {
                    auto nextNodes = LiftTree(func, exitIdx, stopBlockId, visited);
                    nodes.insert(nodes.end(), nextNodes.begin(), nextNodes.end());
                }
            } else if ((block.dwBlockFlags & LoopBlockFlags::ForNumericLoop) == LoopBlockFlags::ForNumericLoop) {
                auto numericForNode = std::make_shared<ForNumericNode>();
                // the loop header for this can be at most
                auto baseRegister = block.lpTail->operands[0].value.reg;
                // control vars.
                auto dwStartValueReg = baseRegister + 2;
                auto increaseBy = baseRegister + 1;
                auto increaseUntilReg = baseRegister;

                uint32_t bodyIdx = -1;
                for (const auto succ : block.successors)
                    if (succ != block.loopLatch) {
                        bodyIdx = succ;
                        break; // FORNLOOP
                    }
                ASSERT(bodyIdx != -1, "bodyIdx == -1");
                std::set<uint32_t> loopVisited = visited;
                LiftedOperand op;
                op.type = LiftedOperandType::Register;
                {
                    PinnedRegisterScope pinScope(this, dwStartValueReg);

                    this->EnterLoop(dwStartValueReg);

                    auto loopBody = LiftTree(func, bodyIdx, *block.loopLatch, loopVisited);
                    numericForNode->lpLoopBody = CreateBlock(loopBody);

                    op.value.reg = increaseBy;
                    op.ssaVersion = block.lpTail->operands[0].ssaVersion;
                    numericForNode->increaseBy = LiftExpression(func, op);
                    op.value.reg = increaseUntilReg;
                    numericForNode->maxIncreased = LiftExpression(func, op);
                    op.value.reg = dwStartValueReg;
                    op.ssaVersion = block.lpTail->operands[0].ssaVersion + 1;
                    numericForNode->loopVariable = LiftExpression(func, op);

                    this->ExitLoop();
                }

                op.value.reg = dwStartValueReg;
                op.ssaVersion = block.lpTail->operands[0].ssaVersion;
                numericForNode->startVariable = LiftExpression(func, op);

                nodes.push_back(numericForNode);

                // continue after loop
                if (exitIdx != stopBlockId) {
                    auto nextNodes = LiftTree(func, exitIdx, stopBlockId, visited);
                    nodes.insert(nodes.end(), nextNodes.begin(), nextNodes.end());
                }
            }
        }
        break;
    }

    case BlockType::Break:
        nodes.push_back(std::make_shared<BreakStatementNode>());
        break;

    case BlockType::Continue:
        nodes.push_back(std::make_shared<ContinueStatementNode>());
        break;

    case BlockType::Return:
        break;

    case BlockType::Standard:
    case BlockType::LoopLatch:
    default: {
        if (!block.successors.empty()) {
            const uint32_t next = block.successors[0];

            if (func->basicBlocks[next].bType == BlockType::LoopHeader && block.bType == BlockType::LoopLatch)
                break;

            auto nextNodes = LiftTree(func, next, stopBlockId, visited);
            nodes.insert(nodes.end(), nextNodes.begin(), nextNodes.end());
        }
        break;
    }
    }

    return nodes;
}

ASTFunction ASTLifter::LiftFunctionInternal(AnalyzedFunction *analyzedFunction) {
    ASTFunction astFunc;
    astFunc.backingFunction = analyzedFunction;

    if (!analyzedFunction->basicBlocks.empty()) {
        std::set<uint32_t> visited;
        astFunc.statements = LiftTree(analyzedFunction, 0, -1, visited);
    }

    for (auto &inner : analyzedFunction->innerFunctions) {
        astFunc.subFunctions.push_back(LiftFunctionInternal(&inner));
    }

    return astFunc;
}

ASTFunction ASTLifter::LiftFunction(AnalyzedFunction *analyzedFunction) {
    this->m_lpRootFunction = analyzedFunction; // set root for internal traversal.
    return LiftFunctionInternal(analyzedFunction);
}