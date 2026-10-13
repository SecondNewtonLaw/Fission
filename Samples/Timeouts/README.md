# Samples/Timeouts — decompiler scalability repros

These inputs stress resource limits rather than correctness. They used to be
slow by design (tens of seconds to minutes); after the scalability pass below
they decompile in seconds. Keep them out of `Samples/StressTests/run_all.ps1`
anyway (large, slow-ish, single-shape).

## seqwhile_16k.lua (~13k+ loop-carried while loops needed; file holds 16k)

- History: first a native stack overflow (`0xC00000FD`) after ~35-50s at
  `--opt 1 --debug 2` — `ASTLifterFunctionState::VariableOf`
  (`Fission.Decompiler/include/ASTLifter.hpp`) recursed once per union-find
  parent link. One loop-carried register across N sequential loops links every
  SSA version forward into a single ~2N chain, and debug-local resolution reads
  the oldest versions first. Fixed with iterative path compression.
- Then slow: ~68s total (CFA Structure Identification ~43s, AST lift ~20s).
  Three quadratic searches, each O(remaining-graph) per loop, fixed with
  direction/reversal + loop-bounded witnesses:
  - `reachesBefore` / `returnsToSharedHeader` (`ControlFlowAnalyzer.cpp`):
    backward search from the barrier-cut target instead of forward from the
    start (same edges, same barriers). 30s → 0.01s.
  - `joinedExit`: collect escaping successors first; skip the forward flood
    when fewer than two exist (simple loops). 6s → 0.01s.
  - `CanReach` (`ASTLifter.cpp`): same reversal when the stop dominates the
    target. 9s of the walk → 0.05s.
  - `FindMergeBlock` (`ASTLifterConditions.cpp`): an arm that reaches its own
    latch through follows-edges strictly inside its loop, with arm and latch
    below the other arm, can never be post-dominated — return -1 without the
    downstream search. 10s → 0.01s.
  - `ComputeLoopBodies` (`ASTLifterInlining.cpp`): sparse member lists +
    per-loop visited sets instead of loops-x-blocks bitsets and rescans;
    membership via the block's own loop list (nesting-safe: a shared mark
    array corrupts nested loops).
- Now: decompiles in ~1.1s (`--vanilla-file`, recompile ok).
- The crash required `--debug 2` (debug-locals resolution is the only deep
  reader); `--debug 0/1` always survived. Diamonds (branches without loops)
  never crashed: their merges do not chain versions forward.
- Fast deterministic regression coverage:
  `Fission.Tests/CFG/CFGCoherence.cpp` —
  "CFG: version parent chains thousands long resolve without overflowing".

## seqwhile_32k.lua (32k loops)

- Used to exceed the quadratic pre-pass cost: aborted at exactly the 120s
  `ScopedDecompileBudget` with `FailedToDecompile`, via the
  `CheckDecompileDeadline()` coverage in the lifter pre-passes, CFG walk, and
  rewriter loops. After the scalability pass it decompiles in ~3.5s instead;
  the budget checks remain as a backstop for other pathological inputs.
