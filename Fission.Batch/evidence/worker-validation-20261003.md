# Batch worker evidence

Native worker accepts caller-supplied source strings, compile options, and existing
decompiler flags. No tokenizer, embedding integration, or runtime lint stage exists.
Corpus data and its 38 profile combinations are verification inputs only.

Executable: `F:/Coding/cxx_cpp/Fission/cmake-build-fuzz-clang/Fission.Batch.Worker.exe`

SHA-256: `aa5fb70339c26913cd288ae1183842a5d740060651dd479059e144c70a137d8c`

Protected executable: `F:/Coding/cxx_cpp/Fission/cmake-build-fuzz-clang/Fission.Batch.exe`

Protected SHA-256, unchanged before and after:
`44de54983239fc27ad995a03bf36a5cd2f0207b6bd51f0fac0b741098e0a7318`

Configure and Release target build passed with Clang 22.1.0. Existing Boost Windows
extension warnings remain. Formatting and diff checks passed. Clang-tidy completed;
remaining source diagnostics concern existing file CLI code. New worker functions
have no diagnostics. Deterministic naming has an explicit, narrow RNG-check exemption.
Unit-test suites and subagents were not run.

Focused executable checks passed:

- 419 same-build file CLI comparisons: 38 O/D/flag profiles across 11 complete source
  fixtures, plus AST/CFG/debug-note/IR capture comparison. Output and non-timing result
  fields match exactly. Fixtures include compilation failure, UTF-8, CRLF, embedded
  NUL, empty source, loops, closures, methods/tables, expressions, and inlining.
- 418 repeated records matched in one worker process.
- 23 invalid requests emitted one error each; a subsequent valid request succeeded.
- Subset selection, empty selection, duplicate source compile reuse, changed source
  with the same ID, complete source/path echo, and clean EOF exit passed.
- A 1 ms decompile deadline failed the large fixture; its following sibling succeeded.
- Normal worker verification and benchmark stderr measured zero bytes.

The protected binary differs from the current shared-library build on 38 of 418
fixture comparisons, in output only. Existing decompiler changes affect `local` versus
`const` emission. Worker/file CLI comparisons use the same build to avoid conflating
that drift with worker behavior. This is output parity evidence, not semantic proof.

Three final benchmark runs used the first 32 SHA-sorted `.lua` files from the existing
native input cache, without size filtering: 135,769 complete source bytes and 1,216
decompilation records per run. All 3,648 worker records matched same-build file CLI;
every run had zero failures. Raw results and source hashes are in
`worker-benchmark-20261003.json`. Fields named `legacy_*` in that JSON refer to the
same-build file CLI timing baseline; the protected binary comparison is separate.

| Median metric | File CLI | Worker |
|---|---:|---:|
| Wall time | 14.0355 s | 13.3220 s |
| Native compilation time | 0.30151 s | 0.07377 s |
| Native decompilation time | 12.4676 s | 12.9701 s |
| Compilations | 1,216 | 288 |
| Compile reuses | 0 | 928 |
| Output bytes | 14,617,150 | 14,808,695 |
| Stderr bytes | 226,632 | 0 |

Worker request size was 161,190 bytes. Seven launch probes measured median file CLI
help process duration of 0.5915 s and worker spawn-to-empty-completion handshake of
0.5992 s. These include process loading and scheduling, not pure OS spawn time.
One resident worker served all three benchmark requests; file CLI launched 38 processes
per run. Startup is outside the worker request wall times above.

Measured median speedup: 1.0536x. Compilation work fell 75.5%, and compile count fell
76.3%. Decompilation remained dominant. Host load varied: individual wall-time speedups
were about 1.58x, 0.92x, and 1.05x. An earlier pre-refactor run measured 1.86x under
higher file CLI overhead; the final executable's figures above are the reported result.

Worker wall timing starts after request JSON construction and includes pipe transfer,
native work, serialization, and client JSON decoding. File CLI wall timing includes
process startup, file reads, native work, and output capture, excluding subsequent
client JSON decoding. Native compilation/decompilation timings are recorded separately.
These are single-worker measurements on 32 real sources, not full-corpus or 24-worker
throughput evidence. Existing deadlines are cooperative and exclude compilation.
