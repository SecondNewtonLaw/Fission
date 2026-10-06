# VTune-guided optimization, 2026-10-05

Final rebuilt worker reduces native decompilation time by **11.05%** against preceding Consumers build in direct paired comparison. All five corpus sweeps faster; outputs identical. Separate direct comparison against pre-optimization worker shows **61.65% less native time**, **2.607x native throughput**.

## Changes

- Inlining decision memo, binary-depth memo, constructor-element memo and active-decision set use existing Boost flat containers. Eliminates node allocation for each memo entry and each active-set insertion/erase. Existing memo clears, recursive cycle guard, decision/evaluation order remain. Containers whose references survive recursion remain unchanged.
- LiteralNode records whether it is a table; both TableLiteralNode constructors set marker. Existing LiteralValue tag preserved. Shared traversal avoids table-subtype RTTI for leaf literals and retains strong table ownership while visiting mutable children. Literal base remains 32 bytes, same as original layout.
- Shared AsExpression dispatches by existing node kinds, retaining RTTI fallback for Unknown/unused FunctionExpression kinds. ScopeAwareRenamer uses it and dispatches declaration counts by kind.
- ClassMethodRewriter read helpers use node-kind dispatch. Bare identifier assignment targets remain writes; call result targets remain excluded from reads; argument shadowing, TableBinaryExpression inheritance, child order and early returns preserved.

No Batch specialization, new dependency or output rule. Product edits limited to five native headers. Formatting/tidy deferred until experiments finished.

## Final paired performance

32 real inputs x 38 profiles, five paired sweeps, 1,216 records per worker per sweep. Full warmup, two-profile chunks, alternating worker order; both workers pinned to logical CPU 20 with ABOVE_NORMAL priority. Shared JobObject caps native memory at 1 GiB and each campaign wall time at 120 seconds. Native timings exclude compilation and transport; process CPU includes request compilation and serialization. Other host jobs remained active.

| Measurement | Consumers build | Final VTune fixes build | Reduction |
| --- | ---: | ---: | ---: |
| Native decompilation | 4.806929s | 4.275846s | 11.05% |
| Process CPU | 5.125000s | 4.562500s | 10.98% |

Final comparison is `interleaved-corpus-final.json`; all 6,080 paired records match. Original-versus-final comparison is `overall-final.json`: three sweeps, all 3,648 paired records match.

| Original-versus-final measurement | Original | Final | Reduction |
| --- | ---: | ---: | ---: |
| Native decompilation | 8.974240s | 3.442028s | 61.65% |
| Resident worker wall | 9.310917s | 3.749221s | 59.73% |
| Process CPU | 9.281250s | 3.687500s | 60.27% |

Campaigns have different host conditions. Compare each pair within its own campaign; do not combine absolute times or add percentages between campaigns.

Pre-format bundle experiment: 11.32% lower corpus native time, 13.69% lower lifting-stage time and 15.34% lower rewriting-stage time. Recent-source native reductions: 3.89%, 4.42%, 13.37%; all 20 corpus/recent sweeps faster, 6,650 paired records matching. These files reference immutable measured snapshot `cmake-build-throughput-evidence-20261004/Fission.Batch.VTuneFixesMeasured-20261005.exe`, SHA-256 `d8e5193b1083abb52bb6e780a97947f1efb14f39b0c9d887d76a62537ba24029`. Final post-format binaries have their own performance, profile and regression checks.

## VTune before/after

Exact final worker profiled with same VTune 2025.8 CLI software Hotspots configuration and call stacks as previous Consumers profile. Six full corpus sweeps: 7,296 successful records, zero failures and stable output fingerprint. Fission symbols resolved; private CRT/system symbols unavailable.

| Sampled CPU attribution | Before | After |
| --- | ---: | ---: |
| dynamic_pointer_cast, aggregated self | 5.019420s (19.50%) | 3.332541s (12.73%) |
| malloc attributed to ShouldInline | 0.438749s | 0.015319s |
| cast self attributed to ForEachSubExpression | 0.716607s | 0s sampled |
| cast self attributed to class read helpers | 0.458476s | 0s sampled |
| ClassMethodRewriter::RewriteStatements, inclusive | 1.433541s | 0.431059s |
| ShouldInline, outermost inclusive | 2.778612s | 2.316371s |

Cast-family self time fell 33.61%; targeted ShouldInline allocation CPU fell 96.51%. Code also removes those node allocations structurally. Zero samples is not a universal claim of zero runtime cost.

Remaining allocation remains significant: total malloc_base attribution increased from 4.705667s to 5.229031s, about 19.98% of final worker CPU. Overall sampled CPU was 25.737582s before and 26.174075s after. These independent profiling sessions are diagnostic, not controlled throughput proof; paired interleaved benchmarks above establish speedup. No claim that total allocator cost decreased. Inclusive entries overlap and cannot be added to self-time buckets.

Full reports: `hotspots-full.csv`, `top-down-full.tsv`, `summary.txt`, `function-summary.json`, `workload.json`, `collection.log`. Saved parser checks self-time sum against total and allocator/cast caller sums against flat hotspot totals.

## Final validation

- 2,204/2,204 wide records match previous fingerprint `63aa36fac52648686b36f5afde1bbe9af8271e6fe860c164bcdf8883c8c9108b`; zero failures. Peak private memory 329,695,232 bytes under 1 GiB worker cap.
- 54 focused consumer cases match: duplicate reads, SETLIST exceptions, distinct consumers and multi-result definitions.
- Stress CLI: 22/22 pass.
- Standalone C++ assertions pass for leaf literals, expression classification, table copy, ordered mutable traversal, parent replacement during callbacks and original literal-base size. Saved `check-vtune-traversal-20261005.cpp` reproduces check.
- clang-tidy on Decompiler.cpp and ASTLifter.cpp with changed-header filter: exit 0, zero errors, 634 visible diagnostics; existing warning debt remains. git diff --check passes.
- Unit-test suites not run. Output parity is regression evidence, not proof of all-bytecode runtime equivalence.

## Binaries

Build directory: `cmake-build-vtune-fixes-20261005`. Clang 22.1.0 Release, full LTO, standard floating-point semantics, sanitizers disabled, compiler cache bypassed. Batch, Worker and CLI compiled successfully.

Final Worker SHA-256: `63b29b8f08ec022bc764c48e031792fe122b7a2ae1ece1a5112406907692dc01`.
Final Batch SHA-256: `b8ef71a046f1cf79dc5c08c4de2c778a2e2d629738725632abdecc0ac0176c97`.
Preceding Consumers Worker SHA-256: `7c19555f9c4f40e77a5354d9219381282433d509e96e0e5a6a54e599e16a9f32`.
Original worker SHA-256: `fd377ea57ca7e207bb868618cf2e09c60d47942476e8440ab028d45713d4f299`.

Final comparison reproduction:

```powershell
python Fission.Batch/tools/check_interleaved_throughput.py --baseline cmake-build-consumers-20261005/Fission.Batch.Worker.exe --optimized cmake-build-vtune-fixes-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/vtune-fixes-recheck.json --repeats 5 --profile-chunk 2 --cpu 20 --memory-mib 1024 --seconds 120
```
