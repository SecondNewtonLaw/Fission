# Scope traversal and initializer analysis performance

Four changes retained as one measured bundle:

- ScopeBlockIntroducer expression scan uses existing shared node-kind walker. Identifier and closure handling remain explicit; operand order and literal subtype handling stay unchanged.
- DeclarationHoister statement walk uses node-kind dispatch, preserving repeat-condition scope, loop operand order and capture discovery behavior.
- DeadLocalEliminator retains initializer self-reference beside cached statement names, removing second initializer traversal for surviving named locals. Declaration targets and type annotations still contribute to statement names, but not initializer self-reference.
- Debug-local naming sorts existing per-register write histories once, uses binary searches for last preceding write and intervening writes, and skips metadata analysis when local metadata is absent. Existing interval boundaries and initializer rules stay unchanged.

Five alternating paired runs against validated paths worker `741ba9ccb0b11b693f2dc7ce4f021ebe80083191306c9c1c6e265e26673e199c`. Both builds use Release, full LTO, whole-program vtables and strict math. One fresh native child at a time, Windows JobObject 1 GiB cap, 120-second watchdog, empty profile selection before measured request. Native time excludes compilation and IPC. Process CPU includes request compilation and response serialization.

| Case | Native before | Native after | Time reduction | CPU before | CPU after |
| --- | ---: | ---: | ---: | ---: | ---: |
| 32 real sources, 38 profiles | 4.548967s | 4.336623s | 4.67% | 4.750000s | 4.531250s |
| Recent source 0, 38 profiles | 2.429940s | 2.297537s | 5.45% | 2.531250s | 2.390625s |
| Recent source 1, 38 profiles | 2.226532s | 2.169723s | 2.55% | 2.328125s | 2.250000s |
| Recent source 2, 38 profiles | 1.241555s | 1.138421s | 8.31% | 1.281250s | 1.203125s |
| 128 repeated-register scopes, two profiles | 0.007195s | 0.006975s | 3.06% | below useful timer resolution | below useful timer resolution |
| 512 repeated-register scopes, two profiles | 0.031059s | 0.031313s | -0.82% | 0.031250s | 0.031250s |

Mixed-corpus process CPU fell 4.61%. Combined hoister pass medians fell from 0.519574s to 0.444538s; scope pass from 0.195482s to 0.125775s; combined dead-local pass medians from 0.403559s to 0.382843s. These sums describe pass medians, not isolated attribution. Changes were measured together. Small repeated-register cases provide parity coverage; they do not demonstrate a useful scaling win. Their source repeats `do local v = read(i); t[i] = v end` and returns the table, using O0/D0 and O0/D2 profiles.

Earlier dispatch-only experiment reduced targeted pass times but left total mixed time flat. Single-pair initializer pilot was noisy and slower overall. Neither result supplies the selected gain above; selected figures come from final five-pair bundle evaluation. Timings remain subject to host noise and do not guarantee improvement for every input.

All 6,670 paired comparisons matched, zero failures. Final formatted worker replay covers 58 inputs across 38 profiles and matches 2,204 reference outputs, zero failures, peak private memory 317.988 MiB. Stress samples: 22/22. Worker/CLI comparisons: 419; repeat records: 418; invalid requests: 23. Progress: four stderr starts and four records, including two expected compile failures. Focused tidy completes without errors; seven recursive-walk warnings and twelve ranges/numeric style warnings remain. An 8,192-branch CFG replay on measured worker also matches previous output, completing under 1 GiB and 30 seconds with peak private memory 119,939,072 bytes. These checks establish tested output parity, not runtime equivalence. Unit-test suite not run.

Measured worker preserved separately before final formatting. Measurements: [scopes-throughput-20261005.json](scopes-throughput-20261005.json). Final binaries, hashes and validation: [candidate-scopes-20261005.json](candidate-scopes-20261005.json). Prior binaries preserved.

Runnable parity/performance check from repository root:

```powershell
python Fission.Batch/tools/check_native_throughput.py --baseline cmake-build-paths-20261005/Fission.Batch.Worker.exe --optimized cmake-build-scopes-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/scopes-recheck.json --repeats 5
```

Existing check asserts comparable outputs match. Selected measurements above additionally used JobObject limits and process CPU counters.
