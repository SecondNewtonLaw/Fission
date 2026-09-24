# Bug Hunting and Fixing Methodology

How to find and fix decompiler bugs in Fission. The rule is **diagnose first, then fix**. Do not patch symptoms, and do not run tools at random hoping something sticks.

## 1. Find

- **Fuzz** with the semantic oracle: `Fission.Fuzzing.exe --count N --seed S --threads T --out <dir>`. Rotate seeds; a campaign on one seed is not coverage. Findings land in `<dir>/crashes/` as `SEM_DIVERGE_*`, `FORWARDREF_IR_DIVERGE_*` (undeclared-local `NO_DECL`) and `_HARD_FAULT_*`.
- **SSA oracle**: `--ssa-oracle --count N --seed S` compares every read's reaching definitions with an independent dataflow over raw bytecode. `EXTRA_REACHING` from raw FOR edges is a known sound over-approximation; any *missing* definition is a real bug.
- **Replay** saved findings on every build: `--replay-dir <dir>`, then diff verdicts against the previous log. A finding counts as fixed only when replay stops reporting it.
- **Write adversarial samples against your own fix.** After fixing a shape, write sources that stress the same mechanism harder (bigger bodies, nested loops, `continue`/`break`/`return` arms, captured locals, a following loop) and run `--sem-file` on them. The fuzzer found a stack overflow in a fix that the regression tests passed.

## 2. Reduce

- Line-reduce the finding (`--sem-file` verdict for SEM, `--replay-dir` for FWD, CLI exit code for crashes).
- Then **hand-reduce**: rewrite the sample into a small, runnable program that prints its observable state. Change one construct at a time (constant vs call, global vs local, trailing statement present or not) until the minimal trigger is isolated. Keep a non-triggering neighbour next to the triggering one; the difference is the diagnosis.
- Do not filter tool output with pattern matches when reading decompiled code. Filters have hidden lines and caused a false alarm before.

## 3. Diagnose (before touching code)

- Read the **CFG** (`Fission.CLI --debug-notes --decompile-test <file>`: block links, loop and latch classification, chosen exits) and the **SSA form** (`--ssa-dump <file>`) of the minimal repro. Name the exact block, edge and decision that goes wrong.
- **Walk the whole pipeline**: Deserializer → BytecodeLifter → ControlFlowAnalyzer → SSABuilder → ASTLifter → rewriters → SourceGenerator. Establish which stage first produces a wrong fact. A wrong AST often starts as a wrong CFA classification.
- **Study the producer.** The Luau compiler (`Compiler/src/Compiler.cpp` in the CPM cache) emits a small, fixed set of shapes. Decompilation is the inverse of those shapes. Before inventing a heuristic, read how the construct is compiled (`compileConditionValue`, `compileExprAndOr`, `compileExprIfElse`, `compileStatIf`, `compileStatWhile`, `compileStatRepeat`, jump threading) and derive the inverse rule from it. Examples:
  - In a condition, only `and`/`or`/`not`/parentheses/comparisons become jump trees. Every other term is computed into one register and tested once.
  - `if c then T else E end` falls through from the condition into T; every false edge jumps forward.
  - `while true … if x then break end end` and `repeat … until x` produce identical bytecode; they differ only in where `continue` lands.
- When you find an existing rule that blocks the fix, find out **why it exists** (`git blame`, the commit message, the tests it protects) before removing it. If the reason cannot be recovered, remove it and prove the removal with targeted semantic samples, not just the test suite.
- Look for **silent failure modes**, such as a lifter path that drops an edge without a note. A band-aid that shrinks the symptom (for example, duplicating a region up to a size cap) leaves the root cause live for larger inputs. Name the root cause and fix it there.
- If the fix touches fragile structure (CFA, SSABuilder, ASTLifter), write down the risks before editing: which other shapes share the code path, and what the fix could capture that it must not.

## 4. Fix

- One root cause per commit. Smallest change that makes the inverse rule correct; match surrounding style.
- Keep invariants explicit in code only where the code cannot express them (one-line comment).
- Temporary debug output is allowed while diagnosing; remove all of it before committing.

## 5. Verify

- **Regression test for every broken case**, added to `Fission.Tests/Decompiler/ReplayDivergenceRegressions.cpp` (semantic parity) or a structural test. **Prove the test catches the bug**: temporarily revert the fix, confirm the test fails, restore it.
- No expected-fail or known-bug tests. A failing case is fixed or it stays open.
- Full CTest (includes the `LoopShapeMatrix` shape matrices and the SSA invariants), replay of all saved finding sets, and a fresh fuzz campaign on a new seed. Check crashes and stack overflows, not only divergences.
- If a check is skipped or fails, say so. Never report unverified work as done.

## 6. Record

- Commit with a message that states the observed failure and the root cause.
- Record non-obvious findings (invariants, compiler shapes, rejected approaches and why) in project memory so the next session does not re-derive them.
