//
// Created by Dottik on 10/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "DenominatorAnalysis.hpp"
#include "Deserializer.hpp"
#include "FissionDebugNotes.hpp"
#include "InlineCallRecovery.hpp"
#include "lua.h"

#include <bitset>
#include <boost/unordered/unordered_flat_set.hpp>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct ASTFunction {
    AnalyzedFunction *backingFunction = nullptr; // not owned by ASTFunction
    std::vector<std::shared_ptr<Statement>> statements;

};

class ControlFlowTask;

// Limit recovery failure to one function.
struct ASTLiftBudgetExceeded {};

// Everything ASTLifter knows about the function it is lifting; Lift starts each function from a fresh one.
struct ASTLifterFunctionState {
    boost::unordered_flat_set<int32_t> m_definedRegisters;
    std::unordered_set<std::string> m_globalNames;
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_pinnedRegisters;
    // Captured-register declarations must remain before their closures.
    boost::unordered_flat_set<int32_t> m_capturedRegisters;
    // SSA values of locals a closure captures by reference; any call may write them.
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_referenceCapturedValues;
    // SSA values a closure captures by copy; Luau copies only a local never written after its declaration.
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_valueCapturedValues;
    // Registers some instruction writes below a register it reads (see AssignedLocal).
    boost::unordered_flat_set<int32_t> m_assignedRegisters;
    // This function's dominator tree, built on first use.
    std::optional<std::map<int32_t, DominatorInfo>> m_dominators;
    // Blocks reachable forward from a merge candidate, per candidate.
    std::unordered_map<uint32_t, boost::unordered_flat_set<uint32_t>> m_forwardReach;
    // Registers declared ahead of a loop whose break arms first write them; their first version assigns.
    boost::unordered_flat_set<int32_t> m_hoistedRegisters;
    // Writes to a by-reference captured local after its capture; without debug locals they reassign that local.
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_capturedVariableWrites;
    // `target op= rhs`: a store writing back, through the load's own register, an arithmetic op on that load.
    struct CompoundAssignment {
        const LiftedInstruction *operation;
        // the load, the op and target temporaries, all rendered by the compound statement
        std::vector<const LiftedInstruction *> inlined;
    };
    std::unordered_map<const LiftedInstruction *, CompoundAssignment> m_compoundAssignments;
    boost::unordered_flat_set<const LiftedInstruction *> m_compoundInlined;

    // Per instruction index: what lifting has rendered of it so far. Copied whole to undo a speculative lift.
    class EmissionLedger {
        enum : uint8_t { kEmitted = 1, kPrelift = 2, kInlineConsumed = 4, kFoldConsumed = 8 };
        std::vector<uint8_t> m_flags;
        bool Test(int32_t index, uint8_t flag) const { return index >= 0 && static_cast<size_t>(index) < m_flags.size() && (m_flags[index] & flag); }
        void Set(int32_t index, uint8_t flag) {
            if (index < 0)
                return;
            if (static_cast<size_t>(index) >= m_flags.size())
                m_flags.resize(index + 1, 0);
            m_flags[index] |= flag;
        }

      public:
        bool Emitted(int32_t index) const { return Test(index, kEmitted); }
        void MarkEmitted(int32_t index) { Set(index, kEmitted); }
        // A re-lift may render the instruction again, unless a pre-pass folded it into a later construct (a generic-for's iterator call).
        void Reopen(int32_t index) {
            if (!Test(index, kPrelift) && index >= 0 && static_cast<size_t>(index) < m_flags.size())
                m_flags[index] &= ~kEmitted;
        }
        // Everything emitted so far was folded by the pre-passes.
        void FreezePrelift() {
            for (auto &flags : m_flags)
                if (flags & kEmitted)
                    flags |= kPrelift;
        }
        // Rendered inside another expression; tail duplication must re-inline a pure read that never emitted a declaration.
        bool InlineConsumed(int32_t index) const { return Test(index, kInlineConsumed); }
        void MarkInlineConsumed(int32_t index) { Set(index, kEmitted | kInlineConsumed); }
        // Rendered inside a folded constructor; force-materialized headers must not emit it again.
        bool FoldConsumed(int32_t index) const { return Test(index, kFoldConsumed); }
        void MarkFoldConsumed(int32_t index) { Set(index, kFoldConsumed); }
    };
    EmissionLedger m_emission;
    std::unordered_map<const LiftedInstruction *, std::string> m_setListKeySnapshots;

    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_phiConsumers;

    // Keep cross-block effects and forward-dependent constructors at their original execution site.
    boost::unordered_flat_set<const LiftedInstruction *> m_forcedMaterialization;

    // Repeat conditions consume terminator-only definitions exactly once.
    boost::unordered_flat_set<const LiftedInstruction *> m_deferToConditionInline;

    // Single-use call-argument closures, keyed by SSA reference.
    std::unordered_map<SSARef, std::shared_ptr<FunctionDeclarationNode>> m_inlineableClosures;
    struct SharedClosure {
        size_t count = 0;
        bool sameCaptures = true;
        std::vector<std::tuple<int32_t, int32_t, int32_t>> captures;
        std::string name;
        std::shared_ptr<FunctionDeclarationNode> declaration;
    };
    std::unordered_map<int32_t, SharedClosure> m_sharedClosures;
    // Constructor fields holding a closure lifted after the constructor; its handler fills them in place.
    std::unordered_map<SSARef, std::vector<std::shared_ptr<FunctionDeclarationNode>>> m_closureSlots;

    // Class declarations collect following NEWCLASSMEMBER operations by SSA reference.
    std::unordered_map<SSARef, std::shared_ptr<ClassDeclarationNode>> m_pendingClasses;

    // Bound recursive definition lookup for hostile graphs.
    int m_expressionDepth = 0;

    // Cap irreducible CFG re-lifts before memory exhaustion.
    uint64_t m_blockLiftBudget = 0;
    uint64_t m_blockLiftsPerformed = 0;

    // FindMergeBlock is pure over a fixed CFG.
    // (branchA, branchB, innermost loop exit, loop jumps exit) -> merge
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, bool>, int32_t> m_mergeCache;

    // Reverse definition map keeps ShouldInline lookup constant-time.
    std::unordered_map<const LiftedInstruction *, std::vector<SSARef>> m_defsByInstruction;

    // Captured SSA references must remain real locals.
    boost::unordered_flat_set<SSARef, std::hash<SSARef>> m_capturedDefs;

    // Branches to the innermost loop exit become break statements.
    std::vector<uint32_t> m_loopExitStack;
    std::vector<std::pair<uint32_t, std::string>> m_earlyForExits;
    // Blocks whose lifting is still in progress up the call chain; a shared tail must not re-enter them.
    std::vector<uint32_t> m_liftingBlocks;
    // Phi outputs of a condition's value term, read as the term's rebuilt expression while the condition is lifted.
    boost::unordered_flat_map<SSARef, std::shared_ptr<Expression>, std::hash<SSARef>> m_valueTermOverrides;

    // ShouldInline inputs remain fixed during a function lift.
    std::unordered_map<const LiftedInstruction *, bool> m_shouldInlineMemo;
    std::unordered_map<const LiftedInstruction *, uint8_t> m_inlineBinaryDepth;
    std::unordered_map<const LiftedInstruction *, bool> m_constructorElementMemo;
    std::unordered_set<const LiftedInstruction *> m_shouldInlineActive;
    // Materialized LOADB-diamond booleans cannot fold into table literals.
    std::unordered_set<const LiftedInstruction *> m_diamondBoolLoads;

    // Natural loop bodies, one per header, merged over its latches; and the registers each body writes.
    struct LoopBody {
        std::vector<bool> blocks;
        std::bitset<256> writes;
    };
    std::vector<LoopBody> m_loops;
    std::vector<std::vector<uint32_t>> m_loopsOfBlock;
    std::vector<int32_t> m_blockOfInstruction;

    // Cap value-arm re-lifts for pathological CFGs.
    uint32_t m_valueArmDuplications = 0;

    // a `continue` runs the latch's statements; its copy is filled with clones of the latch's one lift
    struct LatchCopies {
        std::vector<std::shared_ptr<BlockStatementNode>> pending;
        std::optional<std::vector<std::shared_ptr<Statement>>> lifted;
    };
    std::unordered_map<int32_t, LatchCopies> m_latchCopies;
    std::unordered_set<const BlockStatementNode *> m_latchCopyNodes;

    // what one walk iteration emitted for the block it entered, chained in walk order; a shared tail clones its first lift
    struct BlockLift {
        uint32_t block;
        uint32_t next;
        std::vector<std::shared_ptr<Statement>> statements;
        // break/continue the walk emitted on reaching `next`
        std::vector<std::shared_ptr<Statement>> trailer{};
        std::shared_ptr<BlockLift> successor{};
    };
    std::unordered_map<uint32_t, std::shared_ptr<BlockLift>> m_blockLifts;
    // visited blocks a walk meets as a tail it must repeat, with the loop exit that tail runs to
    std::unordered_map<uint32_t, uint32_t> m_repeatedTails;

    // union-find over the SSA versions phis join; a root identifies one variable
    std::unordered_map<SSARef, SSARef> m_variableParent;

    SSARef VariableOf(const SSARef &ref) {
        const auto found = m_variableParent.find(ref);
        if (found == m_variableParent.end() || found->second == ref)
            return ref;
        return found->second = VariableOf(found->second);
    }
};

class ASTLifter : private ASTLifterFunctionState {
  public:
    std::shared_ptr<Expression> InvertCondition(const std::shared_ptr<Expression> &cond);
    explicit ASTLifter();

    ASTFunction Lift(AnalyzedFunction &analyzedFunction);
    std::shared_ptr<Expression> LiftCondition(const LiftedInstruction *inst);
    void SetDebugNotes(FissionDebugNotes *debugNotes) { m_debugNotes = debugNotes; }
    void SetInlineSources(std::shared_ptr<const InlineSourceMap> sources) { m_inlineSources = std::move(sources); }

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

    // Renders a closure at a constructor slot or its own block's return instead of declaring it; true when handled.
    bool RenderClosureInPlace(const LiftedInstruction &closure, const std::shared_ptr<FunctionDeclarationNode> &function, bool anonymous);

    int32_t m_dwLastFunctionIndex = 0;

  private:
    FissionDebugNotes *m_debugNotes = nullptr;
    std::shared_ptr<const InlineSourceMap> m_inlineSources;
    // Records on each statement the inlined function its code came from, and notes each call rebuilt from an inlined body.
    void StampInlineSources(std::vector<std::shared_ptr<Statement>> &statements);
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

    ControlFlowTask LiftControlFlow(uint32_t currentBlockId, uint32_t stopBlockId, boost::unordered_flat_set<uint32_t> &visited);
    std::string GetFunctionName(DeserializedFunction *lpDeserialized) {
        if (lpDeserialized->debugName.has_value())
            return std::format("{}", *lpDeserialized->debugName);

        if (this->m_dwLastFunctionIndex == 0)
            this->m_dwLastFunctionIndex = rand(); // randomize the starter value to prevent known value name duplication attacks.
        return std::format("anon_{}_{}", this->m_dwLastFunctionIndex++, lpDeserialized->bytecodeId);
    }

    std::vector<std::shared_ptr<Statement>> LiftBlockInstructions(const BasicBlock &block, bool forceDefinitions = false);
    std::vector<std::shared_ptr<Statement>> EmitBlockInstructions(const BasicBlock &block, bool forceDefinitions);
    // targets of `a, t.k = <multi-value>` whose temporaries each feed one adjacent move or store; empty when not that shape
    std::vector<std::shared_ptr<Expression>> FoldMultiAssignment(
        const LiftedInstruction &def, const std::vector<SSARef> &defs, std::vector<std::shared_ptr<Statement>> &statements, bool &declares
    );
    std::shared_ptr<BlockStatementNode> LatchCopy(uint32_t latchId);
    // clones of the first walk from `fromBlockId` up to `stopBlockId`; `second` when it ran into the innermost loop exit
    std::optional<std::pair<std::vector<std::shared_ptr<Statement>>, bool>> CloneLiftedWalk(uint32_t fromBlockId, uint32_t stopBlockId) const;
    void SpliceLatchCopies(std::vector<std::shared_ptr<Statement>> &statements);
    bool CanReach(uint32_t start, uint32_t target, uint32_t stopBlock, const boost::unordered_flat_set<uint32_t> &visitedScopes);
    std::shared_ptr<Expression> LiftExpression(const LiftedOperand &operand, bool forceExpression = false);
    std::shared_ptr<Expression> ConstantLiteral(int32_t index) const;
    std::shared_ptr<Expression>
    LiftCall(const LiftedInstruction &inst, int32_t instructionIndex, bool isNested, std::shared_ptr<Expression> calleeOverride = nullptr);
    // The population stores a constructor folds, in order, and which of their values render as a closure slot.
    struct TableLiteralPlan {
        struct Store {
            int32_t index;
            std::vector<bool> closureSlots;
        };
        std::vector<Store> stores;
        bool foundSetList = false;
        bool tableIsLocal = false;
    };
    // Lifts nothing. `assumeInlined` answers for an inlined table while that is still being decided. Nullopt for a malformed template.
    std::optional<TableLiteralPlan> PlanTableLiteral(const LiftedInstruction &inst, bool assumeInlined);
    std::shared_ptr<TableLiteralNode> RenderTableLiteral(const LiftedInstruction &inst, const TableLiteralPlan &plan);
    std::shared_ptr<TableLiteralNode> LiftTableLiteral(const LiftedInstruction &inst);
    // The location a SETGLOBAL, SETUPVAL or SETTABLE* writes.
    std::shared_ptr<Expression> LiftStoreTarget(const LiftedInstruction &store);
    // Resolve SETLIST elements for folded and deferred constructor paths.
    std::shared_ptr<Expression> LiftSetListElement(const LiftedInstruction &setList, size_t k, bool forceComputed);
    // Identify calls that require truncation when inlined into a spread-tail position.
    bool IsMultretCall(const LiftedInstruction &callDef, int32_t callDefIndex) const;
    bool ShouldInline(const LiftedInstruction *inst);
    // Mark the def chain of a value lifted inline so block lifting does not emit it again.
    void ConsumeInlinedDefs(const LiftedOperand &operand);
    void ConsumeInlinedInputs(const LiftedInstruction &def);
    bool ShouldInlineImpl(const LiftedInstruction *inst);
    bool SingleUse(const LiftedInstruction *inst) const;
    const LiftedInstruction *OnlyUser(const SSARef &ref) const;
    bool MustMaterialize(const LiftedInstruction *inst);
    std::optional<bool> InlinesConstructor(const LiftedInstruction *inst);
    bool InlinesCall(const LiftedInstruction *inst);
    bool InlinesCopy(const LiftedInstruction *inst);
    bool InlinesValue(const LiftedInstruction *inst);
    static bool CanOperationRaise(LiftedOperation op);
    bool KeyMayRaise(const LiftedInstruction &store) const;
    // Luau never reallocates a parameter's register, so any write to it assigns the parameter.
    bool IsParameterRegister(int32_t reg) const { return reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams; }
    // Prevent effectful definitions from crossing retained instructions.
    bool StaysAsStatement(const LiftedInstruction *e);
    // Defers `def`, read by `reader`, into the loop condition that renders it, and queues its inputs (a constructor's
    // population stores included) for the same decision.
    void DeferIntoCondition(const LiftedInstruction &def, const SSARef &value, const LiftedInstruction *reader,
                            std::vector<std::pair<LiftedOperand, const LiftedInstruction *>> &pending);
    // True if the block of `def` dominates every block whose phi reads `value`.
    bool DominatesMerges(const LiftedInstruction &def, const LiftedOperand &value);
    // True if nothing of `inst` renders at its own position: it inlines, peeks at fastcall arguments, or fills an inlined constructor.
    bool RendersInline(const LiftedInstruction *inst);
    bool StoreTargetsFreshTable(const LiftedInstruction *e);
    bool IsConstructorElement(const LiftedInstruction *e);
    bool InliningReordersEffect(const LiftedInstruction *def, const LiftedInstruction *use);
    // True when a binding `def` reads is reassigned before `use`, so inlining would read the new value.
    bool InputRebound(const LiftedInstruction *def, const LiftedInstruction *use, int32_t skipIndex = -1, bool sameRegister = true);
    // True if an instruction satisfying `changes` runs on some path from `def` to `use` without re-running `def`.
    bool ChangedOnPath(const LiftedInstruction *def, const LiftedInstruction *use, const std::function<bool(const LiftedInstruction &)> &changes);
    // Adds to `seen` the blocks reachable from `pending` along successor (or predecessor) edges without entering block `avoid`; false past the budget.
    bool ReachAvoiding(std::vector<uint32_t> pending, bool forward, int32_t avoid, boost::unordered_flat_set<uint32_t> &seen) const;
    void ComputeLoopBodies();
    // True if a location `def` reads outside its registers may be written before one of its uses.
    bool ReadLocationChanged(const LiftedInstruction *def);
    // True if `inst` inlines into an operand of `use` that is evaluated after the operand holding `value`, so rendering keeps their order.
    bool RendersAfter(const LiftedInstruction &inst, const SSARef &value, const LiftedInstruction *use);
    // What a value reads once inlined: its own register inputs first, then those of single-use inputs inlined into it.
    struct ValueReads {
        std::vector<LiftedOperand> registers;
        size_t directRegisters = 0;
        bool capturedLocal = false;
        bool incomplete = false;
        boost::unordered_flat_set<int32_t> globals, upvalues;
    };
    ValueReads CollectReads(const LiftedInstruction *def);
    // A LOADB-diamond boolean load: its value is the comparison collapsed later, not the literal.
    bool IsDiamondBoolLoad(const LiftedOperand &operand) const;
    // Registers a SETLIST stores and a call passes, at the versions read.
    std::vector<LiftedOperand> SetListElements(const LiftedInstruction &setList) const;
    std::vector<LiftedOperand> CallArguments(const LiftedInstruction &call) const;
    // The user of `ref` when every recorded use is the same instruction.
    const LiftedInstruction *SoleUser(const SSARef &ref) const;
    // The one instruction reading `ref`, not counting a table's own population stores or a fastcall peeking at its arguments.
    const LiftedInstruction *ValueReader(const SSARef &ref) const;
    // Follows the inlined value `inst` defines through its single readers, entering an inlined constructor through its population
    // store, until it reaches `target` (a reader, or a constructor it is stored into); returns the value read there.
    std::optional<SSARef> InlinedChainInto(const LiftedInstruction &inst, const LiftedInstruction *target);
    // True if a debug local in `reg` begins right after `inst` (and its AUX word), so `inst` declares it.
    bool BeginsDebugLocal(const LiftedInstruction &inst, int32_t reg) const;
    // True if `inst`'s write to `target` declares a local although its register was used before.
    bool DeclaresLocal(const LiftedInstruction &inst, const LiftedOperand &target) const;
    void CollectCompoundAssignments();
    // Drops compounds whose right-hand side does not fully inline: a part left as a statement would run before the load.
    void KeepOrderedCompoundAssignments();
    int32_t BlockOf(const LiftedInstruction *inst) const {
        const auto &instructions = m_currentFunction->lpLiftedFunction->instructions;
        if (instructions.empty() || inst < instructions.data() || inst >= instructions.data() + instructions.size())
            return -1;
        return m_blockOfInstruction[inst - instructions.data()];
    }
    std::string ResolveVariableName(const LiftedOperand &op, bool markDefined = true);
    // The register's previous write in this block belongs to a differently named variable.
    bool RegisterHeldOtherVariable(const LiftedInstruction &inst, const LiftedOperand &target);
    void SeedEnclosingNames(AnalyzedFunction &target) const;
    int32_t FindMergeBlock(uint32_t branchA, uint32_t branchB, bool loopJumpsExit = false);

    // Pure shared value arms may be re-lifted without duplicating effects.
    bool IsDuplicableValueArm(uint32_t blockId, uint32_t stopBlockId) const;
    // Extend safe re-lifting across pure short-circuit regions that reconverge at one merge.
    bool IsDuplicablePureRegion(uint32_t startId, uint32_t stopBlockId) const;
    // Blocks of a forward region entered only at `start` and ending at `stop` or a return.
    std::optional<std::vector<uint32_t>> SharedTailRegion(uint32_t start, uint32_t stop, size_t maxInstructions = 64, size_t maxBlocks = 6) const;

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
    // Without `build` only the diamond's shape is checked and nothing is lifted.
    std::optional<BoolMaterialization> DetectBooleanMaterialization(uint32_t headerId, bool build = true);

    // Short-circuit OR chain whose headers share one body.
    struct OrChainInfo {
        std::shared_ptr<Expression> condition; // a or b or c ...
        uint32_t bodyIdx;                      // shared then-body block (the OR target)
        uint32_t elseIdx;                      // block reached when every term is false
        std::vector<uint32_t> chainBlocks;     // the header blocks subsumed into `condition`
    };
    // Match consecutive headers ending in the inverted term that distinguishes OR from AND.
    // Lifting the condition marks its definitions processed; without `build` only the structure is returned.
    std::optional<OrChainInfo> DetectOrChain(uint32_t headerId, bool build = true);
    std::optional<OrChainInfo> DetectGuardRegion(uint32_t headerId, bool build = true);

    // Recover an outer infinite loop when it shares an inner loop's header.
    std::optional<uint32_t> DetectInfiniteWhileLatch(uint32_t headerId, uint32_t innerLatchId);
    std::optional<std::string> LoopDebugName(int32_t reg, int32_t prepIndex) const;
};
