# Consumer allocation optimization, 2026-10-05

Final rebuilt worker reduces native decompilation time by **55.80%** against preserved pre-optimization native worker: **9.716737s to 4.295210s**, **2.262x throughput**. This includes all optimization phases and enabled LTO. It is a direct paired comparison, not a sum of percentages from separate runs.

| Measurement | Original | Final | Reduction |
| --- | ---: | ---: | ---: |
| Native decompilation | 9.716737s | 4.295210s | 55.80% |
| Resident worker wall time | 10.084924s | 4.647831s | 53.91% |
| Process CPU | 10.031250s | 4.578125s | 54.36% |
| AST rewriting | 6.605899s | 1.996238s | 69.78% |
| AST lifting | 1.391999s | 1.203260s | 13.56% |

## Method

32 real corpus inputs, 38 profiles, three alternating paired sweeps; 1,216 records per worker per sweep. Full request warms both workers first. Two-profile chunks alternate worker order. Both workers use logical CPU 20 and ABOVE_NORMAL priority; only one request active at a time. Shared JobObject caps aggregate native memory at 1 GiB and campaign wall time at 120 seconds. All 3,648 paired outputs match; zero record failures.

Native time excludes source compilation and JSONL transport. Worker wall time includes compilation, serialization and transport for resident workers; process startup excluded by warmup. Chunking changes compile reuse equally for both builds. Other host workloads remained active, so individual run times vary. Final direct comparison uses medians from the same campaign.

Original worker SHA-256: `fd377ea57ca7e207bb868618cf2e09c60d47942476e8440ab028d45713d4f299`.
Final worker SHA-256: `7c19555f9c4f40e77a5354d9219381282433d509e96e0e5a6a54e599e16a9f32`.
Final Batch SHA-256: `5b8bbd27bfa9f2fed21779b5436cabe577f4b00005be9682747b3e2a55a6274d`.
Final binaries: `cmake-build-consumers-20261005`. Clang 22.1.0 Release, LTO enabled, sanitizers disabled, standard floating-point semantics. Compiler cache bypassed after own ccache launchers stalled; no compiler optimization setting changed.

## This phase

- NEWTABLE inlining tracks one consumer pointer instead of constructing a hash set. Distinct consumers and repeated non-SETLIST reads still reject inlining.
- Call inlining reuses existing SoleUser helper instead of constructing another hash set. Repeated reads still reject inlining except existing SETLIST exception.
- Constructor analysis borrows immutable indexed SSA definitions with a span, using stack array for fallback. No per-instruction vector copies or allocations.

No new cache, dependency, batch specialization or decompilation rule. Unused unordered_set include removed.

Incremental experiment compared Dataflow worker against immutable ConsumersMeasured snapshot. Five interleaved sweeps each:

| Case | Dataflow | Candidate | Reduction |
| --- | ---: | ---: | ---: |
| 32-source corpus, 38 profiles | 4.862116s | 4.718245s | 2.96% |
| Recent source 0, 38 profiles | 2.148107s | 2.157790s | -0.45% |
| Recent source 1, 38 profiles | 2.118214s | 2.083764s | 1.63% |
| Recent source 2, 38 profiles | 1.167942s | 1.109535s | 5.00% |

Four of five corpus sweeps faster; fifth slower. Recent source 0 effectively flat relative to observed run variation. Corpus lifting-stage median fell 4.62%. These are bundle results, not isolated attribution to individual changes. Snapshot predates deferred formatting and final stack-array substitution to remove new magic-number diagnostic. Final rebuilt binary has separate original-versus-final campaign above.

Snapshot SHA-256: `3ec5448b6255f9d19f097400399fce3da138c70042ae7ba77c4966bc1946b6db`. Snapshot stored at `cmake-build-throughput-evidence-20261004/Fission.Batch.ConsumersMeasured-20261005.exe`; copied incremental JSON files reference that exact snapshot.

## Validation

Final rebuilt worker: 3,648 original-versus-final paired records, 54 focused consumer records across O0/O1/O2 and D0/D1/D2, zero failures. Final CLI: 22/22 stress samples pass. Focused cases cover duplicate arithmetic reads, duplicate table arguments, SETLIST duplicate consumers, distinct consumers and multi-result definitions.

Earlier formatted candidate, before stack-array warning fix: 2,204/2,204 wide records match Dataflow fingerprint `63aa36fac52648686b36f5afde1bbe9af8271e6fe860c164bcdf8883c8c9108b`; zero failures, peak private memory 335,405,056 bytes under 1 GiB cap. Wide replay is attributed to that intermediate executable, not final hash.

Formatting deferred until experiments completed. Final clang-tidy exit 0, zero errors, 1,310 visible diagnostics including 174 in changed translation unit; existing warning debt remains. New span-count magic-number diagnostic removed. git diff --check passes. Unit-test suites not run. Output parity is regression evidence, not proof of runtime equivalence for all bytecode.

Final comparison reproduction:

```powershell
python Fission.Batch/tools/check_interleaved_throughput.py --baseline cmake-build-throughput-evidence-20261004/Fission.Batch.Baseline.exe --optimized cmake-build-consumers-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/consumers-overall-recheck.json --repeats 3 --profile-chunk 2 --cpu 20 --memory-mib 1024 --seconds 120
```
