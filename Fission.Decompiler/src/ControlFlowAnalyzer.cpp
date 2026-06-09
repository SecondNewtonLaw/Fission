//
// Created by Dottik on 30/11/2025.
//

#include "ControlFlowAnalyzer.hpp"

#include "DenominatorAnalysis.hpp"
#include "Deserializer.hpp"

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

        // FORNLOOP and FORGLOOP will jump to them. When resolving the jumps this
        // will cause them to not be the block leaders, failing tailing checks
        // and causing blocks to be discarded by mistake.
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

        // case LiftedOperation::FORNPREP:
        // case LiftedOperation::FORGPREP:
        // case LiftedOperation::FORGPREP_INEXT:
        // case LiftedOperation::FORGPREP_NEXT:
        //     return lpInstruction->operands[1].value.imm.n + 2;

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
    ASSERT(it != leaderMap.end(), "leader mapping not properly constructed, or function misused.");
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

        // we will identify where execution goes next
        std::vector<LiftedInstruction *> nextInstructions;

        switch (currentBlock.bTerminator) {
        case BlockTerminator::Fallthrough:
            // immediately to next block.
            nextInstructions.push_back(currentBlock.lpTail + 1);
            break;

        case BlockTerminator::Unconditional: {
            // jumps to offset.
            int32_t offset = GetJumpOffset(currentBlock.lpTail);
            nextInstructions.push_back((currentBlock.lpTail) + offset);
            break;
        }

        case BlockTerminator::Conditional: {
            if ((currentBlock.lpTail->operation == LiftedOperation::FORNLOOP || currentBlock.lpTail->operation == LiftedOperation::FORGLOOP) &&
                currentBlock.bType == BlockType::LoopLatch) {
                // loop header. adjust logic.
                // jump (True/False depends on opcode)
                int32_t offset = GetJumpOffset(currentBlock.lpTail);
                nextInstructions.push_back((currentBlock.lpTail) + offset);

                currentBlock.loopHeader = GetBlockIdAtInstruction((currentBlock.lpTail) + offset, leaderToBlockId);
                blocks.at(*currentBlock.loopHeader).loopLatch = i; // current block is exit for the loop.

                // fallthrough (else)
                nextInstructions.push_back(currentBlock.lpTail + 1);
                currentBlock.loopLatch = i /* self */;
                break;
            }

            if (currentBlock.lpTail->operation == LiftedOperation::JUMPXEQK) {
                // we must check the NOT flag
                ASSERT(currentBlock.lpTail->operands.size() == 4, "missized operands for JUMPXEQK");
                auto isNot = currentBlock.lpTail->operands[3].value.imm.b;

                int32_t offset = GetJumpOffset(currentBlock.lpTail);
                // both polarities: the jump is taken exactly when LiftCondition is true. not-flag
                // clear → `==`, jump-on-equal; not-flag set → `~=`, jump-on-not-equal. so jump-target
                // == cond-TRUE either way (previously the not-flag-clear branch was swapped).
                (void)isNot;
                currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                nextInstructions.push_back(currentBlock.lpTail + 1);
                nextInstructions.push_back((currentBlock.lpTail) + offset);
            } else {
                // jump (True/False depends on opcode)
                int32_t offset = GetJumpOffset(currentBlock.lpTail);

                // fallthrough (else)
                switch (currentBlock.lpTail->operation) {
                case LiftedOperation::JUMPIFNOT:
                default:
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    break;
                case LiftedOperation::JUMPIF:
                    // JUMPIF jumps when R[A] is truthy. LiftCondition emits the raw
                    // expression (no inversion), so jump-target == lifted-cond TRUE
                    // and fallthrough == lifted-cond FALSE.
                    nextInstructions.push_back((currentBlock.lpTail) + offset);
                    currentBlock.ifStatementTrue = GetBlockIdAtInstruction(currentBlock.lpTail + offset, leaderToBlockId);
                    currentBlock.ifStatementFalse = GetBlockIdAtInstruction(currentBlock.lpTail + 1, leaderToBlockId);
                    nextInstructions.push_back(currentBlock.lpTail + 1);
                    break;

                case LiftedOperation::JUMPIFEQ:
                case LiftedOperation::JUMPIFLE:
                    // Positive comparisons: jump when the comparison holds. Jump
                    // target == lifted-cond TRUE, fallthrough == lifted-cond FALSE.
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
            // nowhere to go
            break;

        default:
            break;
        }

        // PCs to BIDs and link.
        for (LiftedInstruction *targetInst : nextInstructions) {
            int32_t targetBlockId = GetBlockIdAtInstruction(targetInst, leaderToBlockId);
            ASSERT(targetBlockId != -1, "bad parsing or invalid bytecode");

            // edge: current->target
            currentBlock.successors.push_back(targetBlockId);

            // edge: target<-current (rev link)
            blocks[targetBlockId].predecessors.push_back(currentBlock.dwBlockId);
        }
    }
}

AnalyzedFunction ControlFlowAnalyzer::DetermineBasicBlocksInternal(LiftedFunction *lpLiftedFunction) {
    std::vector<BasicBlock> basicBlocks;

    std::set<size_t> leaderIndexes;
    leaderIndexes.insert(0); // the first instruction of the function is a leader, as it's the start of a simple standard block.
    size_t totalInstructions = lpLiftedFunction->instructions.size();

    for (size_t currentIndex = 0; currentIndex < totalInstructions; ++currentIndex) {
        auto instruction = &lpLiftedFunction->instructions.at(currentIndex);
        if (this->IsTerminator(instruction->operation)) {
            if (currentIndex + 1 < totalInstructions)
                leaderIndexes.insert(currentIndex + 1);

            // FORNLOOP/FORGLOOP always means a new block.
            // logic may jump to them for branching and control-flow such as loop skipping.
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

    // FORXPREP not in IsTerminator but still needs its fallthrough (body start)
    // and jump target (loop exit) as block leaders. Otherwise body instructions
    // merge into the same block as FORNPREP, breaking if/else detection.
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
        // if (block.lpHead->operation == LiftedOperation::NOP && block.lpHead != block.lpTail) // blocks may be a lonely NOP sometimes.
        //     block.lpHead++; // NOPs are injected for AUXs, they musn't be heads or CFA will begin failing.

        LiftedInstruction *tailInst = block.lpTail;

        block.bType = BlockType::Standard;

        // TODO: FIGURE OUT WHY FORXPREP INSTRUCTIONS ARE BEING USED AS BLOCK TERMINATORS, CAUSING FORXLOOP INSTRUCTIONS TO BREAK!

        switch (tailInst->operation) {
        case LiftedOperation::JUMP: {
            block.bTerminator = BlockTerminator::Unconditional;

            if ((tailInst + 1)->operation == LiftedOperation::FORNLOOP || (tailInst + 1)->operation == LiftedOperation::FORGLOOP) {
                // this means the jump instruction is a break out of a loop. FORXLOOP instructions are
                // in charge of looping back to the beginning.
                auto jmpOffset = GetJumpOffset(tailInst);
                if (jmpOffset > 0)
                    block.bType = BlockType::Break; // possibly breaking out of a loop.
                else
                    block.bType = BlockType::Continue; // possibly breaking out of a loop.

                break;
            }

            auto jmpOffset = GetJumpOffset(tailInst);
            if (jmpOffset < 0) {
                // backward jump = loop back-edge
                block.bType = BlockType::Continue;
            } else {
                // Forward JUMP — leave as Standard. IdentifyStructuresInternal
                // at line ~683 will reclassify as Break/Continue via dominance
                // analysis if this block lives inside a loop.
            }

            break;
        }
        case LiftedOperation::LOADNJUMP: // DO NOT FUCKING ADD FORXPREP OPERATIONS HERE, THEY ARE NOT A TERMINATOR.
            block.bTerminator = BlockTerminator::Unconditional;
            if (GetJumpOffset(tailInst) < 0)
                block.bType = BlockType::LoopLatch;
            else {
                // TODO: we must perform dominance analysis to determine this properly.
                // block.bType = BlockType::Break; // possibly breaking out of a loop.
            }

            break;
        case LiftedOperation::JUMPXEQK:
            block.bTerminator = BlockTerminator::Conditional;
            if (GetJumpOffset(tailInst) < 0) {
                block.bType = BlockType::LoopLatch;
            } else {
                block.bType = BlockType::IfHeader;
            }
            break;

        case LiftedOperation::FORGPREP:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGPREP_NEXT:
        case LiftedOperation::FORNPREP: {
            block.bTerminator = BlockTerminator::Conditional;
            block.bType = BlockType::LoopHeader;
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
            break;
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            block.bTerminator = BlockTerminator::Conditional;
            if (GetJumpOffset(tailInst) < 0)
                block.bType = BlockType::LoopLatch;
            break;
        case LiftedOperation::RETURN:
            block.bTerminator = BlockTerminator::Return;
            block.bType = BlockType::Return;
            break;
        default:
            block.bTerminator = BlockTerminator::Fallthrough;
            block.bType = BlockType::Standard;
            break;
        }

        basicBlocks.push_back(block);
    }

    std::vector<AnalyzedFunction> subfuncs;

    for (auto &func : lpLiftedFunction->subfunctions) {
        auto analyzed = DetermineBasicBlocksInternal(&func);
        subfuncs.push_back(analyzed);
    }

    return AnalyzedFunction{lpLiftedFunction, basicBlocks, {}, {}, subfuncs, {}, {}, {}, {}, {}, {}, {}, {}, {}};
}

void ControlFlowAnalyzer::OptimiseGraphInternal(std::vector<BasicBlock> &blocks) {
    bool changed = true;
    while (changed) {
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

                // the entry block cannot be removed.
                if (currentId == 0)
                    continue;

                // prevent inf loop.
                if (targetId == currentId)
                    continue;

                // reassign predecesors to point to the correct next block, as this block will be yanked
                // we have to simply reassign the predecessor's successor that's us to point to our only successor.
                // this unlinks us from the graph and allows us shorten it.

                std::vector<std::uint32_t> newPredecessors;
                for (int32_t predecesorId : block.predecessors) {
                    BasicBlock &predBlock = blocks[predecesorId];

                    // update successors
                    for (size_t i = 0; i < predBlock.successors.size(); ++i) {
                        if (predBlock.successors[i] == static_cast<uint32_t>(currentId))
                            predBlock.successors[i] = targetId; // reassign to this block's target.
                    }

                    // Update structural links that might reference the removed block
                    auto updateRef = [&](std::optional<uint32_t> &field) {
                        if (field.has_value() && *field == static_cast<uint32_t>(currentId))
                            field = targetId;
                    };
                    updateRef(predBlock.ifStatementTrue);
                    updateRef(predBlock.ifStatementFalse);
                    updateRef(predBlock.loopHeader);
                    updateRef(predBlock.loopLatch);
                    updateRef(predBlock.loopExit);

                    // add our predecessors to our successor blk.
                    BasicBlock &ourSuccessor = blocks[targetId];
                    bool alreadyExists = false;
                    for (int32_t predecessors : ourSuccessor.predecessors) {
                        if (predecessors == predecesorId)
                            alreadyExists = true;
                    }

                    if (!alreadyExists)
                        ourSuccessor.predecessors.push_back(predecesorId); // add our predecessor to the successor.

                    // clear ourselves from predecessors.
                    BasicBlock &targetBlock = blocks[targetId];
                    auto &preds = targetBlock.predecessors;
                    std::erase(preds, currentId);
                }

                block.bType = BlockType::Dead; // mark dead
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
            if (cur == header)
                return true;
            auto it = domInfo.find(cur);
            if (it == domInfo.end())
                return false;
            cur = it->second.idom;
        }
        return false;
    };

    // Pre-pass: detect loops anchored by Continue blocks with backward JUMPs.
    // This catches cases like `while true do break end` where the back-edge
    // has no predecessors (break bypasses it) and fails the dominates check.
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
        blk.loopHeader = headerId;
        header.loopLatch = blk.dwBlockId;
        blk.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
        header.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
        blk.bType = BlockType::LoopLatch;

        if (header.ifStatementFalse.has_value()) {
            blk.loopExit = header.ifStatementFalse.value();
            header.loopExit = header.ifStatementFalse.value();
        }
    }

    for (BasicBlock &block : blocks) {
        if (block.bType == BlockType::Dead || block.bType == BlockType::Error)
            continue;

        for (uint32_t succ : block.successors) {
            if (succ == block.dwBlockId || dominates(succ, block.dwBlockId)) {
                if (blocks[succ].bType != BlockType::LoopHeader && blocks[succ].bType != BlockType::Break && blocks[succ].bType != BlockType::Continue)
                    blocks[succ].bType = BlockType::LoopHeader;

                if (block.bType != BlockType::Return && block.bType != BlockType::Break)
                    block.bType = BlockType::LoopLatch;
            }
        }
    }

    for (BasicBlock &block : blocks) {
        if (block.bType == BlockType::IfHeader) {
            for (int32_t pred : block.predecessors) {
                if (dominates(block.dwBlockId, pred)) {
                    block.bType = BlockType::LoopHeader;
                    break;
                }
            }
        }
    }

    // unconditional self-jump = infinite `while true` with header/body/latch fused in one block.
    // back-edge passes retype it LoopLatch so it never lifts; restore the header role. for-latches
    // self-jump too but conditionally, so the uncond guard excludes them.
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
        for (uint32_t succ : block.successors)
            if (succ != block.dwBlockId) {
                block.loopExit = succ;
                break;
            }
    }

    // during the previous phases we have cleaned up lots of room for identifying the loop structures truly.
    // while n do ... end structures perform their jump to a comparison instruction that jumps out of the loop.
    // repeat ... until n structures perform a comparison at the end, before jumping to a NON comparison instruction at the start of the body of the loop (which
    // has no FORXPREP instruction) for ... do end structures perform a FORXLOOP instruction at the end, before jumping if succeeding to repeat at the
    // instruction after a FORXPREP instruction.

    // recognizing FOR loops.
    for (BasicBlock &block : blocks) {
        if (block.bType != BlockType::LoopHeader)
            continue;

        // FORxPREP must be the terminator, but higher opt levels bundle the start/limit/step LOADs
        // into this block, so don't require it to be the sole instruction.
        if (block.lpTail->operation != LiftedOperation::FORNPREP && block.lpTail->operation != LiftedOperation::FORGPREP_INEXT &&
            block.lpTail->operation != LiftedOperation::FORGPREP && block.lpTail->operation != LiftedOperation::FORGPREP_NEXT)
            continue; // not supported by this pass.

        for (uint32_t succId : block.successors) {
            // we realistically do not care about dominance.
            // we have the guarantee that FORXLOOP instructions will be after a loop header that we know is a FORXPREP and is an only instruction.

            auto &successor = blocks.at(succId);
            if (successor.lpTail != successor.lpHead ||
                (successor.lpTail->operation != LiftedOperation::FORNLOOP && successor.lpTail->operation != LiftedOperation::FORGLOOP))
                continue; // not target

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
    }

    for (BasicBlock &block : blocks) {
        if (block.bType != BlockType::LoopLatch)
            continue; // we only need loop latches to fix the determination and mark the loop type.

        // blocks already wired into a for-loop by the FOR pass: a FORNLOOP/FORGLOOP latch would be
        // mis-tagged repeat-until here, leave it alone.
        constexpr uint32_t kForLoopMask =
            static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) |
            static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed);
        if ((block.dwBlockFlags & kForLoopMask) != 0)
            continue;

        for (uint32_t succ : block.successors) {
            if (succ == block.dwBlockId || dominates(succ, block.dwBlockId)) {
                auto &successor = blocks.at(succ);
                // for-header reached by a separate unconditional back-edge = outer infinite `while`
                // wrapping a for. leave the for's latch intact; the wrapper is recovered at lift time.
                if ((successor.dwBlockFlags & kForLoopMask) != 0)
                    continue;
                // conditional jump.

                auto targetInstruction = block.lpTail + GetJumpOffset(block.lpTail);
                if (block.lpTail->operation == LiftedOperation::JUMP) {
                    // unconditional jump back = while or repeat-until. header's conditional jumping
                    // straight to the latch (not a body block) means header IS the body → repeat-until.
                    bool isRepeatUntil = false;
                    if (successor.bTerminator == BlockTerminator::Conditional) {
                        for (uint32_t headerSucc : successor.successors) {
                            if (headerSucc == block.dwBlockId) {
                                isRepeatUntil = true;
                                break;
                            }
                        }
                    }

                    successor.loopLatch = block.dwBlockId;
                    block.loopHeader = successor.dwBlockId;
                    if (isRepeatUntil) {
                        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::RepeatUntilLoop);
                    } else {
                        block.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
                        successor.dwBlockFlags |= static_cast<uint32_t>(LoopBlockFlags::WhileLoop);
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

                // repeat-until: the latch (conditional JUMPIFNOT) holds the exit as its fallthrough
                // (ifStatementFalse), since the header has no ifStatement fields (header IS the body).
                bool isRepeatUntil = (block.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop;
                if (isRepeatUntil) {
                    // pattern 1: condition in latch (conditional JUMPIF); latch.ifStatementFalse = exit.
                    if (block.ifStatementFalse.has_value()) {
                        block.loopExit = block.ifStatementFalse.value();
                        successor.loopExit = block.ifStatementFalse.value();
                    } else {
                        // pattern 2: condition in header (latch is uncond JUMP); exit = the header
                        // successor that isn't the latch.
                        for (auto headerSucc : successor.successors) {
                            if (headerSucc != block.dwBlockId) {
                                // but Luau may decompose a compound `until a or b` into body if-return
                                // blocks, so the header's "exit" is really a body block that loops back.
                                // prefer a Return-typed successor; pattern 3 below scans if none.
                                if (blocks.at(headerSucc).bType == BlockType::Return) {
                                    block.loopExit = headerSucc;
                                    successor.loopExit = headerSucc;
                                }
                                break;
                            }
                        }
                        // pattern 3: compound condition fully in body; scan for the first Return block.
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

                    // header already tagged RepeatUntilLoop by a different latch (nested repeat sharing
                    // the header); don't overwrite loopExit, the repeat-until logic set it.
                    if ((successor.dwBlockFlags & LoopBlockFlags::RepeatUntilLoop) == LoopBlockFlags::RepeatUntilLoop)
                        continue;

                    if (!successor.ifStatementFalse || dominates(successor.ifStatementFalse.value(), block.dwBlockId)) {
                        block.loopExit = successor.ifStatementTrue.value();
                        successor.loopExit = successor.ifStatementTrue.value();
                    } else {
                        block.loopExit = successor.ifStatementFalse.value();
                        successor.loopExit = successor.ifStatementFalse.value();
                    }
                }
            }
        }
    }

    // Identify Break/Continue: find forward Unconditional JUMPs inside loops
    // and classify them by comparing the jump target against the loop exit.
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

        // Forward JUMP: find the target block
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

        // Find the innermost loop header that dominates this block
        int32_t innermostHeader = -1;
        int32_t innermostExit = -1;
        for (const auto &b : blocks) {
            if (b.bType != BlockType::LoopHeader || !b.loopLatch)
                continue;
            if (!dominates(b.dwBlockId, blk.dwBlockId))
                continue;

            // b dominates our block → we're inside b's loop.
            // Check if b is deeper (more nested) than current innermost.
            bool deeper = (innermostHeader < 0);
            if (!deeper && dominates(innermostHeader, b.dwBlockId))
                deeper = true; // innermost dominates b → b is deeper

            if (deeper) {
                innermostHeader = b.dwBlockId;
                innermostExit = blocks[*b.loopLatch].loopExit.value_or(-1);
            }
        }

        if (innermostHeader < 0)
            continue; // not inside any loop

        if (innermostExit >= 0 && targetId == innermostExit) {
            blk.bType = BlockType::Break;
        } else if (dominates(innermostHeader, targetId)) {
            blk.bType = BlockType::Continue;
        }
    }

    // Identify Break/Continue: find forward Unconditional JUMPs inside loops
}

void ControlFlowAnalyzer::PruneUnreachableBlocks(std::vector<BasicBlock> &blocks) {
    std::vector<bool> reachable(blocks.size(), false);
    std::queue<int32_t> qq;
    qq.push(0);
    reachable[0] = true;

    while (!qq.empty()) {
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
            blocks[i].successors.clear();
            for (auto &b : blocks) {
                std::erase(b.predecessors, i);
                std::erase(b.successors, i);
            }
        }
    }
}

void ControlFlowAnalyzer::DetermineBasicBlocksInternalAdvanced(AnalyzedFunction &func) {
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

void ControlFlowAnalyzer::OptimizeGraph(AnalyzedFunction &func) {
    this->OptimiseGraphInternal(func.basicBlocks);
    for (auto &f : func.innerFunctions)
        this->OptimizeGraph(f);
}

void ControlFlowAnalyzer::PruneUnreachable(AnalyzedFunction &func) {
    this->PruneUnreachableBlocks(func.basicBlocks);
    for (auto &f : func.innerFunctions)
        this->PruneUnreachable(f);
}

void ControlFlowAnalyzer::IdentifyStructures(AnalyzedFunction &func) {
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
