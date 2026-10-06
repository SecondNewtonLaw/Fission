# Rewrite performance

Generic changes: DeadLocalEliminator reads cached statement-name sets by reference, collects each multi-result initializer once, skips call-shadow work with no results, and avoids repeated local-name extraction. ClassMethodRewriter uses node-kind eligibility checks while retaining literal subtype casts. ScopeBlockIntroducer skips lifetime analysis when no scope cuts exist and uses a vector for dense declaration indices.

Native decompile time excludes compilation and IPC. Three alternating paired runs, one fresh worker at a time, hard 1 GiB native memory cap and 120-second request watchdog. Baseline is previous validated lifting worker, SHA-256 `62a2a7b5efca1fdf5f6eed3511b15763ef2f3598623f03de8974e414da8aba8f`.

| Case | Before | After | Time reduction |
| --- | ---: | ---: | ---: |
| 32 real sources, 38 profiles | 5.213230s | 5.016925s | 3.77% |
| 02fcad1e31ff49a00bd4e7bf9221ade6, 38 profiles | 2.727191s | 2.572675s | 5.67% |
| 529ba9f7715705f776a7f847ab469469, 38 profiles | 2.619864s | 2.521227s | 3.76% |
| 3d59c7173079637bf4b165c306076700, 38 profiles | 1.439735s | 1.427642s | 0.84% |
| Synthetic 64 calls with 8 results each, 2 profiles | 0.019480s | 0.017575s | 9.78% |
| Synthetic 64 calls with 32 results each, 2 profiles | 0.118281s | 0.096442s | 18.46% |
| Synthetic 64 calls with 96 results each, 2 profiles | 0.701970s | 0.517055s | 26.34% |

Synthetic cases use one vararg function, repeated lexical scopes, local result bindings and print calls. At 96 results per call, the two dead-local passes fell from 0.24668s to 0.01694s. Collecting an initializer once removes work previously repeated for each result name. Mixed-corpus dead-local passes fell from 0.60265s to 0.44391s. Small whole-pipeline differences, especially 0.84%, remain sensitive to timing noise.

All 4,008 paired output comparisons matched. Final formatted Release worker matched 2,204 reference outputs with zero failures; peak native private memory 318.828125 MiB. Stress script: 22/22. Worker/CLI comparisons: 419; repeat records: 418; invalid requests: 23. Progress check: four stderr starts and four records, including two expected compile failures.

Both Batch and Worker compiled. Formatting/tidy followed completed experiments. Focused clang-tidy completed without errors; style warnings remain. Unit-test suite not run. Output comparisons establish tested output parity, not runtime equivalence. Running corpus job and original/prior binaries untouched.

Exact runs: [rewriters-throughput-20261005.json](rewriters-throughput-20261005.json). Final paths, hashes and checks: [candidate-rewriters-20261005.json](candidate-rewriters-20261005.json). Measured executable separately preserved; final build differs by formatting only.
