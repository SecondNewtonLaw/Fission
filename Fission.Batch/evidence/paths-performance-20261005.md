# Reachability and closure-source search performance

Two generic algorithm changes retained. `ChangedOnPath` caches forward/backward reachability intersection per definition/use block pair, capped at 128 entries per function. It keeps original scan order and checks current instruction bounds and effects on every call. `ClassMethodRewriter` lazily indexes possible binding statements per scope, then searches only those positions for closure sources. Matching, redefinition stops and removal order stay unchanged.

Five alternating paired runs against validated LTO worker `5df6a722223a214da6871b0a69d21395dc42639b971511fe49c23ff03652cdf0`. Both builds use Release, full LTO, whole-program vtables and strict math. One fresh native child at a time, Windows JobObject 1 GiB cap, 120-second watchdog, empty profile selection before measured request. Native decompilation time excludes compilation and IPC. Process CPU includes request compilation and response serialization.

| Case | Native before | Native after | Time reduction |
| --- | ---: | ---: | ---: |
| 32 real sources, 38 profiles | 5.150750s | 4.935315s | 4.18% |
| Recent source 0, 38 profiles | 2.338102s | 2.370419s | -1.38% |
| Recent source 1, 38 profiles | 2.349201s | 2.324461s | 1.05% |
| Recent source 2, 38 profiles | 1.203229s | 1.187158s | 1.34% |
| Closure gap 128, two profiles | 0.010739s | 0.011554s | -7.60% |
| Closure gap 512, two profiles | 0.026889s | 0.018700s | 30.45% |
| Closure gap 2048, two profiles | 0.109092s | 0.074704s | 31.52% |

Mixed-corpus process CPU fell from 5.390625s to 5.156250s, 4.35%. Small recent-source differences and the 128-gap microcase remain noisy; one real case was slower. Synthetic closure cases use one local function, 64 table-field assignments referencing it, and the stated number of intervening expression statements. At 2048 statements, Class Method Rewriter time fell from 0.036294s to 0.003056s. These measurements cover both changes together, not isolated attribution.

All 6,680 paired output comparisons matched with zero failures. Final wide replay covers 58 inputs across 38 profiles: 2,204 matching outputs, zero failures, peak private memory 318.707 MiB. Stress samples: 22/22. Final Worker/CLI comparisons: 419; repeat records: 418; invalid requests: 23. Progress: four stderr starts and four records, including two expected compile failures. Focused tidy completes with three style warnings on existing +1 scan arithmetic; new binding-site ranges warning resolved. An 8,192-branch CFG replay on measured worker also matches previous output and completes under 1 GiB and 30 seconds; its peak private memory is 120,631,296 bytes. These checks establish tested output parity, not runtime equivalence. Unit-test suite not run.

Measured worker preserved separately before final formatting. Final build also uses equivalent `std::ranges::lower_bound` after focused tidy. Measurements: [paths-throughput-20261005.json](paths-throughput-20261005.json). Final binaries, hashes and validation: [candidate-paths-20261005.json](candidate-paths-20261005.json).

Runnable parity/performance check from repository root:

```powershell
python Fission.Batch/tools/check_native_throughput.py --baseline cmake-build-lto-20261005/Fission.Batch.Worker.exe --optimized cmake-build-paths-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/paths-recheck.json --repeats 5
```

Existing check asserts comparable outputs match. Selected measurements above additionally used JobObject limits and process CPU counters. Prior binaries and running corpus job remain untouched.
