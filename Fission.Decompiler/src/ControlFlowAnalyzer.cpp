//
// Created by Dottik on 30/11/2025.
//

#include "ControlFlowAnalyzer.hpp"

bool ControlFlowAnalyzer::IsTerminator(LiftedOperation operation) {
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
    // FORXPREP OPERATIONS AREN'T TERMINATORS, DO NOT ADD HERE.
    case LiftedOperation::FORNLOOP: // terminates a latch block
    case LiftedOperation::FORGLOOP:
    case LiftedOperation::RETURN:
        return true;
    default:
        return false;
    }
}

int32_t ControlFlowAnalyzer::GetJumpOffset(const LiftedInstruction *lpInstruction) {
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

        block.bType = BlockType::Standard;

        switch (tailInst->operation) {
        case LiftedOperation::JUMP:
        case LiftedOperation::LOADNJUMP: // DO NOT FUCKING ADD FORXPREP OPERATIONS HERE, THEY ARE NOT A TERMINATOR.
            block.bTerminator = BlockTerminator::Unconditional;
            if (GetJumpOffset(tailInst) < 0)
                block.bType = BlockType::LoopLatch;
            else
                block.bType = BlockType::Break; // possibly breaking out of a loop.

            if (LiftedOperation::LOADNJUMP == tailInst->operation)
                block.bType = BlockType::Standard;
            break;
        case LiftedOperation::JUMPXEQK:
            block.bTerminator = BlockTerminator::Conditional;
            if (GetJumpOffset(tailInst) < 0) {
                block.bType = BlockType::LoopLatch;
            } else {
                block.bType = BlockType::IfHeader;
            }
        case LiftedOperation::JUMPIF:
        case LiftedOperation::JUMPIFNOT:
        case LiftedOperation::JUMPIFEQ:
        case LiftedOperation::JUMPIFLE:
        case LiftedOperation::JUMPIFLT:
        case LiftedOperation::JUMPIFNOTEQ:
        case LiftedOperation::JUMPIFNOTLE:
        case LiftedOperation::JUMPIFNOTLT:
        case LiftedOperation::FORNLOOP:
        case LiftedOperation::FORGLOOP:
            block.bTerminator = BlockTerminator::Conditional;
            if (tailInst->operation == LiftedOperation::FORNLOOP || tailInst->operation == LiftedOperation::FORGLOOP) {
                if (GetJumpOffset(tailInst) < 0) {
                    block.bType = BlockType::LoopLatch;
                }
            }
            if (GetJumpOffset(tailInst) > 0) {
                block.bType = BlockType::IfHeader;
            }
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

    return AnalyzedFunction{lpLiftedFunction, basicBlocks, subfuncs};
}

void ControlFlowAnalyzer::OptimiseGraph(std::vector<BasicBlock> &blocks) {
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

                std::vector<std::uint32_t> newPredecessors;
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

void ControlFlowAnalyzer::DetermineBasicBlocksInternalAdvanced(AnalyzedFunction &func) {
    this->LinkBasicBlocks(func.basicBlocks);
    this->OptimiseGraph(func.basicBlocks);
    this->PruneUnreachableBlocks(func.basicBlocks);
    for (auto &sub : func.innerFunctions) {
        this->DetermineBasicBlocksInternalAdvanced(sub);
    }
}

AnalyzedFunction ControlFlowAnalyzer::DetermineBasicBlocks(LiftedFunction *lpLiftedFunction) {
    auto analyzed = DetermineBasicBlocksInternal(lpLiftedFunction);
    DetermineBasicBlocksInternalAdvanced(analyzed);
    return analyzed;
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

    ss << "<TR><TD ALIGN=\"LEFT\" BALIGN=\"LEFT\"><B>BLOCK " << block.dwBlockId << " [" << BlockTypeToString(block.bType) << "]</B>";
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
                line << FormatOperand(current->operands[i], isSsa);
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

void GraphVisualizer::GenerateFunctionGraph(std::stringstream &dot, const AnalyzedFunction &func, const std::string &funcPrefix, bool isSsa) {
    dot << "\n    subgraph cluster_" << funcPrefix << " {\n";
    dot << "        label=\"" << EscapeHtml(func.lpLiftedFunction->name) << (isSsa ? " (SSA)" : "") << "\";\n";
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
        dot << "            label=<" << GenerateNodeHtml(block, func.lpLiftedFunction, isSsa) << ">\n";
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