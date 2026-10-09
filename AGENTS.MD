# AGENTS.MD, Fission Project Init

## Methodologies (read before working)

- **Finding and fixing bugs**: [`Agents/Methodologies/BugHunting.md`](Agents/Methodologies/BugHunting.md). Diagnose first, then fix: find, reduce, diagnose across the pipeline and against the Luau compiler's emitted shapes, fix the root cause, verify with a regression test proven to fail without the fix.

## Design references

- **Luau compiler lowering**: [`Agents/Design/LuauCompilerDesign.md`](Agents/Design/LuauCompilerDesign.md). How conditions, and/or, if-expressions, if/while/repeat/for, break/continue and O2 inlining are lowered; the CFA and lifter must invert exactly these shapes.

## Project Init

- **Type**: native-lib + cli-tool (C++23, CMake 3.31+)
- **Lang**: C++23, Luau 0.738 (bytecode v6-v12)
- **Build (Release)**: `cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release && cmake --build cmake-build-release --target Fission.CLI`
- **Build (Debug)**: `cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug && cmake --build cmake-build-debug --target Fission.Tests`
- **Test**: `cmake --build cmake-build-debug --target Fission.Tests && ctest --test-dir cmake-build-debug --output-on-failure`
- **Lint/format**: No scripts. Manually: `clang-format -i <file>`, `clang-tidy -p cmake-build-debug <file>`
- **CI**: GitHub Actions in `.github/workflows/`: `tests.yml`, `fuzz.yml`, `mem-sanitize.yml`, `decompiler-server-test.yml`, `release-server.yml`.
- **Build toggles**: `CMakeLists.txt:10-24` (`USE_CCACHE`, `USE_MIMALLOC`, `PRODUCTION_BUILD`, `USE_LTO`, `ENABLE_ASAN`/`ENABLE_UBSAN`, `ENABLE_PGO_*`, `DEBUG_*`). Sanitizers and mimalloc change build behavior; check these before chasing a build-only difference.

## Command Order + Verification

1. `cmake -S . -B cmake-build-debug -DCMAKE_BUILD_TYPE=Debug` — configure, verify no CMake error
2. `cmake --build cmake-build-debug --target Fission.Tests` — compile, verify no compile error
3. `ctest --test-dir cmake-build-debug --output-on-failure` — all tests pass
4. Format: `clang-format -i <changed files>` before commit
5. Tidy: `clang-tidy -p cmake-build-debug <changed files>` — verify no new warnings

## Architecture + Structure

- Pipeline: Deserializer → BytecodeLifter → ControlFlowAnalyzer → SSABuilder → ASTLifter → AST rewriters (`Fission.Decompiler/include/Rewriters/`) → SourceGenerator. AST-to-AST rewrites go in a rewriter pass, not in ASTLifter.
- CMake targets: Fission.Common (static) → Fission.Decompiler (static) → Fission.CLI (exe); Fission.Server (static) + Fission.Server.CLI (exe); Fission.Fuzzing (exe, BUILD_FUZZING); Fission.Tests (exe, BUILD_TESTING); Fission.Batch + Fission.Batch.Worker (exe, always built, Boost::json)
- Entry: `Decompiler::CommonDecompilerEntry` in `Fission.Decompiler/src/Decompiler.cpp:573`
- Hot paths: `Fission.Decompiler/include/Deserializer.hpp`, `src/BytecodeLifter.cpp`, `src/ASTLifter.cpp`, `src/ASTLifterInlining.cpp`, `src/ASTLifterInlineRegions.cpp`, `src/ASTLifterExpressions.cpp`, `src/ASTLifterConditions.cpp`
- No DLL exports — all static libs

## Conventions + Quirks

- `m_` prefix members, `k` prefix statics, `lp` prefix on Hungarian-lite params (e.g. `lpDeserialized`)
- PascalCase for types/fns, `#pragma once` in headers
- Error: ASSERT (libassert) for invariants, std::optional, DecompileResult enum. Malformed bytecode and budget overruns throw `Fission::DecompilerError` (`BytecodeLifter.cpp:286`, `ControlFlowAnalyzer.cpp:91`, `SafetyGuard.hpp:38`), caught at the entry boundary. Hostile-input handling relies on this; do not convert those paths to return codes.
- Ownership: shared_ptr for AST tree, raw ptr for non-owning backrefs, value+vector for IR/CFG
- **Load-bearing FFlag**: the CLI's `main.cpp` enables all Luau-prefixed FValue<bool> flags (need for integer literal deserialization)
- **DenominatorAnalysis** = dominator analysis (`Fission.Decompiler/include/DenominatorAnalysis.hpp`; non-standard naming)
- **Roblox bytecode**: opcode * 203, truncated to a byte (mod 256), via RobloxClientDecoder (`InstructionDecoder.hpp:21`)
- **Loop/CF fragility**: Fixing one pattern often breaks another. Always regress-check Samples/ after modifying CFA/SSABuilder/ASTLifter

## Existing Instruction Sources

- `CLAUDE.md` at repo root — comprehensive build/arch/conventions (authoritative, do not duplicate)
- `TaskList.md` at repo root — outdated todo scratchpad
- `README.md` — brief project description, no actionable instructions

## Memory Reference

Memory root: `C:\Users\Dottik\.claude\projects\F--Coding-cxx-cpp-Fission\memory\`
Index: `memory/MEMORY.md` (one line per memory; start here)
Projects dir: `memory/projects/Fission/` (structured files)
Flat dir: `memory/` (session/fix files)

Key project memory files:
- `projects/Fission/project/goals.md` — project goals, origin, constraints
- `projects/Fission/project/conventions.md` — naming, error handling, ownership, format, lint
- `projects/Fission/project/testing.md` — test framework, gaps, regression advice
- `projects/Fission/architecture/overview.md` — pipeline, bytecode, flags
- `projects/Fission/architecture/dependency-graph.md` — module deps, hot headers, god-objects
- `projects/Fission/architecture/key-decisions.md` — 10 ADRs with rationale
- `projects/Fission/architecture/memory-links.md` — cross-ref index
- `projects/Fission/build/build-system.md` — cmake flags, compiler flags, build commands
- `projects/Fission/domain/data-conventions.md` — endian, alignment, varint, float wire format
- `projects/Fission/domain/domain-terms.md` — LiftedOperation, SSA, AST nodes, flags
- `projects/Fission/env/dev-environment.md` — IDE, format, tidy, gitignore
- `projects/Fission/handoff/session.md` — findings, gaps, open questions
- `projects/Fission/cross-ref/` — dependency cross-ref notes (boost, luau, libassert, catch2, graphviz)

## Unwritten Rules

- **Regression sweep**: run `Samples/StressTests/run_all.ps1` after any CFA/SSA/AST change.
- **Test gaps accepted**: V7-V8 decode untested (V9 covered by the loop in `Fission.Tests/Decoding/BytecodeV10V12Decoding.cpp`); Roblox decoder untested end-to-end; error paths partly untested — don't assume coverage.
- **RbxCli dual-identity**: PRODUCTION_BUILD strips strings/functions. Fission is both public tool and RbxCli kernel — changes affecting output verbosity must guard behind `#ifndef PRODUCTION_BUILD`
- **No codegen**: GetLuauBuiltinName (`BytecodeLifter.cpp:9`, 200+ switch) hand-maintained — add new ops there when extending LiftedOperation
- **const_cast hack**: `BytecodeLifter::LiftFunctionBytecodeInternal` (`BytecodeLifter.cpp:290`) and `ControlFlowAnalyzer.cpp:88` use `const_cast` to store `DeserializedFunction*` / leaders — intentional workaround, not a cleanup target
- **Inlining effect order**: `ASTLifterInlining.cpp:1254` — inlining must not move throws or effects past a staying statement. Check this before touching ASTLifter inlining.
- **Static lib only**: no DLL visibility macros ever — all targets are STATIC or EXE

## Testing strategy

Prefer integration tests for new features and regressions. Unit-only tests can mirror implementation details, enforce incorrect behavior, and pass while adjacent pipeline stages fail. At Fission's scale, shared integration fixtures are simpler to maintain and provide stronger evidence than duplicating isolated setup and internal assertions across many unit tests.

Decompilation coverage should compile real Luau, run the production decompiler, recompile its output, and execute both versions in the real Luau VM. Compare observable results, mutations, call order, and errors using deterministic fixtures. Exercise relevant optimization and debug levels. Successful fixtures must execute successfully and produce observable evidence; matching empty traces, matching timeouts, or skipped comparisons do not establish correctness.

Test public behavior and stage contracts rather than incidental AST layouts, generated names, or node counts. Keep exact source assertions when source presentation is the contract, including `const` for immutable bindings and `local` for mutable bindings. Do not widen expectations to accept incorrect output, normalize away regressions, or change expected behavior merely to make tests pass. Preserve a failing regression before fixing its cause.

Do not construct isolated AST, IR, CFG, SSA, or rewriter fixtures. Produce inputs with the real Luau compiler or bytecode encoder, drive public stage APIs, and validate with the real consumer or an independent oracle. Decoder metadata tests should feed recovered metadata into an actual compiler/decompiler consumer. Malformed-input tests should exercise rejection through the public decompiler or request API.

Call returned functions when testing behavior. Observe table fields and identity explicitly rather than treating opaque or aliased serialization as evidence. Keep independent SSA/CFG invariants and historical bytecode contracts as component integration tests. Syntax and presentation tests establish compiler interoperability or output contracts; they do not establish execution parity. Calibrate test oracles using real compiled programs, including intentional divergent candidates and actual compiler errors. Avoid mocking the compiler, decompiler, rewriters, or VM.
