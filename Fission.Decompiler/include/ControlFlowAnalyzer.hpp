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

    bool IsTerminator(LiftedOperation operation);

    int32_t GetJumpOffset(const LiftedInstruction *lpInstruction);

    int32_t GetBlockIdAtInstruction(const LiftedInstruction *lpTargetInstruction, const std::map<LiftedInstruction *, int32_t> &leaderMap);

    void LinkBasicBlocks(std::vector<BasicBlock> &blocks);

    AnalyzedFunction DetermineBasicBlocksInternal(LiftedFunction *lpLiftedFunction);

    void OptimiseGraph(std::vector<BasicBlock> &blocks);

  public:
    ControlFlowAnalyzer() = default;

    AnalyzedFunction DetermineBasicBlocks(LiftedFunction *lpLiftedFunction);
};

class GraphVisualizer {
    static std::string EscapeHtml(const std::string &str);

    static std::string BlockTypeToString(BlockType type);

    static std::string OperationToString(LiftedOperation op);

    static std::string FormatOperand(const LiftedOperand &operand);

    static std::string GenerateNodeHtml(const BasicBlock &block, const LiftedFunction *func);

    static void GenerateFunctionGraph(std::stringstream &dot, const AnalyzedFunction &func, const std::string &funcPrefix);

  public:
    static std::string GenerateDotGraph(const AnalyzedFunction &rootAnalysis);
};
