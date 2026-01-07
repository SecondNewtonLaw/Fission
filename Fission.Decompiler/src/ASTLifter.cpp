//
// Created by Dottik on 10/12/2025.
//

#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"

#include <algorithm>

#pragma clang optimize off

static std::shared_ptr<BlockStatementNode> CreateBlock(const std::vector<std::shared_ptr<Statement>> &stmts) {
    auto block = std::make_shared<BlockStatementNode>();
    block->body = stmts;
    return block;
}

std::shared_ptr<Expression> ASTLifter::InvertCondition(const std::shared_ptr<Expression> &cond) {
    if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(cond); unary && unary->op == "not ") {
        return unary->operand;
    }

    if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(cond)) {
        if (binary->op == "==") {
            binary->op = "~=";
            return binary;
        } else if (binary->op == "~=") {
            binary->op = "==";
            return binary;
        } else if (binary->op == "<") {
            binary->op = ">=";
            return binary;
        } else if (binary->op == ">") {
            binary->op = "<=";
            return binary;
        } else if (binary->op == "<=") {
            binary->op = ">";
            return binary;
        } else if (binary->op == ">=") {
            binary->op = "<";
            return binary;
        }
    }

    return std::make_shared<UnaryExpressionNode>("not ", cond);
}

ASTLifter::ASTLifter() {}

ASTFunction ASTLifter::Lift(AnalyzedFunction &analyzedFunction) {
    this->m_currentFunction = &analyzedFunction;
    this->m_definedRegisters.clear();
    this->m_pinnedRegisters.clear();
    this->m_processedInstructions.clear();
    this->m_phiConsumers.clear();

    for (const auto &block : analyzedFunction.basicBlocks) {
        for (const auto &phi : block.phiNodes) {
            for (size_t i = 1; i < phi.operands.size(); ++i) {
                const auto &op = phi.operands[i];
                if (op.type == LiftedOperandType::Register) {
                    m_phiConsumers.insert({op.value.reg, op.ssaVersion});
                }
            }
        }
    }

    ASTFunction ast;
    ast.backingFunction = &analyzedFunction;
    analyzedFunction.PopulateNames();

    if (!analyzedFunction.basicBlocks.empty()) {
        std::set<uint32_t> visited;
        ast.statements = LiftControlFlow(0, -1, visited);
        auto s = std::format(
            R"(
    Fission ~~ Function Information:
        ~ Upvalue Count: {}
        ~ Argument Count: {}
        ~ Debug Name: {}
        ~ Bytecode ID: {}
        ~ Registers Used: R0-R{}
)",
            analyzedFunction.lpLiftedFunction->lpDeserialized->nups, analyzedFunction.lpLiftedFunction->lpDeserialized->numparams,
            analyzedFunction.lpLiftedFunction->lpDeserialized->debugName.value_or("anon/no name"),
            analyzedFunction.lpLiftedFunction->lpDeserialized->bytecodeId, analyzedFunction.lpLiftedFunction->lpDeserialized->maxstacksize - 1
        );

        if (analyzedFunction.lpLiftedFunction->lpDeserialized->bIsMain) {
            s = "\n    Decompiled with the Fission decompiler for RbxCli\n";
        }

        ast.statements.insert(ast.statements.begin(), std::make_shared<CommentNode>(s, true));
    }

    for (auto &subFunc : analyzedFunction.innerFunctions) {
        ASTLifter subLifter;
        ast.subFunctions.push_back(subLifter.Lift(subFunc));
    }

    return ast;
}

std::shared_ptr<Expression> ASTLifter::LiftCondition(const LiftedInstruction *inst) {
    if (!inst)
        return std::make_shared<BooleanLiteralNode>(false);

    switch (inst->operation) {
    case LiftedOperation::JUMPIFNOTEQ:
        return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFEQ:
        return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFLT:
        return std::make_shared<BinaryExpressionNode>("<", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFNOTLT:
        return std::make_shared<BinaryExpressionNode>(">", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFLE:
        return std::make_shared<BinaryExpressionNode>("<=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIFNOTLE:
        return std::make_shared<BinaryExpressionNode>(">=", LiftExpression(inst->operands[0]), LiftExpression(inst->operands[2]));
    case LiftedOperation::JUMPIF:
        return LiftExpression(inst->operands[0]);
    case LiftedOperation::JUMPIFNOT:
        return std::make_shared<UnaryExpressionNode>("not ", LiftExpression(inst->operands[0]));
    case LiftedOperation::JUMPXEQK: {
        if (inst->operands[2].type == LiftedOperandType::ImmediateConstant) {
            auto kIdx = inst->operands[2].value.imm.k;
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs;
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[kIdx];
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
                rhs = std::make_shared<NilLiteralNode>();
                break;
            }

            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }
        if (inst->operands[2].type == LiftedOperandType::ImmediateBool) {
            auto bValue = inst->operands[2].value.imm.b;
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs = std::make_shared<BooleanLiteralNode>(bValue);
            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }

        if (inst->operands[2].type == LiftedOperandType::ImmediateNil) {
            auto notFlag = inst->operands[3].value.imm.b;
            std::shared_ptr<Expression> rhs = std::make_shared<NilLiteralNode>();
            if (notFlag)
                return std::make_shared<BinaryExpressionNode>("~=", LiftExpression(inst->operands[0]), rhs);

            return std::make_shared<BinaryExpressionNode>("==", LiftExpression(inst->operands[0]), rhs);
        }
    }
    default:
        return std::make_shared<BooleanLiteralNode>(false);
    }
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited) {
    std::vector<std::shared_ptr<Statement>> nodes;

    if (currentBlockId == static_cast<uint32_t>(-1) || currentBlockId >= m_currentFunction->basicBlocks.size())
        return nodes;

    // return blocks are allowed to be duplicated, as they have no successors.
    // compilers may inline the return for a break, which is annoying as fuck, and will break our lifting.
    // fuck you luauc.
    if (this->m_currentFunction->basicBlocks.at(currentBlockId).bType != BlockType::Return) {
        if (currentBlockId == stopBlockId || visited.contains(currentBlockId))
            return nodes;
    }

    if (!visited.contains(currentBlockId))
        visited.insert(currentBlockId); // prevent double insertion product of block above.

    const auto &block = m_currentFunction->basicBlocks[currentBlockId];

    auto stmts = LiftBlockInstructions(block);

    switch (block.bType) {
    case BlockType::IfHeader: {
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        if (block.ifStatementTrue.has_value() && block.ifStatementFalse.has_value()) {
            uint32_t trueIdx = block.ifStatementTrue.value();
            uint32_t falseIdx = block.ifStatementFalse.value();

            uint32_t mergeIdx = FindMergeBlock(trueIdx, falseIdx);
            if (mergeIdx == static_cast<uint32_t>(-1) && stopBlockId != static_cast<uint32_t>(-1))
                mergeIdx = stopBlockId;

            bool isSequential = CanReach(trueIdx, falseIdx, mergeIdx, visited);

            if (isSequential)
                mergeIdx = falseIdx;

            uint32_t fallthroughIdx = (!block.successors.empty()) ? block.successors[0] : -1;

            auto jumpCond = LiftCondition(block.lpTail);
            std::shared_ptr<Expression> cond;

            if (trueIdx == fallthroughIdx) {
                if (!isSequential)
                    cond = InvertCondition(jumpCond);
                else
                    cond = (jumpCond);
            } else {
                cond = InvertCondition(jumpCond);
            }

            auto thenStmts = LiftControlFlow(trueIdx, mergeIdx, visited);

            std::vector<std::shared_ptr<Statement>> elseStmts;
            if (!isSequential) {
                elseStmts = LiftControlFlow(falseIdx, mergeIdx, visited);
            }

            if (thenStmts.empty() && !elseStmts.empty()) {
                cond = InvertCondition(jumpCond);
                std::swap(thenStmts, elseStmts);
            }

            auto ifStmt = std::make_shared<IfStatementNode>();
            ifStmt->condition = cond;
            ifStmt->thenBranch = CreateBlock(thenStmts);

            if (!elseStmts.empty()) {
                bool thenIsTerminal = !thenStmts.empty() && (std::dynamic_pointer_cast<ReturnStatementNode>(thenStmts.back()) ||
                                                             std::dynamic_pointer_cast<BreakStatementNode>(thenStmts.back()));

                if (thenIsTerminal) {
                    nodes.push_back(ifStmt);
                    nodes.insert(nodes.end(), elseStmts.begin(), elseStmts.end());
                } else {
                    ifStmt->elseBranch = CreateBlock(elseStmts);
                    nodes.push_back(ifStmt);
                }
            } else {
                nodes.push_back(ifStmt);
            }

            auto after = LiftControlFlow(mergeIdx, stopBlockId, visited);
            nodes.insert(nodes.end(), after.begin(), after.end());
        } else {
            nodes.insert(
                nodes.end(),
                std::make_shared<CommentNode>("Fission: Warning! Failed to determine TRUE and FALSE block! Control flow graph may be malformed.", true)
            );
        }
        break;
    }
    case BlockType::LoopHeader: {
        if (block.loopLatch.has_value()) {
            uint32_t latchIdx = block.loopLatch.value();
            uint32_t exitIdx = block.loopExit.value_or(block.loopLatch.value_or(-1));

            std::set<uint32_t> loopVisited = visited;

            if (nodes.size() >= stmts.size())
                nodes.resize(nodes.size() - stmts.size());

            if ((block.dwBlockFlags & LoopBlockFlags::ForNumericLoop) == LoopBlockFlags::ForNumericLoop) {
                uint32_t bodyIdx = -1;
                for (auto succ : block.successors) {
                    if (succ != block.loopLatch.value_or(block.loopExit.value_or(-1))) {
                        bodyIdx = succ;
                        break;
                    }
                }

                auto forNode = std::make_shared<ForNumericNode>();
                auto forPrepInst = block.lpTail;
                int32_t limitVer = -1, stepVer = -1, startVer = -1;

                if (this->m_currentFunction->implicitUses.contains(forPrepInst)) {
                    const auto &impl = this->m_currentFunction->implicitUses.at(forPrepInst);
                    if (impl.size() >= 3) {
                        limitVer = impl[0];
                        stepVer = impl[1];
                        startVer = impl[2];
                    }
                }

                int baseReg = block.lpTail->operands[0].value.reg;
                {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    {
                        PinnedRegisterScope pin(this, baseReg + 2);

                        this->m_currentFunction->SetVariableName(baseReg + 2, startVer, std::format("i_{}", baseReg + 2));
                        forNode->lpLoopBody = CreateBlock(LiftControlFlow(bodyIdx, *block.loopLatch, loopVisited));

                        op.value.reg = baseReg + 2;
                        op.ssaVersion = startVer;
                        forNode->loopVariable = LiftExpression(op);
                    }

                    op.value.reg = baseReg + 2;
                    op.ssaVersion = startVer;
                    forNode->startVariable = LiftExpression(op);

                    op.value.reg = baseReg;
                    op.ssaVersion = limitVer;
                    forNode->maxIncreased = LiftExpression(op);

                    op.value.reg = baseReg + 1;
                    op.ssaVersion = stepVer;
                    forNode->increaseBy = LiftExpression(op);
                    this->m_currentFunction->ClearVersionName(baseReg, block.lpTail->operands[0].ssaVersion);
                }
                nodes.push_back(forNode);
            } else if ((block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop) {
                /*
                 *
                 */

                // uint32_t falseSucc = -1;
                auto whileNode = std::make_shared<WhileStatementNode>();

                whileNode->condition = InvertCondition(LiftCondition(block.lpTail)); // it appears we have a better expression when inverting the node, why?

                uint32_t bodyStart = -1;
                for (auto s : block.successors)
                    if (s != exitIdx)
                        bodyStart = s;

                if (bodyStart != static_cast<uint32_t>(-1)) {
                    std::set<uint32_t> cloopVisited = visited;
                    cloopVisited.insert(latchIdx);
                    cloopVisited.insert(exitIdx);
                    auto bodyStmts = LiftControlFlow(bodyStart, latchIdx, cloopVisited);

                    if (latchIdx != currentBlockId) {
                        auto latchStmts = LiftBlockInstructions(m_currentFunction->basicBlocks[latchIdx]);
                        bodyStmts.insert(bodyStmts.end(), latchStmts.begin(), latchStmts.end());
                    }

                    whileNode->body = CreateBlock(bodyStmts);
                } else {
                    whileNode->body = CreateBlock({});
                }
                nodes.push_back(whileNode);
            }

            auto after = LiftControlFlow(exitIdx, stopBlockId, visited);
            nodes.insert(nodes.end(), after.begin(), after.end());
        }
        break;
    }
    case BlockType::Break:
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        nodes.push_back(std::make_shared<BreakStatementNode>());
        break;
    case BlockType::Return:
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        break;
    default: {
        nodes.insert(nodes.end(), stmts.begin(), stmts.end());
        if (!block.successors.empty()) {
            auto next = LiftControlFlow(block.successors[0], stopBlockId, visited);
            nodes.insert(nodes.end(), next.begin(), next.end());
        }
        break;
    }
    }

    return nodes;
}

std::vector<std::shared_ptr<Statement>> ASTLifter::LiftBlockInstructions(const BasicBlock &block) {
    std::vector<std::shared_ptr<Statement>> statements;
    if (!block.lpHead)
        return statements;

    for (int i = block.lpHead->instructionIndex; i <= block.lpTail->instructionIndex; ++i) {
        if (m_processedInstructions.contains(i))
            continue;

        const auto &inst = m_currentFunction->lpLiftedFunction->instructions[i];

        if (ShouldInline(&inst))
            continue;

        switch (inst.operation) {
        case LiftedOperation::GETVARARGS: {
            break; // not handled here.
        }
        case LiftedOperation::SETGLOBAL: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[1].value.imm.k];
            auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::get<std::string>(k.constantData)));
            statements.push_back(std::make_shared<AssignmentStatementNode>(left, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLE: {
            auto idxExpr = std::make_shared<IndexExpressionNode>(LiftExpression(inst.operands[1]), LiftExpression(inst.operands[2]));
            statements.push_back(std::make_shared<AssignmentStatementNode>(idxExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::SETTABLEKS: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[2].value.imm.k];
            auto memExpr = std::make_shared<MemberExpressionNode>(LiftExpression(inst.operands[1]), std::get<std::string>(k.constantData));
            statements.push_back(std::make_shared<AssignmentStatementNode>(memExpr, LiftExpression(inst.operands[0])));
            break;
        }
        case LiftedOperation::RETURN: {
            std::vector<std::shared_ptr<Expression>> rets;
            if (m_currentFunction->implicitUses.contains(&inst)) {
                for (int32_t ver : m_currentFunction->implicitUses.at(&inst)) {
                    LiftedOperand op;
                    op.type = LiftedOperandType::Register;
                    op.value.reg = inst.operands[0].value.reg + (int)rets.size();
                    op.ssaVersion = ver;

                    auto def = m_currentFunction->GetDefinition(op);
                    if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::NAMECALL)) {
                        int32_t callIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                        if (m_currentFunction->lpLiftedFunction->instructions[callIdx].operands[2].value.imm.n == 0) {
                            rets.push_back(LiftCall(*def, def->instructionIndex, true));
                            break;
                        }
                    }
                    rets.push_back(LiftExpression(op));
                }
            }

            if (i == (int32_t)this->m_currentFunction->lpLiftedFunction->instructions.size() - 1 && inst.operation == LiftedOperation::RETURN && rets.empty()) {
                for (const auto &pred : block.predecessors)
                    if (this->m_currentFunction->basicBlocks.at(pred).bType == BlockType::IfHeader ||
                        this->m_currentFunction->basicBlocks.at(pred).bType == BlockType::LoopHeader ||
                        this->m_currentFunction->basicBlocks.at(pred).bType ==
                            BlockType::LoopLatch) { // the compiler may inline the break as a RETURN instruction instead.
                        statements.push_back(std::make_shared<ReturnStatementNode>(rets));
                        break;
                    }
                continue; // ignore last return if and only if there's no returns.
            }

            statements.push_back(std::make_shared<ReturnStatementNode>(rets));
            break;
        }
        case LiftedOperation::CALL:
        case LiftedOperation::NAMECALL: {
            auto callExpr = LiftCall(inst, i, false);

            std::vector<std::shared_ptr<Expression>> lhs;
            std::vector<SSARef> defs;
            for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
                if (defInst == &inst)
                    defs.push_back(ref);
            }
            std::ranges::sort(defs, [](auto &a, auto &b) { return a.regIndex < b.regIndex; });

            for (const auto &ref : defs) {
                if (m_currentFunction->useCounts[ref] > 0) {
                    lhs.push_back(
                        std::make_shared<IdentifierExpressionNode>(
                            std::make_shared<Identifier>(ResolveVariableName({LiftedOperandType::Register, {ref.regIndex}, ref.version}))
                        )
                    );
                }
            }

            if (!lhs.empty()) {
                if (auto callNode = std::dynamic_pointer_cast<CallExpressionNode>(callExpr)) {
                    callNode->rets = lhs;
                } else if (auto nameCallNode = std::dynamic_pointer_cast<NameCallExpressionNode>(callExpr)) {
                    nameCallNode->rets = lhs;
                }
            }

            statements.push_back(std::make_shared<ExpressionStatementNode>(callExpr));

            if (inst.operation == LiftedOperation::NAMECALL) {
                m_processedInstructions.insert(i + 1);
                m_processedInstructions.insert(i + 2);
            }
            break;
        }

        case LiftedOperation::DUPCLOSURE: {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[inst.operands[1].value.imm.k];
            const auto duplicatedFunction = std::get<LuauProto>(k.constantData);

            AnalyzedFunction *targetFunc = nullptr;
            for (auto &inner : m_currentFunction->innerFunctions) {
                if (inner.lpLiftedFunction->lpDeserialized->bytecodeId == duplicatedFunction->bytecodeId) {
                    targetFunc = &inner;
                    targetFunc->PopulateNames(); // populate to prevent bad usages.
                    break;
                }
            }

            size_t capIdx = 0;
            bool bUsesCapturesAndMarked = false;
            while (i + 1 + capIdx < m_currentFunction->lpLiftedFunction->instructions.size()) {
                auto &cap = m_currentFunction->lpLiftedFunction->instructions[i + 1 + capIdx];
                if (cap.operation != LiftedOperation::CAPTURE)
                    break;

                if (!bUsesCapturesAndMarked) {
                    bUsesCapturesAndMarked = true;
                    statements.push_back(
                        std::make_shared<CommentNode>(
                            std::format(
                                "Fission: Beginning captures for function with name '{}'",
                                duplicatedFunction->debugName.value_or(std::format("f{}", duplicatedFunction->bytecodeId))
                            ),
                            true
                        )
                    );
                }

                if (duplicatedFunction->upvalueNames.size() > capIdx)
                    statements.push_back(std::make_shared<CommentNode>("Fission: name from debug information.", true));
                else
                    statements.push_back(std::make_shared<CommentNode>("Fission: autogenerated name.", true));

                statements.push_back(
                    std::make_shared<VariableDeclarationNode>(
                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(targetFunc->GetUpvalueName(capIdx))),
                        this->LiftExpression(cap.operands[1])
                    )
                );

                m_processedInstructions.insert(i + 1 + capIdx);
                capIdx++;
            }

            if (bUsesCapturesAndMarked) {
                statements.push_back(
                    std::make_shared<CommentNode>(
                        std::format(
                            "Fission: Ending captures for function with name '{}'",
                            duplicatedFunction->debugName.value_or(std::format("f{}", duplicatedFunction->bytecodeId))
                        ),
                        true
                    )
                );
            }

            ASTLifter subLifter;
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = duplicatedFunction->debugName.value_or(std::format("f{}", duplicatedFunction->bytecodeId));
            m_currentFunction->SetGlobalName(inst.operands[0].value.reg, funcName);

            std::unordered_map<int32_t, std::string> argNames;
            for (int j = 0; j < duplicatedFunction->numparams; ++j) {
                std::string argName = targetFunc->GetVarName(j, 0);
                if (argName.empty() || argName == std::format("v{}", j))
                    argName = std::format("a{}", j);
                argNames[j] = argName;
            }

            auto bodyBlock = std::make_shared<BlockStatementNode>();
            bodyBlock->body = subAst.statements;
            // literally almost the same handler as NEWCLOSURE.

            statements.push_back(
                std::make_shared<FunctionDeclarationNode>(funcName, duplicatedFunction->numparams, argNames, duplicatedFunction->isvararg, bodyBlock, true)
            );

            auto &saveWhere = inst.operands[0];

            if (this->m_currentFunction->users.contains({saveWhere.value.reg, saveWhere.ssaVersion})) {
                auto users = this->m_currentFunction->users[{saveWhere.value.reg, saveWhere.ssaVersion}];
                for (const auto &user : users) {
                    if (user->operation == LiftedOperation::SETGLOBAL) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back()); fDec->bIsLocalDeclaration) {
                m_currentFunction->SetGlobalName(inst.operands[0].value.reg, funcName);
            }

            break;
            break;
        }
        case LiftedOperation::NEWCLOSURE: {
            int protoIdx = inst.operands[1].value.imm.k;
            const auto proto = m_currentFunction->lpLiftedFunction->lpDeserialized->subfunctions[protoIdx];

            AnalyzedFunction *targetFunc = nullptr;
            for (auto &inner : m_currentFunction->innerFunctions) {
                if (inner.lpLiftedFunction->lpDeserialized->bytecodeId == proto->bytecodeId) {
                    targetFunc = &inner;
                    targetFunc->PopulateNames(); // populate to prevent bad usages.
                    break;
                }
            }

            size_t capIdx = 0;
            bool bUsesCapturesAndMarked = false;
            while (i + 1 + capIdx < m_currentFunction->lpLiftedFunction->instructions.size()) {
                auto &cap = m_currentFunction->lpLiftedFunction->instructions[i + 1 + capIdx];
                if (cap.operation != LiftedOperation::CAPTURE)
                    break;

                if (!bUsesCapturesAndMarked) {
                    bUsesCapturesAndMarked = true;
                    statements.push_back(
                        std::make_shared<CommentNode>(
                            std::format(
                                "Fission: Beginning captures for function with name '{}'", proto->debugName.value_or(std::format("f{}", proto->bytecodeId))
                            ),
                            true
                        )
                    );
                }

                if (proto->upvalueNames.size() > capIdx)
                    statements.push_back(std::make_shared<CommentNode>("Fission: name from debug information.", true));
                else
                    statements.push_back(std::make_shared<CommentNode>("Fission: autogenerated name.", true));

                statements.push_back(
                    std::make_shared<VariableDeclarationNode>(
                        std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(targetFunc->GetUpvalueName(capIdx))),
                        this->LiftExpression(cap.operands[1])
                    )
                );

                m_processedInstructions.insert(i + 1 + capIdx);
                capIdx++;
            }

            if (bUsesCapturesAndMarked) {
                statements.push_back(
                    std::make_shared<CommentNode>(
                        std::format("Fission: Ending captures for function with name '{}'", proto->debugName.value_or(std::format("f{}", proto->bytecodeId))),
                        true
                    )
                );
            }

            ASTLifter subLifter;
            ASTFunction subAst = subLifter.Lift(*targetFunc);

            std::string funcName = proto->debugName.value_or(std::format("f{}", proto->bytecodeId));

            std::unordered_map<int32_t, std::string> argNames;
            for (int j = 0; j < proto->numparams; ++j) {
                std::string argName = targetFunc->GetVarName(j, 0);
                if (argName.empty() || argName == std::format("v{}", j))
                    argName = std::format("a{}", j);
                argNames[j] = argName;
            }

            auto bodyBlock = std::make_shared<BlockStatementNode>();
            bodyBlock->body = subAst.statements;

            statements.push_back(std::make_shared<FunctionDeclarationNode>(funcName, proto->numparams, argNames, proto->isvararg, bodyBlock, true));

            auto &saveWhere = inst.operands[0];

            if (this->m_currentFunction->users.contains({saveWhere.value.reg, saveWhere.ssaVersion})) {
                auto users = this->m_currentFunction->users[{saveWhere.value.reg, saveWhere.ssaVersion}];
                for (const auto &user : users) {
                    if (user->operation == LiftedOperation::SETGLOBAL) {
                        // set to global, not a local function.
                        auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back());
                        // local functions may have a debug name. This is exactly to prevent this
                        fDec->bIsLocalDeclaration = false;

                        // instruction is consumed, else we will emit ghost definitions after the declaration.
                        this->m_processedInstructions.insert(user->instructionIndex);
                    }
                }
            }

            if (auto fDec = std::dynamic_pointer_cast<FunctionDeclarationNode>(statements.back()); fDec->bIsLocalDeclaration) {
                m_currentFunction->SetGlobalName(inst.operands[0].value.reg, funcName);
            }

            break;
        }

        case LiftedOperation::MOVE:
            break;

        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            break;

        default: {
            if (inst.operands.empty())
                break;
            if (inst.operands[0].type != LiftedOperandType::Register)
                break;

            const auto *def = m_currentFunction->GetDefinition(inst.operands[0]);
            if (def == &inst) {
                auto isDefined = m_definedRegisters.contains(inst.operands[0].value.reg);
                auto target = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(inst.operands[0])));
                auto val = LiftExpression(inst.operands[0], true);

                if (inst.operands[0].ssaVersion <= 1 || !isDefined)
                    statements.push_back(std::make_shared<VariableDeclarationNode>(target, val));
                else {
                    if (val->nodeKind == ASTNodeKind::BinaryExpression) {
                        // binary expressions may be compounded under specific conditions.
                        if (auto lpBinExpr = std::dynamic_pointer_cast<BinaryExpressionNode>(val); lpBinExpr != nullptr)
                            if (auto identifier = std::dynamic_pointer_cast<IdentifierExpressionNode>(lpBinExpr->left); identifier != nullptr) {
                                // expression is compound.
                                if (identifier->identifier->name == target->identifier->name) {
                                    statements.push_back(std::make_shared<CompoundBinaryExpressionNode>(lpBinExpr->op, lpBinExpr->left, lpBinExpr->right));
                                    break;
                                }
                            }
                    }

                    statements.push_back(std::make_shared<AssignmentStatementNode>(target, val));
                }
            }
            break;
        }
        }
    }
    return statements;
}

bool ASTLifter::CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const std::set<uint32_t> &visitedScopes) {
    if (start == target)
        return true;

    std::queue<uint32_t> q;
    std::set<uint32_t> visited;

    q.push(start);
    visited.insert(start);

    while (!q.empty()) {
        uint32_t curr = q.front();
        q.pop();

        if (curr == target)
            return true;
        if (curr == stopBlock)
            continue;

        if (visited.size() > 5000)
            return false;

        const auto &block = m_currentFunction->basicBlocks[curr];

        for (uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (visitedScopes.contains(succ))
                continue;

            if (!visited.contains(succ)) {
                visited.insert(succ);
                q.push(succ);
            }
        }
    }
    return false;
}

std::shared_ptr<Expression> ASTLifter::LiftExpression(const LiftedOperand &operand, bool forceExpression) {
    if (operand.type == LiftedOperandType::Register && m_pinnedRegisters.contains(operand.value.reg)) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }

    if (operand.type == LiftedOperandType::ImmediateNil)
        return std::make_shared<NilLiteralNode>();
    if (operand.type == LiftedOperandType::ImmediateBool)
        return std::make_shared<BooleanLiteralNode>(operand.value.imm.b);
    if (operand.type == LiftedOperandType::ImmediateInteger)
        return std::make_shared<NumberLiteralNode>(operand.value.imm.n);
    if (operand.type == LiftedOperandType::ImmediateConstant) {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[operand.value.imm.k];
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
            return std::make_shared<NilLiteralNode>();
        }
    }

    const auto *def = m_currentFunction->GetDefinition(operand);

    if (!def || (!forceExpression && !ShouldInline(def))) {
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }

    if (def->operation == LiftedOperation::LOAD) {
        if (def->operands[1].type == LiftedOperandType::ImmediateNil)
            return std::make_shared<NilLiteralNode>();
        if (def->operands[1].type == LiftedOperandType::ImmediateBool)
            return std::make_shared<BooleanLiteralNode>(def->operands[1].value.imm.b);
        if (def->operands[1].type == LiftedOperandType::ImmediateInteger)
            return std::make_shared<NumberLiteralNode>(def->operands[1].value.imm.n);
        if (def->operands[1].type == LiftedOperandType::ImmediateConstant) {
            const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[1].value.imm.k];
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
                return std::make_shared<NilLiteralNode>();
            }
        }
    }

    switch (def->operation) {
    case LiftedOperation::ADD:
        return std::make_shared<BinaryExpressionNode>("+", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));
    case LiftedOperation::SUB:
        return std::make_shared<BinaryExpressionNode>("-", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));
    case LiftedOperation::MUL:
        return std::make_shared<BinaryExpressionNode>("*", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));
    case LiftedOperation::DIV:
        return std::make_shared<BinaryExpressionNode>("/", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));
    case LiftedOperation::MOD:
        return std::make_shared<BinaryExpressionNode>("%", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));
    case LiftedOperation::POW:
        return std::make_shared<BinaryExpressionNode>("^", LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));

    case LiftedOperation::ADDK:
    case LiftedOperation::SUBK:
    case LiftedOperation::MULK:
    case LiftedOperation::DIVK:
    case LiftedOperation::MODK:
    case LiftedOperation::POWK: {
        std::string op = "+";
        if (def->operation == LiftedOperation::SUBK)
            op = "-";
        else if (def->operation == LiftedOperation::MULK)
            op = "*";
        else if (def->operation == LiftedOperation::DIVK)
            op = "/";
        else if (def->operation == LiftedOperation::MODK)
            op = "%";
        else if (def->operation == LiftedOperation::POWK)
            op = "^";

        auto left = LiftExpression(def->operands[1]);
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[2].value.imm.k];
        auto right = std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
        return std::make_shared<BinaryExpressionNode>(op, left, right);
    }

    case LiftedOperation::NOT:
        return std::make_shared<UnaryExpressionNode>("not", LiftExpression(def->operands[1]));
    case LiftedOperation::MINUS:
        return std::make_shared<UnaryExpressionNode>("-", LiftExpression(def->operands[1]));
    case LiftedOperation::LENGTH:
        return std::make_shared<UnaryExpressionNode>("#", LiftExpression(def->operands[1]));

    case LiftedOperation::MOVE:
        return LiftExpression(def->operands[1], forceExpression);

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

        std::shared_ptr<Expression> expr = nullptr;
        for (const auto &op : operands) {
            auto part = LiftExpression(op);
            expr = expr ? std::make_shared<BinaryExpressionNode>("..", expr, part) : part;
        }
        return expr ? expr : std::make_shared<StringLiteralNode>("");
    }

    case LiftedOperation::NEWTABLE:
        return LiftTableLiteral(*def);

    case LiftedOperation::CALL:
    case LiftedOperation::NAMECALL:
        return LiftCall(*def, def->instructionIndex, true);

    case LiftedOperation::GETGLOBAL: {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[1].value.imm.k];
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(std::get<std::string>(k.constantData)));
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

        std::shared_ptr<Expression> curr = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(parts[0]));
        for (size_t i = 1; i < parts.size(); ++i)
            curr = std::make_shared<MemberExpressionNode>(curr, parts[i]);
        return curr;
    }
    case LiftedOperation::GETTABLE:
        return std::make_shared<IndexExpressionNode>(LiftExpression(def->operands[1]), LiftExpression(def->operands[2]));

    case LiftedOperation::GETTABLEKS: {
        const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[def->operands[2].value.imm.k];
        return std::make_shared<MemberExpressionNode>(LiftExpression(def->operands[1]), std::get<std::string>(k.constantData));
    }

    default:
        return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(operand)));
    }
}

std::shared_ptr<Expression> ASTLifter::LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested) {
    if (inst.operation == LiftedOperation::CALL && instructionIndex >= 2) {
        const auto &prev = m_currentFunction->lpLiftedFunction->instructions[instructionIndex - 2];
        if (prev.operation == LiftedOperation::NAMECALL && prev.operands[0].value.reg == inst.operands[0].value.reg) {
            return LiftCall(prev, instructionIndex - 2, isNested);
        }
    }

    bool isNameCall = (inst.operation == LiftedOperation::NAMECALL);
    int32_t callInfoIndex = isNameCall ? instructionIndex + 2 : instructionIndex;

    if (static_cast<size_t>(callInfoIndex) >= m_currentFunction->lpLiftedFunction->instructions.size())
        return std::make_shared<NilLiteralNode>();

    const auto &callInfoInst = m_currentFunction->lpLiftedFunction->instructions[callInfoIndex];
    int regFunc = callInfoInst.operands[0].value.reg;

    std::vector<std::shared_ptr<Expression>> args;
    std::shared_ptr<Expression> callee;
    bool isVararg = false;

    if (isNameCall)
        callee = LiftExpression(inst.operands[1], false);
    else
        callee = LiftExpression(inst.operands[0], false);

    if (callee->nodeKind == ASTNodeKind::LiteralValue) {
        auto literal = std::dynamic_pointer_cast<LiteralNode>(callee);
        if (literal != nullptr)
            literal->bUseParenthesis = true;
    }

    if (m_currentFunction->implicitUses.contains(&callInfoInst)) {
        const auto &argVersions = m_currentFunction->implicitUses.at(&callInfoInst);
        int startOffset = isNameCall ? 1 : 0;
        for (size_t k = startOffset; k < argVersions.size(); ++k) {
            LiftedOperand op;
            op.type = LiftedOperandType::Register;
            op.value.reg = regFunc + 1 + k;
            op.ssaVersion = argVersions[k];

            auto def = m_currentFunction->GetDefinition(op);
            if (def && (def->operation == LiftedOperation::CALL || def->operation == LiftedOperation::NAMECALL)) {
                int32_t actualCallIdx = (def->operation == LiftedOperation::NAMECALL) ? def->instructionIndex + 2 : def->instructionIndex;
                const auto &defCallInfo = m_currentFunction->lpLiftedFunction->instructions[actualCallIdx];
                if (defCallInfo.operands[2].value.imm.n == 0) {
                    m_processedInstructions.insert(def->instructionIndex);
                    if (def->operation == LiftedOperation::NAMECALL) {
                        m_processedInstructions.insert(def->instructionIndex + 1);
                        m_processedInstructions.insert(def->instructionIndex + 2);
                    }
                    args.push_back(LiftCall(*def, def->instructionIndex, true));
                    break;
                }
            }
            if (def && def->operation == LiftedOperation::GETVARARGS) {
                isVararg = true;
                args.push_back(std::make_shared<VarArgExpression>()); // var arg may be present in the middle of arguments, unfunny.
                continue;
            }
            args.push_back(LiftExpression(op, false));
        }
    }

    int32_t prevIdx = instructionIndex - 1;
    if (prevIdx >= 0) {
        const auto &prevInst = m_currentFunction->lpLiftedFunction->instructions[prevIdx];
        if (prevInst.operation == LiftedOperation::CALL && prevInst.operands[2].value.imm.n == 0) {
            if (!m_processedInstructions.contains(prevIdx)) {
                m_processedInstructions.insert(prevIdx);
                args.push_back(LiftCall(prevInst, prevIdx, true));
            }
        }
    }

    std::vector<std::shared_ptr<Expression>> rets;
    if (isNameCall) {
        auto kIdx = inst.operands[2].value.imm.k;
        std::string method = std::get<std::string>(m_currentFunction->lpLiftedFunction->lpDeserialized->constants.at(kIdx).constantData);
        return std::make_shared<NameCallExpressionNode>(
            callee, std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(method)), args, rets, isVararg, isNested
        );
    } else {
        return std::make_shared<CallExpressionNode>(callee, args, rets, isVararg, isNested);
    }
}

std::shared_ptr<TableLiteralNode> ASTLifter::LiftTableLiteral(const LiftedInstruction &inst) {
    std::vector<std::shared_ptr<Expression>> elements;
    int32_t tableReg = inst.operands[0].value.reg;
    size_t scanLimit = 100;
    size_t maxIdx = m_currentFunction->lpLiftedFunction->instructions.size();

    for (size_t i = inst.instructionIndex + 1; i < inst.instructionIndex + scanLimit && i < maxIdx; ++i) {
        const auto &candidate = m_currentFunction->lpLiftedFunction->instructions[i];

        if (candidate.operation == LiftedOperation::JUMP || candidate.operation == LiftedOperation::JUMPIF ||
            candidate.operation == LiftedOperation::JUMPIFNOT || candidate.operation == LiftedOperation::RETURN ||
            candidate.operation == LiftedOperation::BREAK)
            break;

        bool isInitializer = false;

        if (candidate.operation == LiftedOperation::SETLIST) {
            if (candidate.operands[0].value.reg == tableReg) {
                isInitializer = true;

                if (m_currentFunction->implicitUses.contains(&candidate)) {
                    const auto &versions = m_currentFunction->implicitUses.at(&candidate);
                    int startReg = candidate.operands[1].value.reg;

                    for (size_t k = 0; k < versions.size(); ++k) {
                        LiftedOperand itemOp;
                        itemOp.type = LiftedOperandType::Register;
                        itemOp.value.reg = startReg + k;
                        itemOp.ssaVersion = versions[k];
                        elements.push_back(LiftExpression(itemOp));
                    }
                }
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEKS) {
            if (candidate.operands[1].value.reg == tableReg) {
                isInitializer = true;

                auto kIdx = candidate.operands[2].value.imm.k;
                const auto &k = m_currentFunction->lpLiftedFunction->lpDeserialized->constants[kIdx];
                std::string keyStr = std::get<std::string>(k.constantData);

                auto keyExpr = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(keyStr));
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
            }
        } else if (candidate.operation == LiftedOperation::SETTABLEN) {
            if (candidate.operands[1].value.reg == tableReg) {
                isInitializer = true;
                int idx = candidate.operands[2].value.imm.n + 1;

                auto keyExpr = std::make_shared<MemberExpressionNode>(std::make_shared<NumberLiteralNode>(idx));
                auto valExpr = LiftExpression(candidate.operands[0]);

                // SETTABLEN opcodes are emitted sometimes where
                // local v = 2
                // t[v] = n
                // this emits as
                // SETTABLEN TABLEREG, VALUEREG, INDEX (i dont care for the ordering, cope reader).
                // because of this, we have to imply that the keyExpr will be a literal ['']

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
            }
        } else if (candidate.operation == LiftedOperation::SETTABLE) {
            if (candidate.operands[1].value.reg == tableReg) {
                isInitializer = true;

                auto keyExpr = LiftExpression(candidate.operands[2]);
                auto valExpr = LiftExpression(candidate.operands[0]);

                elements.push_back(std::make_shared<BinaryExpressionNode>("=", keyExpr, valExpr));
            }
        }

        if (isInitializer) {
            m_processedInstructions.insert(candidate.instructionIndex);
        }
    }

    return std::make_shared<TableLiteralNode>(elements);
}

bool ASTLifter::ShouldInline(const LiftedInstruction *inst) {
    if (!inst || inst->operands.size() < 1)
        return false;

    // these statements have side-effects which cannot be skipped.
    // the values they produce aren't inlineable in any way, doing so would break them anyway!
    switch (inst->operation) {
    case LiftedOperation::RETURN:
    case LiftedOperation::SETGLOBAL:
    case LiftedOperation::SETUPVAL:
    case LiftedOperation::SETTABLE:
    case LiftedOperation::SETTABLEKS:
    case LiftedOperation::SETTABLEN:
    case LiftedOperation::SETLIST:
        return false;
    default:
        break;
    }

    if (inst->operands.size() > 0 && inst->operands[0].type == LiftedOperandType::Register &&
        (inst->operation != LiftedOperation::MOVE && m_currentFunction->IsSingleUse(inst->operands[0]))) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_phiConsumers.contains(defRef))
            return false;
    }

    if (inst->operation == LiftedOperation::NEWTABLE) {
        SSARef defRef{inst->operands[0].value.reg, inst->operands[0].ssaVersion};
        if (m_currentFunction->useCounts.contains(defRef)) {
            const auto &users = m_currentFunction->users[defRef];
            int realUses = 0;
            for (const auto *user : users) {
                if (user->operation == LiftedOperation::SETLIST && user->operands[0].value.reg == inst->operands[0].value.reg)
                    continue;

                if (user->operation == LiftedOperation::SETLIST && user->operands[0].value.reg != inst->operands[0].value.reg)
                    return true; // two SETLIST references means that this is a nested table. Nested tables are to be inlined.

                if (user->operation == LiftedOperation::MOVE)
                    // MOVE instructions points to the table being reused. It cannot be inlined because of this.
                    return false;
                realUses++;
            }
            // there is exactly one real user of the register, the rest are product of the syntactic sugar, inline it.
            return realUses == 1;
        }
    }

    if (inst->operation == LiftedOperation::CALL || inst->operation == LiftedOperation::NAMECALL) {
        if (inst->operation == LiftedOperation::CALL && inst->operands[2].value.imm.n == 0)
            return true;

        if (inst->operation == LiftedOperation::NAMECALL) {
            int regA = inst->operands[0].value.reg;

            if (inst->operation == LiftedOperation::CALL && inst->operands[2].value.imm.n == 0)
                return true;

            for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
                if (defInst == inst && ref.regIndex == regA) {
                    auto users = m_currentFunction->users[{static_cast<uint8_t>(regA), ref.version}];
                    if (users.size() == 1) {
                        auto op = users[0]->operation;
                        // allow inlining returns, other Calls, and arith ops.
                        if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::NAMECALL || op == LiftedOperation::ADD ||
                            op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV || op == LiftedOperation::MOD ||
                            op == LiftedOperation::POW || op == LiftedOperation::CONCAT || op == LiftedOperation::MINUS || op == LiftedOperation::NOT ||
                            op == LiftedOperation::LENGTH) {
                            return true;
                        }
                    }
                    return false;
                }
            }

            return false;
        }

        int usedDefs = 0;
        SSARef usedRef;

        for (const auto &[ref, defInst] : m_currentFunction->definitionMap) {
            if (defInst == inst) {
                if (m_currentFunction->useCounts[ref] > 0) {
                    usedDefs++;
                    usedRef = ref;
                }
            }
        }

        if (usedDefs > 1)
            return false;
        if (usedDefs == 0)
            return false;

        auto users = m_currentFunction->users[usedRef];
        if (users.size() == 1) {
            auto op = users[0]->operation;
            if (op == LiftedOperation::RETURN || op == LiftedOperation::CALL || op == LiftedOperation::NAMECALL || op == LiftedOperation::ADD ||
                op == LiftedOperation::SUB || op == LiftedOperation::MUL || op == LiftedOperation::DIV || op == LiftedOperation::MOD ||
                op == LiftedOperation::POW || op == LiftedOperation::CONCAT || op == LiftedOperation::MINUS || op == LiftedOperation::NOT ||
                op == LiftedOperation::LENGTH || op == LiftedOperation::JUMPIFEQ || op == LiftedOperation::JUMPIFNOTEQ || op == LiftedOperation::JUMPIFLT ||
                op == LiftedOperation::JUMPIFNOTLT || op == LiftedOperation::JUMPIFLE || op == LiftedOperation::JUMPIFNOTLE || op == LiftedOperation::JUMPIF ||
                op == LiftedOperation::JUMPIFNOT || op == LiftedOperation::JUMPXEQK) {
                return true;
            }
        }
        return false;
    }

    if (m_currentFunction->IsSimpleOrConstant(inst->operands[0]) && !m_currentFunction->IsConsumedByPhi(inst->operands[0]))
        return true;

    if (m_currentFunction->IsSingleUse(inst->operands[0])) {
        if (m_currentFunction->IsConsumedByPhi(inst->operands[0]))
            return false;
        if (inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE)
            return false; // do not omit NEWCLOSURE and DUPCLOSURE.
        return true;
    }

    return false;
}

std::string ASTLifter::ResolveVariableName(const LiftedOperand &op) {
    if (op.type != LiftedOperandType::Register)
        return "err_not_reg";

    m_definedRegisters.insert(op.value.reg);
    std::string name = m_currentFunction->GetVarName(op.value.reg, op.ssaVersion);
    if (name.empty())
        return std::format("v{}", op.value.reg);

    return name;
}

int32_t ASTLifter::FindMergeBlock(uint32_t branchA, uint32_t branchB) {
    if (branchA == branchB)
        return static_cast<int32_t>(branchA);

    if (this->m_currentFunction->basicBlocks.at(branchA).bType == BlockType::Return ||
        this->m_currentFunction->basicBlocks.at(branchB).bType == BlockType::Return)
        return -1; // no merge block when one of them is a return.

    std::set<uint32_t> reachableFromA;
    std::queue<uint32_t> q;

    q.push(branchA);
    reachableFromA.insert(branchA);
    while (!q.empty()) {
        uint32_t curr = q.front();
        q.pop();

        if (reachableFromA.size() > 2000u)
            break;

        const auto &block = m_currentFunction->basicBlocks[curr];
        for (uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (!reachableFromA.contains(succ)) {
                reachableFromA.insert(succ);
                q.push(succ);
            }
        }
    }

    if (reachableFromA.contains(branchB))
        return static_cast<int32_t>(branchB);

    std::queue<uint32_t> q2;
    q2.push(branchB);
    std::set<uint32_t> visitedB;
    visitedB.insert(branchB);

    while (!q2.empty()) {
        const uint32_t curr = q2.front();
        q2.pop();

        for (const auto &block = m_currentFunction->basicBlocks[curr]; uint32_t succ : block.successors) {
            if (block.bType == BlockType::LoopLatch && succ < curr)
                continue;

            if (reachableFromA.contains(succ))
                return static_cast<int32_t>(succ);

            if (!visitedB.contains(succ)) {
                visitedB.insert(succ);
                q2.push(succ);
            }
        }
    }

    return -1;
}