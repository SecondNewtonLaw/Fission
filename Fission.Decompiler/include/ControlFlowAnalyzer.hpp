//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include <set>

#include "BytecodeLifter.hpp"
#include "Deserializer.hpp"

#include <map>
#include <queue>
#include <sstream>

enum class BlockType {
    Standard,

    // structures
    IfHeader,     // The start of an 'if' (has conditional branches).
    LoopHeader,   // top of a loop (where the negative jump would land (must be determined using a LoopLatch block)).
    LoopLatch,    // bottom of a loop (negative jump).

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

enum class LoopBlockFlags {
    WhileLoop = 1 << 1,
    ForNumericLoop = 1 << 2,
    ForGeneralLoop = 1 << 3,
    ForGeneralLoop_Pairs = 1 << 4,
    ForGeneralLoop_Indexed = 1 << 5
};

constexpr LoopBlockFlags operator|(LoopBlockFlags lhs, LoopBlockFlags rhs) {
    return static_cast<LoopBlockFlags>(static_cast<uint32_t>(lhs) | static_cast<uint32_t>(rhs));
}

constexpr LoopBlockFlags operator&(uint32_t lhs, LoopBlockFlags rhs) {
    return static_cast<LoopBlockFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr LoopBlockFlags operator&(LoopBlockFlags lhs, LoopBlockFlags rhs) {
    return static_cast<LoopBlockFlags>(static_cast<uint32_t>(lhs) & static_cast<uint32_t>(rhs));
}

constexpr LoopBlockFlags operator~(LoopBlockFlags flag) { return static_cast<LoopBlockFlags>(~static_cast<uint32_t>(flag)); }

inline LoopBlockFlags &operator|=(LoopBlockFlags &lhs, LoopBlockFlags rhs) {
    lhs = lhs | rhs;
    return lhs;
}

inline LoopBlockFlags &operator&=(LoopBlockFlags &lhs, LoopBlockFlags rhs) {
    lhs = lhs & rhs;
    return lhs;
}

struct BasicBlock {
    std::uint32_t dwBlockId = 0x0;
    std::uint32_t dwBlockFlags = 0x0;
    BlockType bType = BlockType::Error;
    BlockTerminator bTerminator = BlockTerminator::Error;
    LiftedInstruction *lpHead = nullptr;
    LiftedInstruction *lpTail = nullptr;

    std::vector<std::uint32_t> successors;
    std::vector<std::uint32_t> predecessors;

    std::vector<LiftedInstruction> phiNodes;

    std::optional<uint32_t> ifStatementTrue;  // if expr then --[[ this ]] end
    std::optional<uint32_t> ifStatementFalse; // if expr then --[[  ]] else --[[ what we care about ]] end

    std::optional<uint32_t> loopHeader; // contains loop header block idx.
    std::optional<uint32_t> loopLatch;  // contains loop latch block idx.
    std::optional<uint32_t> loopExit;   // contains loop exit block idx.
};

struct SSARef {
    uint8_t regIndex;
    int version;

    bool operator<(const SSARef &other) const {
        if (regIndex != other.regIndex) {
            return regIndex < other.regIndex;
        }
        return version < other.version;
    }

    bool operator==(const SSARef &other) const { return regIndex == other.regIndex && version == other.version; }
    SSARef() = default;
    SSARef(int32_t reg, int32_t ver) : regIndex(reg), version(ver) {}
};

namespace std {
    template <> struct hash<SSARef> {
        std::size_t operator()(SSARef &k) const {
            std::size_t h1 = std::hash<uint8_t>{}(k.regIndex);
            std::size_t h2 = std::hash<int32_t>{}(k.version);

            return h1 ^ (h2 << 1);
        }
        std::size_t operator()(const SSARef &k) const {
            std::size_t h1 = std::hash<uint8_t>{}(k.regIndex);
            std::size_t h2 = std::hash<int32_t>{}(k.version);

            return h1 ^ (h2 << 1);
        }
    };
} // namespace std

struct AnalyzedFunction {
    LiftedFunction *lpLiftedFunction; // not owned by structure.
    std::vector<BasicBlock> basicBlocks;

    std::unordered_map<SSARef, LiftedInstruction *> definitionMap;
    std::unordered_map<const LiftedInstruction *, std::vector<int32_t>> implicitUses;

    std::vector<AnalyzedFunction> innerFunctions;

    std::map<SSARef, int32_t> useCounts;

    [[nodiscard]] LiftedInstruction *GetDefinition(const LiftedOperand &operand) const {
        const auto ssaRef = SSARef{operand.value.reg, operand.ssaVersion};
        if (!definitionMap.contains(ssaRef))
            return nullptr;

        return definitionMap.at(ssaRef);
    }

    [[nodiscard]] bool IsConsumedByPhi(const LiftedOperand &op) const {
        if (op.type != LiftedOperandType::Register)
            return false;

        const auto &userList = users.find({op.value.reg, op.ssaVersion});
        if (userList == users.end())
            return false;

        for (const auto *inst : userList->second) {
            if (inst->operation == LiftedOperation::PHI)
                return true;
        }
        return false;
    }

    [[nodiscard]] int32_t GetUseCount(const LiftedOperand &op) const {
        if (op.type != LiftedOperandType::Register)
            return 0;
        const auto it = useCounts.find({op.value.reg, op.ssaVersion});
        return (it != useCounts.end()) ? it->second : 0;
    }

    [[nodiscard]] bool IsSingleUse(const LiftedOperand &op) const { return GetUseCount(op) == 1; }

    [[nodiscard]] bool IsOperation(const LiftedOperand &op, LiftedOperation targetOp) const {
        const auto *def = GetDefinition(op);
        return def && def->operation == targetOp;
    }

    [[nodiscard]] bool IsSimpleOrConstant(const LiftedOperand &op) const {
        const auto *def = GetDefinition(op);
        if (!def)
            return false;

        switch (def->operation) {
        case LiftedOperation::LOAD:
        case LiftedOperation::MOVE:      // simple aliases
        case LiftedOperation::GETUPVAL:  // upvalue reads are usually safe to inline
        case LiftedOperation::GETIMPORT: // global reads (standard)
        case LiftedOperation::GETGLOBAL:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool HasSideEffects(const LiftedOperand &op) const {
        const auto *def = GetDefinition(op);
        if (!def)
            return false;

        switch (def->operation) {
        case LiftedOperation::CALL:
        case LiftedOperation::NAMECALL:
        case LiftedOperation::SETTABLE:
        case LiftedOperation::SETTABLEKS:
        case LiftedOperation::SETTABLEN:
        case LiftedOperation::SETGLOBAL:
        case LiftedOperation::SETUPVAL:
        case LiftedOperation::NEWCLOSURE: // captures upvalues (side effect-ish)
        case LiftedOperation::DUPCLOSURE: // may create and capture.
            return true;
        default:
            return false;
        }
    }
    [[nodiscard]] int32_t GetBlockId(const LiftedInstruction *inst) const {
        for (const auto &block : basicBlocks) {
            if (!block.lpHead)
                continue;
            if (inst >= block.lpHead && inst <= block.lpTail) {
                return static_cast<int32_t>(block.dwBlockId);
            }
        }
        return -1;
    }

    std::unordered_map<SSARef, std::string> variableNames;
    std::unordered_map<int32_t, std::string> upvalueNames;

    std::unordered_map<int, std::string> globalRegNames;
    std::unordered_map<SSARef, std::string> ssaOverrides;

    void SetGlobalName(int32_t reg, const std::string &name) { globalRegNames[reg] = name; }

    void SetVariableName(int32_t reg, int32_t version, const std::string &name) { variableNames[{static_cast<uint8_t>(reg), version}] = name; }

    std::string GetVarName(int32_t reg, int32_t version) {
        SSARef ref{static_cast<uint8_t>(reg), version};
        if (this->ssaOverrides.contains(ref))
            return this->ssaOverrides.at(ref);

        if (this->globalRegNames.contains(reg))
            return this->globalRegNames.at(reg);

        if (variableNames.contains(ref))
            return variableNames.at(ref);

        return std::format("v{}", reg);
    }

    void SetUpvalueName(int32_t index, const std::string &name) { upvalueNames[index] = name; }
    std::string GetUpvalueName(int32_t index) { return upvalueNames.at(index); }

    void ClearVersionName(int32_t reg, int32_t ver) { ssaOverrides.erase({static_cast<uint8_t>(reg), ver}); }

    void ClearAllVersionNames(int32_t reg) {
        for (auto it = ssaOverrides.begin(); it != ssaOverrides.end();) {
            if (it->first.regIndex == static_cast<uint8_t>(reg)) {
                it = ssaOverrides.erase(it);
            } else {
                ++it;
            }
        }
    }

    void PopulateNames() {
        auto lpDeserialized = this->lpLiftedFunction->lpDeserialized;
        this->upvalueNames.clear();
        this->ssaOverrides.clear();
        this->variableNames.clear();
        this->globalRegNames.clear();

        int32_t uIdx = 0;
        for (const auto &name : lpDeserialized->upvalueNames)
            this->upvalueNames[uIdx++] = name;

        if (lpDeserialized->upvalueNames.size() != lpDeserialized->nups) {
            auto missing = lpDeserialized->nups - lpDeserialized->upvalueNames.size();

            for (uint64_t i = lpDeserialized->upvalueNames.size(); i < missing; i++) {
                this->upvalueNames[i] = std::format("uv_{}", i);
            }
        }

        for (size_t i = 0; i < lpDeserialized->numparams; i++) {
            this->SetGlobalName(i, std::format("arg{}", i));
        }
    }

    std::unordered_map<SSARef, std::vector<LiftedInstruction *>> users;
};

inline std::string BlockTypeToString(BlockType type) {
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

inline std::string BlockTerminatorToString(BlockTerminator term) {
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

    void OptimiseGraphInternal(std::vector<BasicBlock> &blocks);

    bool IsConditional(LiftedOperation operation);
    void IdentifyStructuresInternal(AnalyzedFunction &func);

    void PruneUnreachableBlocks(std::vector<BasicBlock> &blocks);

    void DetermineBasicBlocksInternalAdvanced(AnalyzedFunction &func);

  public:
    ControlFlowAnalyzer() = default;

    AnalyzedFunction DetermineBasicBlocks(LiftedFunction *lpLiftedFunction);

    void OptimizeGraph(AnalyzedFunction &func);

    void PruneUnreachable(AnalyzedFunction &func);

    void IdentifyStructures(AnalyzedFunction &func);
};

enum class GraphContent : uint8_t { IROnly, SSAOnly, Both };

class GraphVisualizer {
    static std::string EscapeHtml(const std::string &str);

    static std::string BlockTypeToString(BlockType type);

    static std::string OperationToString(LiftedOperation op);

    static std::string FormatOperand(const LiftedOperand &operand, bool isSsa);

    static std::string GenerateNodeHtml(const BasicBlock &block, const LiftedFunction *func, bool isSsa);

    static void GenerateFunctionGraph(std::stringstream &dot, const AnalyzedFunction &func, const std::string &funcPrefix, bool isSsa);

  public:
    static std::string GenerateDotGraph(const AnalyzedFunction &rootAnalysis, GraphContent contentMode);
};
