# Release LTO performance

Selected existing `USE_LTO=ON` build option: Clang full LTO and whole-program vtables. Strict math retained. No source changes or project defaults changed. Earlier purity/type-inference fast paths gave no consistent real-source improvement and were reverted. Earlier three-build timing run drifted substantially; it is excluded from selected measurements.

Five alternating paired runs against validated rewriters worker `51801eae5938359e16d565ba837ccbd75678443e557e155f04af8f1eecf6d814`. One fresh native child at a time, hard 1 GiB memory cap and 120-second watchdog. Empty profile selection completes startup before measured request. Native time excludes compilation/IPC. Process CPU time includes request compilation and response serialization.

| Case, each with 38 profiles | Native before | Native LTO | Time reduction | CPU before | CPU LTO |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 real sources | 4.660651s | 4.298713s | 7.77% | 4.875000s | 4.484375s |
| 02fcad1e31ff49a00bd4e7bf9221ade6 | 2.364267s | 2.306501s | 2.44% | 2.421875s | 2.375000s |
| 529ba9f7715705f776a7f847ab469469 | 2.235581s | 2.241673s | -0.27% | 2.312500s | 2.343750s |
| 3d59c7173079637bf4b165c306076700 | 1.219996s | 1.188198s | 2.61% | 1.250000s | 1.234375s |

Mixed-corpus process CPU work fell 8.01%. Small recent-source differences remain sensitive to timing noise; one source was essentially unchanged.

All 6,650 paired comparisons matched. Final LTO worker matched 2,204 reference outputs with zero failures; peak native private memory 318.84765625 MiB. Stress: 22/22. Worker/CLI comparisons: 419; repeats: 418; invalid requests: 23. Progress: four stderr starts and four records, including two expected compile failures. Unit-test suite not run; tested output parity does not establish runtime equivalence.

Both Batch and Worker compiled in `cmake-build-lto-20261005`. Prior binaries and running corpus job untouched. Exact measurements: [lto-throughput-20261005.json](lto-throughput-20261005.json). Final paths, hashes and checks: [candidate-lto-20261005.json](candidate-lto-20261005.json).

Runnable parity/performance check using existing tool, from repository root:

```powershell
python Fission.Batch/tools/check_native_throughput.py --baseline cmake-build-rewriters-20261005/Fission.Batch.Worker.exe --optimized cmake-build-lto-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/lto-recheck.json --repeats 5
```

This existing check asserts comparable outputs match. Selected measurements above additionally used Windows JobObject caps and process CPU counters.
