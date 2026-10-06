# Dataflow and structure-identification performance

Closer audit covered SSA, lifter pre-passes, value lifetimes, CFG cleanup, dominance queries and shared renamers. Main large-CFG bottleneck was structure identification: repeated dominator-parent walks and whole-block searches for loop headers. Dominator computation itself already uses Lengauer-Tarjan and remains unchanged.

Retained changes:

- Build dominator-tree entry/exit intervals once; answer known ancestry queries in constant time. Keep original parent walk for unknown nodes and retain deadline checks.
- Index eligible loop headers once for forward-jump classification. Skip this final pass when no eligible loop exists. Preserve header order; this pass changes standard blocks to break/continue and does not change indexed headers.
- Inspect closure opcodes before capture analysis and use existing instruction-to-block index. Debug initializer dominance checks use same index.
- Index next same-block definition lazily for overlap checks, preserving direct next-definition cases. Reuse visited-set storage between lifetime walks; visitation order and cycle rules remain unchanged.
- Remove dead edges with one stable filter per edge list after marking unreachable blocks. Keep invalid neighbor IDs and original diagnostic ordering; skip filtering when nothing was pruned.
- Compute SSA liveness with 256-bit register masks instead of scalar register/edge loops. Register limit comes from byte-sized VM metadata. Preserve prep-only reads, latch-edge behavior, generic-for variable filtering, predecessor worklist order and wild-target guards.
- Use node kinds and typed visitors in shared renamer loop-binding and declaration checks. No naming rules changed.

Both compared builds use Release, full LTO, whole-program vtables and strict math. Baseline is validated scopes worker `e491b83af5f463cb3a26b5ef40f7b57a587514bb94bac30e23a1abae5ac11b45`. Measured candidate `a868e84d89ecd293b1cd72f7bdf8c5abe0e01d67e608b29405e7e62b9d67ffe7` is preserved separately before final formatting.

Real-source evaluation uses five paired sweeps. Two-profile requests alternate between workers, following full warm-up. Both workers use logical CPU 20 and above-normal priority, with one request active at a time. Shared Windows JobObject limits aggregate native memory to 1 GiB and total benchmark wall time to 120 seconds. Native time excludes compilation and IPC; process CPU includes request compilation and response serialization. Request chunking changes compile reuse compared with one full request, equally for both builds.

| Case, 38 profiles | Native before | Native after | Time reduction |
| --- | ---: | ---: | ---: |
| 32 real sources | 4.005203s | 3.860095s | 3.62% |
| Recent source 0 | 2.068145s | 2.032755s | 1.71% |
| Recent source 1 | 2.088155s | 2.062093s | 1.25% |
| Recent source 2 | 1.105477s | 1.027482s | 7.06% |

Candidate was faster in all 20 real-source sweeps. Mixed-corpus process CPU medians fell from 4.281250s to 4.109375s, 4.01%. Changes were measured together; these are bundle results, not isolated attribution.

Synthetic 8,192-branch nil/phi CFG, O1/D1, three alternating fresh-child pairs: native median **11.367090s to 1.228624s**, **89.19% lower**, **9.25x faster**. Structure-identification median fell from **9.298340s to 0.013543s**. Each child used hard 1 GiB memory limit and 30-second watchdog; record budget remained 20 seconds. All three outputs matched baseline. This large synthetic improvement does not imply the same gain on ordinary scripts.

Selected measurements matched 6,650 real-source records and three large-CFG records, zero failures. Exact cases and pass timings: [dataflow-throughput-20261005.json](dataflow-throughput-20261005.json). Final build hashes and validation: [candidate-dataflow-20261005.json](candidate-dataflow-20261005.json).

Earlier whole-request and pinned fresh-child campaigns were noisy during other host workloads; their results are excluded from selected gains. An earlier large-CFG attempt failed a record assertion without retaining its original reason. A bounded standalone replay then completed in 11.06 seconds with matching output. Final CFA changes subsequently passed all three large-CFG pairs in about 1.2 seconds. Failure diagnostics are now retained by benchmark helper. Earlier artifacts remain in validation scratch directory; this history is not presented as proof of a recovered original cause.

Runnable selected real-source check:

```powershell
python Fission.Batch/tools/check_interleaved_throughput.py --baseline cmake-build-scopes-20261005/Fission.Batch.Worker.exe --optimized cmake-build-dataflow-20261005/Fission.Batch.Worker.exe --request cmake-build-throughput-evidence-20261004/request-32.json --out cmake-build-throughput-evidence-20261004/dataflow-recheck.json --repeats 5 --profile-chunk 2 --cpu 20 --memory-mib 1024 --seconds 120
```

Final formatted worker matched 2,204 reference outputs, zero failures, peak private memory 334,692,352 bytes under 1 GiB cap. Formatted build also replayed large CFG with matching output. Stress samples: 22/22. Worker/CLI comparisons: 419; repeats: 418; invalid requests: 23. Progress: four stderr starts and four records, including two expected compile failures. Focused tidy completed without errors and retains 38 diagnostics for numeric/conversion style, existing narrowing conversions, recursive walks, function size and reusable visitor forwarding. No warning-free claim.

Output parity checks do not establish runtime equivalence. Unit-test suite not run. Prior binaries and other running tasks preserved.
