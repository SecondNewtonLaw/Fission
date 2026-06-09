//
// Created by Dottik on 10/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "lua.h"

#include <memory>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

    std::vector<ASTFunction> subFunctions;
};

class ASTLifter {
  public:
    std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond);
    explicit ASTLifter();

    ASTFunction Lift(AnalyzedFunction &analyzedFunction);
    std::shared_ptr<Expression> LiftCondition(const LiftedInstruction *inst);

    std::unordered_set<int32_t> m_definedRegisters;
    std::unordered_set<int32_t> m_pinnedRegisters;

    std::unordered_set<int32_t> m_processedInstructions;

    struct PinnedRegisterScope {
        ASTLifter *m_lpLifter;
        int32_t dwReg;

        PinnedRegisterScope(ASTLifter *lifter, int32_t reg) : m_lpLifter(lifter), dwReg(reg) { m_lpLifter->m_pinnedRegisters.insert(reg); }

        ~PinnedRegisterScope() { m_lpLifter->m_pinnedRegisters.erase(dwReg); }

        PinnedRegisterScope(const PinnedRegisterScope &) = delete;
        PinnedRegisterScope &operator=(const PinnedRegisterScope &) = delete;
    };

    std::set<SSARef> m_phiConsumers;

    // Closures detected as single-use call-argument candidates. The NEWCLOSURE /
    // DUPCLOSURE handler builds a FunctionDeclarationNode marked
    // `bAnonymousInline = true`, parks it here keyed by the closure's SSA ref,
    // and skips pushing it as a top-level statement. LiftExpression substitutes
    // it in-place when the call argument lookup reaches the same SSA ref.
    std::unordered_map<SSARef, std::shared_ptr<FunctionDeclarationNode>> m_inlineableClosures;

    std::unordered_map<std::string, DeserializedFunction *> m_takenFunctionNames;
    int32_t m_dwLastFunctionIndex = 0;

  private:
    AnalyzedFunction *m_currentFunction = nullptr;

    // Stack of innermost-loop exit blocks. While lifting a loop body, a branch that
    // targets the top entry is a `break` (normal completion flows through the latch,
    // never straight to the exit), so LiftControlFlow emits a BreakStatementNode
    // instead of inlining the post-loop code.
    std::vector<uint32_t> m_loopExitStack;

    std::vector<std::shared_ptr<Statement>> LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, std::set<uint32_t> &visited);
    std::string GetFunctionName(DeserializedFunction *lpDeserialized) {
        if (lpDeserialized->debugName.has_value())
            return std::format("{}", *lpDeserialized->debugName);

        if (this->m_dwLastFunctionIndex == 0)
            this->m_dwLastFunctionIndex = rand(); // randomize the starter value to prevent known value name duplication attacks.
        return std::format("anon_{}_{}", this->m_dwLastFunctionIndex++, lpDeserialized->bytecodeId);
    }

    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const BasicBlock &block, bool forceDefinitions = false);
    bool CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const std::set<uint32_t> &visitedScopes);
    std::shared_ptr<Expression> LiftExpression(const LiftedOperand &operand, bool forceExpression = false);
    std::shared_ptr<Expression> LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested);
    std::shared_ptr<TableLiteralNode> LiftTableLiteral(const LiftedInstruction &inst);
    bool ShouldInline(const LiftedInstruction *inst);
    uint32_t FindBlockForInstruction(const LiftedInstruction *inst) const;
    std::string ResolveVariableName(const LiftedOperand &op);
    int32_t FindMergeBlock(uint32_t branchA, uint32_t branchB);

    // When a register is the target of a phi at the merge block, any
    // `local vR = ...` the branches emitted is wrongly block-scoped: the value
    // must outlive the `if`. Convert those branch declarations into plain
    // assignments and hoist a single uninitialized `local vR` before the if.
    void HoistPhiLocals(int32_t mergeIdx, const std::shared_ptr<IfStatementNode> &ifStmt,
                        std::vector<std::shared_ptr<Statement>> &nodes, const std::unordered_set<int32_t> &definedBeforeBranches);

    // Result of recognising the LOADB-diamond that Luau emits when a comparison
    // is materialised into a register as a boolean value (e.g. `x = a ~= b`).
    struct BoolMaterialization {
        std::shared_ptr<Statement> assignment; // `Rd = <comparison>`
        uint32_t continueBlock;                // merge block (T) to keep lifting from
    };
    // Detect the diamond rooted at `headerId`: a comparison/truth jump whose
    // fall-through block is a single `LOADB Rd,bF (+jump)` and whose jump target
    // begins with `LOADB Rd,bT` (bT != bF). Collapses it into `Rd = cond` (or its
    // negation). Returns nullopt when the shape does not match. On success the
    // two boolean loads are marked processed.
    std::optional<BoolMaterialization> DetectBooleanMaterialization(uint32_t headerId);

    // A short-circuit OR-chain that Luau lowers into a run of IfHeaders all
    // branching to one shared `then` body (`if a or b or c then BODY else ELSE`).
    struct OrChainInfo {
        std::shared_ptr<Expression> condition; // a or b or c ...
        uint32_t bodyIdx;                      // shared then-body block (the OR target)
        uint32_t elseIdx;                      // block reached when every term is false
        std::vector<uint32_t> chainBlocks;     // the header blocks subsumed into `condition`
    };
    // Detect the OR-chain rooted at `headerId`: consecutive IfHeaders whose
    // jump-to-true edge targets a common body, chained through their fall-through,
    // with the final term inverted (its false edge reaches the body). Returns
    // nullopt unless the run is at least two links and ends with that inverted
    // final term — the signature that distinguishes an OR-chain from an AND-chain
    // (whose links uniformly share their true edge with no inverted terminator).
    // Without this the shared body is mistaken for the merge block and emitted as
    // an unconditional tail, clobbering later sibling branches.
    std::optional<OrChainInfo> DetectOrChain(uint32_t headerId);

    // `while <const-true> do` has no header test, so Luau emits only an unconditional
    // back-edge. When that back-edge's latch targets a block that is itself an inner
    // loop header (e.g. a for), both loops share the header and the single loopLatch
    // slot holds only the inner loop — the outer infinite while is lost. Returns that
    // outer latch (a LoopLatch predecessor jumping back unconditionally, distinct from
    // the inner loop's own latch) so the inner loop can be wrapped in `while true`.
    std::optional<uint32_t> DetectInfiniteWhileLatch(uint32_t headerId, uint32_t innerLatchId);
};
