# Fission 1.0.0-beta

First beta. This release focuses on semantic faithfulness: decompiled output is checked against the Luau compiler and VM, and every divergence found by fuzzing has been closed or proven to be oracle noise.

Compared with `0.2.0-alpha`.

## Highlights

- No semantic divergences across a 200,000-sample AST fuzz campaign (40 seeds, O0-O2, debug levels 1-2, with and without mutation).
- Large obfuscated modules (`ANTI_DECOMPILE_GAMECORE`) now recompile under Luau's 200-local limit.
- Output reads more like hand-written Luau: debug local names, record constructors, guards, `else` arms, and comparison returns are recovered instead of lowered forms.
- SSA is validated against an independent reaching-definitions analysis of the raw bytecode.
- Linux builds of `Fission.Server`.

## Decompiler correctness

- Preserved evaluation order across table constructors, arithmetic, calls, guards, and every expression that can raise.
- Preserved first-error behavior: raising reads are no longer reordered past stores, inlined constructors keep their raising position under `and`/`or`/`not`, and constant folding no longer removes runtime errors.
- Preserved captured locals, parameters, and closure bindings when closures reassign them; shared closure identity is kept across branches.
- A closure stored into itself (`g[k] = g`) keeps its local.
- A nested `local function` whose reads reach an outer scope is demoted to a plain assignment.
- Globals named like generated locals stay global; closures can no longer shadow global names.
- Fixed `CALL` result versions used for inlining and effect ordering.
- Fixed `FASTCALL3` register unions, generic-for latch reads, and call definitions at generic-loop prep.
- Fixed variadic `SETLIST` tails after computed keys, conditional and global table keys before a variadic tail, long variadic lists, and deep nested constructors.
- Fixed deep compiler expression chains, mixed call and table chains, and bounded arithmetic-chain lifting.
- Vector constants survive indirect writes; non-finite vector components render as Luau numbers.
- Negative constant bases of `^` keep their parentheses (`(-2) ^ x`).
- Integer and normalized legacy parameter type tags are recovered; NaN table-key errors are preserved.

## Control flow

- Loop exits are taken from the instruction after `JUMPBACK`; post-latch jumps are no longer read as breaks.
- Returns join correctly across later loops; shared post-loop effects and inlined returns from `for` loops are retained.
- Fixed repeat loops whose exit is the next loop's header, repeat headers shared with an enclosing infinite loop, and `while true` wraps with shared headers.
- Merges must post-dominate paths that loop back; a loop exit is never chosen as an `if` merge; `break` and in-loop `return` no longer disqualify a merge.
- Loop-body `if`/`else` arms join at the latch instead of emitting `continue`.
- Compound guards that end in bare returns lift as guards, not `if`/`else`.
- Or-chains and arbitrary trees of pure tests lift as one condition; value-computed terms fold into compound conditions.
- Unreachable blocks are pruned before structure identification.
- Control-flow coroutines use an explicit trampoline, preventing native stack overflow on very large graphs.

## SSA

- Added an SSA oracle (`--ssa-oracle`, invariant I10) comparing every read's definitions with a reaching-definitions dataflow over the raw bytecode.
- `LOADB` skip jumps now merge at a phi; comparison materializations no longer see only `true`.
- Loops headed at the entry block get header phis.
- `FOR*PREP` reads are loop-entry reads; `FORNLOOP` reads limit, step and index and defines only the index.
- Generic-for variables are dead on the loop's entry edge.
- Numeric-for bound lookup no longer truncates SSA versions to 8 bits.
- `LOADB` comparison diamonds fold into a single conditional value (`return x == y`).

## Natural output

- Debug local names are restored, including locals initialized by populated constructors.
- Record constructors with mixed constant and computed fields stay a single constructor; `DUPTABLE` template fields fill in place.
- O0 record keys stored through a loaded string key recover record syntax.
- `export` declarations are recovered from `table.freeze` module returns.
- Computed keys keep their brackets; concatenation grouping is preserved.
- A loop arm's own temporaries are declared inside the arm instead of being hoisted out of the loop, keeping large modules below the 200-local limit.

## Fuzzing and oracles

- Compiler effect-trace oracle: original and decompiled sources are compiled and their lookup/store/call order compared.
- Semantic oracle fixture P3 makes `nil`, booleans, numbers, integers and functions answer every operator with a traced marker, so type-incoherent programs keep running and operator order is compared. Unjudged samples fell from about 56% to about 21%.
- Equal errors count as judged when a comparable trace precedes them.
- Divergences are re-checked against re-runs of the original, filtering nondeterministic iteration order.
- Per-reason `unch:` buckets explain unjudged samples.
- Roblox Vector3 is modelled in the semantic oracle.
- Roblox corpus roundtrip audit, scoped forward-reference detection, and a 49-sample local-register replay corpus.

## Server

- `Fission.Server` builds on Linux; the release workflow's Linux artifact and `/health` smoke test are unblocked.
- Added a client design document.

## Tests

- Suite grew from 428 to 705 tests.
- Added compiler-lowering audits, semantic replay regressions, loop-shape, capture and merge matrices, SSA invariant coverage, and fuzz-derived regressions.
- Removed vacuous cases and split oversized test translation units.

## CI and build

- Split validation workflows; tests run separately from fuzzing.
- Build caches persist, including partial caches after failed builds.
- Expanded fuzz campaigns.
- Address and undefined-behavior sanitizer job. Luau.VM is exempt from `float-cast-overflow`, since Luau converts NaN and infinity to int by design.
- Luau third-party header warnings are suppressed under Clang `-Werror`.

## Validation

- Windows test suite: 705 tests passed, 0 failed.
- 200,000-sample AST fuzz campaign: 0 semantic divergences, 0 forward-reference IR divergences, 0 crashes.
- Protected Roblox samples decompile and recompile, including `ANTI_DECOMPILE_GAMECORE`.

Output is checked for Luau semantic equivalence. These results do not claim Roblox runtime equivalence for scripts that depend on Roblox APIs.
