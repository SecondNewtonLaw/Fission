# Fission.BinaryNinja

A native Binary Ninja architecture + BinaryView plugin that loads compiled Luau
bytecode, disassembles it with proper Luau mnemonics, and lifts the Fission IR to
Binary Ninja's Low Level IL (BNIL). Binary Ninja then produces MLIL and HLIL from
that IL like it would for any native architecture.

## What it does

- Recognises vanilla Luau bytecode files (versions accepted by the Fission
  deserializer).
- Provides a **Pseudo Luau** language representation in the view drop-down. For
  Luau bytecode it renders each proto's own Fission decompilation; for any other
  architecture it maps the function's HLIL to Luau-shaped syntax.
- Adds `Fission > Show Pseudo-Luau (Whole Chunk)`, `(Function)` and `(Address)`,
  which run Fission's own decompiler and open the requested scope in a report
  tab.
- Maps the file into a synthetic address space, keeping instruction arrays in
  executable segments and metadata/string tables in readable data segments.
- Creates a Binary Ninja function for every Luau proto and names it from the
  proto's debug name (falling back to `proto_<id>` / `main`).
- Defines data variables for interned strings so pointer operands resolve to
  real data.
- Lifts each Luau instruction from the Fission IR stream:
  - arithmetic becomes real LLIL float operations, and numeric intrinsics
    (`%`, `//`, `^`) declare `double` inputs/outputs so Binary Ninja renders
    their constants as numbers instead of raw 64-bit bit patterns;
  - branches become BNIL `if`/`goto` with correct Luau `D`-relative targets;
  - numeric `for` loops become float compares and back edges;
  - generic `for` loops call the iterator through a named intrinsic;
  - `CALL`/`NAMECALL` become real BNIL `call` instructions with a fixed
    argument/return ABI (`a0..a15` in, `v0..v15` out), so HLIL shows
    `sub_xxx(arg0, arg1)` and direct closure calls become call-graph edges;
  - globals, imports and upvalues are loads/stores from named synthetic data
    slots (`luau_globals.pcall`, `luau_globals.coroutine.create`,
    `upvalues_<proto>.upval_<n>`);
  - tables are pointer objects with stable field offsets, so `t.x` becomes a
    load/store instead of an opaque intrinsic;
  - closures become function pointers and captures write to their child proto's
    upvalue slot;
  - upvalue slots with numeric-only uses are typed `double`, while slots used as
    tables, closures, or call targets stay raw 8-byte values;
  - `pcall`/`xpcall`/`coroutine.resume` lower to semantic intrinsics whose
    outputs are named `ok` and `result`; `coroutine.create` lowers to an
    intrinsic whose output is named `thread`, porting Fission's call-result
    renaming into the architecture's result registers;
  - call-result naming follows Fission's rewriters: child lookups
    (`GetService`, `FindFirstChild`, `WaitForChild`), constructors
    (`Instance.new`, `Type.new`, `Type.from*`), getters (`GetX`), attributes
    (`GetAttribute`), `require` path leaves, property reads, and `#length`
    results (`count`);
  - Binary Ninja has no HLIL `try`/`catch`, so protected calls stay builtin
    intrinsics with semantic result names rather than a try/catch statement.
- Proto instruction arrays stay at their file offsets. Luau arrays can start at
  arbitrary byte offsets, so `GetInstructionAlignment()` is 1 and Binary Ninja
  accepts the exact unaligned function starts instead of shifting them.

### Synthetic environment layout

The loader builds four named memory regions instead of scattering anonymous
slots through the file image:

| Section | Permissions | Contents |
| --- | --- | --- |
| `.luau_functions` | read-only data | `LuauFunctionTable` — one named function-pointer field per proto |
| `.luau_globals` | read/write `.data` | `LuauGlobals` — one named field per global/import (`pcall`, `coroutine.create`, …); library paths keep their dots (`math.floor`) |
| `.luau_upvalues` | read/write `.data` | one `LuauUpvalues_<proto>` struct per proto, one `upval_<n>` field per upvalue; packed to exactly `nups * 8` bytes |
| `.luau_tables` | read/write `.data` | one `LuauTable` struct per `NEWTABLE`/`DUPTABLE` site, with named field offsets |

`GETGLOBAL`/`GETIMPORT` read `luau_globals.<name>`, `GETUPVAL` reads
`upvalues_<proto>.upval_<n>`, `SETUPVAL`/`CAPTURE` write the same field, and
table accesses use stable field offsets inside the table object. The function
table is a read-only representation of the proto table; closure creation still
emits a direct `ConstPointer` to the child proto so call-graph edges and
indirect values stay precise.

## Requirements

- Binary Ninja 6.0 (the API revision in this repo matches
  `binaryninja-api@775c3b5eed8598217b20e1087afe7aa092a9b4b1`).
- A matching `binaryninja-api` checkout. Point `BN_API_PATH` at it. The checkout
  must include the `vendor/fmt` submodule; the GitHub source tarball does not.
- MSVC/clang-cl release build (`/MD`). The plugin links the Fission static
  libraries, so build the whole tree with the same configuration.
- `USE_MIMALLOC=OFF` is recommended when building the plugin.

## Build

```powershell
cmake -S . -B cmake-build-release -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DBUILD_BINARY_NINJA_PLUGIN=ON `
  -DBN_API_PATH="C:\path\to\binaryninja-api" `
  -DBN_INSTALL_DIR="C:\Users\<you>\AppData\Local\Programs\Vector35\BinaryNinja"

cmake --build cmake-build-release --target Fission.BinaryNinja Fission.LuauCompile Fission.LuauRun Fission.BinaryNinja.Verify
cmake --build cmake-build-release --target Fission.BinaryNinja.Check
```

The post-build step copies `Fission.BinaryNinja.dll` into the Binary Ninja user
plugin directory (`%APPDATA%\Binary Ninja\plugins` on Windows). Restart Binary
Ninja after installing or updating a native plugin.

## Trying it

Compile a Luau source file to bytecode:

```powershell
cmake-build-release/out/bin/Fission.LuauCompile Fission.BinaryNinja/samples/kitchen_sink.luau kitchen_sink.luac -O2 -g2
cmake-build-release/out/bin/Fission.LuauRun kitchen_sink.luac
```

Open `kitchen_sink.luac` in Binary Ninja. It is recognised as `Luau Bytecode`,
every proto is listed as a function, and the HLIL view shows the register-based
VM code with loop structure and calls.

Ready-made fixtures are checked in next to the sample sources:

| File | Size | Protos | Instructions | Purpose |
| --- | ---: | ---: | ---: | --- |
| `samples/kitchen_sink.luac` | 7.7 KB | 21 | 932 | Feature-dense language coverage at `-O2` |
| `samples/kitchen_sink_O0.luac` | 8.5 KB | 21 | 1,112 | Same program at `-O0` (different lowering shapes) |
| `samples/stress.luac` | 111 KB | 521 | 13,461 | 240 generated functions, loops, closures, metatables, coroutines at `-O2` |
| `samples/stress_O0.luac` | 121 KB | 521 | 15,825 | Same stress program at `-O0` |
| `samples/renamers.luac` | 952 B | 3 | 85 | GetService/constructor/getter/attribute/require/property and pcall naming fixture |

Regenerate the stress source and re-verify:

```powershell
python Fission.BinaryNinja/samples/generate_stress.py
cmake-build-release/out/bin/Fission.LuauCompile Fission.BinaryNinja/samples/stress.luau Fission.BinaryNinja/samples/stress.luac -O2 -g2
cmake-build-release/out/bin/Fission.BinaryNinja.Verify Fission.BinaryNinja/samples/stress.luac
cmake-build-release/out/bin/Fission.LuauRun Fission.BinaryNinja/samples/stress.luac
```

`Fission.BinaryNinja.Verify` is a Binary-Ninja-independent check of the
deserializer/lifter/image layer: it asserts that every proto has lifted IR, that
the lifted stream is 1:1 with the raw instruction slots, and that every jump
target is inside the owning proto.

Inside Binary Ninja, `tests/smoke_test.py` opens `stress.luac`, checks the view
and architecture names, and forces LLIL/MLIL/HLIL for all 521 functions.

## Pseudo Luau view

The view selector offers `Pseudo Luau` for every open file:

- `LuauBytecode`: each function renders that proto's own Fission
  decompilation. Nested protos remain addressable as their own functions; the
  parent's source keeps the closure literal the compiler emitted.
- anything else: the mapper walks the function's HLIL and prints Luau-shaped
  text. Tokens are typed (keywords, variables, fields, numbers, strings,
  operators, braces), so Binary Ninja's syntax colors apply. Every
  `BNHighLevelILOperation` is handled: `HLIL_SWITCH` becomes an
  `if`/`elseif` chain, SSA-only operations map to their non-SSA equivalents,
  and float, bit-manipulation, struct-initialiser and comparison operations
  have readable forms.

Statement indentation is emitted as literal spaces, so the rendered layout does
not depend on how Binary Ninja groups token lines.

## Layout

| Path | Purpose |
| --- | --- |
| `include/Image.hpp`, `src/Image.cpp` | Deserializes the file with Fission, lifts the IR, assigns per-image base addresses, and indexes protos |
| `include/Architecture.hpp`, `src/Architecture.cpp` | Luau architecture: instruction info/text and IR -> BNIL lowering |
| `include/View.hpp`, `src/View.cpp` | BinaryView type: detection, segment layout, symbols, data variables, functions |
| `include/Intrinsics.hpp`, `src/Intrinsics.cpp` | Remaining VM-level intrinsics (concat, length, mod, pow, vararg, class ops) |
| `src/Plugin.cpp` | Core plugin registration |
| `tools/LuauCompile.cpp` | Source -> bytecode compiler used for samples/tests |
| `tools/LuauRun.cpp` | Bytecode runner used as an execution oracle |
| `tools/Verify.cpp` | Binary-Ninja-independent image/lifter verifier (`Fission.BinaryNinja.Check`) |
| `tools/DumpProto.cpp` | Per-proto decompilation probe, useful when auditing function-level output |
| `samples/kitchen_sink.luau` | Feature-dense, deterministic sample |

## Notes and limitations

- Roblox-encoded bytecode is not enabled yet; only vanilla Luau bytecode is
  auto-detected.
- The IL models Luau values as eight-byte words. `nil` and `false` share the
  zero value, which is sufficient for control flow but not a bit-accurate value
  representation.
- Globals/upvalues/tables use one shared synthetic object per slot/site, so
  multiple closure instances of the same proto share upvalue storage and table
  field offsets are layout hints rather than recovered types.
- `CONCAT`, `LENGTH`, `MOD`, `POW`, `IDIV`, varargs and class operations remain
  intrinsics because LLIL has no exact equivalent for them.
- The entry prologue copies argument registers into the VM registers. When a
  branch targets instruction 0 (a loop header at function entry), the prologue
  is omitted so a back edge cannot reset the parameters; in that shape the
  parameters are not materialized into `R0..` for analysis.
- The architecture has no assembly support; patching bytecode from the UI is not
  implemented.

