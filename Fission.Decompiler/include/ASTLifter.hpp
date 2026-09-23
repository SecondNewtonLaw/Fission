//
// Created by Dottik on 10/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "FissionDebugNotes.hpp"
#include "lua.h"

#include <boost/unordered/unordered_flat_set.hpp>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

};

class ControlFlowTask;

// Limit recovery failure to one function.
struct ASTLiftBudgetExceeded {};

class ASTLifter {
  public:
    std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond);
    explicit ASTLifter();

    ASTFunction Lift(AnalyzedFunction &analyzedFunction);
    std::shared_ptr<Expression> LiftCondition(const LiftedInstruction *inst);
    void SetDebugNotes(FissionDebugNotes *debugNotes) { m_debugNotes = debugNotes; }

    boost::unordered_flat_set<int32_t> m_definedRegisters;
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_pinnedRegisters;
    // Captured-register declarations must remain before their closures.
    boost::unordered_flat_set<int32_t> m_capturedRegisters;

    boost::unordered_flat_set<int32_t> m_processedInstructions;

    // Tail duplication must re-inline pure reads that never emitted a declaration.
    boost::unordered_flat_set<int32_t> m_inlineConsumedDefs;
    // defs rendered inside a folded constructor; force-materialized headers must not emit them again
    boost::unordered_flat_set<int32_t> m_foldConsumedDefs;

    struct PinnedRegisterScope {
        ASTLifter *m_lpLifter;
        SSARef m_ref;
        bool m_inserted;

        PinnedRegisterScope(ASTLifter *lifter, SSARef ref)
            : m_lpLifter(lifter), m_ref(ref), m_inserted(lifter->m_pinnedRegisters.insert(ref).second) {}

        ~PinnedRegisterScope() {
            if (m_inserted)
                m_lpLifter->m_pinnedRegisters.erase(m_ref);
        }

        PinnedRegisterScope(const PinnedRegisterScope &) = delete;
        PinnedRegisterScope &operator=(const PinnedRegisterScope &) = delete;
    };

    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_phiConsumers;

    // Keep cross-block effects and forward-dependent constructors at their original execution site.
    boost::unordered_flat_set<const LiftedInstruction *> m_forcedMaterialization;

    // Repeat conditions consume terminator-only definitions exactly once.
    boost::unordered_flat_set<const LiftedInstruction *> m_deferToConditionInline;

    // Single-use call-argument closures, keyed by SSA reference.
    std::unordered_map<SSARef, std::shared_ptr<FunctionDeclarationNode>> m_inlineableClosures;

    // Class declarations collect following NEWCLASSMEMBER operations by SSA reference.
    std::unordered_map<SSARef, std::shared_ptr<ClassDeclarationNode>> m_pendingClasses;

    int32_t m_dwLastFunctionIndex = 0;

  private:
    FissionDebugNotes *m_debugNotes = nullptr;
    std::string m_debugFunction;

    template <typename... Args> void Explain(std::format_string<Args...> format, Args &&...args) const {
        if (m_debugNotes)
            m_debugNotes->Add(FissionDebugStage::AST, format, std::forward<Args>(args)...);
    }

    template <typename... Args> void Explain(BasicBlock &block, std::format_string<Args...> format, Args &&...args) const {
        if (!m_debugNotes || !m_debugNotes->Enabled())
            return;
        m_debugNotes->AddBlock(FissionDebugStage::AST, m_debugFunction, block.dwBlockId, block.analysisNotes,
                               std::format(format, std::forward<Args>(args)...));
    }

    void ExplainKeep(const LiftedInstruction *definition, std::string_view reason, const LiftedInstruction *consumer = nullptr,
                     const LiftedInstruction *barrier = nullptr) const;

    AnalyzedFunction *m_currentFunction = nullptr;

    // Reject malformed constant indices at the decompiler safety boundary.
    const LuauConstant &ConstantAt(long idx) const;

    // Bound recursive definition lookup for hostile graphs.
    int m_expressionDepth = 0;

    // Cap irreducible CFG re-lifts before memory exhaustion.
    uint64_t m_blockLiftBudget = 0;
    uint64_t m_blockLiftsPerformed = 0;

    // FindMergeBlock is pure over a fixed CFG.
    // (branchA, branchB, innermost loop exit) -> merge
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, int32_t> m_mergeCache;

    // Reverse definition map keeps ShouldInline lookup constant-time.
    std::unordered_map<const LiftedInstruction *, std::vector<SSARef>> m_defsByInstruction;

    // Captured SSA references must remain real locals.
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_capturedDefs;

    // Branches to the innermost loop exit become break statements.
    std::vector<uint32_t> m_loopExitStack;
    // Blocks whose lifting is still in progress up the call chain; a shared tail must not re-enter them.
    std::vector<uint32_t> m_liftingBlocks;
    // Phi outputs of a condition's value term, read as the term's rebuilt expression while the condition is lifted.
    boost::unordered_flat_map<SSARef, std::shared_ptr<Expression>, std::hash<SSARef>> m_valueTermOverrides;

    ControlFlowTask LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, boost::unordered_flat_set<uint32_t> &visited);
    std::string GetFunctionName(DeserializedFunction *lpDeserialized) {
        if (lpDeserialized->debugName.has_value())
            return std::format("{}", *lpDeserialized->debugName);

        if (this->m_dwLastFunctionIndex == 0)
            this->m_dwLastFunctionIndex = rand(); // randomize the starter value to prevent known value name duplication attacks.
        return std::format("anon_{}_{}", this->m_dwLastFunctionIndex++, lpDeserialized->bytecodeId);
    }

    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const BasicBlock &block, bool forceDefinitions = false);
    bool CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const boost::unordered_flat_set<uint32_t> &visitedScopes);
    std::shared_ptr<Expression> LiftExpression(const LiftedOperand &operand, bool forceExpression = false);
    std::shared_ptr<Expression> LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested);
    std::shared_ptr<TableLiteralNode> LiftTableLiteral(const LiftedInstruction &inst);
    // Resolve SETLIST elements for folded and deferred constructor paths.
    std::shared_ptr<Expression> LiftSetListElement(const LiftedInstruction &setList, size_t k, bool forceComputed);
    // Identify calls that require truncation when inlined into a spread-tail position.
    bool IsMultretCall(const LiftedInstruction &callDef, int32_t callDefIndex) const;
    bool ShouldInline(const LiftedInstruction *inst);
    // Mark the def chain of a value lifted inline so block lifting does not emit it again.
    void ConsumeInlinedDefs(const LiftedOperand &operand);
    void ConsumeInlinedInputs(const LiftedInstruction &def);
    bool ShouldInlineImpl(const LiftedInstruction *inst);
    // ShouldInline inputs remain fixed during a function lift.
    std::unordered_map<const LiftedInstruction *, bool> m_shouldInlineMemo;
    std::unordered_set<const LiftedInstruction *> m_shouldInlineActive;
    // Materialized LOADB-diamond booleans cannot fold into table literals.
    std::unordered_set<int32_t> m_diamondBoolRegs;
    static bool CanOperationRaise(LiftedOperation op);
    // Prevent effectful definitions from crossing retained instructions.
    bool StaysAsStatement(const LiftedInstruction *e);
    bool StoreTargetsFreshTable(const LiftedInstruction *e);
    bool IsConstructorElement(const LiftedInstruction *e);
    bool InliningReordersEffect(const LiftedInstruction *def, const LiftedInstruction *use);
    // True when a binding `def` reads is reassigned before `use`, so inlining would read the new value.
    bool InputRebound(const LiftedInstruction *def, const LiftedInstruction *use, int32_t skipIndex = -1, bool sameRegister = true);
    uint32_t FindBlockForInstruction(const LiftedInstruction *inst) const;
    std::string ResolveVariableName(const LiftedOperand &op, bool markDefined = true);
    void SeedEnclosingNames(AnalyzedFunction &target) const;
    int32_t FindMergeBlock(uint32_t branchA, uint32_t branchB);

    // Pure shared value arms may be re-lifted without duplicating effects.
    bool IsDuplicableValueArm(uint32_t blockId, uint32_t stopBlockId) const;
    // Extend safe re-lifting across pure short-circuit regions that reconverge at one merge.
    bool IsDuplicablePureRegion(uint32_t startId, uint32_t stopBlockId) const;
    // Blocks of a small forward region entered only at `start` and ending at `stop` or a return.
    std::optional<std::vector<uint32_t>> SharedTailRegion(uint32_t start, uint32_t stop) const;
    // Cap value-arm re-lifts for pathological CFGs.
    uint32_t m_valueArmDuplications = 0;

    // Hoist phi targets that must outlive branch scopes.
    void HoistPhiLocals(
        int32_t mergeIdx, uint32_t stopBlockId, const std::shared_ptr<IfStatementNode> &ifStmt, std::vector<std::shared_ptr<Statement>> &nodes,
        const boost::unordered_flat_set<int32_t> &definedBeforeBranches
    );

    // Materialized comparison encoded as a LOADB diamond.
    struct BoolMaterialization {
        std::shared_ptr<Statement> assignment; // `Rd = <comparison>`
        uint32_t continueBlock;                // merge block (T) to keep lifting from
    };
    // Collapse a matching LOADB diamond into a boolean expression.
    std::optional<BoolMaterialization> DetectBooleanMaterialization(uint32_t headerId);

    // Short-circuit OR chain whose headers share one body.
    struct OrChainInfo {
        std::shared_ptr<Expression> condition; // a or b or c ...
        uint32_t bodyIdx;                      // shared then-body block (the OR target)
        uint32_t elseIdx;                      // block reached when every term is false
        std::vector<uint32_t> chainBlocks;     // the header blocks subsumed into `condition`
    };
    // Match consecutive headers ending in the inverted term that distinguishes OR from AND.
    std::optional<OrChainInfo> DetectOrChain(uint32_t headerId);
    std::optional<OrChainInfo> DetectGuardRegion(uint32_t headerId);

    // Recover an outer infinite loop when it shares an inner loop's header.
    std::optional<uint32_t> DetectInfiniteWhileLatch(uint32_t headerId, uint32_t innerLatchId);
};
