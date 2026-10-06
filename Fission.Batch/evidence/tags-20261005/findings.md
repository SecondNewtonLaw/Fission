# Further cast and lifting optimization, 2026-10-05

Final standard-allocator worker: `cmake-build-tags-20261005/Fission.Batch.Worker.exe`, SHA-256 `13360649d0141f3fc5f78687fce0dfb7a7ddab1813fe0e8be420b64e6bdc33e2`.
Final Batch: SHA-256 `a417bcf36396f8a4ae6b03fa55b559dffdae04d5307895fd08ff2b378dc16f43`.
Immediate preceding worker: `cmake-build-vtune-fixes-20261005/Fission.Batch.Worker.exe`, SHA-256 `63b29b8f08ec022bc764c48e031792fe122b7a2ae1ece1a5112406907692dc01`.

## Changes

- Add literal subtype tag and `AsLiteral<T>` with RTTI fallback for unknown/custom types. Replace 102 concrete literal RTTI casts across 23 files. Existing AST node kinds remain unchanged.
- Use existing node-kind dispatch in declaration coalescing/recursive initializer handling, inline region marking, const-local marking and function/call-fact collection. Preserve traversal order and ownership.
- Reuse `CollectReads` pending/seen scratch storage per lifter; keep cached read map stable across recursive queries. Reuse one function-map lookup for call facts.
- AST base and literal base sizes remain 16 and 32 bytes on this Windows toolchain, verified against original-layout stand-ins. No cross-platform ABI claim.

## Final paired measurement

`interleaved-corpus-final.json`: five alternating, warmed pairs; 32 real sources across 38 profiles, 1,216 records per sweep, 6,080 paired records total. Single logical CPU 20, above-normal priority, 1 GiB aggregate native memory cap, 120-second watchdog. Exact output fingerprints match; zero failures. All five native-time pairs improve.

| Median | Preceding build | Final build | Reduction |
|---|---:|---:|---:|
| Native decompilation | 3.457312 s | 3.287482 s | 4.9122% |
| Process CPU | 3.734375 s | 3.546875 s | 5.0209% |

Earlier pre-format direct-tag snapshot measured 7.1386% corpus reduction and 2.4576%, 4.8750%, 5.3303% on three recent-source sets. Those results identify their exact executable snapshots; final binary result above takes precedence. Variance prevents treating percentages as additive.

## VTune

Intel VTune 2025.8 CLI software sampling, attached directly to capped final worker; six full corpus sweeps, exact fingerprint checked. See `workload.json`, `collection.log`, `hotspots-full.csv`, `top-down-full.tsv`, and `function-summary.json`.

Compared with prior independent capture: dynamic casts sampled self CPU 3.332541 -> 2.424440 s; share 12.7322% -> 11.8822%. `CollectReads` nearest-caller malloc samples 0.349864 -> 0.151987 s. Optimized coalescing, inline marking and const statement walker show no remaining direct cast samples. Independent captures are diagnostic, not paired speed measurements.

Remaining sampled leaders: malloc 18.9225%, dynamic casts 11.8822%, std::format 3.8292%. Largest remaining cast callers include DeadLocalEliminator, PropertyRenamer and SourceGenerator. No global allocator override was introduced.

## Validation

- Final broad replay: 2,204 records, zero failures, exact preceding-build fingerprint `63aa36fac52648686b36f5afde1bbe9af8271e6fe860c164bcdf8883c8c9108b`; peak private bytes 331,284,480. Result in `cmake-build-throughput-evidence-20261004/tags-final-wide-20261005.json`.
- Focused consumer comparison: 54 paired records, zero failures (`focused-consumers-final.json`).
- CLI stress: 22/22 successful checks, `cmake-build-throughput-evidence-20261004/tags-stress-20261005`.
- `check-tags.cpp` assertion probe: literal RTTI parity, custom/unknown/null fallbacks, child order, table copying, mutable parent lifetime and size checks passed after final rebuild.
- Deferred formatting completed after experiments. Six-TU clang-tidy finished with zero errors and 961 warnings; existing warning debt remains. `git diff --check` passes. No unit-test suite run.
- Final Batch, Worker and CLI built successfully with Release Clang 22.1.0, full LTO, standard floating-point semantics, sanitizers and compiler cache disabled.
- Twelve protected prior executables rehashed unchanged. Other dirty checkout changes preserved.
