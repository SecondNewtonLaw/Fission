# Scoped mimalloc integration, 2026-10-05

No prior mimalloc integration found. Added optional static mimalloc v3.5.4, pinned commit `f8401befa675adb13b98decababd7fdc59572477`.

## Scope and switch

`USE_MIMALLOC` defaults OFF; configure with `-DUSE_MIMALLOC=ON` to enable. ASan disables this dependent option. `Fission.Common` publicly links `mimalloc-static` and exports `FISSION_USE_MIMALLOC` to Fission/downstream targets. Compile-command audit: zero upstream translation units receive this definition.

`Fission.Common/include/FissionAllocator.hpp` provides `Fission::MakeShared<T>` and `Fission::Allocator<T>`. Current source uses the factory at 432 shared-object allocation sites in 21 Fission.Decompiler files. Enabled mode routes those objects and their control blocks through mimalloc; disabled mode uses original `std::make_shared`. Ordinary internal STL containers retain their existing allocators unless explicitly using `Fission::Allocator`.

`MI_OVERRIDE=OFF`: no global malloc/free/new/delete interception. Upstream Luau, Boost and CRT allocation behavior remains unchanged. Downstream code can opt in to the same factory/allocator.

Static build only; no allocator DLL. Tests/object/shared builds and statistics disabled. Upstream profiling capability kept at its supported default after `MI_PROFILE=OFF` exposed an upstream pprof fallback signature compile error; upstream source untouched. Runtime profiling remains inactive unless enabled by mimalloc's environment options.

## Measured allocator comparison

Five warmed alternating pairs per set, same source code and build settings, scoped factory OFF versus ON, 1 GiB native memory cap and 120-second watchdog. Exact outputs match. Measurement JSON records immutable executable snapshots.

| Set | Native-time change with mimalloc |
|---|---:|
| 32-source corpus | 2.7724% slower |
| Recent set 0 | 2.0120% faster |
| Recent set 1 | 3.1860% faster |
| Recent set 2 | 0.3236% slower |

Corpus process CPU also 1.4706% slower. Results mixed, so default remains OFF. This is an available scoped allocator choice, not a demonstrated universal improvement.

## Final builds and checks

Enabled artifacts in `cmake-build-mimalloc-20261005`:

- Worker SHA-256 `b1d677cb352bcf4a0787b84b0c4e3034536ed223865f2ed84f2f459d9bbf251f`.
- Batch SHA-256 `4866234442c6439d3b280b92309624aa7be7c1083af9a842c8d87f980d8fa273`.
- Batch, Worker and CLI all built successfully after deferred formatting.

Final replay: 2,204 records, zero failures, same exact fingerprint as standard mode and preceding baseline; peak private bytes 329,945,088. CLI stress 22/22. Replay validation wall times are not paired performance evidence.

`check-scoped-mimalloc.cpp` assertion probe passed against final static library: Fission shared object in mimalloc heap, 128-byte alignment correct, ordinary `std::make_shared` and `malloc` outside mimalloc heap, downstream explicit-allocator vector inside mimalloc heap, weak-pointer lifetime preserved.

Standard-mode final artifacts available separately in `cmake-build-tags-20261005`; existing protected executables unchanged. No upstream source patch, global allocator hook, commit or publish.
