//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include <set>

#include "BytecodeLifter.hpp"

#include <map>

enum class BlockType {
    Standard,

    // structures
    IfHeader,   // The start of an 'if' (has conditional branches).
    LoopHeader, // top of a loop (where the negative jump would land (must be determined using a LoopLatch block)).
    LoopLatch,  // bottom of a loop (negative jump).

    // control flow
    Break,    // jumps out of the current loop context.
    Continue, // jumps to a LoopLatch/LoopHeader.
    Return,   // exits running procedure.
    Dead,     // dead block, unused/ unlinked from graph.
    Error
};

enum class BlockTerminator {
    Fallthrough,   // executes into next block without change in control flow.
    Unconditional, // JUMP without a condition.
    Conditional,   // JUMPIF/JUMPIFNOT (goes to A or B).
    Return,        // RETURN exits the running function
    Error
};

struct BasicBlock {
    std::uint32_t dwBlockId = 0x0;
    BlockType bType = BlockType::Error;
    BlockTerminator bTerminator = BlockTerminator::Error;
    LiftedInstruction *lpHead = nullptr;
    LiftedInstruction *lpTail = nullptr;

    std::vector<std::uint32_t> successors;
    std::vector<std::uint32_t> predecessors;
};

struct AnalyzedFunction {
    LiftedFunction *lpLiftedFunction; // not owned by structure.
    std::vector<BasicBlock> basicBlocks;

    std::vector<AnalyzedFunction> innerFunctions;
};

class ControlFlowAnalyzer {

    bool IsTerminator(LiftedOperation operation) {
        switch (operation) {
            // unconditionals
        case LiftedOperation::JUMP:
        case LiftedOperation::LOADNJUMP:
            // conditionals
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::JUMPXEQK:
        case LiftedOperation::FORNLOOP: // terminates a latch block
        case LiftedOperation::FORGLOOP:
        case LiftedOperation::RETURN:
            return true;
        default:
            return false;
        }
    }

    int32_t GetJumpOffset(const LiftedInstruction *lpInstruction) {
        ASSERT(!lpInstruction->operands.empty(), "no operands available, cannot caluclate jump offset.");

        switch (lpInstruction->operation) {
        case LiftedOperation::JUMP:
            return lpInstruction->operands[0].value.imm.n;
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            return lpInstruction->operands[1].value.imm.n;

        case LiftedOperation::LOADNJUMP:
            return lpInstruction->operands[2].value.imm.n;

        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::JUMPXEQK:
            return lpInstruction->operands[1].value.imm.n;

        default:
            return 0;
        }
    }

    int32_t GetBlockIdAtInstruction(const LiftedInstruction *lpTargetInstruction, const std::map<LiftedInstruction *, int32_t> &leaderMap) {
        auto it = leaderMap.find(const_cast<LiftedInstruction *>(lpTargetInstruction));
        ASSERT(it != leaderMap.end(), "leader mapping not properly constructed, or function misused.");
        return it->second;
    }

    void LinkBasicBlocks(std::vector<BasicBlock> &blocks) {
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
                nextInstructions.push_back((currentBlock.lpTail + 1) + offset);
                break;
            }

            case BlockTerminator::Conditional: {
                // jump (True/False depends on opcode)
                int32_t offset = GetJumpOffset(currentBlock.lpTail);
                nextInstructions.push_back((currentBlock.lpTail + 1) + offset);

                // fallthrough (else)
                nextInstructions.push_back(currentBlock.lpTail + 1);
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

    AnalyzedFunction DetermineBasicBlocksInternal(LiftedFunction *lpLiftedFunction) {
        std::vector<BasicBlock> basicBlocks;

        std::set<size_t> leaderIndexes;
        leaderIndexes.insert(0); // the first instruction of the function is a leader, as it's the start of a simple standard block.
        size_t totalInstructions = lpLiftedFunction->instructions.size();

        size_t currentIndex = 0;
        while (totalInstructions >= currentIndex++) {
            if (currentIndex == totalInstructions)
                break; // end of list.

            auto instruction = &lpLiftedFunction->instructions.at(currentIndex);
            if (this->IsTerminator(instruction->operation)) {
                // the instruction after a terminator is a new leader, since it defines the end of the previous block.
                if (totalInstructions > currentIndex + 1)
                    leaderIndexes.insert(currentIndex + 1);

                if (instruction->operation != LiftedOperation::RETURN) {
                    const int32_t offset = GetJumpOffset(instruction);
                    // offsets in jump are relative to next instruction. Some opcodes may not do this, as they sometimes rather be 0 or 1, we optimize these off
                    // during the lifting stage, discarding those who provide absolutely no contribution
                    // however we will have to later check the index maths to make sure this is consistent
                    if (const int32_t targetIndex = static_cast<int32_t>(currentIndex + 1) + offset;
                        targetIndex >= 0 && targetIndex < static_cast<int32_t>(lpLiftedFunction->instructions.size())) {
                        leaderIndexes.insert(static_cast<size_t>(targetIndex));
                    }
                }
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

            LiftedInstruction *tailInst = block.lpTail;

            if (tailInst->operation == LiftedOperation::RETURN) {
                block.bTerminator = BlockTerminator::Return;
                block.bType = BlockType::Return;
            } else if (tailInst->operation == LiftedOperation::JUMP || tailInst->operation == LiftedOperation::LOADNJUMP) {
                block.bTerminator = BlockTerminator::Unconditional;

                // loop latch, likely jumping back to the beginning of a loop.
                // during analysis, we will have to see anyway if the jump is a 'continue'.
                if (GetJumpOffset(tailInst) < 0)
                    block.bType = BlockType::LoopLatch;
                else
                    block.bType = BlockType::Standard;
            } else if (IsTerminator(tailInst->operation)) {
                // terminator block, conditional.
                block.bTerminator = BlockTerminator::Conditional;
                block.bType = BlockType::Standard; // we can assumpe it may be the header of an if block, but during linking we will evaluate it properly
            } else {
                // not a terminator, falls through
                block.bTerminator = BlockTerminator::Fallthrough;
                block.bType = BlockType::Standard;
            }

            basicBlocks.push_back(block);
        }
        // for (size_t currentInsn = 0; currentInsn < lpLiftedFunction->instructions.size(); currentInsn++) {
        //     auto *currentInstruction = &lpLiftedFunction->instructions.at(currentInsn);

        //     if (IsJump(currentInstruction->operation)) {
        //         if (IsConditional(currentInstruction->operation)) {
        //             currentBlock.bTerminator = BlockTerminator::Conditional;
        //         }
        //         currentBlock.lpTail = currentInstruction;
        //     }

        //     if (currentInstruction->operation == LiftedOperation::RETURN) {
        //         // return of block.
        //         currentBlock.bType = BlockType::Return;
        //         currentBlock.bTerminator = BlockTerminator::Return;
        //         currentBlock.lpTail = currentInstruction;
        //         basicBlocks.push_back(currentBlock);
        //         continue;
        //     }

        //     if (currentBlock.lpTail != nullptr && currentBlock.lpHead != nullptr &&
        //         currentBlock.bTerminator != BlockTerminator::Error /*&& currentBlock.bType != BlockType::Error*/) {
        //         // emplace back to block list and clear current block for next block.

        //         switch (currentBlock.lpTail->operation) {
        //         default:
        //             break;

        //         case LiftedOperation::JUMP:
        //         case LiftedOperation::LOADNJUMP: {
        //             currentBlock.bTerminator = BlockTerminator::Unconditional;
        //             break;
        //         }
        //         }

        //         basicBlocks.push_back(currentBlock);
        //         currentBlock = {++currentBlock.dwBlockId, BlockType::Error, BlockTerminator::Error, currentBlock.lpTail, nullptr};

        //         switch (currentBlock.lpHead->operation) { // determine next block's type using the tail of the previous as a base.
        //         default: {
        //             currentBlock.bType = BlockType::Standard;
        //             break;
        //         }
        //         case LiftedOperation::JUMP:
        //         case LiftedOperation::LOADNJUMP: {
        //             LiftedOperand *jumpToInstruction = nullptr;
        //             if (currentBlock.lpHead->operation == LiftedOperation::LOADNJUMP)
        //                 jumpToInstruction = &currentBlock.lpHead->operands.at(2);
        //             else
        //                 jumpToInstruction = &currentBlock.lpHead->operands.at(0);

        //             if (jumpToInstruction->value.imm.n < 0) {      // jumps back, likely a loop latch.
        //                 currentBlock.bType = BlockType::LoopLatch; // TODO: this could be a loop latch or continue. We have to see what it could be.
        //             } else {
        //                 currentBlock.bType = BlockType::Standard; // TODO: this could also be emitted for breaks inside of loops, handle this properly.
        //             }
        //             break;
        //         }
        //         case LiftedOperation::FORNLOOP:
        //         case LiftedOperation::FORGLOOP:
        //             currentBlock.bType = BlockType::LoopHeader;
        //             break;

        //         case LiftedOperation::JUMPIF:
        //         case LiftedOperation::JUMPIFNOT:
        //         case LiftedOperation::JUMPIFEQ:
        //         case LiftedOperation::JUMPIFLE:
        //         case LiftedOperation::JUMPIFLT:
        //         case LiftedOperation::JUMPIFNOTEQ:
        //         case LiftedOperation::JUMPIFNOTLE:
        //         case LiftedOperation::JUMPIFNOTLT:
        //         case LiftedOperation::JUMPXEQK: {
        //             currentBlock.bType = BlockType::IfHeader;
        //             auto &jumpsToInstructionOffsetted = currentBlock.lpHead->operands.at(1);
        //             currentBlock.lpTail = currentBlock.lpHead + jumpsToInstructionOffsetted.value.imm.n; // move to the end of the block.
        //             currentBlock.bTerminator = BlockTerminator::Fallthrough;                             // likely a fall through.
        //             break;
        //         }
        //         }
        //     }
        // }

        std::vector<AnalyzedFunction> subfuncs;

        for (auto &func : lpLiftedFunction->subfunctions) {
            auto analyzed = DetermineBasicBlocksInternal(&func);
            subfuncs.push_back(analyzed);
        }

        return AnalyzedFunction{lpLiftedFunction, basicBlocks, subfuncs};
    }

    void OptimiseGraph(std::vector<BasicBlock> &blocks) {
        bool changed = true;
        while (changed) {
            changed = false;

            for (auto &block : blocks) {
                if (block.bType == BlockType::Error)
                    continue; // deleted/dead

                bool isEmpty = true;
                for (LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
                    if (inst->operation != LiftedOperation::NOP && !IsTerminator(inst->operation) && inst->operation != LiftedOperation::JUMP) {
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
                    for (int32_t predecesorId : block.predecessors) {
                        BasicBlock &predBlock = blocks[predecesorId];

                        // update successors
                        for (size_t i = 0; i < predBlock.successors.size(); ++i) {
                            if (predBlock.successors[i] == static_cast<uint32_t>(currentId))
                                predBlock.successors[i] = targetId; // reassign to this block's target.
                        }

                        // add our predecessors to our successor blk.
                        BasicBlock &ourSuccessor = blocks[targetId];
                        bool alreadyExists = false;
                        for (int32_t predecessors : ourSuccessor.predecessors) {
                            if (predecessors == predecesorId)
                                alreadyExists = true;
                        }

                        if (!alreadyExists)
                            ourSuccessor.predecessors.push_back(predecesorId); // add our predecessor to the successor.
                    }

                    // clear ourselves from predecessors.
                    BasicBlock &targetBlock = blocks[targetId];
                    auto &preds = targetBlock.predecessors;
                    std::erase(preds, currentId);

                    block.bType = BlockType::Dead; // mark dead
                    block.successors.clear();
                    block.predecessors.clear();

                    changed = true; // reanalyze graph.
                }
            }
        }
    }

  public:
    ControlFlowAnalyzer() = default;

    AnalyzedFunction DetermineBasicBlocks(LiftedFunction *lpLiftedFunction) {
        auto analyzed = DetermineBasicBlocksInternal(lpLiftedFunction);
        LinkBasicBlocks(analyzed.basicBlocks);
        this->OptimiseGraph(analyzed.basicBlocks);

        for (auto &sub : analyzed.innerFunctions) {
            LinkBasicBlocks(sub.basicBlocks);
            this->OptimiseGraph(analyzed.basicBlocks);
        }
        return analyzed;
    }
};