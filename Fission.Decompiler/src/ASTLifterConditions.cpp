//
// Created by Dottik on 23/9/2026.
//

#include "ASTLifter.hpp"
#include "ASTLifterShared.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <set>

#ifndef NDEBUG
#if defined(__clang__)
#pragma clang optimize off
#endif
#endif

std::optional<ASTLifter::BoolMaterialization> ASTLifter::DetectBooleanMaterialization(uint32_t headerId) {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (headerId >= blocks.size())
        return std::nullopt;
    const auto &H = blocks[headerId];
    if (!H.ifStatementTrue.has_value() || !H.ifStatementFalse.has_value() || !H.lpTail)
        return std::nullopt;

    const uint32_t tIdx = *H.ifStatementTrue;  // jump-taken target
    const uint32_t fIdx = *H.ifStatementFalse; // fall-through
    if (tIdx >= blocks.size() || fIdx >= blocks.size())
        return std::nullopt;

    auto firstReal = [&](const BasicBlock &blk) -> const LiftedInstruction * {
        if (!blk.lpHead || !blk.lpTail)
            return nullptr;
        for (int i = blk.lpHead->instructionIndex; i <= blk.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            if (ins.operation != LiftedOperation::NOP)
                return &ins;
        }
        return nullptr;
    };
    auto countReal = [&](const BasicBlock &blk) -> int {
        if (!blk.lpHead || !blk.lpTail)
            return 0;
        int c = 0;
        for (int i = blk.lpHead->instructionIndex; i <= blk.lpTail->instructionIndex; ++i)
            if (m_currentFunction->lpLiftedFunction->instructions[i].operation != LiftedOperation::NOP)
                ++c;
        return c;
    };
    auto isBoolLoad = [](const LiftedInstruction *ins) {
        return ins && ins->operands.size() >= 2 && ins->operands[0].type == LiftedOperandType::Register &&
               ins->operands[1].type == LiftedOperandType::ImmediateBool;
    };

    // Fall-through block F: exactly one real instruction, a `LOADB Rd,bF` that jumps over T
    // into the merge M, and reached only from the header.
    const auto &F = blocks[fIdx];
    if (countReal(F) != 1)
        return std::nullopt;
    const LiftedInstruction *fLoad = firstReal(F);
    if (!fLoad || fLoad->operation != LiftedOperation::LOADNJUMP || !isBoolLoad(fLoad))
        return std::nullopt;
    if (F.successors.size() != 1 || F.predecessors.size() != 1 || F.predecessors[0] != headerId)
        return std::nullopt;
    const uint32_t mIdx = F.successors[0];

    // Jump target T: exactly `LOADB Rd,bT` for the same register, falling into M.
    const auto &T = blocks[tIdx];
    if (countReal(T) != 1)
        return std::nullopt;
    const LiftedInstruction *tLoad = firstReal(T);
    if (!tLoad || tLoad->operation != LiftedOperation::LOAD || !isBoolLoad(tLoad))
        return std::nullopt;
    if (T.successors.size() != 1 || T.successors[0] != mIdx || T.predecessors.size() != 1 || T.predecessors[0] != headerId)
        return std::nullopt;

    const uint8_t reg = fLoad->operands[0].value.reg;
    if (tLoad->operands[0].value.reg != reg)
        return std::nullopt;
    const bool bF = fLoad->operands[1].value.imm.b;
    const bool bT = tLoad->operands[1].value.imm.b;
    if (bF == bT)
        return std::nullopt; // not a true/false split; leave it alone.

    // The merge must be entered only from the two loads, so that `reg` is provably the diamond's boolean.
    if (mIdx >= blocks.size() || std::set<uint32_t>(blocks[mIdx].predecessors.begin(), blocks[mIdx].predecessors.end()) != std::set<uint32_t>{tIdx, fIdx})
        return std::nullopt;

    // Only collapse genuine condition jumps; LiftCondition yields BooleanLiteral
    // for opcodes it cannot turn into a comparison/truth test.
    auto cond = LiftCondition(H.lpTail);
    if (!cond || std::dynamic_pointer_cast<BooleanLiteralNode>(cond))
        return std::nullopt;

    // Control reaches T directly when the jump is taken (cond true -> bT) and via
    // F when it is not (cond false -> bF). So reg == cond when bT is true, else !cond.
    std::shared_ptr<Expression> value = (bT && !bF) ? cond : InvertCondition(cond);

    LiftedOperand target = tLoad->operands[0];
    for (const auto &phi : blocks[mIdx].phiNodes)
        if (phi.operands[0].value.reg == reg)
            target = phi.operands[0];
    const bool isDefined = m_definedRegisters.contains(reg);
    const bool isParameter = reg < m_currentFunction->lpLiftedFunction->lpDeserialized->numparams;
    auto ident = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ResolveVariableName(target)));

    std::shared_ptr<Statement> assignment;
    if ((target.ssaVersion <= 1 && !isParameter) || !isDefined)
        assignment = std::make_shared<VariableDeclarationNode>(ident, value);
    else
        assignment = std::make_shared<AssignmentStatementNode>(ident, value);
    m_definedRegisters.insert(reg);

    // Consume both boolean loads so block lifting does not re-emit them.
    m_processedInstructions.insert(fLoad->instructionIndex);
    m_processedInstructions.insert(tLoad->instructionIndex);

    return BoolMaterialization{assignment, mIdx};
}

void ASTLifter::HoistPhiLocals(
    int32_t mergeIdx, uint32_t stopBlockId, const std::shared_ptr<IfStatementNode> &ifStmt, std::vector<std::shared_ptr<Statement>> &nodes,
    const boost::unordered_flat_set<int32_t> &definedBeforeBranches
) {
    if (mergeIdx < 0 || mergeIdx >= static_cast<int32_t>(m_currentFunction->basicBlocks.size()))
        return;

    const auto &mergeBlock = m_currentFunction->basicBlocks[mergeIdx];
    if (mergeBlock.phiNodes.empty())
        return;

    // Top-level statements of a branch body; null-safe.
    auto branchBody = [](const std::shared_ptr<BlockStatementNode> &branch) -> std::vector<std::shared_ptr<Statement>> * {
        return branch ? &branch->body : nullptr;
    };

    std::vector<std::vector<std::shared_ptr<Statement>> *> branches;
    if (auto *b = branchBody(ifStmt->thenBranch))
        branches.push_back(b);
    if (auto *b = branchBody(ifStmt->elseBranch))
        branches.push_back(b);

    for (const auto &phi : mergeBlock.phiNodes) {
        if (phi.operands.empty() || phi.operands[0].type != LiftedOperandType::Register)
            continue;

        int32_t reg = phi.operands[0].value.reg;
        std::string name = m_currentFunction->GetVarName(reg, phi.operands[0].ssaVersion);
        if (name.empty())
            name = std::format("v{}", reg);

        // Find every initialized `local <name> = ...` at branch top-level. If a
        // branch carries one, the value escapes the branch and must be hoisted.
        bool needsHoist = false;
        for (auto *body : branches) {
            for (auto &stmt : *body) {
                // Case 1: `local <name> = <expr>` (VariableDeclarationNode). Convert to a
                // bare assignment so the hoisted pre-declaration is the sole `local`.
                if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt); decl && decl->value != nullptr) {
                    auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(decl->identifier);
                    if (!ident || !ident->identifier || ident->identifier->name != name)
                        continue;

                    stmt = std::make_shared<AssignmentStatementNode>(decl->identifier, decl->value);
                    needsHoist = true;
                    continue;
                }

                // Calls stay in place; only their local-declaration marker moves to the hoisted binding.
                if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
                    auto retMatches = [&](const std::vector<std::shared_ptr<Expression>> &rets) -> bool {
                        if (rets.size() != 1)
                            return false;
                        auto ident = std::dynamic_pointer_cast<IdentifierExpressionNode>(rets[0]);
                        return ident && ident->identifier && ident->identifier->name == name;
                    };
                    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression); nameCall && retMatches(nameCall->rets)) {
                        nameCall->bIsLocalDeclaration = false;
                        needsHoist = true;
                    } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression); call && retMatches(call->rets)) {
                        call->bIsLocalDeclaration = false;
                        needsHoist = true;
                    }
                }
            }
        }

        // Declare only new branch-local merges. Existing outer bindings and enclosing stop-block phis
        // must keep their owning scope or branch declarations will shadow the value seen after the merge.
        if (needsHoist && !definedBeforeBranches.contains(reg) && mergeIdx != static_cast<int32_t>(stopBlockId)) {
            nodes.push_back(std::make_shared<VariableDeclarationNode>(std::make_shared<Identifier>(name)));
            m_definedRegisters.insert(reg);
        }
    }
}

bool ASTLifter::IsDuplicableValueArm(uint32_t blockId, uint32_t stopBlockId) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (blockId >= blocks.size() || stopBlockId >= blocks.size())
        return false;
    const auto &b = blocks[blockId];
    // only a plain value block whose single successor IS the branch merge; not a header/loop/return, and
    // not a block that flows somewhere else first (duplicating a larger region would be unsound).
    if (b.bType != BlockType::Standard || b.successors.size() != 1 || b.successors[0] != stopBlockId || !b.lpHead || !b.lpTail)
        return false;

    // every real instruction must be a pure, idempotent load/move: re-running it cannot raise, has no
    // side effect, and yields the same value. arithmetic (can raise on bad types), calls, and any
    // table/global/upvalue store are excluded; duplicating those would move or double an effect.
    bool feedsMergePhi = false;
    for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
        const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
        if (ins.operation == LiftedOperation::NOP)
            continue;
        if (ins.operation != LiftedOperation::LOAD && ins.operation != LiftedOperation::MOVE)
            return false;
        if (ins.operands.empty() || ins.operands[0].type != LiftedOperandType::Register)
            return false;
        const int32_t dst = ins.operands[0].value.reg;
        for (const auto &phi : blocks[stopBlockId].phiNodes)
            if (!phi.operands.empty() && phi.operands[0].type == LiftedOperandType::Register && phi.operands[0].value.reg == dst)
                feedsMergePhi = true;
    }
    // require the block to actually feed the merge phi; proves it is a value arm, not an empty block.
    return feedsMergePhi;
}

std::optional<std::vector<uint32_t>> ASTLifter::SharedTailRegion(uint32_t start, uint32_t stop) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (start >= blocks.size() || stop >= blocks.size() || start == stop)
        return std::nullopt;
    // small, forward-only, entered only through `start`, and ending at `stop`, a return, or a `break` out of the innermost loop
    constexpr size_t kMaxBlocks = 6, kMaxInstructions = 64;
    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    std::vector<uint32_t> region;
    boost::unordered_flat_set<uint32_t> inRegion;
    std::vector<uint32_t> pending{start};
    size_t instructions = 0;
    while (!pending.empty()) {
        const uint32_t id = pending.back();
        pending.pop_back();
        if (id == stop || id == loopExit)
            continue;
        if (id >= blocks.size())
            return std::nullopt;
        if (!inRegion.insert(id).second)
            continue;
        const auto &block = blocks[id];
        if (inRegion.size() > kMaxBlocks || !block.lpHead || (block.successors.empty() && block.bType != BlockType::Return) ||
            std::ranges::find(m_liftingBlocks, id) != m_liftingBlocks.end())
            return std::nullopt;
        instructions += static_cast<size_t>(block.lpTail - block.lpHead) + 1;
        if (instructions > kMaxInstructions)
            return std::nullopt;
        for (const uint32_t successor : block.successors) {
            // a loop wholly inside the tail is copied with it; a back-edge out of the region is not
            if (successor <= id) {
                if (!inRegion.contains(successor))
                    return std::nullopt;
                continue;
            }
            pending.push_back(successor);
        }
        region.push_back(id);
    }
    for (const uint32_t id : region)
        if (id != start)
            for (const uint32_t predecessor : blocks[id].predecessors)
                if (!inRegion.contains(predecessor))
                    return std::nullopt;
    return region;
}

bool ASTLifter::IsDuplicablePureRegion(uint32_t startId, uint32_t stopBlockId) const {
    const auto &blocks = m_currentFunction->basicBlocks;
    if (startId >= blocks.size() || stopBlockId >= blocks.size() || startId == stopBlockId)
        return false;

    // cap region size: real short-circuit value regions are a handful of blocks. a hostile deeply nested
    // boolean DAG must not turn each duplication into unbounded work (the global m_valueArmDuplications
    // cap bounds the count of duplications; this bounds the cost of each).
    constexpr size_t kMaxRegionBlocks = 32;

    // a block is part of a pure short-circuit region iff it only loads/moves values (a value block) or
    // branches on a register's truthiness (JUMPIF/JUMPIFNOT terminator). anything else; a store, call,
    // arithmetic, comparison, global/table op; can raise or have an effect, so it is not duplicable.
    const auto isPureValueOrTruthiness = [&](const BasicBlock &b) -> bool {
        if (!b.lpHead || !b.lpTail)
            return false;
        if (b.bType != BlockType::Standard && b.bType != BlockType::IfHeader)
            return false;
        for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            switch (ins.operation) {
            case LiftedOperation::NOP:
            case LiftedOperation::LOAD:
            case LiftedOperation::MOVE:
                continue;
            case LiftedOperation::JUMPIF:
            case LiftedOperation::JUMPIFNOT:
                // a truthiness branch on a register is pure, but only valid as the block terminator.
                if (&ins != b.lpTail || ins.operands.empty() || ins.operands[0].type != LiftedOperandType::Register)
                    return false;
                continue;
            default:
                return false;
            }
        }
        return true;
    };

    // forward flood from startId, stopping at stopBlockId (the merge boundary). every block reached must
    // be pure and every successor must stay inside the region or hit stopBlockId; an escape to a
    // non-pure block, a return, or a dangling edge means stopBlockId does not post-dominate the region
    // and re-lifting up to it would pull in something with side effects.
    boost::unordered_flat_set<uint32_t> region;
    std::vector<uint32_t> stack{startId};
    bool feedsMergePhi = false;
    while (!stack.empty()) {
        const uint32_t cur = stack.back();
        stack.pop_back();
        if (cur == stopBlockId)
            continue; // the merge is the boundary; do not traverse into it.
        if (cur >= blocks.size())
            return false;
        if (!region.insert(cur).second)
            continue;
        if (region.size() > kMaxRegionBlocks)
            return false;
        const auto &b = blocks[cur];
        if (!isPureValueOrTruthiness(b) || b.successors.empty())
            return false; // impure, or a pure exit that is not the merge (region does not reconverge).
        // a value block that defines a register read by the merge phi proves this is a value-producing
        // short-circuit, not an arbitrary pure region.
        for (int i = b.lpHead->instructionIndex; i <= b.lpTail->instructionIndex; ++i) {
            const auto &ins = m_currentFunction->lpLiftedFunction->instructions[i];
            if ((ins.operation != LiftedOperation::LOAD && ins.operation != LiftedOperation::MOVE) || ins.operands.empty() ||
                ins.operands[0].type != LiftedOperandType::Register)
                continue;
            const int32_t dst = ins.operands[0].value.reg;
            for (const auto &phi : blocks[stopBlockId].phiNodes)
                if (!phi.operands.empty() && phi.operands[0].type == LiftedOperandType::Register && phi.operands[0].value.reg == dst)
                    feedsMergePhi = true;
        }
        for (const uint32_t succ : b.successors) {
            if (succ >= blocks.size())
                return false;
            stack.push_back(succ);
        }
    }
    return feedsMergePhi;
}

int32_t ASTLifter::FindMergeBlock(uint32_t branchA, uint32_t branchB) {
    if (branchA == branchB)
        return static_cast<int32_t>(branchA);

    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    if (branchB == loopExit)
        std::swap(branchA, branchB);
    const auto cacheKey = std::make_tuple(branchA, branchB, loopExit);
    if (const auto it = m_mergeCache.find(cacheKey); it != m_mergeCache.end())
        return it->second;

    const auto &blocks = m_currentFunction->basicBlocks;
    // a conditional jump into the innermost loop's latch is a `continue`, not flow into the merge
    const auto continuesLoop = [&](const BasicBlock &from, uint32_t to) {
        if (loopExit == InvalidBlockId || from.bTerminator != BlockTerminator::Conditional || blocks[to].bType != BlockType::LoopLatch)
            return false;
        return std::ranges::find(blocks[to].successors, loopExit) != blocks[to].successors.end();
    };

    // true iff every forward path from x hits M before an exit (back-edges skipped to stay acyclic). The
    // merge must post-dominate both branches; a non-post-dominator causes exponential shared-tail re-lift.
    const auto postDominates = [&](uint32_t m, uint32_t x) -> bool {
        // a `break` arm leaves the region without flowing into any merge
        if (m == x || x == loopExit)
            return true;
        boost::unordered_flat_set<uint32_t> seen;
        std::vector<uint32_t> stack{x};
        bool reached = false;
        const auto mergeReaches = [&](uint32_t target) {
            auto [reach, fresh] = m_forwardReach.try_emplace(m);
            if (fresh) {
                std::vector<uint32_t> pending{m};
                while (!pending.empty()) {
                    const uint32_t id = pending.back();
                    pending.pop_back();
                    if (reach->second.insert(id).second)
                        for (const uint32_t succ : blocks[id].successors)
                            if (succ > id)
                                pending.push_back(succ);
                }
            }
            return reach->second.contains(target);
        };
        while (!stack.empty()) {
            const uint32_t cur = stack.back();
            stack.pop_back();
            if (cur == m)
                continue; // this path reached M
            if (!seen.insert(cur).second)
                continue;
            const auto &block = blocks[cur];
            for (const uint32_t succ : block.successors) {
                // block ids follow instruction order, so a backward edge continues a loop (bridges and latches alike)
                if (succ <= cur) {
                    if (succ < x)
                        return false; // the enclosing loop's next iteration is reached without passing M
                    continue;         // an inner loop's back-edge: ignore to keep the walk acyclic
                }
                if (succ == m)
                    reached = true;
                else if (continuesLoop(block, succ) || succ == loopExit)
                    continue; // `continue` and `break` leave the region without flowing into any merge
                else
                    stack.push_back(succ);
            }
            // an exit is reachable from x without passing through M; an early `return` leaves the region like `break`.
            // Not when M returns too (arms ending in copies of one `return` are value arms) or M reaches that return (a join).
            if (block.successors.empty() &&
                (block.bType != BlockType::Return ||
                 (loopExit == InvalidBlockId && (blocks[m].bType == BlockType::Return || mergeReaches(cur)))))
                return false;
        }
        return reached;
    };

    int32_t result = -1;
    const auto usesTerminalFold = [&](uint32_t id) {
        const auto &block = blocks.at(id);
        if (block.bType != BlockType::Return)
            return false;
        // statement arms jump to a shared exit; value arms fall into their return
        if (std::ranges::any_of(block.predecessors, [&](uint32_t pred) {
                return blocks[pred].lpTail && blocks[pred].lpTail->operation == LiftedOperation::JUMP;
            }))
            return false;
        if (block.lpTail->operands.size() > 1 && block.lpTail->operands[1].value.imm.n == 1)
            return true;
        return std::all_of(block.lpHead, block.lpTail + 1, [](const LiftedInstruction &instruction) {
            return instruction.operation == LiftedOperation::RETURN || instruction.operation == LiftedOperation::NOP;
        });
    };
    if (!usesTerminalFold(branchA) && !usesTerminalFold(branchB)) {
        // the walk skips `break` edges, so the loop exit would trivially post-dominate a body that falls into it;
        // a `continue` edge likewise reaches the latch before the code the other paths merge into
        const auto follows = [&](const BasicBlock &block, uint32_t cur, uint32_t succ) {
            return succ > cur && (succ != loopExit || succ == branchA) && (!continuesLoop(block, succ) || succ == branchA || succ == branchB);
        };
        // a block post-dominating branchA is reached from it; the rest need no walk
        boost::unordered_flat_set<uint32_t> fromA{branchA};
        if (branchA != loopExit)
            for (std::vector<uint32_t> pending{branchA}; !pending.empty();) {
                const uint32_t cur = pending.back();
                pending.pop_back();
                for (const uint32_t succ : blocks[cur].successors)
                    if (follows(blocks[cur], cur, succ) && fromA.insert(succ).second)
                        pending.push_back(succ);
            }
        // nearest block (BFS from branchB) that post-dominates both branches is the immediate merge.
        boost::unordered_flat_set<uint32_t> visited;
        std::queue<uint32_t> q;
        q.push(branchB);
        visited.insert(branchB);
        while (!q.empty()) {
            const uint32_t cur = q.front();
            q.pop();
            if ((branchA == loopExit || fromA.contains(cur)) && postDominates(cur, branchA) && postDominates(cur, branchB)) {
                result = static_cast<int32_t>(cur);
                break;
            }
            const auto &block = blocks[cur];
            for (const uint32_t succ : block.successors)
                if (follows(block, cur, succ) && visited.insert(succ).second)
                    q.push(succ);
        }
    }

    m_mergeCache.emplace(cacheKey, result);
    return result;
}

std::optional<ASTLifter::OrChainInfo> ASTLifter::DetectOrChain(uint32_t headerId) {
    if (auto guard = DetectGuardRegion(headerId))
        return guard;
    const auto &blocks = m_currentFunction->basicBlocks;
    if (headerId >= blocks.size())
        return std::nullopt;
    const auto &head = blocks[headerId];
    if (head.bType != BlockType::IfHeader || !head.ifStatementTrue.has_value() || !head.ifStatementFalse.has_value())
        return std::nullopt;

    // The shared OR target is the head's jump-to-true edge. Every link of the
    // chain must reach this same block.
    const uint32_t body = head.ifStatementTrue.value();
    const auto truthyClosure = [&](uint32_t id) -> const LiftedInstruction * {
        if (id >= blocks.size())
            return nullptr;
        const auto &b = blocks[id];
        if (!b.lpTail || (b.lpTail->operation != LiftedOperation::JUMPIF && b.lpTail->operation != LiftedOperation::JUMPIFNOT) ||
            b.lpTail->operands.empty() || b.lpTail->operands[0].type != LiftedOperandType::Register)
            return nullptr;
        for (auto *inst = b.lpHead; inst && inst < b.lpTail; ++inst)
            if ((inst->operation == LiftedOperation::NEWCLOSURE || inst->operation == LiftedOperation::DUPCLOSURE) && !inst->operands.empty() &&
                inst->operands[0].type == LiftedOperandType::Register && inst->operands[0].value.reg == b.lpTail->operands[0].value.reg &&
                inst->operands[0].ssaVersion == b.lpTail->operands[0].ssaVersion) {
                // `x and function() end or y` also carries the closure out as the value; only a bare test folds to true
                const auto users = m_currentFunction->users.find(SSARef{static_cast<uint8_t>(inst->operands[0].value.reg), inst->operands[0].ssaVersion});
                if (users != m_currentFunction->users.end() && std::ranges::any_of(users->second, [&](const auto *user) { return user != b.lpTail; }))
                    return nullptr;
                return inst;
            }
        return nullptr;
    };
    const auto isLink = [&](uint32_t id) {
        if (id >= blocks.size())
            return false;
        const auto &b = blocks[id];
        if (b.bType != BlockType::IfHeader || !b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value() ||
            (b.ifStatementTrue.value() != body && b.ifStatementFalse.value() != body))
            return false;
        const auto *closure = truthyClosure(id);
        for (auto *inst = b.lpHead; inst && inst < b.lpTail; ++inst)
            if (inst != closure && inst->operation != LiftedOperation::NOP && inst->operation != LiftedOperation::PHI && !RendersInline(inst))
                return false;
        return true;
    };

    // Structural walk first (no condition lifting): follow the run while each link
    // shares `body`. A true edge hitting body continues through the false edge; the
    // final false edge must hit body, distinguishing OR dispatch from value folding.
    struct Link {
        uint32_t blockId;
        bool invert;
    };
    std::vector<Link> links;
    std::set<uint32_t> guard;
    uint32_t cur = headerId;
    uint32_t elseIdx = InvalidBlockId;
    bool complete = false;
    bool invertedFinal = false;
    bool truthyFinal = false;

    while (cur < blocks.size() && !guard.contains(cur)) {
        const auto &b = blocks[cur];
        if (b.bType != BlockType::IfHeader || !b.ifStatementTrue.has_value() || !b.ifStatementFalse.has_value())
            break;
        if (!b.lpTail || b.bTerminator != BlockTerminator::Conditional)
            break;

        const uint32_t bt = b.ifStatementTrue.value();
        const uint32_t bf = b.ifStatementFalse.value();

        if (bt == body) {
            links.push_back({cur, false});
            guard.insert(cur);
            // a test also reached from outside the chain is a join of the enclosing code, not the next term
            const bool enteredFromChain = bf < blocks.size() && std::ranges::all_of(blocks[bf].predecessors, [&](uint32_t p) { return guard.contains(p); });
            if (isLink(bf) && !guard.contains(bf) && enteredFromChain) {
                cur = bf; // fall-through is the next link
                continue;
            }
            elseIdx = bf;
            complete = true;
            truthyFinal = truthyClosure(cur) != nullptr;
            break;
        }
        if (bf == body) {
            links.push_back({cur, true}); // inverted final term: false edge reaches body
            guard.insert(cur);
            elseIdx = bt;
            complete = true;
            invertedFinal = true;
            break;
        }
        break; // does not share the body -> end of (non-)chain
    }

    if (!complete || links.size() < 2 || elseIdx == InvalidBlockId)
        return std::nullopt;
    if (!invertedFinal && !truthyFinal) {
        // Every link jumps to `body`: an `and` guard, read as `not a or not b`. Only when `body` is a real
        // arm that joins `elseIdx` later; if `body` is itself the join, this is a folded `x = a and b` value.
        const int32_t join = FindMergeBlock(body, elseIdx);
        if (join < 0 || static_cast<uint32_t>(join) == body || static_cast<uint32_t>(join) == elseIdx ||
            CanReach(elseIdx, body, static_cast<uint32_t>(join), {headerId}))
            return std::nullopt;
    }

    // Structure confirmed. Lift each link's condition (the condition under which it
    // reaches `body`) and OR-fold left to right.
    std::vector<std::shared_ptr<Expression>> conditions;
    conditions.reserve(links.size());
    for (const auto &lk : links) {
        auto c = truthyClosure(lk.blockId)
                     ? std::shared_ptr<Expression>(std::make_shared<BooleanLiteralNode>(blocks[lk.blockId].lpTail->operation == LiftedOperation::JUMPIF))
                     : LiftCondition(blocks[lk.blockId].lpTail);
        if (lk.invert)
            c = InvertCondition(c);
        conditions.push_back(std::move(c));
    }
    while (conditions.size() > 1) {
        std::vector<std::shared_ptr<Expression>> next;
        next.reserve((conditions.size() + 1) / 2);
        for (size_t i = 0; i < conditions.size(); i += 2) {
            if (i + 1 == conditions.size())
                next.push_back(std::move(conditions[i]));
            else
                next.push_back(std::make_shared<BinaryExpressionNode>("or", std::move(conditions[i]), std::move(conditions[i + 1])));
        }
        conditions = std::move(next);
    }

    OrChainInfo info;
    info.condition = std::move(conditions.front());
    info.bodyIdx = body;
    info.elseIdx = elseIdx;
    info.chainBlocks.reserve(links.size());
    for (const auto &lk : links)
        info.chainBlocks.push_back(lk.blockId);
    return info;
}

std::optional<ASTLifter::OrChainInfo> ASTLifter::DetectGuardRegion(uint32_t headerId) {
    const auto &blocks = m_currentFunction->basicBlocks;
    using Expr = std::shared_ptr<Expression>;

    // Luau computes a condition term other than and/or/not/comparison into one register and tests it once
    // (compileConditionValue -> compileExprAuto): a single-entry region whose paths join at a test block
    // holding the only phi, for that register.
    struct ValueTerm {
        uint32_t test;
        std::set<uint32_t> region;
    };
    // rebuilds the value flowing from `x` into `join`; with build == false it only checks the shape
    std::map<std::pair<uint32_t, uint32_t>, Expr> valueMemo;
    std::function<std::optional<Expr>(const ValueTerm &, uint32_t, uint32_t, bool)> valueOf;
    const auto edgeValue = [&](uint32_t from, uint32_t join, bool build) -> std::optional<Expr> {
        const auto &target = blocks[join];
        const auto at = std::ranges::find(target.predecessors, from);
        const auto index = static_cast<size_t>(at - target.predecessors.begin()) + 1;
        if (at == target.predecessors.end() || target.phiNodes.size() != 1 || index >= target.phiNodes.front().operands.size())
            return std::nullopt;
        return build ? LiftExpression(target.phiNodes.front().operands[index], true) : Expr{};
    };
    const auto diamond = [&](const ValueTerm &term, uint32_t x, uint32_t join, bool build) -> std::optional<Expr> {
        const auto &block = blocks[x];
        const uint32_t yesArm = *block.ifStatementTrue, noArm = *block.ifStatementFalse;
        const auto arm = [&](uint32_t target) { return target == join ? edgeValue(x, join, build) : valueOf(term, target, join, build); };
        const auto *tail = block.lpTail;
        // `r = a; JUMPIF(NOT) r -> join` keeps `a` as the value on that edge: `a or rest` / `a and rest`
        if ((tail->operation == LiftedOperation::JUMPIF || tail->operation == LiftedOperation::JUMPIFNOT) && !tail->operands.empty() &&
            tail->operands[0].type == LiftedOperandType::Register && (yesArm == join || noArm == join)) {
            const auto &target = blocks[join];
            const auto at = std::ranges::find(target.predecessors, x);
            const auto index = static_cast<size_t>(at - target.predecessors.begin()) + 1;
            if (at != target.predecessors.end() && target.phiNodes.size() == 1 && index < target.phiNodes.front().operands.size()) {
                const auto &input = target.phiNodes.front().operands[index];
                if (input.type == LiftedOperandType::Register && input.value.reg == tail->operands[0].value.reg &&
                    input.ssaVersion == tail->operands[0].ssaVersion) {
                    const bool truthyToJoin = (tail->operation == LiftedOperation::JUMPIF) == (yesArm == join);
                    Expr left = build ? LiftExpression(tail->operands[0], true) : Expr{};
                    const auto rest = arm(yesArm == join ? noArm : yesArm);
                    if (!rest)
                        return std::nullopt;
                    return build ? Expr(std::make_shared<BinaryExpressionNode>(truthyToJoin ? "or" : "and", left, *rest)) : Expr{};
                }
            }
        }
        Expr condition = build ? LiftCondition(tail) : Expr{};
        const auto yes = arm(yesArm);
        const auto no = yes ? arm(noArm) : std::nullopt;
        if (!yes || !no)
            return std::nullopt;
        if (!build)
            return Expr{};
        // a comparison already is the boolean its `true`/`false` arms spell out
        const auto literal = [](const Expr &value, bool expected) {
            const auto boolean = std::dynamic_pointer_cast<BooleanLiteralNode>(value);
            return boolean && boolean->value == expected;
        };
        if (literal(*yes, true) && literal(*no, false)) {
            const auto compare = std::dynamic_pointer_cast<BinaryExpressionNode>(condition);
            const auto negation = std::dynamic_pointer_cast<UnaryExpressionNode>(condition);
            if ((compare && (compare->op == "==" || compare->op == "~=" || compare->op == "<" || compare->op == "<=" || compare->op == ">" ||
                             compare->op == ">=")) ||
                (negation && negation->op == "not "))
                return condition;
        }
        if (const auto negated = std::dynamic_pointer_cast<UnaryExpressionNode>(condition); negated && negated->op == "not ")
            return Expr(std::make_shared<IfExpressionNode>(negated->operand, *no, *yes));
        return Expr(std::make_shared<IfExpressionNode>(condition, *yes, *no));
    };
    valueOf = [&](const ValueTerm &term, uint32_t x, uint32_t join, bool build) -> std::optional<Expr> {
        if (x == join || !term.region.contains(x))
            return std::nullopt;
        if (build)
            if (const auto memo = valueMemo.find({x, join}); memo != valueMemo.end())
                return memo->second;
        const auto &block = blocks[x];
        std::optional<Expr> result;
        if (block.bTerminator == BlockTerminator::Conditional) {
            if (!block.ifStatementTrue || !block.ifStatementFalse)
                return std::nullopt;
            const int32_t inner = FindMergeBlock(*block.ifStatementTrue, *block.ifStatementFalse);
            if (inner < 0)
                return std::nullopt;
            if (static_cast<uint32_t>(inner) == join) {
                result = diamond(term, x, join, build);
            } else {
                // an inner value (a nested if-expression or and/or) joins first; its phi reads as that value
                const auto innerId = static_cast<uint32_t>(inner);
                if (!term.region.contains(innerId) || blocks[innerId].phiNodes.size() != 1)
                    return std::nullopt;
                const auto innerValue = diamond(term, x, innerId, build);
                if (!innerValue)
                    return std::nullopt;
                if (build) {
                    const auto &out = blocks[innerId].phiNodes.front().operands[0];
                    m_valueTermOverrides[SSARef{static_cast<uint8_t>(out.value.reg), out.ssaVersion}] = *innerValue;
                }
                result = valueOf(term, innerId, join, build);
            }
        } else {
            if (block.successors.size() != 1)
                return std::nullopt;
            const uint32_t next = block.successors.front();
            result = next == join ? edgeValue(x, join, build) : valueOf(term, next, join, build);
        }
        if (build && result)
            valueMemo[{x, join}] = *result;
        return result;
    };
    const auto detectValueTerm = [&](uint32_t entryId) -> std::optional<ValueTerm> {
        const auto &entry = blocks[entryId];
        if (entry.bTerminator != BlockTerminator::Conditional || !entry.ifStatementTrue || !entry.ifStatementFalse)
            return std::nullopt;
        const int32_t join = FindMergeBlock(*entry.ifStatementTrue, *entry.ifStatementFalse);
        if (join < 0 || static_cast<uint32_t>(join) <= entryId)
            return std::nullopt;
        const auto testId = static_cast<uint32_t>(join);
        const auto &test = blocks[testId];
        if (test.bType != BlockType::IfHeader || !test.ifStatementTrue || !test.ifStatementFalse || test.phiNodes.size() != 1 || !test.lpTail ||
            test.bTerminator != BlockTerminator::Conditional)
            return std::nullopt;
        const auto &phiOut = test.phiNodes.front().operands[0];
        const int32_t valueReg = phiOut.value.reg;
        ValueTerm term{testId, {}};
        std::vector<uint32_t> walk{entryId};
        while (!walk.empty()) {
            const uint32_t id = walk.back();
            walk.pop_back();
            if (id == testId || term.region.contains(id))
                continue;
            if (id < entryId || id > testId || term.region.size() >= 16)
                return std::nullopt;
            const auto &block = blocks[id];
            if (!block.lpHead || block.successors.empty() || block.phiNodes.size() > 1 ||
                (block.phiNodes.size() == 1 && block.phiNodes.front().operands[0].value.reg != valueReg))
                return std::nullopt;
            for (const uint32_t successor : block.successors) {
                if (successor <= id)
                    return std::nullopt;
                walk.push_back(successor);
            }
            term.region.insert(id);
        }
        // entered only through the entry, left only through the test
        for (const uint32_t id : term.region)
            if (id != entryId)
                for (const uint32_t pred : blocks[id].predecessors)
                    if (!term.region.contains(pred))
                        return std::nullopt;
        for (const uint32_t pred : test.predecessors)
            if (!term.region.contains(pred))
                return std::nullopt;
        // the joined value is read only by the test
        if (const auto users = m_currentFunction->users.find(SSARef{static_cast<uint8_t>(valueReg), phiOut.ssaVersion}); users != m_currentFunction->users.end())
            for (const auto *user : users->second)
                if (BlockOf(user) != join)
                    return std::nullopt;
        // every write of the value register is a value the expression reads; everything else inlines into it
        std::set<int32_t> readVersions;
        for (const uint32_t id : term.region) {
            for (const auto &phi : blocks[id].phiNodes)
                for (size_t i = 1; i < phi.operands.size(); ++i)
                    readVersions.insert(phi.operands[i].ssaVersion);
            const auto *tail = blocks[id].lpTail;
            if ((tail->operation == LiftedOperation::JUMPIF || tail->operation == LiftedOperation::JUMPIFNOT) && !tail->operands.empty() &&
                tail->operands[0].type == LiftedOperandType::Register && tail->operands[0].value.reg == valueReg)
                readVersions.insert(tail->operands[0].ssaVersion);
        }
        for (size_t i = 1; i < test.phiNodes.front().operands.size(); ++i)
            readVersions.insert(test.phiNodes.front().operands[i].ssaVersion);
        std::vector<uint32_t> checked(term.region.begin(), term.region.end());
        checked.push_back(testId);
        for (const uint32_t id : checked) {
            const auto &block = blocks[id];
            for (const LiftedInstruction *inst = block.lpHead; inst && inst <= block.lpTail; ++inst) {
                if (inst->operation == LiftedOperation::NOP || inst->operation == LiftedOperation::PHI)
                    continue;
                if (inst == block.lpTail && (block.bTerminator == BlockTerminator::Conditional || inst->operation == LiftedOperation::JUMP))
                    continue;
                if (!inst->operands.empty() && inst->operands[0].type == LiftedOperandType::Register && inst->operands[0].value.reg == valueReg && id != testId) {
                    if (!readVersions.contains(inst->operands[0].ssaVersion))
                        return std::nullopt;
                    continue;
                }
                if (!RendersInline(inst))
                    return std::nullopt;
            }
        }
        if (!valueOf(term, entryId, testId, false))
            return std::nullopt;
        return term;
    };

    std::set<uint32_t> headers, leaves, termBlocks;
    std::map<uint32_t, ValueTerm> valueTerms; // by entry
    std::map<uint32_t, uint32_t> termEntryOf; // test -> entry
    enum class Walk { Complete, TooManyExits, Rejected };
    // tests with ids above `lastTerm` are exits rather than terms
    const auto walk = [&](uint32_t lastTerm) {
        headers.clear();
        leaves.clear();
        termBlocks.clear();
        valueTerms.clear();
        termEntryOf.clear();
        std::vector<uint32_t> pending{headerId};
        while (!pending.empty()) {
            const auto id = pending.back();
            pending.pop_back();
            if (id >= blocks.size() || id < headerId || headers.size() > 24)
                return Walk::Rejected;
            if (leaves.size() > 2)
                return Walk::TooManyExits;
            if (headers.contains(id) || leaves.contains(id) || valueTerms.contains(id))
                continue;
            if (id > lastTerm) {
                leaves.insert(id);
                continue;
            }
            if (id != headerId && !termBlocks.contains(id))
                if (auto term = detectValueTerm(id); term && term->test <= lastTerm) {
                    const uint32_t testId = term->test;
                    if (headers.contains(testId) || termBlocks.contains(testId))
                        return Walk::Rejected;
                    termBlocks.insert(term->region.begin(), term->region.end());
                    termBlocks.insert(testId);
                    headers.insert(testId);
                    termEntryOf[testId] = id;
                    for (const auto successor : {*blocks[testId].ifStatementTrue, *blocks[testId].ifStatementFalse}) {
                        if (successor <= testId)
                            return Walk::Rejected;
                        pending.push_back(successor);
                    }
                    valueTerms.emplace(id, std::move(*term));
                    continue;
                }
            const auto &block = blocks[id];
            bool conditionOnly = block.bType == BlockType::IfHeader && block.ifStatementTrue && block.ifStatementFalse && block.phiNodes.empty();
            if (conditionOnly && id != headerId)
                for (auto *inst = block.lpHead; inst < block.lpTail; ++inst)
                    if (inst->operation != LiftedOperation::NOP && !RendersInline(inst))
                        conditionOnly = false;
            if (!conditionOnly) {
                leaves.insert(id);
                continue;
            }
            headers.insert(id);
            for (const auto successor : {*block.ifStatementTrue, *block.ifStatementFalse}) {
                if (successor <= id)
                    return Walk::Rejected;
                pending.push_back(successor);
            }
        }
        return leaves.size() > 2 ? Walk::TooManyExits : Walk::Complete;
    };
    // the then-body may open with its own test; every condition term is emitted before it, so retreat the latest term to an exit
    for (uint32_t lastTerm = InvalidBlockId;;) {
        const auto result = walk(lastTerm);
        if (result == Walk::Rejected)
            return std::nullopt;
        if (result == Walk::Complete)
            break;
        if (headers.size() < 2)
            return std::nullopt;
        lastTerm = *headers.rbegin() - 1;
    }
    if (headers.size() < 2 || leaves.size() != 2)
        return std::nullopt;
    const auto bareReturn = [&](uint32_t id) {
        const auto &block = blocks[id];
        return block.bType == BlockType::Return && block.lpTail->operands.size() > 1 && block.lpTail->operands[1].value.imm.n == 1 &&
               std::all_of(block.lpHead, block.lpTail + 1, [](const LiftedInstruction &inst) {
                   return inst.operation == LiftedOperation::NOP || inst.operation == LiftedOperation::RETURN;
               });
    };
    for (const auto id : headers)
        if (id != headerId && !termEntryOf.contains(id))
            for (const auto pred : blocks[id].predecessors)
                if (!headers.contains(pred))
                    return std::nullopt;
    for (const auto &[entry, term] : valueTerms)
        for (const auto pred : blocks[entry].predecessors)
            if (!headers.contains(pred))
                return std::nullopt;
    // a def folded into the condition no longer reaches a phi that reads it on the path out of its test
    for (const auto leaf : leaves)
        for (const auto &phi : blocks[leaf].phiNodes)
            for (size_t i = 1; i < phi.operands.size(); ++i)
                if (const auto *def = phi.operands[i].type == LiftedOperandType::Register ? m_currentFunction->GetDefinition(phi.operands[i]) : nullptr) {
                    const int defBlock = BlockOf(def);
                    if (defBlock >= 0 && static_cast<uint32_t>(defBlock) != headerId &&
                        (headers.contains(static_cast<uint32_t>(defBlock)) || termBlocks.contains(static_cast<uint32_t>(defBlock))))
                        return std::nullopt;
                }

    uint32_t body = *leaves.begin(), exit = *leaves.rbegin();
    const uint32_t loopExit = m_loopExitStack.empty() ? InvalidBlockId : m_loopExitStack.back();
    const bool exitLeavesIteration =
        exit == loopExit || (loopExit != InvalidBlockId && blocks[exit].bType == BlockType::LoopLatch &&
                             std::ranges::find(blocks[exit].successors, loopExit) != blocks[exit].successors.end());
    if (bareReturn(body) || exitLeavesIteration)
        std::swap(body, exit);
    // pure value arms fold into one short-circuit value expression further up
    if (const int32_t join = FindMergeBlock(body, exit); join >= 0) {
        const auto pureArm = [&](uint32_t arm) {
            return arm == static_cast<uint32_t>(join) || IsDuplicableValueArm(arm, join) || IsDuplicablePureRegion(arm, join);
        };
        if (pureArm(body) && pureArm(exit))
            return std::nullopt;
    }

    const auto binary = [](const char *op, const Expr &lhs, const Expr &rhs) -> Expr { return std::make_shared<BinaryExpressionNode>(op, lhs, rhs); };
    std::map<uint32_t, Expr> conditions;
    conditions[body] = std::make_shared<BooleanLiteralNode>(true);
    conditions[exit] = std::make_shared<BooleanLiteralNode>(false);
    const auto overridesBefore = m_valueTermOverrides;
    for (auto it = headers.rbegin(); it != headers.rend(); ++it) {
        const auto &block = blocks[*it];
        const auto termEntry = termEntryOf.find(*it);
        if (termEntry != termEntryOf.end()) {
            const auto &out = block.phiNodes.front().operands[0];
            m_valueTermOverrides[SSARef{static_cast<uint8_t>(out.value.reg), out.ssaVersion}] =
                *valueOf(valueTerms.at(termEntry->second), termEntry->second, *it, true);
        }
        auto condition = LiftCondition(block.lpTail);
        auto yes = conditions.at(*block.ifStatementTrue), no = conditions.at(*block.ifStatementFalse);
        const auto yesBool = std::dynamic_pointer_cast<BooleanLiteralNode>(yes);
        const auto noBool = std::dynamic_pointer_cast<BooleanLiteralNode>(no);
        Expr result;
        if (yesBool && noBool && yesBool->value != noBool->value)
            result = yesBool->value ? condition : InvertCondition(condition);
        else if (yesBool && noBool)
            result = std::make_shared<IfExpressionNode>(condition, yes, no);
        else if (yesBool)
            result = yesBool->value ? binary("or", condition, no) : binary("and", InvertCondition(condition), no);
        else if (noBool)
            result = noBool->value ? binary("or", InvertCondition(condition), yes) : binary("and", condition, yes);
        else if (auto either = std::dynamic_pointer_cast<BinaryExpressionNode>(no); either && either->op == "or" && either->right == yes)
            result = binary("or", binary("and", InvertCondition(condition), either->left), yes);
        else if (auto either = std::dynamic_pointer_cast<BinaryExpressionNode>(yes); either && either->op == "or" && either->right == no)
            result = binary("or", binary("and", condition, either->left), no);
        else
            result = std::make_shared<IfExpressionNode>(condition, yes, no);
        conditions[*it] = result;
        if (termEntry != termEntryOf.end())
            conditions[termEntry->second] = result;
    }
    m_valueTermOverrides = overridesBefore;
    std::vector<uint32_t> chainBlocks(headers.begin(), headers.end());
    chainBlocks.insert(chainBlocks.end(), termBlocks.begin(), termBlocks.end());
    return OrChainInfo{conditions.at(headerId), body, exit, std::move(chainBlocks)};
}
