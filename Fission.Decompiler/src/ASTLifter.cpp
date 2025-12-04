//
// Created by Pixeluted on 04/12/2025.
//
#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "Deserializer.hpp"

std::shared_ptr<Expression> ASTLifter::LiftExpression(const AnalyzedFunction *func, const LiftedOperand &operand) {
    const auto definitionInstruction = func->GetDefinition(operand);
    if (!definitionInstruction) {
        // Likely upvalue or argument
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::format("v{}", std::to_string(operand.value.reg))));
    }

    switch (definitionInstruction->operation) {
    case LiftedOperation::LOAD: {
        if (definitionInstruction->operands[1].type == LiftedOperandType::ImmediateNil) {
            return std::make_shared<NilLiteralNode>();
        }
        if (definitionInstruction->operands[1].type == LiftedOperandType::ImmediateBool) {
            return std::make_shared<BooleanLiteralNode>(definitionInstruction->operands[1].value.imm.b);
        }
        if (definitionInstruction->operands[1].type == LiftedOperandType::ImmediateInteger) {
            return std::make_shared<NumberLiteralNode>(definitionInstruction->operands[1].value.imm.n);
        }
        if (definitionInstruction->operands[1].type == LiftedOperandType::ImmediateConstant) {
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
                return std::make_shared<CommentNode>("<!-- complex constant -->");
            }
        }
    }
    case LiftedOperation::MOVE: {
        return LiftExpression(func, definitionInstruction->operands[1]);
    }
    case LiftedOperation::ADD: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);

        return std::make_shared<BinaryExpressionNode>("+", left, right);
    }
    case LiftedOperation::SUB: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<BinaryExpressionNode>("-", left, right);
    }
    case LiftedOperation::MUL: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<BinaryExpressionNode>("*", left, right);
    }
    case LiftedOperation::DIV: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<BinaryExpressionNode>("/", left, right);
    }
    case LiftedOperation::MOD: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<BinaryExpressionNode>("%", left, right);
    }
    case LiftedOperation::POW: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        auto right = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<BinaryExpressionNode>("^", left, right);
    }
    case LiftedOperation::ADDK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("+", left, right);
    }
    case LiftedOperation::SUBK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("-", left, right);
    }
    case LiftedOperation::MULK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("*", left, right);
    }
    case LiftedOperation::DIVK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("/", left, right);
    }
    case LiftedOperation::MODK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("%", left, right);
    }
    case LiftedOperation::POWK: {
        auto left = LiftExpression(func, definitionInstruction->operands[1]);
        std::shared_ptr<Expression> right = nullptr;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TNUMBER) {
            right = std::make_shared<CommentNode>("Invalid constant");
        } else {
            right = std::make_shared<NumberLiteralNode>(std::get<double>(constant.constantData));
        }

        return std::make_shared<BinaryExpressionNode>("^", left, right);
    }
    case LiftedOperation::NOT: {
        auto expr = LiftExpression(func, definitionInstruction->operands[1]);
        return std::make_shared<UnaryExpressionNode>("not", expr);
    }
    case LiftedOperation::MINUS: {
        auto expr = LiftExpression(func, definitionInstruction->operands[1]);
        return std::make_shared<UnaryExpressionNode>("-", expr);
    }
    case LiftedOperation::LENGTH: {
        auto expr = LiftExpression(func, definitionInstruction->operands[1]);
        return std::make_shared<UnaryExpressionNode>("#", expr);
    }
    case LiftedOperation::GETTABLE: {
        auto table = LiftExpression(func, definitionInstruction->operands[1]);
        auto key = LiftExpression(func, definitionInstruction->operands[2]);
        return std::make_shared<IndexExpressionNode>(table, key);
    }
    case LiftedOperation::GETTABLEKS: {
        auto table = LiftExpression(func, definitionInstruction->operands[1]);
        std::string keyName;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[2].value.imm.k];
        if (constant.kType != LUA_TSTRING) {
            keyName = "Invalid constant";
        } else {
            keyName = std::get<std::string>(constant.constantData);
        }

        return std::make_shared<MemberExpressionNode>(table, keyName);
    }
    case LiftedOperation::GETTABLEN: {
        auto table = LiftExpression(func, definitionInstruction->operands[1]);

        auto indexVal = definitionInstruction->operands[2].value.imm.n;
        auto key = std::make_shared<NumberLiteralNode>(indexVal);

        return std::make_shared<IndexExpressionNode>(table, key);
    }
    case LiftedOperation::GETGLOBAL: {
        std::string name;

        const auto &constant = func->lpLiftedFunction->lpDeserialized->constants[definitionInstruction->operands[1].value.imm.k];
        if (constant.kType != LUA_TSTRING) {
            name = "Invalid constant";
        } else {
            name = std::get<std::string>(constant.constantData);
        }

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
        auto functionArguments = duplicatedFunction->isvararg ? -1 : duplicatedFunction->numparams;
        auto functionArgumentNames = std::unordered_map<int32_t, std::string>();
        if (!duplicatedFunction->isvararg) {
            for (int i = 1; i < functionArguments; i++) {
                functionArgumentNames[i] = std::format("v{}", i);
            }
        }

        return std::make_shared<FunctionDeclarationNode>(functionName, functionArguments, functionArgumentNames);
    }
    case LiftedOperation::NEWCLOSURE: {
        const auto protoUsed = func->lpLiftedFunction->lpDeserialized->subfunctions[definitionInstruction->operands[1].value.imm.k];

        // If we ever change the logic inside the bytecode lifter, this only needs to changed too, thx.
        auto functionName = protoUsed->debugName ? protoUsed->debugName.value() : std::format("f{}", protoUsed->bytecodeId);
        auto functionArguments = protoUsed->isvararg ? -1 : protoUsed->numparams;
        auto functionArgumentNames = std::unordered_map<int32_t, std::string>();
        if (!protoUsed->isvararg) {
            for (int i = 1; i < functionArguments; i++) {
                functionArgumentNames[i] = std::format("v{}", i);
            }
        }

        return std::make_shared<FunctionDeclarationNode>(functionName, functionArguments, functionArgumentNames);
    }

    default:
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("reg_" + std::to_string(operand.value.reg)));
    }
}

ASTFunction ASTLifter::LiftFunctionInternal(AnalyzedFunction *analyzedFunction) {
    auto liftedFunction = ASTFunction{};

    // We have to look for "roots", these are instructions that have side effects (e.g. modifying registers)
    // Then from those, we look at it's register and analyze where it's registers came from to build an AST Statement
    // We will not consider operations like GETIMPORT/GETGLOBAL to be these because they load stuff, not change stuff
    // Doing so would result in having expressions like v1 = print, v1(stuff), which we don't want
    for (const auto &instruction : analyzedFunction->lpLiftedFunction->instructions) {

        switch (instruction.operation) {
        case LiftedOperation::CALL: {
            int regFunc = instruction.operands[0].value.reg;
            size_t argCount = instruction.operands[1].value.imm.n - 1;

            auto callee = LiftExpression(analyzedFunction, instruction.operands[0]);

            std::vector<std::shared_ptr<Expression>> args;

            if (analyzedFunction->implicitUses.contains(&instruction)) {
                const auto &versions = analyzedFunction->implicitUses.at(&instruction);
                for (size_t i = 0; i < argCount && i < versions.size(); ++i) {
                    int regArg = regFunc + 1 + i;

                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = regArg;
                    op.ssaVersion = versions[i];
                    args.push_back(LiftExpression(analyzedFunction, op));
                }
            } else {
                args.push_back(std::make_shared<CommentNode>("ERROR: Missing SSA versions for arguments"));
            }

            auto callExpr = std::make_shared<CallExpressionNode>(callee, args);
            liftedFunction.statements.push_back(std::make_shared<ExpressionStatementNode>(callExpr));
            break;
        }

        case LiftedOperation::SETGLOBAL: {
            int kIndex = instruction.operands[1].value.imm.k;

            std::string name;
            const auto &constant = analyzedFunction->lpLiftedFunction->lpDeserialized->constants[kIndex];
            if (constant.kType == LUA_TSTRING) {
                name = std::get<std::string>(constant.constantData);
            } else {
                name = "INVALID_GLOBAL";
            }

            auto left = std::make_shared<Identifier>(name);
            auto leftSide = std::make_shared<IdentifierExpressionNode>(left);

            auto rightSide = LiftExpression(analyzedFunction, instruction.operands[0]);

            liftedFunction.statements.push_back(std::make_shared<AssignmentStatementNode>(leftSide, rightSide));
            break;
        }

        case LiftedOperation::SETTABLE: {
            auto table = LiftExpression(analyzedFunction, instruction.operands[1]);
            auto key = LiftExpression(analyzedFunction, instruction.operands[2]);
            auto value = LiftExpression(analyzedFunction, instruction.operands[0]);

            liftedFunction.statements.push_back(std::make_shared<AssignmentStatementNode>(std::make_shared<IndexExpressionNode>(table, key), value));
            break;
        }

        case LiftedOperation::SETTABLEKS: {
            int kIndex = instruction.operands[2].value.imm.k;

            std::string keyName;
            const auto &constant = analyzedFunction->lpLiftedFunction->lpDeserialized->constants[kIndex];
            if (constant.kType == LUA_TSTRING) {
                keyName = std::get<std::string>(constant.constantData);
            } else {
                keyName = "INVALID_GLOBAL";
            }

            auto table = LiftExpression(analyzedFunction, instruction.operands[1]);
            auto value = LiftExpression(analyzedFunction, instruction.operands[0]);

            auto left = std::make_shared<MemberExpressionNode>(table, keyName);
            liftedFunction.statements.push_back(std::make_shared<AssignmentStatementNode>(left, value));
            break;
        }

        case LiftedOperation::RETURN: {
            int regStart = instruction.operands[0].value.reg;
            size_t count = instruction.operands[1].value.imm.n - 1;

            std::vector<std::shared_ptr<Expression>> rets;

            if (analyzedFunction->implicitUses.contains(&instruction)) {
                const auto &versions = analyzedFunction->implicitUses.at(&instruction);

                for (size_t i = 0; i < count && i < versions.size(); ++i) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = regStart + i;
                    op.ssaVersion = versions[i];

                    rets.push_back(LiftExpression(analyzedFunction, op));
                }
            } else {
                if (count > 0) {
                    rets.push_back(std::make_shared<CommentNode>("CRITICAL: Missing SSA versions for RETURN statement"));
                }
            }

            liftedFunction.statements.push_back(std::make_shared<ReturnStatementNode>(rets));
            break;
        }

        default:
            break;
        }
    }

    for (auto &subFunction : analyzedFunction->innerFunctions) {
        liftedFunction.subFunctions.emplace_back(LiftFunctionInternal(&subFunction));
    }

    return liftedFunction;
}

ASTFunction ASTLifter::LiftFunction(AnalyzedFunction *analyzedFunction) { return LiftFunctionInternal(analyzedFunction); }