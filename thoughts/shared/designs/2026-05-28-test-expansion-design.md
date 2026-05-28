---
date: 2026-05-28
topic: "Test Expansion & Gap Coverage"
status: draft
---

## Problem Statement

The decompiler stress test analysis (`Samples/StressTests/results/ANALYSIS.md`) documents 12 distinct bug patterns across 20 samples, including 4 critical (body-eating loop bugs, garbled short-circuit) and 3 major (elseif flattening, or-chain not folded, compound condition splitting). Only 40 Catch2 tests exist, and none target these specific failure modes — the existing tests check structural coherence and simple roundtrips, not output correctness for complex control flow.

## Constraints

- All tests use the existing roundtrip pattern: `Decompiler::DecompileTestCode` → assert output substrings
- No new dependencies, no golden-file framework
- Must pass with current build; tests document known-bugs as `CHECK_FALSE` or `REQUIRE` for fixed cases
- Tests live in `Fission.Tests/Decompiler/` for roundtrip/semantic tests, `Fission.Tests/CFG/` for structural tests

## Approach

Target each stress-test bug pattern with one or more regression tests. Each test:
1. Takes Luau source that triggers the bug
2. Compiles + decompiles through `DecompileOrFail` (existing helper in LiftingSemantics.cpp)
3. Asserts output contains expected control-flow keywords (`repeat`, `while`, `and`, `or`, `elseif`)
4. Asserts output does NOT contain known-bad patterns (empty body, missing statements)

Tests are organized by severity (critical → major → minor) matching the stress test analysis.

## Test Plan

### Critical Tests (new file: Fission.Tests/Decompiler/ControlFlowRegressions.cpp)

1. **Repeat-until body preserved** (`[Decompiler][Loop]`)
   - Input: `repeat i = i + 1 until i >= 10`
   - Assert: output contains `i = i + 1` (not eaten)
   - Current behavior: body eaten → `while 10 >= v0 do end`

2. **Repeat-until with computation body** (`[Decompiler][Loop]`)
   - Input: `repeat acc = acc + x; x = x - 1 until x <= 0`
   - Assert: output contains `acc =` and `x =` assignments
   - Current: body eaten

3. **While-true-break body preserved** (`[Decompiler][Loop]`)
   - Input: `while true do x = x + 1; if x >= 100 then break end end`
   - Assert: output contains `x = x + 1` (not eaten)
   - Current: `while 100 >= v0 do end` with empty body

4. **And-or mixed short-circuit** (`[Decompiler][ShortCircuit]`)
   - Input: `a and b or c and d` (via function args for debug names)
   - Assert: output contains `and`, `or`, no self-assign (`v2 = v2`)
   - Current: completely garbled with self-assign

5. **Break in else branches** (`[Decompiler][Loop]`)
   - Input: `for i=1,20 do if cond then elseif cond2 then else break end end`
   - Assert: no `break` token appears inside else-branch without enclosing loop context
   - Current: spurious `break` inserted in else branches, kills enclosing loop

6. **While-true-break with multiple exits** (`[Decompiler][Loop]`)
   - Input: nested `while true do ... if cond1 break ... if cond2 break end`
   - Assert: body statements survive

### Major Tests (additions to LiftingSemantics.cpp)

7. **Elseif chain produces elseif keyword** (`[Decompiler][Elseif]`)
   - Input: `if A then ... elseif B then ... elseif C then ... else ... end`
   - Assert: output contains `elseif` keyword at least once
   - Current: flattened to separate `if` statements

8. **Or chain with 3+ operands folded** (`[Decompiler][ShortCircuit]`)
   - Input: `local v = a or b or c or "default"`
   - Assert: output contains single expression with `or`, not nested if-return blocks
   - Current: separate if-return blocks for each operand

9. **While compound condition** (`[Decompiler][Loop]`)
   - Input: `while x < y and y > 0 do`
   - Assert: output contains `and` in the while condition
   - Current: split into `while x < y do if y > 0 then ... end`

### Minor Tests (additions to LiftingSemantics.cpp)

10. **Table init preserved in pairs()** (`[Decompiler][GenericFor]`)
    - Input: `for k, v in pairs({a=1, b=2}) do print(k, v) end`
    - Assert: output contains `{a=1` or `{["a"]=1`
    - Current: `pairs({ })` — table init lost

11. **Loop variable naming from debug info** (`[Decompiler][NumericFor]`)
    - Already partially covered; add assertion that no `_N` suffix appears when debug names are available

## New File Structure

```
Fission.Tests/Decompiler/
  ControlFlowRegressions.cpp  <-- new: critical loop/break/body-eating tests
  DecompilationRoundtrip.cpp  <-- unchanged
  LiftingSemantics.cpp        <-- add ~5 tests for elseif/or-chain/compound-while
```

## Execution Order

1. Create `ControlFlowRegressions.cpp` with tests 1-6
2. Add tests 7-11 to `LiftingSemantics.cpp`
3. Register new file in `CMakeLists.txt` add_executable list
4. Build and run: `cmake --build cmake-build-debug --target Fission.Tests && ctest --test-dir cmake-build-debug --output-on-failure`

## Expected Results

- All new tests will FAIL initially (they document known bugs)
- Each test failure message shows the actual decompiler output vs expected pattern
- As bugs are fixed in `ControlFlowAnalyzer` / `ASTLifter` / `SSABuilder`, tests will start passing
- This gives us regression coverage + a bug-tracker in test form

## Open Questions

- Should we merge the roundtrip helper (`EnableLuauFFlagsOnce` + `DecompileOrFail`) into a shared header? Current duplication across files. — Worth doing as a follow-up refactor.
- For repeat-until tests that fail with empty body, should we assert on body contents or just non-empty body length? — Body contents: grep for the specific assignment statement.
