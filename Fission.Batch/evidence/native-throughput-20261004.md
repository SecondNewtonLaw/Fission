# Native batch throughput, 2026-10-04

Fission.Batch remains a generic batch decompiler. Worker accepts caller-provided
source, profiles, and existing decompiler flags. Corpus selection belongs to the
verification tools.

Changes remove repeated work without changing lifting or rewriting rules:

- Shared AST traversal dispatches through existing node kinds. Literal subtype
  checks, child order, mutable child references, and shared ownership remain.
- Each basic block indexes SETLIST consumers once. Constructors select the same
  first later consumer by register and SSA version.
- Function state records whether NEWCLASS occurs, avoiding class-name scans in
  functions without classes.
- DominatesMerges uses the existing phi-consumer index to return immediately when
  no merge consumes the value.
- Merge discovery accepts direct joins in either branch orientation under the
  existing forward-flow, loop, and terminal-fold guards. Other shapes retain the
  full search.

Release binaries build in `cmake-build-throughput-20261004` with Clang 22.1.0,
sanitizers disabled, standard floating-point semantics, and LTO disabled. Targets:
Fission.Batch, Fission.Batch.Worker, Fission.CLI.

Final Batch SHA-256:
`3612e30ceb3fcce4945652af2c7aec7d97689a3e4bef6c6362cbbf006f895e77`.
Final Worker SHA-256:
`989d8642a9b4f6484ae92076975dd05288468cafd9086aeff31e64ec10a8e4d7`.

Baseline worker is preserved in
`cmake-build-throughput-evidence-20261004/Fission.Batch.Baseline.exe`.
SHA-256: `fd377ea57ca7e207bb868618cf2e09c60d47942476e8440ab028d45713d4f299`.
Protected corpus Batch executable remains unchanged at
`cmake-build-fuzz-clang/Fission.Batch.exe`, SHA-256
`44de54983239fc27ad995a03bf36a5cd2f0207b6bd51f0fac0b741098e0a7318`.

Verification covers 58 sources across 38 compile/decompiler profiles: 32
SHA-sorted corpus sources, 22 stress samples, and four top-level samples. Exact
record comparisons exclude request identifiers, paths, reuse metadata, and timing
fields. Output, status, echoed source, and captured artifacts remain compared.
All 2,204 records match the baseline with zero failures and no worker stderr.

Stress CLI checks passed 22/22, including parsing generated output. Worker
protocol checks passed 419 CLI comparisons, 418 repeated records, 23 invalid
requests, exact UTF-8/CRLF/NUL source echo, source reuse, request recovery, timeout
followed by success, and clean EOF exit. Protocol checks used the optimization
build before the final traversal-helper extraction; final output comparison and
stress checks used the final binary hash above. Experimental NEWCLASS bytecode also
preserved output and AST in a focused positive-path comparison.

Synthetic scaling checks preserve outputs. With 8,192 named table constructors,
SETLIST indexing reduced AST lifting from 0.787s to 0.523s. With 8,192 conditional
nil assignments, lifting fast paths reduced AST lifting from 3.212s to 1.791s and
native decompilation from 13.885s to 12.504s. These are focused runs, not corpus
medians. CFG structure identification remains expensive on that synthetic shape.

The paired resident-worker benchmark uses 32 real sources and 38 profiles,
alternating baseline/optimized order across three repeats. JSON evidence records
binary and request hashes, exact output fingerprints, per-stage medians, and
transport-inclusive wall time. Native decompilation timings exclude compilation
and JSONL transport.

Final paired medians:

| Measurement | Baseline | Optimized | Speedup |
| --- | ---: | ---: | ---: |
| Native decompilation | 9.486s | 5.183s | 1.830x |
| Worker wall time | 9.719s | 5.436s | 1.788x |
| AST rewriting | 6.472s | 2.621s | 2.470x |
| AST lifting | 1.363s | 1.384s | 0.985x |

Each of six runs produced 1,216 successful records with the same non-timing
fingerprint. Neither worker emitted stderr. AST lifting on this real-source set
did not improve; focused synthetic shapes exercise the algorithmic fast paths.

Deferred formatting completed after optimization checks. Final builds, stress
checks, and exact output comparison passed. Clang-tidy completed without errors;
recursive traversal and other source diagnostics remain. The new traversal-size
diagnostic was removed by extracting repeated child-visiting patterns.

```powershell
python Fission.Batch/tools/check_native_throughput.py --baseline cmake-build-throughput-evidence-20261004/Fission.Batch.Baseline.exe --optimized cmake-build-throughput-20261004/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out Fission.Batch/evidence/native-throughput-20261004.json --repeats 3
```

Unit-test suites and subagents were not used. Matching this corpus is regression
evidence, not proof of semantic equivalence for all bytecode.
