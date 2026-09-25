// Compile real Luau through SSABuilder and assert invariants used by later stages:
//   I1  users / useCounts agree           (every recorded use is counted once)
//   I2  every def maps back to an instruction
//   I3  intra-block def-before-use         (a value's def precedes its use in the
//                                           same block; the SSA dominance property
//                                           that guarantees no forward reference)
//   I4  in-place read-modify ops version   (`GETTABLEKS R,R,K`, `ADD R,R,x`: the
//                                           read sees the OLD version, the write a
//                                           fresh one; they must differ)
//   I5  a used version resolves to a def   (or is a parameter / entry value)
//   I7  global dominance                    (a def's block dominates every use's
//                                           block; catches versions leaking across
//                                           dominator-tree siblings)
//   I8  one def per (instruction, register) (an instruction never forks a register
//                                           into two SSA versions)
//   I9  synthetic phis have no bytecode PC  (their identity cannot alias a real instruction)
//   I10 reaching definitions match the VM   (phi-flattened defs of every read equal an independent
//                                            dataflow over the raw bytecode; a superset is tolerated)

#include "../../Fission.Fuzzing/include/SSAOracle.hpp"
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "DenominatorAnalysis.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "SSABuilder.hpp"

#include "Luau/Common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    bool IsRegRead(const LiftedInstruction &inst, size_t i) {
        const AccessType m = SSABuilder::GetRegisterAccess(inst, i);
        return m == AccessType::Read || m == AccessType::ReadWrite;
    }

    // I1-I3 + I5, recursively over a function and its nested functions.
    void CheckInvariants(const AnalyzedFunction &fn) {
        for (const auto &block : fn.basicBlocks)
            for (const auto &phi : block.phiNodes) {
                INFO("phi in block=" << block.dwBlockId << " has bytecode index=" << phi.instructionIndex);
                CHECK(phi.instructionIndex == -1);
            }

        // I1: users <-> useCounts agree.
        for (const auto &[ref, insts] : fn.users) {
            const auto uc = fn.useCounts.find(ref);
            CHECK(uc != fn.useCounts.end());
            if (uc != fn.useCounts.end())
                CHECK(static_cast<size_t>(uc->second) == insts.size());
        }

        // I2: every definition maps to a real instruction.
        for (const auto &[ref, def] : fn.definitionMap)
            CHECK(def != nullptr);

        // I3: intra-block def-before-use (SSA dominance within a block). Phi nodes are
        //     exempt: a loop phi legally consumes a value defined later on the back-edge,
        //     and a phi itself is defined at the block head (dominates the whole block).
        for (const auto &[usedRef, insts] : fn.users) {
            const auto defIt = fn.definitionMap.find(usedRef);
            if (defIt == fn.definitionMap.end() || defIt->second == nullptr)
                continue; // parameter / entry / never-written register (reads nil); no instruction def
            const LiftedInstruction *def = defIt->second;
            if (def->operation == LiftedOperation::PHI)
                continue;
            const int defBlock = fn.GetBlockId(def);
            for (const LiftedInstruction *u : insts) {
                if (!u || u->operation == LiftedOperation::PHI)
                    continue;
                const int useBlock = fn.GetBlockId(u);
                if (defBlock != -1 && defBlock == useBlock) {
                    INFO("reg=" << static_cast<int>(usedRef.regIndex) << " ver=" << usedRef.version << " defIdx=" << def->instructionIndex
                                << " useIdx=" << u->instructionIndex << " block=" << defBlock);
                    CHECK(def->instructionIndex < u->instructionIndex);
                }
            }
        }

        // I5: a used (reg,version) resolves to a def, or is the function-entry version 0.
        for (const auto &[usedRef, insts] : fn.users) {
            if (fn.definitionMap.contains(usedRef))
                continue;
            INFO("unresolved use reg=" << static_cast<int>(usedRef.regIndex) << " ver=" << usedRef.version);
            CHECK(usedRef.version == 0);
        }

        for (const auto &inner : fn.innerFunctions)
            CheckInvariants(inner);
    }

    // I4: an in-place read-modify instruction (dest register == a read source register)
    //     must give its write a different version than the read it consumes.
    void CheckInPlaceVersioning(const AnalyzedFunction &fn) {
        for (const auto &block : fn.basicBlocks) {
            if (!block.lpHead)
                continue;
            for (const LiftedInstruction *p = block.lpHead; p && p <= block.lpTail; ++p) {
                const auto &inst = *p;
                if (inst.operands.empty() || inst.operands[0].type != LiftedOperandType::Register)
                    continue;
                if (SSABuilder::GetRegisterAccess(inst, 0) != AccessType::Write && SSABuilder::GetRegisterAccess(inst, 0) != AccessType::ReadWrite)
                    continue;
                const int destReg = inst.operands[0].value.reg;
                const int destVer = inst.operands[0].ssaVersion;
                for (size_t i = 1; i < inst.operands.size(); ++i) {
                    if (inst.operands[i].type != LiftedOperandType::Register || !IsRegRead(inst, i))
                        continue;
                    if (inst.operands[i].value.reg == destReg) {
                        INFO("in-place op idx=" << inst.instructionIndex << " reg=" << destReg << " readVer=" << inst.operands[i].ssaVersion
                                                << " writeVer=" << destVer);
                        CHECK(inst.operands[i].ssaVersion != destVer); // read sees the old version, write a fresh one
                    }
                }
            }
        }
        for (const auto &inner : fn.innerFunctions)
            CheckInPlaceVersioning(inner);
    }

    // I6: every register write defines a resolvable SSA value. For each instruction that writes
    //     its destination register, the (reg, version) it produces must appear in definitionMap
    //     (so every later reader can resolve it). This is the dual of I5 (every use resolves) and,
    //     run across the opcode-coverage sweep below, exercises the SSA write/versioning path of
    //     every IR instruction Fission emits.
    void CheckWriteDefs(const AnalyzedFunction &fn) {
        for (const auto &block : fn.basicBlocks) {
            if (!block.lpHead)
                continue;
            for (const LiftedInstruction *p = block.lpHead; p && p <= block.lpTail; ++p) {
                const auto &inst = *p;
                if (inst.operands.empty() || inst.operands[0].type != LiftedOperandType::Register)
                    continue;
                const AccessType acc = SSABuilder::GetRegisterAccess(inst, 0);
                if (acc != AccessType::Write && acc != AccessType::ReadWrite)
                    continue;
                const SSARef ref{static_cast<uint8_t>(inst.operands[0].value.reg), inst.operands[0].ssaVersion};
                INFO("write without def reg=" << static_cast<int>(ref.regIndex) << " ver=" << ref.version << " idx=" << inst.instructionIndex);
                CHECK(fn.definitionMap.contains(ref));
            }
        }
        for (const auto &inner : fn.innerFunctions)
            CheckWriteDefs(inner);
    }

    // I7: global dominance; the block a value is defined in must dominate every block it
    //     is used in. This is THE structural SSA property; a violation means a version
    //     leaked out of its dominator subtree (e.g. a latch def visible to a sibling
    //     branch; the exact bug class of the FORNLOOP/FORGLOOP version-stack leak).
    //     A phi is defined at its own block; each phi input must be available at the end of the
    //     predecessor it arrives from.
    void CheckDominance(const AnalyzedFunction &fn) {
        const auto domInfo = AnalyzeDenominators(fn);
        std::unordered_map<const LiftedInstruction *, int> phiBlock;
        for (const auto &block : fn.basicBlocks)
            for (const auto &phi : block.phiNodes)
                phiBlock[&phi] = static_cast<int>(block.dwBlockId);
        const auto blockOf = [&](const LiftedInstruction *inst) {
            const auto it = phiBlock.find(inst);
            return it != phiBlock.end() ? it->second : fn.GetBlockId(inst);
        };

        auto dominates = [&](int a, int b) {
            // walk b's idom chain up to the entry; true if we pass a
            int runner = b;
            int hops = 0;
            while (runner != -1 && hops++ < 4096) {
                if (runner == a)
                    return true;
                const auto it = domInfo.find(runner);
                if (it == domInfo.end())
                    return false;
                runner = it->second.idom;
            }
            return false;
        };

        for (const auto &[usedRef, insts] : fn.users) {
            const auto defIt = fn.definitionMap.find(usedRef);
            if (defIt == fn.definitionMap.end() || defIt->second == nullptr)
                continue; // function-entry value
            const LiftedInstruction *def = defIt->second;
            const int defBlock = blockOf(def);
            if (defBlock == -1)
                continue;
            for (const LiftedInstruction *u : insts) {
                if (!u || u->operation == LiftedOperation::PHI)
                    continue;
                const int useBlock = fn.GetBlockId(u);
                if (useBlock == -1 || useBlock == defBlock)
                    continue; // same-block ordering is I3's job
                INFO("def does not dominate use: reg=" << static_cast<int>(usedRef.regIndex) << " ver=" << usedRef.version << " defBlock=" << defBlock
                                                       << " useBlock=" << useBlock << " defIdx=" << def->instructionIndex
                                                       << " useIdx=" << u->instructionIndex);
                CHECK(dominates(defBlock, useBlock));
            }
        }

        for (const auto &block : fn.basicBlocks)
            for (const auto &phi : block.phiNodes)
                for (size_t p = 0; p < block.predecessors.size() && p + 1 < phi.operands.size(); ++p) {
                    const SSARef input{phi.operands[0].value.reg, phi.operands[p + 1].ssaVersion};
                    const auto defIt = fn.definitionMap.find(input);
                    if (input.version < 0 || defIt == fn.definitionMap.end())
                        continue;
                    const int defBlock = blockOf(defIt->second);
                    INFO("phi input not available on its edge: reg=" << static_cast<int>(input.regIndex) << " ver=" << input.version
                                                                      << " defBlock=" << defBlock << " pred=" << block.predecessors[p]
                                                                      << " phiBlock=" << block.dwBlockId);
                    CHECK(dominates(defBlock, static_cast<int>(block.predecessors[p])));
                }

        for (const auto &inner : fn.innerFunctions)
            CheckDominance(inner);
    }

    // I8: an instruction defines each register at most once. Two definitionMap entries for
    //     the same (instruction, register) mean the builder forked one write into two SSA
    //     versions (the GETVARARGS double-def class): uses bind one version while the
    //     operand carries the other, and downstream naming desyncs.
    void CheckSingleDefPerInst(const AnalyzedFunction &fn) {
        std::set<std::pair<const LiftedInstruction *, int>> seen;
        for (const auto &[ref, def] : fn.definitionMap) {
            if (!def)
                continue;
            const auto key = std::make_pair(def, static_cast<int>(ref.regIndex));
            INFO("double def: op=" << static_cast<int>(def->operation) << " idx=" << def->instructionIndex
                                   << " reg=" << static_cast<int>(ref.regIndex) << " ver=" << ref.version);
            CHECK(seen.insert(key).second);
        }
        for (const auto &inner : fn.innerFunctions)
            CheckSingleDefPerInst(inner);
    }

    // Compile -> deserialize -> lift -> CFA -> SSA, then run every invariant check.
    // Runs inline so the LiftedFunction (which the AnalyzedFunction points into) stays alive.
    void CheckSSA(const std::string &source, int optLevel = 1) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 1;
        const std::string bc = Luau::compile(source, opts);
        REQUIRE(!bc.empty());
        REQUIRE(bc[0] != '\0'); // byte 0 == 0 marks a compile error

        Fission::InstructionDecoder decoder{};
        Deserializer deserializer{};
        const auto deser = deserializer.Deserialize(bc);
        REQUIRE(deser.has_value());
        REQUIRE_FALSE(deser->functions.empty());

        BytecodeLifter lifter{&decoder};
        LiftedFunction lifted = lifter.LiftDeserializedBytecode(*deser);

        ControlFlowAnalyzer cfa{};
        AnalyzedFunction fn = cfa.DetermineBasicBlocks(&lifted);
        cfa.OptimizeGraph(fn);
        cfa.PruneUnreachable(fn);
        cfa.IdentifyStructures(fn);

        SSABuilder ssa{};
        ssa.Build(fn);

        CheckInvariants(fn);
        CheckInPlaceVersioning(fn);
        CheckWriteDefs(fn);
        CheckDominance(fn);
        CheckSingleDefPerInst(fn);

        const auto oracle = fuzz::CheckSSAAgainstVM(fn);
        for (const auto &mismatch : oracle.mismatches) {
            INFO(mismatch.function << " pc=" << mismatch.pc << " r" << mismatch.reg << " " << mismatch.operation << " " << mismatch.detail << "\n"
                                   << oracle.dumps);
            CHECK(mismatch.kind == "EXTRA_REACHING");
        }
    }

} // namespace

TEST_CASE("SSA: reaching definitions match VM dataflow", "[SSA][Invariant][Oracle]") {
    // loop headed at the entry block: its back-edge def must reach the in-place read
    CheckSSA("local function f(n) repeat n *= 2 until n > 5 return n end return f");
    CheckSSA("local function f(a, b) repeat b %= 'x' while b(false) do print() end until nil end return f");
    // LOADB-materialized comparisons merge both loads
    CheckSSA("local function f(x) local b = x ~= 52.5 return nil + b, x < 3 end return f");
    // generic-for state from a branch merge; prep reads see only the entry value
    CheckSSA("local function f(c, a, b) local s = 0 for _, v in next, if c then a else b do s += v end return s end return f");
    CheckSSA("local function f(t) local s = 0 for k, v in t do s = s + v end return s end return f");
    CheckSSA(
        R"(makeIterator = function()
        return function(_, index)
            if index < 1 then return index + 1 end
        end, nil, 0
    end
    local function f(flag)
        print(flag, if flag then 1 else 2, function() end)
        for value in makeIterator() do break end
        return 0
    end
    return f(true), f(false))",
        2
    );
    // numeric-for limit from a branch merge inside an outer loop
    CheckSSA("local function f(c) local n = 0 repeat for i = 1, (if c then 2 else 3) do print(i) end n += 1 until n == 2 end return f");
    CheckSSA(
        R"(local function dispatch(n)
        local result = 1
        if n == 1 then result += 2 return result end
        if n == 2 then result += 3 return result end
        return result
    end
    return dispatch(1), dispatch(2), dispatch(3))",
        2
    );
}

TEST_CASE("SSA: straight-line code is sound", "[SSA][Invariant]") {
    CheckSSA("local a = 1 local b = a + 2 local c = a + b return a, b, c");
}

TEST_CASE("SSA: in-place table field read-modify versions correctly", "[SSA][Invariant]") {
    // `t.x = t.x + 1` and the nested `({...}).field` pattern both emit GETTABLEKS/GETTABLE
    // with dest == source; the exact in-place case that must get a fresh version.
    CheckSSA("local t = { x = 1 } t.x = t.x + 1 return t.x");
    CheckSSA("local a = { data = ({ field = 'a-b' }).field } local b = (20.5).e[true] return a, b");
}

TEST_CASE("SSA: in-place arithmetic accumulator versions correctly", "[SSA][Invariant]") {
    CheckSSA("local function f(n) local s = 0 for i = 1, n do s = s + i end return s end return f");
}

TEST_CASE("SSA: branch join (if/else) is sound", "[SSA][Invariant]") {
    CheckSSA("local function f(a) local b = a if a > 0 then b = a * 2 else b = -a end return b end return f");
}

TEST_CASE("SSA: while loop with carried variable is sound", "[SSA][Invariant]") {
    CheckSSA("local function f(n) local x = n while x > 0 do x = x - 1 end return x end return f");
}

TEST_CASE("SSA: numeric and generic for loops are sound", "[SSA][Invariant]") {
    CheckSSA("local t = { 1, 2, 3 } local s = 0 for i, v in ipairs(t) do s = s + v end return s");
    CheckSSA("local m = { a = 1, b = 2 } local s = 0 for k, v in pairs(m) do s = s + v end return s");
}

TEST_CASE("SSA: calls and multiple returns are sound", "[SSA][Invariant]") {
    CheckSSA("local ok, err = pcall(function() error('x') end) return ok, err");
    CheckSSA("local function f(...) local t = { ... } return #t, select('#', ...) end return f");
}

TEST_CASE("SSA: nested closures capturing upvalues are sound", "[SSA][Invariant]") {
    CheckSSA("local function outer(x) local y = x + 1 return function() return x + y end end return outer");
}

TEST_CASE("SSA: nested table constructors are sound", "[SSA][Invariant]") {
    CheckSSA("local t = { a = { b = { c = 1 } }, d = { 1, 2, { 3, 4 } } } return t");
}

TEST_CASE("SSA: nested table indexed inside a constructor is sound", "[SSA][Invariant]") {
    // The forward-ref class: an inner table built then indexed, as an element of an
    // outer constructor (SETLIST) or a keyed value (SETTABLEKS). SSA must version the
    // inner table's NEWTABLE/populate/read coherently (def precedes use).
    CheckSSA("local function f(k) local t = { 43, ({ 10, 20, 30 })[k] } return t end return f");
    CheckSSA("local function f() local t = { data = ({ field = 'a-b' }).field } return t end return f");
    CheckSSA("local v6 = { 43, ({ 'x', 'y', field = '' })[2] } local v9 = (20.5).e return v6, v9");
}

TEST_CASE("SSA: repeat-until with carried state is sound", "[SSA][Invariant]") {
    CheckSSA("local function f(n) local i = 0 repeat i = i + 1 until i >= n return i end return f");
}

TEST_CASE("SSA: SETLIST array elements (forward-ref shapes) are sound", "[SSA][Invariant][SetList]") {
    // The exact lifter forward-ref shapes, validated at the SSA layer: the inner populated
    // table's NEWTABLE / SETLIST / SETTABLEKS / GETTABLEN versions must be coherent (def
    // precedes use, the element register resolves), whether the indexed table is element 2
    // or element 1 and whether the outer table is a real local or a return value.
    CheckSSA("local t = { 43, ({ 'a', 'b', f = '' })[2] } t[1] = 5 return t");
    CheckSSA("local t = { ({ 'a', 'b', f = '' })[2], 43 } print(t) print(t) return t");
    CheckSSA("local t = { { x = 1 }, { y = 2 } } t[1].x = 9 return t");
    CheckSSA("local function f(a, b) return { a + 1, b * 2, a - b } end return f");
    CheckSSA("local t = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 } return t"); // pure array SETLIST
    CheckSSA("local n = 1 local t = { n, n + 1, n + 2 } return t");   // elements read an earlier local
}

TEST_CASE("SSA: loop-latch versions do not leak to dominator siblings", "[SSA][Invariant][LoopScope]") {
    // FORNLOOP/FORGLOOP bump loop-register versions inside the latch block. Those versions
    // must pop when Rename leaves the latch's dominator subtree; code after/beside the loop
    // that reuses the same register slots must NOT see them (the version-stack-leak class).
    // numeric for, then the loop registers' slots are reused by fresh locals
    CheckSSA("local function f(n) local s = 0 for i = 1, n do s = s + i end local q = s + 1 local r = q * 2 return r end return f");
    // two sequential numeric fors sharing the same register window
    CheckSSA("local function f(n) local a = 0 for i = 1, n do a = a + i end for j = 1, n do a = a - j end return a end return f");
    // loop followed by sibling if/else whose branches reuse the slots
    CheckSSA("local function f(n) for i = 1, n do print(i) end if n > 0 then local x = n + 1 return x else local y = n - 1 return y end end return f");
    // generic for, then slot reuse
    CheckSSA("local function f(t) local s = 0 for k, v in pairs(t) do s = s + v end local w = s * 2 return w end return f");
    // nested loops: inner latch versions must not leak into the outer loop's later iterations' siblings
    CheckSSA("local function f(n) local s = 0 for i = 1, n do for j = 1, i do s = s + j end s = s + i end return s end return f");
    // loop inside a branch: latch subtree is a sibling of the else arm
    CheckSSA("local function f(n) if n > 0 then for i = 1, n do print(i) end else local z = -n return z end return n end return f");
}

TEST_CASE("SSA: GETVARARGS defines each register exactly once", "[SSA][Invariant][Vararg]") {
    // the double-def class: the generic write pass defines base, the explicit branch must
    // not fork a second version of it. All widths: 1, 2, 3 targets and multret.
    CheckSSA("local function f(...) local a = ... return a end return f");
    CheckSSA("local function f(...) local a, b = ... return a, b end return f");
    CheckSSA("local function f(...) local a, b, c = ... return a + b, c end return f");
    CheckSSA("local function f(...) return ... end return f");             // multret GETVARARGS
    CheckSSA("local function f(...) local t = { ... } return t end return f");
    CheckSSA("local function f(...) print(...) local a = ... return a end return f"); // two GETVARARGS, same base
}

TEST_CASE("SSA: multret tails record every consumed register", "[SSA][Invariant][Vararg][Multret]") {
    // VariadicTailCount shapes: SETLIST/RETURN with count==0 must use-record the whole
    // R(start)..R(producer-base) span, not just the first element.
    CheckSSA("local function f(g, a, b) return a, b, g() end return f");
    CheckSSA("local function f(g, a, b) local t = { a, b, g() } return t end return f");
    CheckSSA("local function f(a, b, ...) return a, b, ... end return f");
    CheckSSA("local function f(a, ...) local t = { a, ... } return t end return f");
    CheckSSA("local function f(g) return g(g()) end return f"); // multret call feeding a call
}

TEST_CASE("SSA: loop header/latch operand versions are coherent", "[SSA][Invariant][LoopScope]") {
    // FORNPREP/FORGPREP record implicit reads of base..base+2; those must resolve to the
    // pre-header defs (start/limit/step or gen/state/control), and FORGLOOP's structural
    // defs must land header phis for every declared loop var.
    CheckSSA("local function f(a, b, c) local s = 0 for i = a, b, c do s = s + i end return s end return f");
    CheckSSA("local function f(t) local s = '' for k, v in next, t do s = s .. k .. tostring(v) end return s end return f");
    CheckSSA("local function f(t) for i, v in ipairs(t) do if v > 2 then return i end end return -1 end return f");
    // loop var captured by a closure (CAPTURE interacts with the loop-var versions)
    CheckSSA("local function f(n) local fns = {} for i = 1, n do fns[i] = function() return i end end return fns end return f");
    // break edge: latch versions on the exit path
    CheckSSA("local function f(n) local s = 0 for i = 1, n do s = s + i if s > 10 then break end end return s end return f");
}

// "Validate all SSA forms for all IR instructions": a single sweep that drives every IR
// instruction Fission emits through SSA construction and runs the full invariant set
// (def-before-use, in-place versioning, every use resolves, every write defines). If any
// opcode's read/write access pattern is mis-modelled, one of these snippets trips an invariant.
TEST_CASE("SSA: broad IR opcode coverage is sound", "[SSA][Invariant][Coverage]") {
    // arithmetic (ADD/SUB/MUL/DIV/IDIV/MOD/POW) + their K forms, unary MINUS/NOT/LENGTH
    CheckSSA("local function f(a, b) return a + b - a * b / (b + 1) % 3 ^ 2 // 2, -a, not b, #('x') end return f");
    // comparisons / branches (JUMPIF*, JUMPXEQK) feeding an if
    CheckSSA("local function f(a, b) if a == b then return 1 elseif a < b then return 2 elseif a <= b then return 3 end return 0 end return f");
    // string ops: CONCAT spine
    CheckSSA("local function f(a, b, c) return a .. b .. c .. 'x' end return f");
    // table get/set: GETTABLE/GETTABLEKS/GETTABLEN, SETTABLE/SETTABLEKS/SETTABLEN
    CheckSSA("local t = {} t.a = 1 t[2] = 2 t['k'] = t.a + t[2] return t[1], t.a, t['k']");
    // globals / imports / upvalues: GETGLOBAL/SETGLOBAL, GETIMPORT, GETUPVAL/SETUPVAL, CAPTURE
    CheckSSA("g = 1 local x = g + 1 local function f() g = x x = x + 1 return math.floor(x) end return f");
    // calls: CALL, NAMECALL, multi-return, FASTCALL paths
    CheckSSA("local function f(t) local a, b = t:method(1, 2) return string.format('%d', a + b), select('#', f) end return f");
    // varargs: PREPVARARGS / GETVARARGS
    CheckSSA("local function f(...) local t = { ... } return #t, (...) end return f");
    // closures + captured upvalues: NEWCLOSURE/DUPCLOSURE + CAPTURE
    CheckSSA("local function outer(x) local y = x + 1 return function() y = y + 1 return x + y end end return outer");
    // numeric + generic for (FORNPREP/FORNLOOP, FORGPREP/FORGLOOP), accumulation
    CheckSSA("local s = 0 for i = 1, 10, 2 do s = s + i end for _, v in ipairs({1, 2, 3}) do s = s + v end return s");
    // while + repeat carrying state across the back-edge (phi)
    CheckSSA("local function f(n) local x = n while x > 1 do x = x - 1 end repeat x = x + 1 until x >= n return x end return f");
    // boolean materialization / and-or chains
    CheckSSA("local function f(a, b, c) return a and b or c, a or b and c end return f");
}

TEST_CASE("SSA: FASTCALL2 and CAPTURE access modes are explicit", "[SSA][Invariant][Access]") {
    LiftedInstruction fastcall{LiftedOperation::FASTCALL2, 0};
    fastcall.operands.resize(4);
    fastcall.operands[0].type = LiftedOperandType::ImmediateInteger;
    fastcall.operands[1].type = LiftedOperandType::Register;
    fastcall.operands[2].type = LiftedOperandType::ImmediateInteger;
    fastcall.operands[3].type = LiftedOperandType::Register;
    CHECK(SSABuilder::GetRegisterAccess(fastcall, 1) == AccessType::Read);
    CHECK(SSABuilder::GetRegisterAccess(fastcall, 3) == AccessType::Read);

    LiftedInstruction capture{LiftedOperation::CAPTURE, 0};
    capture.operands.resize(2);
    capture.operands[0].type = LiftedOperandType::ImmediateInteger;
    capture.operands[1].type = LiftedOperandType::Register;
    for (int mode = 0; mode <= 1; ++mode) {
        capture.operands[0].value.imm.n = mode;
        CHECK(SSABuilder::GetRegisterAccess(capture, 1) == AccessType::Read);
    }
    capture.operands[0].value.imm.n = 2;
    CHECK(SSABuilder::GetRegisterAccess(capture, 1) == AccessType::NoAccess);
}

TEST_CASE("SSA: FORNLOOP defines only a fresh control-variable version", "[SSA][Invariant][LoopScope]") {
    EnableLuauFFlagsOnce();
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    const std::string bytecode = Luau::compile(
        "local function sum(limit, step) local total = 0 for i = 1, limit, step do total += i end return total end return sum", opts
    );
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode[0] != '\0');

    Fission::InstructionDecoder decoder{};
    Deserializer deserializer{};
    const auto deserialized = deserializer.Deserialize(bytecode);
    REQUIRE(deserialized.has_value());
    BytecodeLifter lifter{&decoder};
    LiftedFunction lifted = lifter.LiftDeserializedBytecode(*deserialized);
    ControlFlowAnalyzer cfa{};
    AnalyzedFunction function = cfa.DetermineBasicBlocks(&lifted);
    cfa.OptimizeGraph(function);
    cfa.PruneUnreachable(function);
    cfa.IdentifyStructures(function);
    SSABuilder ssa{};
    ssa.Build(function);

    bool sawNumericLoop = false;
    std::vector<const AnalyzedFunction *> pending{&function};
    while (!pending.empty()) {
        const auto *current = pending.back();
        pending.pop_back();
        for (const auto &inner : current->innerFunctions)
            pending.push_back(&inner);
        for (const auto &block : current->basicBlocks) {
            if (!block.lpHead)
                continue;
            for (const auto *instruction = block.lpHead; instruction <= block.lpTail; ++instruction) {
                if (instruction->operation != LiftedOperation::FORNLOOP)
                    continue;
                sawNumericLoop = true;
                REQUIRE(instruction->operands.size() >= 3);
                const int base = instruction->operands[0].value.reg;
                CHECK(SSABuilder::GetRegisterAccess(*instruction, 0) == AccessType::Read);
                CHECK(SSABuilder::GetRegisterAccess(*instruction, 2) == AccessType::Read);

                const auto uses = current->implicitUses.find(instruction);
                CHECK(uses != current->implicitUses.end());
                if (uses != current->implicitUses.end()) {
                    REQUIRE(uses->second.size() == 3);
                    CHECK(uses->second[0] == instruction->operands[0].ssaVersion);
                    CHECK(uses->second[2] == instruction->operands[2].ssaVersion);
                }

                std::vector<SSARef> definitions;
                for (const auto &[reference, owner] : current->definitionMap)
                    if (owner == instruction)
                        definitions.push_back(reference);
                REQUIRE(definitions.size() == 1);
                CHECK(definitions[0].regIndex == base + 2);
                CHECK(definitions[0].version > instruction->operands[2].ssaVersion);
            }
        }
    }
    REQUIRE(sawNumericLoop);
}

TEST_CASE("CFG: coincident branch targets create one edge", "[SSA][Invariant][CFG]") {
    const auto reg = [](uint8_t value) {
        LiftedOperand operand{};
        operand.type = LiftedOperandType::Register;
        operand.value.reg = value;
        return operand;
    };
    const auto imm = [](int32_t value) {
        LiftedOperand operand{};
        operand.type = LiftedOperandType::ImmediateInteger;
        operand.value.imm.n = value;
        return operand;
    };
    LiftedFunction lifted{};
    lifted.instructions = {
        {LiftedOperation::LOAD, 0, {reg(0), imm(1)}},
        {LiftedOperation::JUMPIF, 1, {reg(0), imm(0)}},
        {LiftedOperation::LOAD, 2, {reg(1), imm(7)}},
        {LiftedOperation::RETURN, 3, {reg(1), imm(2)}},
    };
    ControlFlowAnalyzer cfa{};
    auto analyzed = cfa.DetermineBasicBlocks(&lifted);

    bool sawBranch = false;
    for (const auto &block : analyzed.basicBlocks) {
        if (block.lpTail && block.lpTail->operation == LiftedOperation::JUMPIF) {
            sawBranch = true;
            CHECK(block.successors.size() == 1);
        }
        CHECK(std::set<uint32_t>(block.successors.begin(), block.successors.end()).size() == block.successors.size());
        CHECK(std::set<uint32_t>(block.predecessors.begin(), block.predecessors.end()).size() == block.predecessors.size());
    }
    REQUIRE(sawBranch);
}
