# Bounded native memory investigation

No offending sample is confirmed. This investigation did not reproduce the
128 GiB incident in native Fission.

Deployed worker SHA-256:
`989d8642a9b4f6484ae92076975dd05288468cafd9086aeff31e64ec10a8e4d7`.
Replays used the matching existing isolated build. Neither deployed binary nor
corpus preparation was changed or restarted. Source and native caches were read
without modification.

Each native replay ran alone in a Windows Job Object with a 1 GiB process/job
memory ceiling and a 30-second watchdog per request. Native decompile budget was
5 seconds per record; all tested records succeeded without reaching that budget.
The hard memory ceiling was separately checked: a 256 MiB Python allocation in
a 64 MiB job failed with MemoryError. Diagnostic reader bounds each response to
16 MiB. Failed, incomplete, and timed-out requests remain visible in the report.

Seven real inputs were tested across 38 profiles: four source-cache selections
(including 120–123K-character files), then the slowest and largest-output inputs
recovered from recent native checkpoints. Three repeats per case, except the
38K-character source which ran 20 repeated requests in one persistent process.
Total: 1,444 successful records. Outputs matched across each case's repetitions.
Maximum native private-memory peak was approximately 22.2 MiB. The 20-request
case ended around 7 MiB private memory, with an approximately 8.1 MiB peak.

| Recovered source ID | Why selected | Native peak private memory |
| --- | --- | ---: |
| 02fcad1e31ff49a00bd4e7bf9221ade6 | Slowest saved success, about 0.99s | 13.9 MiB |
| 529ba9f7715705f776a7f847ab469469 | Second slowest saved success | 14.1 MiB |
| 3d59c7173079637bf4b165c306076700 | Largest saved output, 304,047 characters | 9.3 MiB |

These are tested candidates, not incident causes. Recent checkpoint report
contains 9,216 records: 9,020 successes and 196 compile failures. Largest saved
output was modest; an input still executing could be absent from checkpoints.

Latest interrupted request `2c182a2873dc454bb0165052b625d863` saved only 42
`o1d2` records. Last returned source was
`552324e23768c57acb67db73ecf7b619`. Request manifests were not persisted before
execution, so its unreturned inputs cannot be read directly from those records.
The preceding saved request has 128 different source IDs with zero overlap:
choosing its next lexicographic hash would identify the wrong input.

Native source inspection found request-local compile caches, per-record result
serialization, fresh Decompiler instances, and reset function-lifting state.
Those facts and these replays do not rule out an untested AST/CFG expansion or
leak. Full 128-source requests across 18 Python workers were not replayed.
Python batch retention and backend memory are being investigated separately.

Separate throughput finding: on the slow recent GUI-building source, native
time across 38 profiles was about 8.6s; DeclarationHoister's three passes consumed
about 6.1s. Declaration lookup and earlier-mention checks repeatedly scan the
same blocks. A new fast-path plan requires user confirmation before C++ edits.

See `native-memory-20261004.json` for hashes, exact peaks, record counts, and
individual replay report paths. Replay tool: `tools/check_worker_memory.py`.
