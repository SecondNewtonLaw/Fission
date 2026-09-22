# Draft release notes

Comparison: `0.2.0-alpha...dev`

## Fission.Server

- Added Linux support for `Fission.Server`, including portable warning handling, networking code, and target configuration.
- Unblocked the existing release workflow's Linux artifact build and health-check smoke test.
- Added a client design document covering transport, request lifecycle, errors, generated clients, and compatibility.

## Decompiler correctness

- Preserved source evaluation order across table constructors, arithmetic, calls, guards, and other expressions that can raise errors.
- Preserved captured locals, closure bindings, propagated parameter names, class values, loop values, and phi-backed debug names.
- Corrected adjacent loop classification, repeat/while boundaries, generic-for SSA definitions, loop-carried closures, and sibling loop bindings.
- Replaced recursive coroutine transfer with an explicit trampoline, preventing native stack overflow on large control-flow graphs such as the RegEx corpus sample.
- Added safe scope blocks for reused locals. Large generated modules now stay below Luau's 200-local-register limit without changing crossing bindings.
- Preserved fixed-result varargs and nested calls where Luau multi-return behavior would otherwise change argument or table cardinality.
- Preserved long variadic `SETLIST` constructors. Ten-element nested call lists no longer collapse to empty tables.
- Improved dead-local elimination and declaration hoisting so generated bindings remain declared after final scope rewriting.

## Corpus and fuzzing

- Added Roblox bytecode corpus recompilation and lifted-IR comparison modes.
- Added generated-local forward-reference detection with scope-aware diagnostics.
- Added a 49-sample local-register regression manifest and replay script.
- Added saved semantic regressions for corpus crashes, malformed control flow, first-error order, closure capture, class reconstruction, and loop reconstruction.
- Split current fuzz-campaign regressions into a separate translation unit to reduce incremental test compilation cost.
- Improved corpus diagnostics for decompile, recompile, IR-lift, and IR-mismatch failures.

## Tests and samples

- Split control-flow rewrite regressions out of the oversized control-flow test translation unit.
- Added coverage for class declarations, SSA invariants, scope capture, IR equivalence, complex control flow, and Luau lifting semantics.
- Added encoded ItemSpawn guard samples and tightened stress-test result handling.
- Corrected logical-precedence coverage so Clang warning-as-error builds succeed.

## Commits since `0.2.0-alpha`

- `7c4c525` test: clarify logical precedence
- `41b9e0c` docs: design Fission.Server client
- `20d15a7` fix(decompiler): preserve guard and closure bindings
- `f2d50c3` feat(fuzzing): add Roblox corpus roundtrip audit
- `d977c0b` fix(decompiler): scope reused locals
- `c587d27` fix(decompiler): preserve expression order
- `157b0cd` fix(decompiler): preserve captured local bindings
- `fb990e1` build(server): support Linux builds
- `9e01da6` fix(decompiler): preserve fuzzed semantics
- `7a66e31` fix(fuzzing): detect scoped forward refs
- `0897911` fix(decompiler): preserve class and loop values
- `5cf1074` test(fuzzing): replay local register corpus
- `b439a9e` fix(decompiler): trampoline control-flow coroutines
- `9daa6a8` fix(decompiler): preserve adjacent loop bindings
- `fc20826` fix(decompiler): hoist propagated bindings
- `41d185e` fix(decompiler): retain long variadic table lists
- `14e7ca1` test(decompiler): split campaign regressions

## Validation

- Windows CTest suite: 496 tests passed.
- Local-register corpus replay: 49 samples passed, 0 failed.
- Exact RegEx stack-overflow replay: decompiled and recompiled successfully.
- Exact character-title replay: original and recompiled IR contain 1,727 calls and 145 `SETLIST` operations; no empty call-list constructors remain.

Raw IR signatures are not byte-for-byte identical for every corpus sample. Remaining differences include materialized moves, branch-polarity reconstruction, and explicit returns replacing jumps to shared exits. These results do not claim Roblox runtime equivalence for scripts requiring Roblox APIs.
