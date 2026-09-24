//
// Created by Dottik on 30/11/2025.
//

#include "ControlFlowAnalyzer.hpp"

#include "DenominatorAnalysis.hpp"
#include "Deserializer.hpp"
#include "SSABuilder.hpp"
#include "SafetyGuard.hpp"

#include <libassert/assert.hpp>

bool ControlFlowAnalyzer::IsTerminator(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::JUMP:
    case LiftedOperation::LOADNJUMP:
        return true;
    case LiftedOperation::JUMPIF:
    case LiftedOperation::JUMPIFNOT:
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPXEQK:
    case LiftedOperation::CMPPROTO:
        return true;

    case LiftedOperation::FORNLOOP:
    case LiftedOperation::FORGLOOP:
        return true;

    case LiftedOperation::RETURN:
        return true;

        // FORNLOOP and FORGLOOP targets must remain leaders or tail checks discard their blocks.
    case LiftedOperation::FORGPREP_INEXT:
    case LiftedOperation::FORGPREP_NEXT:
    case LiftedOperation::FORGPREP:
    case LiftedOperation::FORNPREP:
        return false;

    default:
        return false;
    }
}

int32_t ControlFlowAnalyzer::GetJumpOffset(const LiftedInstruction *lpInstruction) {
    if (lpInstruction->operation == LiftedOperation::NOP)
        return 1;
    ASSERT(!lpInstruction->operands.empty(), "no operands available, cannot calculate jump offset.");

    switch (lpInstruction->operation) {
    case LiftedOperation::JUMP:
        return lpInstruction->operands[0].value.imm.n;

    case LiftedOperation::LOADNJUMP:
        return lpInstruction->operands[2].value.imm.n;

    case LiftedOperation::FORNPREP:
        return lpInstruction->operands[1].value.imm.n;
    case LiftedOperation::FORGPREP:
    case LiftedOperation::FORGPREP_INEXT:
    case LiftedOperation::FORGPREP_NEXT:
        return lpInstruction->operands[1].value.imm.n + 1;

    case LiftedOperation::FORNLOOP:
    case LiftedOperation::FORGLOOP:
        return lpInstruction->operands[1].value.imm.n;

    case LiftedOperation::JUMPXEQK:
        return lpInstruction->operands[1].value.imm.n;

    case LiftedOperation::CMPPROTO:
        return lpInstruction->operands[1].value.imm.n;

    case LiftedOperation::JUMPIF:
    case LiftedOperation::JUMPIFNOT:
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTLT:
        return lpInstruction->operands[1].value.imm.n + 1;

    default:
        return 0;
    }
}

int32_t ControlFlowAnalyzer::GetBlockIdAtInstruction(const LiftedInstruction *lpTargetInstruction, const std::map<LiftedInstruction *, int32_t> &leaderMap) {
    auto it = leaderMap.find(const_cast<LiftedInstruction *>(lpTargetInstruction));
    // A target between instructions or on an AUX word is malformed bytecode.
    if (it == leaderMap.end())
        throw Fission::DecompilerError("malformed bytecode: jump target is not a basic-block leader");
    return it->second;
}

void ControlFlowAnalyzer::LinkBasicBlocks(std::vector<BasicBlock> &blocks) {
    // lookup map (Leader Instruction -> Block ID)
    // speeds up finding the block id for leaders, else this shit will be so slow we'll cry on big scripts.
    std::map<LiftedInstruction *, int32_t> leaderToBlockId;
    for (const auto &block : blocks) {
        leaderToBlockId[block.lpHead] = block.dwBlockId;
    }

    for (size_t i = 0; i < blocks.size(); i++) {
        BasicBlock &currentBlock = blocks[i];

        std::vector<LiftedInstruction *> nextInstructions;

        switch (currentBlock.bTerminator) {
        case BlockTerminator::Fallthrough:
            if (i + 1 < blocks.size())
                nextInstructions.push_back(currentBlock.lpTail + 1);
            break;

        case BlockTerminator::Unconditional: {
            int32_t offset = GetJumpOffset(currentBlock.lpTail);
            nextInstructions.push_back((currentBlock.lpTail) + offset);
            break;
        }

        case BlockTerminator::Conditional: {
            if ((currentBlock.lpTail->operation == LiftedOperation::FORNLOOP || currentBlock.lpTail->operation == LiftedOperation::FORGLOOP) &&
                currentBlock.bType == BlockType::LoopLatch) {
                int32_t offset = GetJumpOffset(currentBlock.lpTail);
                nextInstructions.push_back((currentBlock.lpTail) + offset);

                currentBlock.loopHeader = GetBlockIdAtInstruction((currentBlock.lpTail) + offset, leaderToBlockId);
                blocks.at(*currentBlock.loopHeader).loopLatch = i; // current block is exit for the loop.
                ExplainDetail(currentBlock, "link: {} back-edge selects B{} as loop header", OperationToString(currentBlock.lpTail->operation), *currentBlock.loopHeader);
                ExplainDetail(blocks.at(*currentBlock.loopHeader), "link: B{} {} selects this block as loop header", i, OperationToString(currentBlock.lpTail->operation));

                nextInstructions.push_back(currentBlock.lpTail + 1);
                currentBlock.loopLatch = i /* self */;
                break;
            }

            if (currentBlock.lpTail->operation == LiftedOperation::JUMPXEQK) {
                ASSERT(currentBlock.lpTail->operands.size() == 4, "missized operands for JUMPXEQK");
                auto isNot = currentBlock.lpTail->operands[3].value.imm.b;

                int32_t offset = GetJumpOffset(currentBlock.lpTail);
                // LiftCondition includes the NOT flag, so jump targets always represent true branches.
                (void)isNot;
                currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                nextInstructions.push_back(currentBlock.lpTail + 1);
                nextInstructions.push_back((currentBlock.lpTail) + offset);
            } else {
                int32_t offset = GetJumpOffset(currentBlock.lpTail);

                switch (currentBlock.lpTail->operation) {
                case LiftedOperation::JUMPIFNOT:
                default:
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    break;
                case LiftedOperation::JUMPIF:
                    // JUMPIF targets the true branch of LiftCondition's unchanged expression.
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    break;

                case LiftedOperation::JUMPIFEQ:
                case LiftedOperation::JUMPIFLE:
                    // Positive comparisons jump when LiftCondition is true.
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    break;

                case LiftedOperation::JUMPIFNOTEQ:
                case LiftedOperation::JUMPIFNOTLE:
                case LiftedOperation::JUMPIFNOTLT:
                case LiftedOperation::FORGLOOP: // uses aux.
                case LiftedOperation::JUMPIFLT:
                case LiftedOperation::CMPPROTO: // V11. Fallthrough = proto matches, jump = proto does not match.
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    break;
                }
            }
            break;
        }

        case BlockTerminator::Return:
            break;

        default:
            break;
        }

        for (LiftedInstruction *targetInst : nextInstructions) {
            int32_t targetBlockId = GetBlockIdAtInstruction(targetInst, leaderToBlockId);
            ASSERT(targetBlockId != -1, "bad parsing or invalid bytecode");

            if (std::ranges::find(currentBlock.successors, targetBlockId) == currentBlock.successors.end())
                currentBlock.successors.push_back(targetBlockId);

            auto &predecessors = blocks[targetBlockId].predecessors;
            if (std::ranges::find(predecessors, currentBlock.dwBlockId) == predecessors.end())
                predecessors.push_back(currentBlock.dwBlockId);
        }

        if (currentBlock.ifStatementTrue && currentBlock.ifStatementFalse) {
            ExplainDetail(
                currentBlock, "link: {} maps true to B{} and false to B{}", OperationToString(currentBlock.lpTail->operation), *currentBlock.ifStatementTrue,
                *currentBlock.ifStatementFalse
            );
        } else if (currentBlock.successors.size() == 1) {
            ExplainDetail(
                currentBlock, "link: {} edge targets B{}", currentBlock.bTerminator == BlockTerminator::Fallthrough ? "fallthrough" : "jump",
                currentBlock.successors.front()
            );
        } else if (currentBlock.successors.empty()) {
            ExplainDetail(currentBlock, "link: no successor; path terminates here");
        }
    }
    if (!blocks.empty() && blocks.back().bTerminator == BlockTerminator::Fallthrough) {
        std::vector<bool> visited(blocks.size(), false);
        std::vector<uint32_t> pending{blocks.back().dwBlockId};
        while (!pending.empty()) {
            Fission::CheckDecompileDeadline();
            const auto id = pending.back();
            pending.pop_back();
            if (id == 0)
                throw Fission::DecompilerError("malformed bytecode: reachable fallthrough past the instruction stream");
            if (visited[id])
                continue;
            visited[id] = true;
            pending.insert(pending.end(), blocks[id].predecessors.begin(), blocks[id].predecessors.end());
        }
    }
}

std::vector<BasicBlock> ControlFlowAnalyzer::PartitionBlocks(LiftedFunction *lpLiftedFunction) {
    std::vector<BasicBlock> basicBlocks;

    std::set<size_t> leaderIndexes;
    leaderIndexes.insert(0); // the first instruction of the function is a leader, as it's the start of a simple standard block.
    size_t totalInstructions = lpLiftedFunction->instructions.size();

    for (size_t currentIndex = 0; currentIndex < totalInstructions; ++currentIndex) {
        auto instruction = &lpLiftedFunction->instructions.at(currentIndex);
        if (this->IsTerminator(instruction->operation)) {
            if (currentIndex + 1 < totalInstructions)
                leaderIndexes.insert(currentIndex + 1);

            // FORNLOOP and FORGLOOP targets must start blocks because other branches can skip to them.
            if (instruction->operation == LiftedOperation::FORNLOOP || instruction->operation == LiftedOperation::FORGLOOP ||
                instruction->operation == LiftedOperation::FORNPREP || instruction->operation == LiftedOperation::FORGPREP ||
                instruction->operation == LiftedOperation::FORGPREP_INEXT || instruction->operation == LiftedOperation::FORGPREP_NEXT ||
                instruction->operation == LiftedOperation::JUMP) {
                leaderIndexes.insert(currentIndex);
            }

            if (instruction->operation != LiftedOperation::RETURN) {
                const int32_t offset = GetJumpOffset(instruction);
                int64_t targetIndex = static_cast<int64_t>(currentIndex) + offset;
                if (targetIndex >= 0 && targetIndex < static_cast<int64_t>(totalInstructions)) {
                    leaderIndexes.insert(static_cast<size_t>(targetIndex));
                }
            }
        }
    }

    // FORXPREP needs leaders for both body entry and loop exit despite not being a terminator.
    for (size_t currentIndex = 0; currentIndex < totalInstructions; ++currentIndex) {
        auto instruction = &lpLiftedFunction->instructions.at(currentIndex);
        if (instruction->operation == LiftedOperation::FORNPREP || instruction->operation == LiftedOperation::FORGPREP ||
            instruction->operation == LiftedOperation::FORGPREP_INEXT || instruction->operation == LiftedOperation::FORGPREP_NEXT) {
            if (currentIndex + 1 < totalInstructions)
                leaderIndexes.insert(currentIndex + 1);
            const int32_t offset = GetJumpOffset(instruction);
            int64_t targetIndex = static_cast<int64_t>(currentIndex) + offset;
            if (targetIndex >= 0 && targetIndex < static_cast<int64_t>(totalInstructions))
                leaderIndexes.insert(static_cast<size_t>(targetIndex));
        }
    }

    int32_t blockIdCounter = 0;
    auto it = leaderIndexes.begin();

    while (it != leaderIndexes.end()) {
        Fission::CheckDecompileDeadline();
        size_t startIndex = *it;
        ++it; // next leader is likely the end of the block.

        size_t endIndex = 0x0;
        if (it == leaderIndexes.end()) {
            endIndex = totalInstructions - 1;
        } else {
            endIndex = *it - 1;
        }

        BasicBlock block;
        block.dwBlockId = blockIdCounter++;
        block.lpHead = &lpLiftedFunction->instructions[startIndex];
        block.lpTail = &lpLiftedFunction->instructions[endIndex];
        LiftedInstruction *tailInst = block.lpTail;

        block.bType = BlockType::Standard;

        // Malformed bytecode may end with JUMP instead of RETURN.
        const LiftedInstruction *lpInstructionsEnd = lpLiftedFunction->instructions.data() + lpLiftedFunction->instructions.size();
        const bool hasNextInst = (tailInst + 1) < lpInstructionsEnd;

        switch (tailInst->operation) {
        case LiftedOperation::JUMP: {
            block.bTerminator = BlockTerminator::Unconditional;

            if (hasNextInst && ((tailInst + 1)->operation == LiftedOperation::FORNLOOP || (tailInst + 1)->operation == LiftedOperation::FORGLOOP)) {
                // FORXLOOP owns the back-edge; this forward jump leaves the loop.
                auto jmpOffset = GetJumpOffset(tailInst);
                if (jmpOffset > 0)
                    block.bType = BlockType::Break; // possibly breaking out of a loop.
                else
                    block.bType = BlockType::Continue; // possibly breaking out of a loop.

                ExplainDetail(block, "partition: JUMP before FORxLOOP uses offset {}; provisional {}", jmpOffset, jmpOffset > 0 ? "break" : "continue");

                break;
            }

            auto jmpOffset = GetJumpOffset(tailInst);
            if (jmpOffset < 0) {
                block.bType = BlockType::Continue;
                ExplainDetail(block, "partition: backward JUMP offset {} creates continue/back-edge candidate", jmpOffset);
            } else {
                // Dominance analysis later classifies forward jumps inside loops.
                ExplainDetail(block, "partition: forward JUMP offset {}; loop pass decides break role", jmpOffset);
            }

            break;
        }
        case LiftedOperation::LOADNJUMP: // DO NOT FUCKING ADD FORXPREP OPERATIONS HERE, THEY ARE NOT A TERMINATOR.
            block.bTerminator = BlockTerminator::Unconditional;
            if (GetJumpOffset(tailInst) < 0) {
                block.bType = BlockType::LoopLatch;
                ExplainDetail(block, "partition: LOADNJUMP offset {} creates loop-latch candidate", GetJumpOffset(tailInst));
            } else {
                ExplainDetail(block, "partition: LOADNJUMP offset {} is forward", GetJumpOffset(tailInst));
            }

            break;
        case LiftedOperation::JUMPXEQK:
            block.bTerminator = BlockTerminator::Conditional;
            if (GetJumpOffset(tailInst) < 0) {
                block.bType = BlockType::LoopLatch;
                ExplainDetail(block, "partition: backward JUMPXEQK offset {} creates loop-latch candidate", GetJumpOffset(tailInst));
            } else {
                block.bType = BlockType::IfHeader;
                ExplainDetail(block, "partition: forward JUMPXEQK offset {} creates conditional header", GetJumpOffset(tailInst));
            }
            break;

        case LiftedOperation::FORGPREP:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGPREP_NEXT:
        case LiftedOperation::FORNPREP: {
            block.bTerminator = BlockTerminator::Conditional;
            block.bType = BlockType::LoopHeader;
            ExplainDetail(block, "partition: {} is loop-preparation opcode", OperationToString(tailInst->operation));
            break;
        }
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
            block.bType = BlockType::IfHeader;
            block.bTerminator = BlockTerminator::Conditional;
            ExplainDetail(block, "partition: {} creates conditional header", OperationToString(tailInst->operation));
            break;
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            block.bTerminator = BlockTerminator::Conditional;
            if (GetJumpOffset(tailInst) < 0) {
                block.bType = BlockType::LoopLatch;
                ExplainDetail(block, "partition: {} offset {} creates loop latch", OperationToString(tailInst->operation), GetJumpOffset(tailInst));
            }
            break;
        case LiftedOperation::RETURN:
            block.bTerminator = BlockTerminator::Return;
            block.bType = BlockType::Return;
            ExplainDetail(block, "partition: RETURN terminates function path");
            break;
        default:
            block.bTerminator = BlockTerminator::Fallthrough;
            block.bType = BlockType::Standard;
            ExplainDetail(block, "partition: tail {} falls through", OperationToString(tailInst->operation));
            break;
        }

        basicBlocks.push_back(block);
    }
    return basicBlocks;
}

AnalyzedFunction ControlFlowAnalyzer::DetermineBasicBlocksInternal(LiftedFunction *lpLiftedFunction) {
    SetDebugFunction(lpLiftedFunction);
    auto basicBlocks = PartitionBlocks(lpLiftedFunction);

    std::vector<AnalyzedFunction> subfuncs;

    for (auto &func : lpLiftedFunction->subfunctions) {
        auto analyzed = DetermineBasicBlocksInternal(&func);
        subfuncs.push_back(analyzed);
    }

    return AnalyzedFunction{lpLiftedFunction, basicBlocks, {}, {}, subfuncs, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

void ControlFlowAnalyzer::OptimiseGraphInternal(std::vector<BasicBlock> &blocks) {
    bool changed = true;
    while (changed) {
        Fission::CheckDecompileDeadline();
        changed = false;

        for (auto &block : blocks) {
            if (block.bType == BlockType::Error)
                continue; // deleted/dead

            bool isEmpty = true;
            for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
                if (((inst->operation != LiftedOperation::NOP && !IsTerminator(inst->operation)) || inst->operation == LiftedOperation::JUMP ||
                     inst->operation == LiftedOperation::LOADNJUMP) ||
                    inst->operation == LiftedOperation::FORNLOOP || inst->operation == LiftedOperation::FORGLOOP) {
                    isEmpty = false; // meaningful operations present.
                    break;
                }
            }

            // we can only optimize the graph if the successor is only one, and it's unconditional/fallthrough, else we will fuck things up.
            if (isEmpty && block.successors.size() == 1) {
                int32_t currentId = block.dwBlockId;
                int32_t targetId = block.successors[0];

                if (currentId == 0)
                    continue;

                if (targetId == currentId)
                    continue;

                // Redirect predecessors before removing this block.

                std::vector<std::uint32_t> newPredecessors;
                for (int32_t predecesorId : block.predecessors) {
                    BasicBlock &predBlock = blocks[predecesorId];

                    for (size_t i = 0; i < predBlock.successors.size(); ++i) {
                        if (predBlock.successors[i] == static_cast<uint32_t>(currentId))
                            predBlock.successors[i] = targetId; // reassign to this block's target.
                    }

                    auto updateRef = [&](std::optional<uint32_t> &field) {
                        if (field.has_value() && *field == static_cast<uint32_t>(currentId))
                            field = targetId;
                    };
                    updateRef(predBlock.ifStatementTrue);
                    updateRef(predBlock.ifStatementFalse);
                    updateRef(predBlock.loopHeader);
                    updateRef(predBlock.loopLatch);
                    updateRef(predBlock.loopExit);

                    BasicBlock &ourSuccessor = blocks[targetId];
                    bool alreadyExists = false;
                    for (int32_t predecessors : ourSuccessor.predecessors) {
                        if (predecessors == predecesorId)
                            alreadyExists = true;
                    }

                    if (!alreadyExists)
                        ourSuccessor.predecessors.push_back(predecesorId); // add our predecessor to the successor.

                    BasicBlock &targetBlock = blocks[targetId];
                    auto &preds = targetBlock.predecessors;
                    std::erase(preds, currentId);
                }

                block.bType = BlockType::Dead; // mark dead
                Explain(block, "optimize: empty forwarding block removed; predecessors redirected to B{}", targetId);
                block.successors.clear();
                block.predecessors.clear();

                changed = true; // reanalyze graph.
            }
        }
    }
}
bool ControlFlowAnalyzer::IsConditional(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::JUMPIF:
    case LiftedOperation::JUMPIFNOT:
    case LiftedOperation::JUMPIFEQ:
    case LiftedOperation::JUMPIFLE:
    case LiftedOperation::JUMPIFLT:
    case LiftedOperation::JUMPIFNOTEQ:
    case LiftedOperation::JUMPIFNOTLE:
    case LiftedOperation::JUMPIFNOTLT:
    case LiftedOperation::JUMPXEQK:
    case LiftedOperation::CMPPROTO:
        return true;

    case LiftedOperation::FORGPREP:
    case LiftedOperation::FORGPREP_INEXT:
    case LiftedOperation::FORGPREP_NEXT:
    case LiftedOperation::FORNPREP:
    case LiftedOperation::FORNLOOP:
    case LiftedOperation::FORGLOOP:
        return true;
    default:
        return false;
    }
}

void ControlFlowAnalyzer::IdentifyStructuresInternal(AnalyzedFunction &func) {
    auto &blocks = func.basicBlocks;
    if (blocks.empty())
        return;

    const auto domInfo = AnalyzeDenominators(func);

    auto dominates = [&](int32_t header, int32_t latchCandidate) -> bool {
        int32_t cur = latchCandidate;
        while (cur != -1) {
            Fission::CheckDecompileDeadline();
            if (cur == header)
                return true;
            auto it = domInfo.find(cur);
            if (it == domInfo.end())
                return false;
            cur = it->second.idom;
        }
        return false;
    };

    auto reachesBefore = [&](uint32_t start, uint32_t target, uint32_t stop) {
        if (start >= blocks.size() || target >= blocks.size())
            return false;
        std::vector<bool> seen(blocks.size());
        std::queue<uint32_t> pending;
        pending.push(start);
        seen[start] = true;
        while (!pending.empty()) {
            Fission::CheckDecompileDeadline();
            const uint32_t current = pending.front();
            pending.pop();
            if (current == target)
                return true;
            if (current == stop)
                continue;
            for (const uint32_t next : blocks[current].successors)
                if (next < blocks.size() && !seen[next]) {
                    seen[next] = true;
                    pending.push(next);
                }
        }
        return false;
    };

    const LiftedInstruction *const firstInstruction = func.lpLiftedFunction->instructions.data();
    std::vector<int32_t> blockAtLeader(func.lpLiftedFunction->instructions.size() + 1, -1);
    for (const BasicBlock &b : blocks)
        if (b.lpHead && b.bType != BlockType::Dead)
            blockAtLeader[b.lpHead - firstInstruction] = static_cast<int32_t>(b.dwBlockId);

    // Luau leaves a while/repeat loop only through the instruction after its JUMPBACK; both the failing test and
    // every `break` target it. A multi-block condition (`while a or b`) exits from a block other than the header.
    auto exitAfterLatch = [&](const BasicBlock &latch, const BasicBlock &header) -> std::optional<uint32_t> {
        if (!latch.lpTail || !header.lpHead || latch.lpTail < header.lpHead)
            return std::nullopt;
        const auto &instructions = func.lpLiftedFunction->instructions;
        auto afterIndex = static_cast<size_t>(latch.lpTail - firstInstruction) + 1;
        // Luau threads a jump to the loop end through the forward JUMPs that follow it (the skip over an enclosing else)
        const auto exitAt = [&](size_t index) -> std::optional<uint32_t> {
            if (index >= blockAtLeader.size() || blockAtLeader[index] < 0)
                return std::nullopt;
            const auto &after = blocks[blockAtLeader[index]];
            for (const uint32_t pred : after.predecessors)
                if (pred < blocks.size() && blocks[pred].lpHead && blocks[pred].lpHead >= header.lpHead && blocks[pred].lpTail <= latch.lpTail)
                    return after.dwBlockId;
            return std::nullopt;
        };
        for (int hops = 0; hops < 8 && afterIndex < instructions.size(); ++hops) {
            if (const auto exit = exitAt(afterIndex))
                return exit;
            // a zero-length jump is lifted as NOP; the exit starts after it
            size_t next = afterIndex;
            while (next < instructions.size() && instructions[next].operation == LiftedOperation::NOP)
                ++next;
            if (next != afterIndex)
                if (const auto exit = exitAt(next))
                    return exit;
            if (next >= instructions.size() || instructions[next].operation != LiftedOperation::JUMP || GetJumpOffset(&instructions[next]) <= 0)
                break;
            afterIndex = next + static_cast<size_t>(GetJumpOffset(&instructions[next]));
        }
        return std::nullopt;
    };

    // An O2-inlined `return` leaves the loop through code placed inside it, past the natural exit. Every
    // non-returning exit then runs straight into the return label, which is the loop's real exit.
    auto joinedExit = [&](uint32_t exit, const BasicBlock &latch, const BasicBlock &header) -> uint32_t {
        const auto natural = exitAfterLatch(latch, header);
        if (!natural)
            return exit;
        std::vector<bool> region(blocks.size());
        std::vector<uint32_t> pending{latch.dwBlockId};
        region[header.dwBlockId] = region[latch.dwBlockId] = true;
        while (!pending.empty()) {
            const uint32_t current = pending.back();
            pending.pop_back();
            for (const uint32_t pred : blocks[current].predecessors)
                if (pred < blocks.size() && !region[pred] && pred > header.dwBlockId && pred < latch.dwBlockId) {
                    region[pred] = true;
                    pending.push_back(pred);
                }
        }
        const auto chain = [&](uint32_t start) {
            std::vector<uint32_t> path{start};
            for (int hops = 0; hops < 16 && blocks[path.back()].successors.size() == 1 && blocks[path.back()].successors[0] > path.back(); ++hops)
                path.push_back(blocks[path.back()].successors[0]);
            return path;
        };
        std::vector<std::vector<uint32_t>> exits;
        for (uint32_t id = header.dwBlockId; id <= latch.dwBlockId; ++id) {
            if (!region[id])
                continue;
            for (const uint32_t succ : blocks[id].successors) {
                if (succ >= blocks.size() || region[succ] || std::ranges::any_of(exits, [&](const auto &path) { return path.front() == succ; }))
                    continue;
                if (!blocks[succ].successors.empty())
                    exits.push_back(chain(succ));
            }
        }
        const bool bypassesNatural = std::ranges::any_of(exits, [&](const auto &path) {
            return path.front() < latch.dwBlockId && std::ranges::find(path, *natural) == path.end();
        });
        if (exits.size() < 2 || !bypassesNatural)
            return exit;
        for (const uint32_t candidate : exits.front()) {
            if (candidate <= latch.dwBlockId)
                continue;
            if (std::ranges::all_of(exits, [&](const auto &path) { return std::ranges::find(path, candidate) != path.end(); }))
                return candidate == *natural ? exit : candidate;
        }
        return exit;
    };

    // True when `start` re-enters `header` through another back-edge without first passing an enclosing loop's
    // header (which precedes it): the loops share this header, so `start` is inside the outer one, not an exit.
    auto returnsToSharedHeader = [&](uint32_t start, const BasicBlock &header, const BasicBlock &latch) {
        std::vector<bool> seen(blocks.size());
        std::vector<uint32_t> pending{start};
        while (!pending.empty()) {
            const uint32_t current = pending.back();
            pending.pop_back();
            if (current >= blocks.size() || seen[current] || current == latch.dwBlockId || current < header.dwBlockId)
                continue;
            seen[current] = true;
            for (const uint32_t next : blocks[current].successors) {
                if (next == header.dwBlockId)
                    return true;
                pending.push_back(next);
            }
        }
        return false;
    };

    // A break can bypass an otherwise predecessor-free back-edge in `while true`.
    for (BasicBlock &blk : blocks) {
        if (blk.bType != BlockType::Continue)
            continue;
        if (blk.bTerminator != BlockTerminator::Unconditional)
            continue;
        if (!blk.lpTail || blk.lpTail->operation != LiftedOperation::JUMP)
            continue;

        int32_t jmpOff = GetJumpOffset(blk.lpTail);
        if (jmpOff >= 0)
            continue;

        LiftedInstruction *targetInst = blk.lpTail + jmpOff;
        int32_t headerId = -1;
        for (const auto &b : blocks) {
            if (b.lpHead <= targetInst && targetInst <= b.lpTail) {
                headerId = b.dwBlockId;
                break;
            }
        }
        if (headerId < 0)
            continue;

        auto &header = blocks[headerId];

        // A back-edge into a FORxPREP header belongs to an enclosing while loop. Marking that header
        // as both loop types makes ASTLifter choose WhileLoop and drop the for body.
        const bool headerIsForPrep =
            header.lpTail && (header.lpTail->operation == LiftedOperation::FORNPREP || header.lpTail->operation == LiftedOperation::FORGPREP ||
                              header.lpTail->operation == LiftedOperation::FORGPREP_NEXT || header.lpTail->operation == LiftedOperation::FORGPREP_INEXT);
        if (headerIsForPrep) {
            blk.bType = BlockType::LoopLatch; // still surface the back-edge so the wrapper-detect succeeds
            Explain(blk, "structure: back-edge reaches FORxPREP B{}; keep latch visible without overwriting for-loop kind", headerId);
            Explain(header, "structure: B{} back-edge encloses this for header", blk.dwBlockId);
            continue;
        }

        blk.loopHeader = headerId;
        header.loopLatch = blk.dwBlockId;
        blk.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
        header.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
        blk.bType = BlockType::LoopLatch;
        Explain(blk, "structure: backward JUMP targets B{}; provisional while latch", headerId);
        Explain(header, "structure: B{} backward JUMP targets this block; provisional while header", blk.dwBlockId);

        // lifting reads the exit as a header successor; only a header that never exits itself takes the post-latch block
        // a successor that returns to the header through a sibling back-edge is still inside a loop
        std::optional<uint32_t> loopExit;
        for (const uint32_t succ : header.successors) {
            if (!reachesBefore(succ, blk.dwBlockId, header.dwBlockId) && !returnsToSharedHeader(succ, header, blk)) {
                loopExit = joinedExit(succ, blk, header);
                break;
            }
        }
        if (!loopExit)
            if (const auto natural = exitAfterLatch(blk, header))
                loopExit = joinedExit(*natural, blk, header);
        if (loopExit.has_value()) {
            blk.loopExit = loopExit.value();
            header.loopExit = loopExit.value();
            Explain(blk, "structure: B{} chosen as provisional loop exit; header successor cannot reach latch B{}", *loopExit, blk.dwBlockId);
            Explain(header, "structure: B{} chosen as provisional loop exit; it cannot reach latch B{}", *loopExit, blk.dwBlockId);
        } else {
            Explain(header, "structure: no header successor escapes latch B{}; loop considered infinite", blk.dwBlockId);
        }
    }

    for (BasicBlock &block : blocks) {
        if (block.bType == BlockType::Dead || block.bType == BlockType::Error)
            continue;

        for (uint32_t succ : block.successors) {
            if (succ == block.dwBlockId || dominates(succ, block.dwBlockId)) {
                if (blocks[succ].bType != BlockType::LoopHeader && blocks[succ].bType != BlockType::Break && blocks[succ].bType != BlockType::Continue)
                    blocks[succ].bType = BlockType::LoopHeader;

                Explain(blocks[succ], "structure: B{} -> B{} is back-edge because B{} dominates B{}", block.dwBlockId, succ, succ, block.dwBlockId);

                if (block.bType != BlockType::Return && block.bType != BlockType::Break) {
                    block.bType = BlockType::LoopLatch;
                    Explain(block, "structure: edge to dominating B{} makes this block a loop latch", succ);
                }
            }
        }
    }

    for (BasicBlock &block : blocks) {
        if (block.bType == BlockType::IfHeader) {
            for (int32_t pred : block.predecessors) {
                if (dominates(block.dwBlockId, pred)) {
                    block.bType = BlockType::LoopHeader;
                    Explain(block, "structure: conditional block dominates predecessor B{}; classify as loop header", pred);
                    break;
                }
            }
        }
    }

    // An unconditional self-jump represents a fused infinite-while header, body, and latch.
    for (BasicBlock &block : blocks) {
        if (block.bTerminator != BlockTerminator::Unconditional)
            continue;
        bool selfLoops = false;
        for (uint32_t succ : block.successors)
            if (succ == block.dwBlockId) {
                selfLoops = true;
                break;
            }
        if (!selfLoops)
            continue;

        block.bType = BlockType::LoopHeader;
        block.loopHeader = block.dwBlockId;
        block.loopLatch = block.dwBlockId;
        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
        Explain(block, "structure: unconditional self-edge forms infinite while header and latch");
        for (uint32_t succ : block.successors)
            if (succ != block.dwBlockId) {
                block.loopExit = succ;
                Explain(block, "structure: non-self successor B{} is infinite-loop exit", succ);
                break;
            }
    }

    // FOR loops pair a FORxPREP header with a FORxLOOP latch.
    for (BasicBlock &block : blocks) {
        if (block.bType != BlockType::LoopHeader)
            continue;

        // Optimized bytecode can place start, limit, and step loads before the FORxPREP terminator.
        if (block.lpTail->operation != LiftedOperation::FORNPREP && block.lpTail->operation != LiftedOperation::FORGPREP_INEXT &&
            block.lpTail->operation != LiftedOperation::FORGPREP && block.lpTail->operation != LiftedOperation::FORGPREP_NEXT)
            continue; // not supported by this pass.

        for (uint32_t succId : block.successors) {
            // Opcode pairing determines this relation without a dominance test.

            auto &successor = blocks.at(succId);
            if (successor.lpTail != successor.lpHead ||
                (successor.lpTail->operation != LiftedOperation::FORNLOOP && successor.lpTail->operation != LiftedOperation::FORGLOOP))
                continue; // not target
            // A threaded exit can reach an enclosing loop's latch; only the latch of this loop's registers pairs.
            if (successor.lpTail->operands.empty() || block.lpTail->operands.empty() ||
                successor.lpTail->operands[0].value.reg != block.lpTail->operands[0].value.reg)
                continue;

            auto loopFlag = LoopBlockFlags::WhileLoop;

            auto checkTarget = block.lpTail;

            if (checkTarget->operation == LiftedOperation::FORGPREP_INEXT) {
                loopFlag = LoopBlockFlags::ForGeneralLoop_Indexed;
            } else if (checkTarget->operation == LiftedOperation::FORGPREP_NEXT) {
                loopFlag = LoopBlockFlags::ForGeneralLoop_Pairs;
            } else if (checkTarget->operation == LiftedOperation::FORGPREP) {
                loopFlag = LoopBlockFlags::ForGeneralLoop;
            } else if (checkTarget->operation == LiftedOperation::FORNPREP) {
                loopFlag = LoopBlockFlags::ForNumericLoop;
            }

            if (loopFlag != LoopBlockFlags::WhileLoop) {
                successor.loopHeader = block.dwBlockId;
                block.loopLatch = successor.dwBlockId;
                block.dwBlockFlags |= static_cast<uint32_t>(loopFlag);
                successor.dwBlockFlags |= static_cast<uint32_t>(loopFlag);

                for (uint32_t succId2 : successor.successors) {
                    if (dominates(successor.dwBlockId, succId2)) {
                        block.loopExit = succId2;
                        successor.loopExit = succId2;
                        break;
                    }
                }
            }
        }

        // An unconditional break can prune the FORxLOOP latch. Recover the loop from FORxPREP's exit
        // target before an enclosing back-edge can misclassify the header.
        constexpr uint32_t kForMaskFP = static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
                                        static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                        static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed);
        if ((block.dwBlockFlags & kForMaskFP) == 0 && !block.lpTail->operands.empty()) {
            const int baseReg = block.lpTail->operands[0].value.reg;
            const int32_t headerIdx = block.lpTail->instructionIndex;
            const LiftedOperation wantLoop = (block.lpTail->operation == LiftedOperation::FORNPREP) ? LiftedOperation::FORNLOOP : LiftedOperation::FORGLOOP;
            const auto loopFlag = (block.lpTail->operation == LiftedOperation::FORGPREP_INEXT)  ? LoopBlockFlags::ForGeneralLoop_Indexed
                                  : (block.lpTail->operation == LiftedOperation::FORGPREP_NEXT) ? LoopBlockFlags::ForGeneralLoop_Pairs
                                  : (block.lpTail->operation == LiftedOperation::FORGPREP)      ? LoopBlockFlags::ForGeneralLoop
                                                                                                : LoopBlockFlags::ForNumericLoop;
            BasicBlock *latch = nullptr;
            for (BasicBlock &cand : blocks) {
                if (!cand.lpTail || cand.lpTail->operation != wantLoop || cand.lpTail->operands.empty())
                    continue;
                if (cand.lpTail->operands[0].value.reg != baseReg || cand.lpTail->instructionIndex <= headerIdx)
                    continue;
                if (!latch || cand.lpTail->instructionIndex < latch->lpTail->instructionIndex)
                    latch = &cand; // nearest FOR*LOOP after this prep with matching base register
            }
            if (latch) {
                latch->loopHeader = block.dwBlockId;
                block.loopLatch = latch->dwBlockId;
                block.dwBlockFlags |= static_cast<uint32_t>(loopFlag);
                latch->dwBlockFlags |= static_cast<uint32_t>(loopFlag);
                // Resolve loop fallthrough from the latch during lifting.
            }
        }
    }

    for (BasicBlock &block : blocks) {
        if (block.bType != BlockType::LoopLatch)
            continue; // we only need loop latches to fix the determination and mark the loop type.

        // Do not reclassify FORNLOOP or FORGLOOP latches as repeat-until.
        constexpr uint32_t kForLoopMask = static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
                                          static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                          static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed);
        if ((block.dwBlockFlags & kForLoopMask) != 0)
            continue;

        for (uint32_t succ : block.successors) {
            if (succ == block.dwBlockId || dominates(succ, block.dwBlockId)) {
                auto &successor = blocks.at(succ);
                const bool innermostLatch = !successor.loopLatch || block.lpTail->instructionIndex < blocks[*successor.loopLatch].lpTail->instructionIndex;
                // A separate back-edge into a for header belongs to an enclosing infinite while loop.
                if ((successor.dwBlockFlags & kForLoopMask) != 0) {
                    Explain(block, "structure: back-edge to for header B{} belongs to enclosing loop; do not replace for metadata", successor.dwBlockId);
                    continue;
                }

                // A back-edge through an inner loop exit belongs to an enclosing loop.
                if (successor.loopLatch && successor.loopExit && *successor.loopLatch != block.dwBlockId &&
                    blocks[*successor.loopLatch].lpTail->instructionIndex < block.lpTail->instructionIndex && dominates(*successor.loopExit, block.dwBlockId) &&
                    !dominates(*successor.loopExit, *successor.loopLatch)) {
                    block.loopHeader = successor.dwBlockId;
                    Explain(
                        block, "structure: back-edge reaches B{} through inner-loop exit B{}; preserve inner latch B{} and treat this as enclosing loop edge",
                        successor.dwBlockId, *successor.loopExit, *successor.loopLatch
                    );
                    Explain(
                        successor, "structure: B{} is enclosing back-edge through inner-loop exit B{}; primary latch remains nearer B{}", block.dwBlockId,
                        *successor.loopExit, *successor.loopLatch
                    );
                    continue;
                }
                // conditional jump.

                auto targetInstruction = block.lpTail + GetJumpOffset(block.lpTail);
                if (block.lpTail->operation == LiftedOperation::JUMP) {
                    BasicBlock *conditionalLatch = nullptr;
                    uint32_t conditionalExit = 0;
                    bool cleanBridge = true;
                    for (auto *instruction = block.lpHead; instruction && instruction < block.lpTail; ++instruction)
                        if (instruction->operation != LiftedOperation::NOP && instruction->operation != LiftedOperation::PHI) {
                            cleanBridge = false;
                            break;
                        }
                    for (const uint32_t predId : block.predecessors) {
                        if (!cleanBridge || block.predecessors.size() != 1)
                            break;
                        if (predId >= blocks.size())
                            continue;
                        auto &pred = blocks[predId];
                        if (pred.bTerminator != BlockTerminator::Conditional || pred.successors.size() != 2 || pred.loopLatch.has_value() ||
                            !dominates(successor.dwBlockId, predId))
                            continue;
                        const auto back = std::find(pred.successors.begin(), pred.successors.end(), block.dwBlockId);
                        if (back == pred.successors.end())
                            continue;
                        const uint32_t other = pred.successors[0] == block.dwBlockId ? pred.successors[1] : pred.successors[0];
                        if (reachesBefore(other, block.dwBlockId, successor.dwBlockId))
                            continue;
                        conditionalLatch = &pred;
                        conditionalExit = other;
                        Explain(
                            block, "structure: candidate repeat bridge; conditional B{} selects back-edge B{} versus exit B{}", pred.dwBlockId, block.dwBlockId,
                            other
                        );
                        break;
                    }

                    if (conditionalLatch && successor.bTerminator == BlockTerminator::Conditional)
                        for (const uint32_t headerSucc : successor.successors) {
                            if (headerSucc == conditionalExit)
                                continue;
                            bool reachesReturn = false;
                            std::vector<bool> seen(blocks.size());
                            std::queue<uint32_t> pending;
                            pending.push(headerSucc);
                            while (!pending.empty()) {
                                const uint32_t current = pending.front();
                                pending.pop();
                                if (current >= blocks.size() || current == block.dwBlockId || current == successor.dwBlockId || current == conditionalExit ||
                                    seen[current])
                                    continue;
                                seen[current] = true;
                                if (blocks[current].bType == BlockType::Return) {
                                    reachesReturn = true;
                                    break;
                                }
                                for (const uint32_t next : blocks[current].successors)
                                    pending.push(next);
                            }
                            if (reachesReturn) {
                                Explain(
                                    block,
                                    "structure: reject repeat candidate B{}; header arm B{} reaches return without passing latch B{} or exit B{}",
                                    conditionalLatch->dwBlockId, headerSucc, block.dwBlockId, conditionalExit
                                );
                                conditionalLatch = nullptr;
                                break;
                            }
                        }

                    if (conditionalLatch && conditionalExit < blocks.size() && blocks[conditionalExit].bTerminator == BlockTerminator::Unconditional &&
                        blocks[conditionalExit].lpTail && blocks[conditionalExit].lpTail->operation == LiftedOperation::JUMP &&
                        GetJumpOffset(blocks[conditionalExit].lpTail) > 0) {
                        Explain(
                            block, "structure: reject repeat candidate B{}; proposed exit B{} is forward-jump bridge", conditionalLatch->dwBlockId,
                            conditionalExit
                        );
                        conditionalLatch = nullptr;
                    }

                    if (conditionalLatch) {
                        successor.dwBlockFlags &= ~static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        successor.loopLatch = conditionalLatch->dwBlockId;
                        successor.loopExit = conditionalExit;
                        conditionalLatch->bType = BlockType::LoopLatch;
                        conditionalLatch->dwBlockFlags &= ~static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                        conditionalLatch->dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        conditionalLatch->loopHeader = successor.dwBlockId;
                        conditionalLatch->loopExit = conditionalExit;
                        block.dwBlockFlags &= ~(static_cast<uint32_t>(LoopBlockFlags::WhileLoop) |
                                                static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop));
                        block.loopExit.reset();
                        block.bType = BlockType::Continue;
                        Explain(
                            successor, "structure: conditional B{} before bridge B{} proves repeat-until; latch B{}, exit B{}", conditionalLatch->dwBlockId,
                            block.dwBlockId, conditionalLatch->dwBlockId, conditionalExit
                        );
                        Explain(*conditionalLatch, "structure: repeat condition exits to B{} and otherwise returns to header B{}", conditionalExit, successor.dwBlockId);
                        Explain(block, "structure: bridge into B{} demoted to continue after repeat latch moved to B{}", successor.dwBlockId, conditionalLatch->dwBlockId);
                        continue;
                    }

                    // A conditional header that jumps straight to the latch is a repeat-until body, unless its other arm
                    // stays in the loop (a multi-block `a or b` condition whose exit test comes later).
                    bool isRepeatUntil = false;
                    if (successor.bTerminator == BlockTerminator::Conditional && successor.successors.size() == 2) {
                        const auto direct = std::find(successor.successors.begin(), successor.successors.end(), block.dwBlockId);
                        if (direct != successor.successors.end()) {
                            const uint32_t other = successor.successors[0] == block.dwBlockId ? successor.successors[1] : successor.successors[0];
                            isRepeatUntil = other != block.dwBlockId && !reachesBefore(other, block.dwBlockId, successor.dwBlockId);
                        }
                    }

                    if (innermostLatch)
                        successor.loopLatch = block.dwBlockId;
                    block.loopHeader = successor.dwBlockId;
                    if (isRepeatUntil) {
                        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        Explain(block, "structure: header B{} branches directly to this latch; classify repeat-until", successor.dwBlockId);
                        Explain(successor, "structure: conditional header reaches latch B{} directly; classify repeat-until", block.dwBlockId);
                    } else {
                        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                        Explain(block, "structure: no post-body conditional proved repeat; retain while classification for header B{}", successor.dwBlockId);
                        Explain(successor, "structure: back-edge B{} lacks repeat proof; retain while classification", block.dwBlockId);
                    }
                } else {
                    auto lpPreHead = targetInstruction;
                    if (lpPreHead->operation == LiftedOperation::FORNPREP || lpPreHead->operation == LiftedOperation::FORGPREP ||
                        lpPreHead->operation == LiftedOperation::FORGPREP_NEXT || lpPreHead->operation == LiftedOperation::FORGPREP_INEXT) {
                        auto flags = LoopBlockFlags::WhileLoop;
                        if (lpPreHead->operation == LiftedOperation::FORGPREP_INEXT) {
                            flags = LoopBlockFlags::ForGeneralLoop_Indexed;
                        } else if (lpPreHead->operation == LiftedOperation::FORGPREP_NEXT) {
                            flags = LoopBlockFlags::ForGeneralLoop_Pairs;
                        } else if (lpPreHead->operation == LiftedOperation::FORGPREP) {
                            flags = LoopBlockFlags::ForGeneralLoop;
                        } else if (lpPreHead->operation == LiftedOperation::FORNPREP) {
                            flags = LoopBlockFlags::ForNumericLoop;
                        }

                        ASSERT(flags != LoopBlockFlags::WhileLoop, "Impossible.");

                        successor.loopLatch = block.dwBlockId;
                        block.loopHeader = successor.dwBlockId;
                        block.dwBlockFlags |= static_cast<uint32_t>(flags);
                        successor.dwBlockFlags |= static_cast<uint32_t>(flags);
                    } else {
                        successor.loopLatch = block.dwBlockId;
                        block.loopHeader = successor.dwBlockId;
                        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                    }
                }

                // A repeat-until latch stores its exit in the false branch.
                bool isRepeatUntil = (block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop;
                if (isRepeatUntil) {
                    // Condition stored in the latch.
                    if (block.ifStatementFalse.has_value()) {
                        block.loopExit = block.ifStatementFalse.value();
                        successor.loopExit = block.ifStatementFalse.value();
                    } else {
                        // Condition stored in the header; its non-latch successor is the exit.
                        size_t backEdges = 0;
                        for (uint32_t pred : successor.predecessors)
                            backEdges += pred < blocks.size() && blocks[pred].bType == BlockType::LoopLatch;
                        for (auto headerSucc : successor.successors) {
                            if (headerSucc != block.dwBlockId) {
                                if (backEdges > 1 || blocks.at(headerSucc).bType == BlockType::Return) {
                                    block.loopExit = headerSucc;
                                    if (innermostLatch)
                                        successor.loopExit = headerSucc;
                                }
                                break;
                            }
                        }
                        // Compound condition stored in body blocks.
                        if (!block.loopExit.has_value()) {
                            for (auto &b : blocks) {
                                if (b.dwBlockId == successor.dwBlockId || b.dwBlockId == block.dwBlockId)
                                    continue;
                                if (b.bType == BlockType::Return) {
                                    block.loopExit = b.dwBlockId;
                                    successor.loopExit = b.dwBlockId;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    if (!successor.ifStatementFalse && !successor.ifStatementTrue)
                        continue;

                    // Nested repeats can share a header; preserve the first repeat's exit.
                    if ((successor.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop)
                        continue;

                    std::optional<uint32_t> loopExit;
                    for (const uint32_t succId : successor.successors)
                        if (!reachesBefore(succId, block.dwBlockId, successor.dwBlockId) && !returnsToSharedHeader(succId, successor, block)) {
                            loopExit = joinedExit(succId, block, successor);
                            break;
                        }
                    if (!loopExit && block.lpTail->operation == LiftedOperation::JUMP)
                        if (const auto natural = exitAfterLatch(block, successor))
                            loopExit = joinedExit(*natural, block, successor);
                    if (loopExit.has_value()) {
                        block.loopExit = loopExit.value();
                        successor.loopExit = loopExit.value();
                    }
                }
            }
        }
    }

    for (BasicBlock &header : blocks) {
        if (header.bType != BlockType::LoopHeader ||
            (header.dwBlockFlags & static_cast<uint32_t>(LoopBlockFlags::WhileLoop)) == 0 || !header.loopLatch || !header.loopExit)
            continue;
        if (*header.loopExit >= blocks.size() || blocks[*header.loopExit].bType != BlockType::LoopHeader ||
            !reachesBefore(*header.loopExit, *header.loopLatch, header.dwBlockId))
            continue;
        for (const uint32_t succ : header.successors) {
            if (succ == *header.loopExit || (reachesBefore(succ, *header.loopLatch, header.dwBlockId) && blocks[*header.loopExit].loopExit != succ))
                continue;
            header.loopExit = succ;
            blocks[*header.loopLatch].loopExit = succ;
            break;
        }
    }

    // Classify forward jumps inside loops against the loop exit.
    for (BasicBlock &blk : blocks) {
        if (blk.bType != BlockType::Standard)
            continue;
        if (blk.bTerminator != BlockTerminator::Unconditional)
            continue;
        if (!blk.lpTail || blk.lpTail->operation != LiftedOperation::JUMP)
            continue;

        int32_t jmpOffset = GetJumpOffset(blk.lpTail);
        if (jmpOffset <= 0)
            continue; // backward jump, handled elsewhere

        LiftedInstruction *targetInst = blk.lpTail + jmpOffset;
        int32_t targetId = -1;
        for (const auto &b : blocks) {
            if (b.lpHead <= targetInst && targetInst <= b.lpTail) {
                targetId = b.dwBlockId;
                break;
            }
        }
        if (targetId < 0)
            continue;

        int32_t innermostHeader = -1;
        int32_t innermostExit = -1;
        int32_t innermostLatch = -1;
        for (const auto &b : blocks) {
            if (b.bType != BlockType::LoopHeader || !b.loopLatch)
                continue;
            if (!dominates(b.dwBlockId, blk.dwBlockId))
                continue;

            // Choose the deepest dominating loop header.
            bool deeper = (innermostHeader < 0);
            if (!deeper && dominates(innermostHeader, b.dwBlockId))
                deeper = true; // innermost dominates b -> b is deeper

            if (deeper) {
                innermostHeader = b.dwBlockId;
                innermostExit = blocks[*b.loopLatch].loopExit.value_or(-1);
                innermostLatch = static_cast<int32_t>(*b.loopLatch);
            }
        }

        if (innermostHeader < 0)
            continue; // not inside any loop

        // A jump over an else-arm to a merge inside the body is plain structure; only reaching the
        // latch (possibly through empty jump blocks) skips the rest of the iteration.
        const auto reachesLatchDirectly = [&](int32_t id) {
            for (int hops = 0; hops < 8 && id >= 0 && static_cast<size_t>(id) < blocks.size(); ++hops) {
                if (id == innermostLatch)
                    return true;
                const auto &b = blocks[id];
                if (b.successors.size() != 1 || !b.lpHead)
                    return false;
                for (const LiftedInstruction *inst = b.lpHead; inst <= b.lpTail; ++inst)
                    if (inst->operation != LiftedOperation::NOP && inst->operation != LiftedOperation::JUMP)
                        return false;
                id = static_cast<int32_t>(b.successors[0]);
            }
            return false;
        };

        if (innermostExit >= 0 && targetId == innermostExit) {
            blk.bType = BlockType::Break;
            Explain(blk, "structure: forward JUMP targets innermost loop exit B{}; classify break", targetId);
        } else if (dominates(innermostHeader, targetId) && reachesLatchDirectly(targetId)) {
            blk.bType = BlockType::Continue;
            Explain(blk, "structure: forward JUMP reaches loop latch B{} of header B{}; classify continue", innermostLatch, innermostHeader);
        }
    }

    // Classify forward jumps inside loops.
}

void ControlFlowAnalyzer::PruneUnreachableBlocks(std::vector<BasicBlock> &blocks) {
    std::vector<bool> reachable(blocks.size(), false);
    std::queue<int32_t> qq;
    qq.push(0);
    reachable[0] = true;

    while (!qq.empty()) {
        Fission::CheckDecompileDeadline();
        const int32_t id = qq.front();
        qq.pop();
        for (int32_t succ : blocks[id].successors) {
            if (!reachable[succ]) {
                reachable[succ] = true;
                qq.push(succ);
            }
        }
    }

    for (size_t i = 0; i < blocks.size(); ++i) {
        if (!reachable[i]) {
            blocks[i].bType = BlockType::Dead;
            Explain(blocks[i], "prune: no path from entry B0 reaches this block");
            blocks[i].successors.clear();
            for (auto &b : blocks) {
                std::erase(b.predecessors, i);
                std::erase(b.successors, i);
            }
        }
    }
}

void ControlFlowAnalyzer::DetermineBasicBlocksInternalAdvanced(AnalyzedFunction &func) {
    SetDebugFunction(func.lpLiftedFunction);
    this->LinkBasicBlocks(func.basicBlocks);
    for (auto &sub : func.innerFunctions) {
        this->DetermineBasicBlocksInternalAdvanced(sub);
    }
}

AnalyzedFunction ControlFlowAnalyzer::DetermineBasicBlocks(LiftedFunction *lpLiftedFunction) {
    DEBUG_ASSERT(lpLiftedFunction != nullptr);
    auto analyzed = DetermineBasicBlocksInternal(lpLiftedFunction);
    DetermineBasicBlocksInternalAdvanced(analyzed);
    return analyzed;
}

// Luau copies the `return` after an if-chain into every arm. When the copies return a variable the chain reads or
// assigns, the earlier copies become jumps to the last one, restoring the chain's shared exit.
bool ControlFlowAnalyzer::ConvergeReturns(AnalyzedFunction &func) {
    auto &blocks = func.basicBlocks;
    const auto dominators = AnalyzeDenominators(func);
    const auto idom = [&](int32_t id) {
        const auto it = dominators.find(id);
        return it == dominators.end() ? -1 : it->second.idom;
    };

    std::map<std::pair<int32_t, int32_t>, std::vector<uint32_t>> copies;
    for (const auto &block : blocks) {
        const auto *tail = block.lpTail;
        if (block.bType != BlockType::Dead && tail && tail->operation == LiftedOperation::RETURN && tail->operands.size() >= 2 &&
            tail->operands[0].type == LiftedOperandType::Register && tail->operands[1].value.imm.n >= 2 && dominators.contains(block.dwBlockId))
            copies[{tail->operands[0].value.reg, tail->operands[1].value.imm.n}].push_back(block.dwBlockId);
    }

    for (const auto &[shape, members] : copies) {
        if (members.size() < 2)
            continue;
        const int32_t first = shape.first, last = shape.first + shape.second - 2;
        const uint32_t kept = members.back();

        // a temporary each arm computes afresh is not a shared variable; an arm that only returns is a guard clause
        bool live = false, work = false;
        for (const uint32_t id : members) {
            std::set<int32_t> written;
            for (auto *inst = blocks[id].lpHead; inst <= blocks[id].lpTail; ++inst) {
                if (id != kept && inst->operation != LiftedOperation::NOP && inst->operation != LiftedOperation::RETURN)
                    work = true;
                for (size_t i = 0; i < inst->operands.size(); ++i) {
                    const auto &operand = inst->operands[i];
                    const auto access = SSABuilder::GetRegisterAccess(*inst, i);
                    if (operand.type == LiftedOperandType::Register && operand.value.reg >= first && operand.value.reg <= last &&
                        (access == AccessType::Read || access == AccessType::ReadWrite) && !written.contains(operand.value.reg))
                        live = true;
                }
                for (size_t i = 0; i < inst->operands.size(); ++i) {
                    const auto &operand = inst->operands[i];
                    const auto access = SSABuilder::GetRegisterAccess(*inst, i);
                    if (operand.type == LiftedOperandType::Register && operand.value.reg >= first && operand.value.reg <= last &&
                        (access == AccessType::Write || access == AccessType::ReadWrite))
                        written.insert(operand.value.reg);
                }
            }
        }
        if (!live || !work)
            continue;

        int32_t head = static_cast<int32_t>(members.front());
        for (const uint32_t id : members) {
            std::set<int32_t> ancestors;
            for (int32_t cursor = head; cursor != -1; cursor = idom(cursor))
                ancestors.insert(cursor);
            int32_t cursor = static_cast<int32_t>(id);
            while (cursor != -1 && !ancestors.contains(cursor))
                cursor = idom(cursor);
            head = cursor;
        }
        if (head < 0)
            continue;

        // the copies must hang off the chain through forward edges only; a loop in between would turn a copy into a break
        bool straight = std::ranges::none_of(blocks[head].predecessors, [&](uint32_t pred) { return pred >= static_cast<uint32_t>(head); });
        std::set<uint32_t> region(members.begin(), members.end());
        std::vector<uint32_t> pending(members.begin(), members.end());
        while (straight && !pending.empty()) {
            const uint32_t id = pending.back();
            pending.pop_back();
            if (std::ranges::any_of(blocks[id].successors, [&](uint32_t succ) { return succ <= id; }))
                straight = false;
            if (id == static_cast<uint32_t>(head))
                continue;
            for (const uint32_t pred : blocks[id].predecessors) {
                if (pred >= id)
                    straight = false;
                else if (region.insert(pred).second)
                    pending.push_back(pred);
            }
        }
        if (!straight)
            continue;

        auto *const instructions = func.lpLiftedFunction->instructions.data();
        const auto target = blocks[kept].lpTail - instructions;
        for (const uint32_t id : members) {
            if (id == kept)
                continue;
            auto *copy = blocks[id].lpTail;
            copy->operation = LiftedOperation::JUMP;
            copy->operands.assign(1, LiftedOperand{});
            copy->operands[0].type = LiftedOperandType::ImmediateInteger;
            copy->operands[0].value.imm.n = static_cast<int32_t>(target - (copy - instructions));
            copy->instructionRemarks = std::format("INFO: copied return converges on PC {}", target);
        }
        Explain(blocks[kept], "optimize: {} copies of this return converge here", members.size());
        return true;
    }
    return false;
}

void ControlFlowAnalyzer::OptimizeGraph(AnalyzedFunction &func) {
    SetDebugFunction(func.lpLiftedFunction);
    for (int round = 0; round < 64 && this->ConvergeReturns(func); ++round) {
        func.basicBlocks = this->PartitionBlocks(func.lpLiftedFunction);
        this->LinkBasicBlocks(func.basicBlocks);
    }
    this->OptimiseGraphInternal(func.basicBlocks);
    for (auto &f : func.innerFunctions)
        this->OptimizeGraph(f);
}

void ControlFlowAnalyzer::PruneUnreachable(AnalyzedFunction &func) {
    SetDebugFunction(func.lpLiftedFunction);
    this->PruneUnreachableBlocks(func.basicBlocks);
    for (auto &f : func.innerFunctions)
        this->PruneUnreachable(f);
}

void ControlFlowAnalyzer::IdentifyStructures(AnalyzedFunction &func) {
    SetDebugFunction(func.lpLiftedFunction);
    this->IdentifyStructuresInternal(func);

    for (auto &f : func.innerFunctions)
        this->IdentifyStructures(f);
}

std::string GraphVisualizer::EscapeHtml(const std::string &str) {
    std::string result;
    for (char c : str) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '\"':
            result += "&quot;";
            break;
        case '\'':
            result += "&#39;";
            break;
        case '\n':
            result += "<BR ALIGN=\"LEFT\"/>";
            break;
        case '\t':
            result += "&nbsp;&nbsp;&nbsp;&nbsp;";
            break;
        default:
            result += c;
            break;
        }
    }
    return result;
}

std::string GraphVisualizer::BlockTypeToString(BlockType type) {
    switch (type) {
    case BlockType::Standard:
        return "Standard";
    case BlockType::IfHeader:
        return "If";
    case BlockType::LoopHeader:
        return "LoopHead";
    case BlockType::LoopLatch:
        return "LoopLatch";
    case BlockType::Break:
        return "Break (Loop)";
    case BlockType::Continue:
        return "Continue (Loop)";
    case BlockType::Return:
        return "Return";
    case BlockType::Dead:
        return "Dead";
    default:
        return "Unknown";
    }
}

std::string GraphVisualizer::OperationToString(LiftedOperation op) { return std::string(::OperationToString(op)); }

std::string GraphVisualizer::FormatOperand(const LiftedOperand &operand, bool isSsa) {
    switch (operand.type) {
    case LiftedOperandType::Register:
        if (isSsa && operand.ssaVersion != -1) {
            return std::format("R{}<SUB>{}</SUB>", operand.value.reg, operand.ssaVersion);
        }
        if (isSsa) {
            return std::format("R{}<SUB>undef</SUB>", operand.value.reg);
        }
        return std::format("R{}", operand.value.reg);
    case LiftedOperandType::ImmediateNil:
        return "nil";
    case LiftedOperandType::ImmediateInteger:
        return std::format("0x{:X}", static_cast<uint32_t>(operand.value.imm.n));
    case LiftedOperandType::ImmediateBool:
        return operand.value.imm.b ? "true" : "false";
    case LiftedOperandType::ImmediateConstant:
        return std::format("K{}", operand.value.imm.k);
    case LiftedOperandType::ImmediateAux:
        return std::format("AUX_{}", operand.value.imm.u);
    default:
        return "?";
    }
}
std::string GraphVisualizer::GenerateNodeHtml(const BasicBlock &block, const LiftedFunction *func, bool isSsa) {
    std::stringstream ss;

    ss << "<TABLE BORDER=\"1\" CELLBORDER=\"0\" CELLSPACING=\"0\" CELLPADDING=\"4\">";

    std::string spec = "";

    if (block.bType == BlockType::LoopHeader || block.bType == BlockType::LoopLatch) {
        if ((block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop) == LoopBlockFlags::ForGeneralLoop) {
            spec = "/For Loop (General Form)";
        } else if ((block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop_Indexed) == LoopBlockFlags::ForGeneralLoop_Indexed) {
            spec = "/For Loop (Indexed Next Form)";
        } else if ((block.dwBlockFlags & LoopBlockFlags::ForGeneralLoop_Pairs) == LoopBlockFlags::ForGeneralLoop_Pairs) {
            spec = "/For Loop (Next Form)";
        } else if ((block.dwBlockFlags & LoopBlockFlags::ForNumericLoop) == LoopBlockFlags::ForNumericLoop) {
            spec = "/For Loop (Numeric Form)";
        } else if ((block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop) {
            spec = "/repeat-until structure";
        } else if ((block.dwBlockFlags & LoopBlockFlags::WhileLoop) == LoopBlockFlags::WhileLoop) {
            spec = "/while structure";
        } else {
            spec = "/unrecognized structure";
        }
    }

    ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><B>BLOCK " << block.dwBlockId << " [" << BlockTypeToString(block.bType) << spec << "]</B>";
    if (block.bTerminator == BlockTerminator::Conditional)
        ss << " (Cond)";
    else if (block.bTerminator == BlockTerminator::Unconditional)
        ss << " (Jump)";
    ss << "</TD></TR>";

    ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#888888\">--------------------------------------------------</FONT></TD></TR>";

    if (isSsa) {
        for (const auto &phi : block.phiNodes) {
            std::stringstream line;
            line << FormatOperand(phi.operands[0], isSsa) << " = ϕ(";
            for (size_t i = 1; i < phi.operands.size(); ++i) {
                line << FormatOperand(phi.operands[i], isSsa);
                if (i < phi.operands.size() - 1)
                    line << ", ";
            }
            line << ")";
            ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><I>" << line.str() << "</I></TD></TR>";
        }
        if (!block.phiNodes.empty()) {
            ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#888888\">--------------------------------------------------</FONT></TD></TR>";
        }
    }

    if (block.lpHead && block.lpTail) {
        if (block.dwBlockId == 0) {
            std::stringstream line;
            line << "; Function Debug Name: " << func->name << "\n";
            line << "; Bytecode ID: " << static_cast<int32_t>(func->lpDeserialized->bytecodeId) << "\n";
            line << "; Defined at line: " << static_cast<int32_t>(func->lpDeserialized->lineDefined) << "\n";
            line << "; Total Registers Used: " << static_cast<int32_t>(func->lpDeserialized->maxstacksize) << "\n";
            line << "; Number of Upvalues: " << static_cast<int32_t>(func->lpDeserialized->nups) << "\n";
            line << "; Number of Arguments: " << static_cast<int32_t>(func->lpDeserialized->numparams);
            if (func->lpDeserialized->numparams > 0)
                line << " (R0 - R" << static_cast<int>(func->lpDeserialized->numparams) - 1 << ")";
            line << "\n" << "; Is Variadic: " << (func->lpDeserialized->isvararg ? "Yes" : "No") << ".\n";

            line << "";
            ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><FONT COLOR=\"#005500\">" << EscapeHtml(line.str()) << "</FONT></TD></TR>";
        }
        const LiftedInstruction *current = block.lpHead;
        while (true) {
            if (current < func->instructions.data() || current >= func->instructions.data() + func->instructions.size()) {
                ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"RED\">(Ptr Error)</FONT></TD></TR>";
                break;
            }

            std::stringstream line;
            auto idx = std::distance(func->instructions.data(), current);

            line << "_" << idx << ": " << OperationToString(current->operation) << " ";

            for (size_t i = 0; i < current->operands.size(); ++i) {
                line << FormatOperand(current->operands[i], isSsa);
                if (i < current->operands.size() - 1)
                    line << ", ";
            }

            if (current->instructionRemarks) {
                std::string cmt = *current->instructionRemarks;
                if (cmt.find("INFO: ") == 0)
                    cmt = cmt.substr(6);
                line << "  <FONT COLOR=\"#005500\">; " << EscapeHtml(cmt) << "</FONT>";
            }

            ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\">" << line.str() << "</TD></TR>";

            if (current == block.lpTail)
                break;
            current++;
        }
    } else {
        ss << "<TR><TD ALIGN=\"LEFT\">(Empty)</TD></TR>";
    }

    if (!block.analysisNotes.empty()) {
        ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#888888\">--------------------------------------------------</FONT></TD></TR>";
        ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#555555\"><B>WHY</B></FONT></TD></TR>";
        for (const auto &note : block.analysisNotes)
            ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><FONT POINT-SIZE=\"9\" COLOR=\"#555555\">" << EscapeHtml(note) << "</FONT></TD></TR>";
    }

    ss << "</TABLE>";
    return ss.str();
}

void GraphVisualizer::GenerateFunctionGraph(std::stringstream &dot, const AnalyzedFunction &func, const std::string &funcPrefix, bool isSsa) {
    dot << "\n    subgraph cluster_" << funcPrefix << " {\n";
    dot << "        label=\"" << EscapeHtml(func.lpLiftedFunction->name) << (isSsa ? " (SSA)" : "") << "\";\n";
    dot << "        style=filled; color=lightgrey; node [style=filled,color=white];\n";

    for (const auto &block : func.basicBlocks) {
        if (block.bType == BlockType::Error)
            continue;

        std::string uniqueNodeId = std::format("{}_BLK_{}", funcPrefix, block.dwBlockId);

        std::string fillColor = "white";
        if (block.dwBlockId == 0)
            fillColor = "#E8F5E9"; // Entry
        else if (block.bType == BlockType::Return)
            fillColor = "#FFEBEE"; // Exit
        else if (block.bType == BlockType::LoopHeader)
            fillColor = "#FFF8E1"; // Loop Head
        else if (block.bType == BlockType::LoopLatch)
            fillColor = "#F5F5F5"; // Loop Latch

        dot << "        " << uniqueNodeId << " [\n";
        dot << "            shape=plain\n";
        dot << "            label=<" << GenerateNodeHtml(block, func.lpLiftedFunction, isSsa) << ">\n";
        dot << "            fillcolor=\"" << fillColor << "\"\n";
        dot << "            fontname=\"Courier New\" fontsize=10\n";
        dot << "        ];\n";
    }

    for (const auto &block : func.basicBlocks) {
        if (block.bType == BlockType::Dead || block.bType == BlockType::Error)
            continue;

        std::string srcId = std::format("{}_BLK_{}", funcPrefix, block.dwBlockId);

        bool isFornPrep = (block.lpTail && block.lpTail->operation == LiftedOperation::FORNPREP) ||
                          (block.lpTail && block.lpTail->operation == LiftedOperation::FORGPREP) ||
                          (block.lpTail && block.lpTail->operation == LiftedOperation::FORGPREP_NEXT) ||
                          (block.lpTail && block.lpTail->operation == LiftedOperation::FORGPREP_INEXT);
        bool isFornLoop = (block.lpTail && (block.lpTail->operation == LiftedOperation::FORNLOOP || block.lpTail->operation == LiftedOperation::FORGLOOP));

        if (block.bTerminator == BlockTerminator::Conditional) {
            if (isFornPrep) {
                // FORNPREP: Fallthrough=Enter(Green), Jump=Skip(Red)
                if (block.successors.size() >= 1) // Jump Target (Skip)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[0])
                        << " [label=\"Skip\", color=\"red3\", fontcolor=\"red3\"];\n";
                if (block.successors.size() >= 2) // Fallthrough (Enter)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[1])
                        << " [label=\"Enter\", color=\"green4\", fontcolor=\"green4\", weight=2];\n";
            } else if (isFornLoop) {
                // FORNLOOP: Jump=Loop(Blue), Fallthrough=Exit(Red)
                if (block.successors.size() >= 1) // Jump Target (Loop Back)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[0])
                        << " [label=\"Loop\", style=dashed, color=\"blue\", fontcolor=\"blue\"];\n";
                if (block.successors.size() >= 2) // Fallthrough (Exit)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[1])
                        << " [label=\"Exit\", color=\"red3\", fontcolor=\"red3\", penwidth=2];\n";
            } else {
                // Standard If/Else
                if (block.successors.size() >= 1)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[0])
                        << " [label=\"True\", color=\"green4\", fontcolor=\"green4\"];\n";
                if (block.successors.size() >= 2)
                    dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[1])
                        << " [label=\"False\", color=\"red3\", fontcolor=\"red3\"];\n";
            }
        } else {
            for (size_t i = 0; i < block.successors.size(); ++i) {
                bool isBackEdge = (block.successors[i] <= block.dwBlockId);
                dot << "        " << srcId << " -> " << std::format("{}_BLK_{}", funcPrefix, block.successors[i]);
                if (isBackEdge && block.bTerminator == BlockTerminator::Unconditional)
                    dot << " [style=dashed, color=blue, label=\"Back\"];\n";
                else
                    dot << " [color=black];\n";
            }
        }

        // link closures to their children
        if (block.lpHead && block.lpTail) {
            const LiftedInstruction *curr = block.lpHead;
            while (true) {
                if (curr->operation == LiftedOperation::NEWCLOSURE) {
                    if (curr->operands.size() >= 2 && curr->operands[1].type == LiftedOperandType::ImmediateConstant) {
                        int protoIndex = (curr->operands[1].value.imm.k);
                        std::string childEntryId = std::format("{}_SUB_{}_BLK_0", funcPrefix, protoIndex);
                        dot << "        " << srcId << " -> " << childEntryId
                            << " [label=\"Create a New Instance of Closure\", style=dotted, color=purple, fontcolor=purple];\n";
                    }
                } else if (curr->operation == LiftedOperation::DUPCLOSURE) {
                    if (curr->operands.size() >= 3 && curr->operands[2].type == LiftedOperandType::ImmediateInteger) {
                        int protoIndex = (curr->operands[2].value.imm.n);
                        std::string childEntryId = std::format("{}_SUB_{}_BLK_0", funcPrefix, protoIndex);
                        dot << "        " << srcId << " -> " << childEntryId
                            << " [label=\"Attempt to Duplicate Instance of Closure\", style=dotted, color=purple, fontcolor=purple];\n";
                    }
                }
                if (curr == block.lpTail)
                    break;
                curr++;
            }
        }
    }

    int subFuncIndex = 0;
    for (const auto &sub : func.innerFunctions) {
        std::string subPrefix = std::format("{}_SUB_{}", funcPrefix, subFuncIndex++);
        GenerateFunctionGraph(dot, sub, subPrefix, isSsa);
    }
    dot << "    }\n";
}

std::string GraphVisualizer::GenerateDotGraph(const AnalyzedFunction &rootAnalysis, GraphContent contentMode) {
    std::stringstream dot;
    dot << "digraph LuauCFG {\n";
    dot << "    compound=true;\n";
    dot << "    labelloc=\"t\";\n";
    dot << "    fontname=\"Courier New\";\n";

    if (contentMode == GraphContent::IROnly || contentMode == GraphContent::Both) {
        dot << "subgraph cluster_non_ssa {\n";
        dot << "    label=\"Non-SSA\";\n";
        GenerateFunctionGraph(dot, rootAnalysis, "ROOT_NON_SSA", false);
        dot << "}\n";
    }

    if (contentMode == GraphContent::SSAOnly || contentMode == GraphContent::Both) {
        dot << "subgraph cluster_ssa {\n";
        dot << "    label=\"SSA\";\n";
        GenerateFunctionGraph(dot, rootAnalysis, "ROOT_SSA", true);
        dot << "}\n";
    }

    dot << "}\n";
    return dot.str();
}
