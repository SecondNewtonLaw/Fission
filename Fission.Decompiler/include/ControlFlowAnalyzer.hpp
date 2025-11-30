//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include <set>

#include "BytecodeLifter.hpp"

#include <map>
#include <sstream>

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

std::string BlockTypeToString(BlockType type) {
    switch (type) {
    case BlockType::Standard:
        return "Standard";
    case BlockType::IfHeader:
        return "IfHeader";
    case BlockType::LoopHeader:
        return "LoopHeader";
    case BlockType::LoopLatch:
        return "LoopLatch";
    case BlockType::Break:
        return "Break";
    case BlockType::Continue:
        return "Continue";
    case BlockType::Return:
        return "Return";
    case BlockType::Error:
        return "Error";
    case BlockType::Dead:
        return "Dead/Pruned/Optimized Away";
    default:
        return "Unknown";
    }
}

std::string BlockTerminatorToString(BlockTerminator term) {
    switch (term) {
    case BlockTerminator::Fallthrough:
        return "Fallthrough";
    case BlockTerminator::Unconditional:
        return "Unconditional";
    case BlockTerminator::Conditional:
        return "Conditional";
    case BlockTerminator::Return:
        return "Return";
    case BlockTerminator::Error:
        return "Error";
    default:
        return "Unknown";
    }
}

class ControlFlowAnalyzer {

    bool IsTerminator(LiftedOperation operation) {
        switch (operation) {
            // unconditionals
        case LiftedOperation::FORNPREP:
        case LiftedOperation::FORGPREP:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGPREP_NEXT:
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
        case LiftedOperation::FORNPREP:
        case LiftedOperation::FORGPREP:
        case LiftedOperation::FORGPREP_INEXT:
        case LiftedOperation::FORGPREP_NEXT:
            return lpInstruction->operands[1].value.imm.n - 1 /* we must lay ourselves into a prep instruction. */;

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

        for (size_t currentIndex = 0; currentIndex < totalInstructions; ++currentIndex) {
            auto instruction = &lpLiftedFunction->instructions.at(currentIndex);
            if (this->IsTerminator(instruction->operation)) {
                if (currentIndex + 1 < totalInstructions)
                    leaderIndexes.insert(currentIndex + 1);
                if (instruction->operation != LiftedOperation::RETURN) {
                    const int32_t offset = GetJumpOffset(instruction);
                    int64_t targetIndex = static_cast<int64_t>(currentIndex) + 1 + offset;
                    if (targetIndex >= 0 && targetIndex < static_cast<int64_t>(totalInstructions)) {
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
            } else if (tailInst->operation == LiftedOperation::FORNLOOP || tailInst->operation == LiftedOperation::FORGLOOP) {
                block.bTerminator = BlockTerminator::Conditional; // only if it needs to continue.

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
            this->OptimiseGraph(sub.basicBlocks);
        }
        return analyzed;
    }
};

class GraphVisualizer {
  private:
    static std::string EscapeHtml(const std::string &str) {
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

    static std::string BlockTypeToString(BlockType type) {
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
            return "Break";
        case BlockType::Continue:
            return "Cont";
        case BlockType::Return:
            return "Return";
        case BlockType::Dead:
            return "Dead";
        default:
            return "Unknown";
        }
    }

    static std::string OperationToString(LiftedOperation op) { return std::string(::OperationToString(op)); }

    static std::string FormatOperand(const LiftedOperand &operand) {
        switch (operand.type) {
        case LiftedOperandType::Register:
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

    static std::string GenerateNodeHtml(const BasicBlock &block, const LiftedFunction *func) {
        std::stringstream ss;

        ss << "<TABLE BORDER=\"1\" CELLBORDER=\"0\" CELLSPACING=\"0\" CELLPADDING=\"4\">";

        ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><B>BLOCK " << block.dwBlockId << " [" << BlockTypeToString(block.bType) << "]</B>";
        if (block.bTerminator == BlockTerminator::Conditional)
            ss << " (Cond)";
        else if (block.bTerminator == BlockTerminator::Unconditional)
            ss << " (Jump)";
        ss << "</TD></TR>";

        ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"#888888\">--------------------------------------------------</FONT></TD></TR>";

        if (block.lpHead && block.lpTail) {
            const LiftedInstruction *current = block.lpHead;
            while (true) {
                if (current < func->instructions.data() || current >= func->instructions.data() + func->instructions.size()) {
                    ss << "<TR><TD ALIGN=\"LEFT\"><FONT COLOR=\"RED\">(Ptr Error)</FONT></TD></TR>";
                    break;
                }

                if (current->operation == LiftedOperation::NOP) {
                    if (current == block.lpTail)
                        break;
                    current++;
                    continue;
                }

                std::stringstream line;
                auto idx = std::distance(func->instructions.data(), current);

                line << "_" << idx << ": " << OperationToString(current->operation) << " ";

                for (size_t i = 0; i < current->operands.size(); ++i) {
                    line << EscapeHtml(FormatOperand(current->operands[i]));
                    if (i < current->operands.size() - 1)
                        line << ", ";
                }

                if (current->comment) {
                    std::string cmt = *current->comment;
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

    static void GenerateFunctionGraph(std::stringstream &dot, const AnalyzedFunction &func, const std::string &funcPrefix) {
        dot << "\n    subgraph cluster_" << funcPrefix << " {\n";
        dot << "        label=\"" << EscapeHtml(func.lpLiftedFunction->name) << "\";\n";
        dot << "        style=filled; color=lightgrey; node [style=filled,color=white];\n";

        for (const auto &block : func.basicBlocks) {
            if (block.bType == BlockType::Dead || block.bType == BlockType::Error)
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
            dot << "            label=<" << GenerateNodeHtml(block, func.lpLiftedFunction) << ">\n";
            dot << "            fillcolor=\"" << fillColor << "\"\n";
            dot << "            fontname=\"Courier New\" fontsize=10\n";
            dot << "        ];\n";
        }

        for (const auto &block : func.basicBlocks) {
            if (block.bType == BlockType::Dead || block.bType == BlockType::Error)
                continue;

            std::string srcId = std::format("{}_BLK_{}", funcPrefix, block.dwBlockId);

            bool isFornPrep = (block.lpTail && block.lpTail->operation == LiftedOperation::FORNPREP);
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
                            dot << "        " << srcId << " -> " << childEntryId << " [label=\"NewClosure\", style=dotted, color=purple, fontcolor=purple];\n";
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
            GenerateFunctionGraph(dot, sub, subPrefix);
        }
        dot << "    }\n";
    }

  public:
    static std::string GenerateDotGraph(const AnalyzedFunction &rootAnalysis) {
        std::stringstream dot;
        dot << "digraph LuauCFG {\n";
        dot << "    compound=true;\n";
        dot << "    labelloc=\"t\";\n";
        dot << "    fontname=\"Courier New\";\n";
        GenerateFunctionGraph(dot, rootAnalysis, "ROOT");
        dot << "}\n";
        return dot.str();
    }
};
