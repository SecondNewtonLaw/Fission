# Fission.Fuzzing

Random-Luau fuzzer for the decompiler. Generates diverse valid Luau, runs each sample through the
full pipeline with behavioral comparisons, attributes crashes per pipeline stage, and saves
semantically matching samples as a regression corpus.

## Build & run
```
cmake --build cmake-build-release --target Fission.Fuzzing
./cmake-build-release/Fission.Fuzzing.exe --count 5000 --seed 1
```
Flags: `--count N`, `--seed S`, `--out DIR` (default: this folder), `--max-corpus N`,
`--threads N`, `--sugar-only`, `--check-sugar SOURCE OUTPUT`, `--minimize-file PATH`, `--replay-dir DIR`,
`--minimize-budget N`, `--mutate`, `--deser`, `--file PATH`, `--sem-file PATH`, `--deser-file PATH`,
`--roblox-file PATH`, `--roblox-recompile PATH`, `--roblox-compare PATH`, `--roblox-corpus DIR`, `--corpus-start N`, `--corpus-limit N`, and `--repro-mutate PATH SEED`. Exit code is nonzero when any crash,
invalid output, generator failure, or semantic divergence surfaces.

`--ssa-oracle` checks SSA instead of execution: for every register read, Fission's phi-flattened
definitions must equal an independent reaching-definitions dataflow over the raw Luau bytecode
([include/SSAOracle.hpp](include/SSAOracle.hpp)). It runs `--count` generated samples, or every `.lua`
under `--replay-dir`, and saves findings with an SSA dump to `<out>/ssa-oracle/`. `EXTRA_REACHING`
(a sound superset, e.g. the FORNPREP-skip-through-latch shorthand) is imprecision; every other kind
is a missed definition.

`--replay-dir` recursively validates every unique saved source finding in one deterministic run. It checks
recompilation, generated-local declaration order, and execution traces across all semantic fixtures, then
reports every remaining failure instead of stopping after the first one. Forward-reference classes and
semantic trace-pair hashes group failures by cause. Raw opcode differences are diagnostic only.

`--roblox-recompile` is the focused generated-local gate: Roblox bytecode must decompile successfully,
the emitted source must compile with Luau, generated register names must be declared before use, and
the compiled output must survive a second decompile/recompile pass with the same binding checks.
Run `test_local_registers.ps1` to replay the known 200-local corpus fixtures.

## Two generator front-ends (alternated per sample)
- **Luau parser-AST** ([src/LuauAstGenerator.cpp](src/LuauAstGenerator.cpp)) — builds real
  `Luau::AstStat`/`AstExpr` trees and prints them (this Luau version dropped the transpiler, so the
  generator carries its own printer). Broad, syntactically-diverse input incl. constructs the
  decompiler never emits.
  Eight of fifteen variants use bounded programs covering fixture-dependent branches, loop
  updates with continue/break, parallel assignment, multiple returns, closure state, and
  mixed short-circuit guards, compound assignments, and method sugar. Generated functions are
  invoked with inputs reaching both outcomes.
- **Fission AST** ([src/FissionAstGenerator.cpp](src/FissionAstGenerator.cpp)) — builds Fission's
  own AST and renders it with the real `SourceGenerator`. This **fuzzes SourceGenerator itself**: a
  well-formed AST that prints non-compiling Luau is a SourceGenerator bug.
  35% of this stream generates executable captured-table or effectful dispatch programs.
  Captured tables vary branch polarity, array width, and mutation values; dispatch chains vary
  from 24 to 112 arms and execute first, middle, last, and unmatched selectors. Other trees mix
  nested functions, if-expressions, indexed writes, and multiline strings and keys.

Reports include `family:<name>:generated`, `:match`, `:diverge`, and `:unchecked` counts.
Use a fixed seed and thread count to reproduce a campaign. Generated closures in bounded
families are called explicitly; comparing a returned function alone does not exercise its body.

## Syntax sugar recovery

`--sugar-only` selects executable sugar programs from the Luau AST generator. It varies
`+=`, `-=`, `*=`, `/=`, `//=`, `%=`, `^=`, and `..=` across locals, fields, and indexed targets.
Some indexed writes call functions in the index and right operand to observe evaluation order
and repeated evaluation. Other cases combine `function object:method(...)`, colon calls,
record fields, `elseif`, and if-expressions. Interaction cases combine side-effecting receivers,
`__index`/`__newindex`, nested loops, `continue`, captured mutable state, and multiple returns.
All three families also run in ordinary campaigns.

For recompilable outputs, the harness parses input and output and counts sugar AST nodes.
Comments and string contents do not count. `sugar:<form>:input` and `:retained` report input
counts and the smaller input/output count; `:reduced-cases` counts programs losing occurrences.
`SUGAR_COUNT_REDUCED` retains the coarse count diagnostic. `SUGAR_REDUCED` compares named function
paths and normalized compound targets (argument slots, local bindings, fields, and index expressions).
Repeated function names receive occurrence suffixes. Sugar in a different function or on a different
target cannot compensate for a lost occurrence. Returns containing the equivalent local binary update
are accepted when execution matches. Other forms are counted within each function.

Source/output pairs and `.sugar.txt` reports include seed, index, family, semantic verdict, and
function/target keys. These remain review candidates: renamed or inlined functions and changed local
binding order can prevent matching. This is not full SSA correspondence. Sugar reduction alone does
not fail the semantic campaign. `--check-sugar SOURCE OUTPUT` checks a pair directly: exit 0 means
matching execution with no structural losses, 1 means divergence or loss, and 2 means unchecked or
invalid input.

## Failure minimization

Campaigns automatically attempt to reduce up to three distinct semantic and three structural findings.
`--minimize-budget N` bounds candidate attempts per finding (default 64; zero disables automatic
minimization). Statement deletion and expression reduction use parsed AST spans. Each accepted candidate
must compile and recompile, preserve the original program's comparable observed traces across all
fixtures, and retain either the same structural function/target finding or the same semantic fixture,
statuses, and normalized errors. Structural reduction also requires matching execution. Opaque traces
and timeouts are not accepted. Results are rechecked before saving.

`finding_*.min.lua`, `.min.out.lua`, and `.min.txt` contain the smaller source, regenerated output,
signature, traces or structural evidence, attempt count, and byte reduction. Irreducible inputs are
reported as unchanged; bounded reduction does not promise a global minimum. Use
`--minimize-file PATH --out DIR --minimize-budget 256` to reduce a saved reproducer independently.

Run `./Fission.Fuzzing/test_sugar.ps1 -Fuzzer ./cmake-build-debug/Fission.Fuzzing.exe` for a
reproducible check that every supported sugar family/operator is generated, measured, and runs
with matching behavior, plus cross-function/target checks, valid return inlining, and reduction of
a padded structural reproducer.

## Oracle & crash scopes ([include/FuzzOracle.hpp](include/FuzzOracle.hpp))
Per sample: compile-check → per-stage attributed run (Deserializer → Lifter → CFA → SSA → ASTLifter
→ rewriters → SourceGenerator, each pinned so a libassert abort is attributed to its stage) → full
pipeline → semantic comparison, with lifted opcode sequences retained for diagnostics. Outcomes include `CRASH`
(per stage), `INVALID_RECOMPILE`, `SOURCEGEN_INVALID`, `FORWARDREF_IR_*`, `OK_IR_STABLE`,
`OK_SEM_MATCH`, `SEM_DIVERGE`, `SEM_UNCHECKED`, and `GEN_NONCOMPILE`.

Every recompilable output runs against all three VM fixtures, including outputs with identical
opcode names. Matching errors alone, timeouts, opaque values, metatables, shared table or buffer
references, and depth-limited traces remain unchecked. These runs do not enter the passing corpus. A semantic
match describes observed execution under those fixtures, not a proof for every possible input.
Sparse table storage layout is ignored; numeric string keys, string bytes, and double precision
remain distinguishable. Divergence trace files include seed, sample index, fixture, and statuses.

On Windows, an unhandled fault saves `_HARD_FAULT_<thread-id>.lua` with the current source and
its `front=`/`seed=`/`idx=` header. Recheck saved sources with `--sem-file PATH`; exit codes are
0 for matching execution, 1 for divergence or invalid output, and 2 for unchecked execution or
an oracle/input error.

### `--mutate` (opt-in, off by default)
Feeds corrupted bytecode to the deserializer. The decompiler's safety boundary turns libassert
aborts into graceful `FailedToDecompile`, but a **raw memory fault or stack overflow on hostile
bytecode hard-crashes the process** — that is itself a finding (a gap in the hostile-input
contract). Run it in isolation; the breadcrumb identifies the offending sample.

## Findings (2026-06-27 campaign)
- **FIXED — leading `;` on a parenthesised first statement.** When the chunk's first statement was a
  call whose callee needs parens (`(true)(x)`), SourceGenerator emitted a leading `;`; luau has no
  empty statement, so the output failed to recompile. Fix: `Visit(RootNode)` keeps `firstStmt` true
  across the header comment. Regression test in IREquivalence.cpp `[Regression][Fuzz]`.
- **FIXED — Luau printer statement merge** (generator-side): consecutive statement-expressions glued
  across newlines; terminate expression-ending statements with `;`.
- **FIXED — break spilled outside a loop.** Regression coverage keeps nested-loop exits inside their
  owning loop; fuzz failures are release-blocking instead of being marked expected.
- `FORWARDREF` hits are mostly the noisy register-naming class (cross-scope `vN` reuse, the deep
  call-split) — see [[fix-table-coalesce-forward-ref]]; verify same-scope before treating as real.

5000 samples, seed 2: 0 CRASH, 1 INVALID_RECOMPILE (the break-spill above), 97% OK.

## Corpus
Compiling, semantically matching samples are saved to `corpus/`. A committed seed set is exercised by the
Catch2 test `[Fuzz][Corpus]` in Fission.Tests, which asserts each still decompiles without crashing
and recompiles. Running the fuzzer adds more (gitignore or curate as desired).
