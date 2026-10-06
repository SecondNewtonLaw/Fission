# Persistent source worker

Build `Fission.Batch.Worker` to produce a separate executable while keeping an existing
`Fission.Batch` executable in use. Both targets support the existing file CLI and
`--worker`.

```text
cmake --build cmake-build-fuzz-clang --target Fission.Batch.Worker -j 2
cmake-build-fuzz-clang/Fission.Batch.Worker.exe --worker
```

Send one UTF-8 JSON object per stdin line. Read stdout until that request's terminal
`complete` or `error` event before sending another request. EOF exits with code zero.
Worker execution is sequential; the caller owns process parallelism.

```json
{"requestId":"batch-1","kind":"source","budgetMs":120000,"inputs":[{"id":"source-sha256","path":"F:/virtual/source.lua","source":"local x = 1\nreturn x"}],"profiles":[{"id":"o0d1_base","optimizationLevel":0,"debugLevel":1,"flags":["--no-comments","--no-ir"]},{"id":"o0d1_types","optimizationLevel":0,"debugLevel":1,"flags":["--no-comments","--no-ir","--types"],"inputIds":["source-sha256"]}]}
```

`path` is an identity label. The worker compiles the complete `source` string,
including CRLF, Unicode, and embedded NUL bytes, without opening that path.
Input IDs and profile IDs must be nonempty and unique within their respective arrays.
Both arrays must be nonempty. `kind` must be `source`.

Optimization and debug levels are required integers in 0..2. `budgetMs` defaults to
120000 and accepts integers in 1..2147483647. It applies to each decompilation through
the existing cooperative deadline checks. Compilation, JSON parsing, and output
serialization are outside that deadline.

`reportProgress` is an optional boolean, defaulting to `false`. When `true`, the
worker emits and flushes one JSONL event on **stderr** before compiling/decompiling
each selected input/profile. Read this control channel independently of stdout:

```json
{"event":"record-start","requestId":"batch-1","profileId":"o0d1_base","id":"source-sha256","path":"F:/virtual/source.lua"}
```

Callers can retain this identity alongside the complete request manifest to
identify an input interrupted by a process exit or resource limit. A start event
does not imply success. Result and completion events retain their existing
fields/counts on stdout. Requests omitting the flag, or setting it to `false`, emit
only the existing events. Invalid flag types produce an error on stdout before
any start event. Start events are independent of stdout record buffering.

Profile `flags` default to an empty array. Supported flags match the file CLI:
`--ast`, `--cfg`, `--notes`, `--recover-inline`, `--no-comments`, `--no-names`,
`--types`, `--roblox-types`, `--no-ir`. Roblox type profiles normally combine
`--types` with `--roblox-types`. Unknown flags, unknown fields, wrong types, and
out-of-range parameters produce an error rather than being ignored or clamped.

Omitted `inputIds` selects all inputs. An empty array selects none. IDs must exist
and may occur only once in a selection. Profiles run in request order; each selected
input group runs in lexicographic `path` order, matching the file CLI's ordering for
the same path labels. Anonymous-name RNG resets at each profile to match a fresh
CLI process and prevent earlier requests or profiles from changing those names.

Each result retains the native record fields and adds `event: "record"`,
`requestId`, `id`, `profileId`, and `compileReused`. Complete `input` and `path`
echoes are retained even on compilation failure. Compilation failures also carry
levels, bytecode size, empty output, and timings. Successful siblings remain usable.

Compiled bytecode is shared only for identical complete source strings and identical
O/D options within one request. Each record gets a fresh decompiler. `compileSeconds`
measures actual compilation work and is zero on cache reuse; `decompileSeconds` and
`totalSeconds` measure that record's work. No cache survives a request.

```json
{"event":"complete","requestId":"batch-1","records":2,"failures":0}
{"event":"error","requestId":"bad-request","error":"unsupported profile flag: --unknown"}
```

The worker validates the entire request before emitting records. Invalid requests
emit one terminal error, with `requestId` when it was available as a parsed string,
then accept the next line. Compile/decompile failures count as records and contribute
to `failures`; they do not end the worker. Ordinary worker execution emits no stderr
progress chatter. Existing recovered assertion diagnostics can still write stderr.
Stdout contains only JSONL events. Output flushes after 32 records or 64 KiB,
whichever comes first, and after every terminal event.

`tools/check_worker.py` runs focused executable/protocol checks and benchmarks, without
unit-test suites. It compares all 38 corpus profiles against the same executable's
file CLI, checks the protected legacy binary separately for build drift, and uses
32 complete cached corpus sources for repeated throughput measurements.

```text
python Fission.Batch/tools/check_worker.py --worker <worker.exe> --legacy <protected-batch.exe> --corpus <native-input-cache-directory> --out <evidence.json>
```
