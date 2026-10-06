# Lifting and naming performance

Generic optimizations: node-kind dispatch in LoopVariableRenamer; per-function definition-site index; cached value-read dependency summaries; single SSA definition/name lookups; direct reserved-name suffix checks; reverse lookup for reused disambiguated names.

Native decompile time excludes compilation and IPC. Three alternating paired runs, one fresh worker at a time. Baseline worker SHA-256: `fccb3a1ece0cbcc2f6c3b1915a81d704d83507fd315d2cb2b924d0e5d7ad5582`.

| Case | Before | After | Time reduction |
| --- | ---: | ---: | ---: |
| 32 real sources, 38 profiles | 4.926164s | 4.694942s | 4.69% |
| 02fcad1e31ff49a00bd4e7bf9221ade6, 38 profiles | 2.388877s | 2.364939s | 1.00% |
| 529ba9f7715705f776a7f847ab469469, 38 profiles | 2.377615s | 2.289138s | 3.72% |
| 3d59c7173079637bf4b165c306076700, 38 profiles | 1.334861s | 1.262556s | 5.42% |
| Synthetic 2,048 reserved-name scopes, 2 profiles | 0.152281s | 0.121592s | 20.15% |

Small differences, especially 1%, remain sensitive to timing noise. Mixed-corpus LoopVariableRenamer time fell from 0.172489s to 0.044515s; AST lifting from 1.325109s to 1.250158s. These measurements cover the combined changes.

All 4,008 paired output comparisons matched. Final formatted build matched 2,204 reference outputs: zero failures, peak native private memory 317.8125 MiB under 1 GiB hard limit and 120-second request watchdog. Stress script: 22/22. Worker/CLI comparisons: 419; repeat records: 418; invalid requests: 23. Progress: four stderr starts and four records, including two expected compile failures.

Both Release targets compiled. Formatting followed completed optimization experiments. Focused clang-tidy completed without errors; style warnings remain. Unit-test suite was not run. Output comparisons establish tested output parity, not runtime equivalence.

Exact runs: [lifting-throughput-20261004.json](lifting-throughput-20261004.json). Final executable hashes and checks: [candidate-lifting-20261004.json](candidate-lifting-20261004.json). Measured executable remains separately preserved; final build differs by formatting only. Original and prior workers remain unchanged.
