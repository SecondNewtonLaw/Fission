# CHECK_PENDING

Independent Luau compiler (lowering) ↔ Fission (lifting) comparison, started 2026-09-25 against Luau 0.738 (`F:/cpm_cache/luau/1735`) and `dev` @ `e1467b58`. Each entry lists the compiler path, the Fission stage that goes wrong, a reduced source, and the observed evidence. Items stay here until someone confirms/fixes them.

Evidence tool: `cmake-build-fuzz-current/Fission.Fuzzing.exe --sem-file <src> --opt N [--debug N]` (VM trace parity) and `Fission.CLI.exe --decompile-test <src> --opt N` (output/IR). Failing matrices live in `Agents/CheckPendingProbes/`, and each prints one line per form, so the diverging form numbers are the differing trace lines.

Summary: the opcode decode layer matches `Bytecode.h`. Expression, assignment, call, table, loop, closure and constant lowering all round-trip under the VM oracle. Nine semantic defects were found; all are resolved (see Resolutions):

| # | Area | Scope | Status |
|---|---|---|---|
| P1 | ASTLifter structuring: shared value arm into a single-value `return` | O0–O2. Hits everyday idioms: `return x ~= nil and x or d`, `return a and (b or c) or d`, `return (a or b) and c` | **resolved** |
| P2 | ASTLifter structuring: loop-body tail after a nested `break` next to an `… continue` arm | O0–O2 | **resolved** |
| P3 | ASTLifter declarations: O2 move-elided inline parameter redeclares a loop-carried local | O2, debug 2 | **resolved** |
| P4 | Naming: `arg{N}` parameters capture same-named globals | all levels | **resolved** |
| P5 | Naming: `_v{N}` collision prefix drops `local` and writes a global | O0–O2 | **resolved** |
| P6 | `OptimizeIR`: constant propagation across closure writes to a captured local | O0/O1 with `--optimize-ir` | **resolved** |
| P7 | Classes: self-reference (debug 0) and forward reference to a later class (all levels) | experimental syntax | **resolved** |
| P8 | `AutoNameVariables`: `FindFirstChild`/`GetService`-derived names shadow globals (`local print = …`) | CLI/Server default naming | **resolved** |
| P9 | Vanilla entry points emit Roblox-only `Vector3.new` for vector constants | stock Luau VMs | **resolved** |

P1 and P2 shared the silent "edge into an already-lifted block → drop" fallback. P6, P8 and P9 were invisible to the existing oracle, which always decompiles with flags `0` and installs a fake `Vector3`.

## Resolutions (2026-09-25)

Every item has a red→green regression in `Fission.Tests/Decompiler/CheckPendingRegressions.cpp` (O0/O1/O2, plus debug 0/2 where relevant). Before the fixes all nine failed. After them, and the naturalness/minor fixes below, the full suite passes 663/663. `Samples/StressTests/run_all.ps1` passes 22/22, and the saved 2,968-source replay still has 560 semantic passes, 2,408 unchecked and no divergence. Replayed sources whose recompiled bytecode differs from the original's opcode sequence dropped from 546 to 4. Executable probe matrices match under flags `0`, `AutoNameVariables` and `--optimize-ir`; export probes are VM-oracle UNRUNNABLE.

- **P1** — `ASTLifter::LiftControlFlow`: a return-only stop block that carries a phi is now the join of an inner test when the other arm reaches it, and it is lifted once after the `if` rather than copied into each arm. `HoistPhiLocals` also removes a nested diamond's value-less `local <name>` from an arm, because the outer merge owns that binding. `IfExpressionFolder` now re-folds once the arms have folded, simplifies `if c then x or y else y` → `c and x or y` and `if c then y else x and y` → `(c or x) and y` (both exact), and turns `local v = <and/or/if-expr>; return v` into `return <expr>`. Output is now the source idiom, e.g. `return (arg0 or arg1) and arg2`.
- **P2** — `FindMergeBlock` has a `loopJumpsExit` retry mode. In it, an unconditional `Continue` block and a statement `JUMP` into a shared return-only block count as leaving the region. The retry runs when the normal merge is missing, is the innermost latch, or is a jumped-to shared return; in the last case the join must be reachable from both arms, so `elseif` chains are unaffected. Merges past the region stop fall back to the stop when only one arm reaches it. Arms without a join inside a loop now walk to the enclosing stop, so latch edges still lift as `continue`. This also fixes O2-unrolled loops with `break`/`continue`.
- **P3** — `ASTLifter::BeginsDebugLocal` no longer treats a write as declaring a debug local that starts next, if another debug local already owns that register across the write.
- **P4** — `AnalyzedFunction::PopulateNames` `_`-prefixes a generated `arg{N}` that matches a global read in the function tree. `IsParameterNameShaped` feeds `globalAutoNameCollisions`.
- **P5** — `ASTLifter::Lift` declares any `_`-prefixed collision name at function top when it is assigned but never declared in that function (`ScanBinding`).
- **P6** — `ConstantPropagation::RunOne` treats every SSA definition of a register captured by reference (`CAPTURE` mode 1) as overdefined.
- **P7** — `HoistedClassName` recognises a class's preallocation `LOADNIL` and its `NEWCLASS`. The `LOADNIL` is no longer emitted (the `class` statement binds the name for the block), and reference captures of either version take the class name when no debug names exist.
- **P8** — `RobloxTypeInferer::Infer` adds every global identifier read in the function tree (`ScopeAwareRenamer::CollectGlobalNames`) to its taken-name set before generating names.
- **P9** — `SourceGenerator::vectorConstructor` is `Vector3.new` for `RobloxClientDecoder` input and `vector.create` otherwise. `vector.create` is valid in both stock Luau and Roblox. Five existing expectations that fed Roblox-profile bytecode through the identity-decoder entry now expect `vector.create`.

## Confirmed semantic divergences

### P1 — `return (x or y) and z` drops the shared `z` arm on the `x` falsy / `y` truthy path (O0/O1/O2)

Compiler: `compileExprAndOr` → `compileConditionValue` for the `or` operand of an `and`. The `or` left test jumps straight to the `z` load when `x` is truthy; when `x` is falsy it loads `y` into the target and branches on it, falling through into the same `z` load when `y` is truthy (`Compiler.cpp` `compileExprAndOr`/`compileConditionValue`).

Bytecode/CFG (`--opt 1`), function `c`:

```
B0: JUMPIF R0 -> B2          ; x truthy
B1: MOVE R3, R1 ; JUMPIFNOT R3 -> B3   ; y falsy -> return y
B2: MOVE R3, R2              ; preds {B0, B1}
B3: RETURN R3
```

Fission output:

```lua
local v3
if not arg0 then
    v3 = arg1
    if not v3 then
        return v3
    end
else
    v3 = arg2
    return v3
end
return v3        -- reached with v3 = y, never assigns z
```

Repro: `local function c(x, y, z) return (x or y) and z end print(c(false, true, 0))` → original `0`, decompiled `true`. `--sem-file` DIVERGE at O0 and O1 (O2 constant-folds the call so it matches). First wrong stage: ASTLifter structuring — `B2` has two predecessors but is lifted only as the `else` arm of `B0`; the `B1` fall-through edge into `B2` is lost. Expected: `return (arg0 or arg1) and arg2`.

Neighbour sweep (all inputs from `{false, true, nil, 0}`), O0 and O1 both diverge for:
- `local v = (x or y) and z; return v`
- `return (x or y) and (z or w)`
- `return (x or y or z) and w`
- `return (x == 1 or y) and z`

Same sweep matches for `if (x or y) and z then`, `print("k", (x or y) and z)` (call argument), `(x or y) and z or "fb"`, `not ((x or y) and z)`, `(x and y) or z`, `(x and y) and z`, `x or (y and z)`. So the defect is specific to an `or`-chain on the left of `and` when the result lands in a local/return register.

Wider matrix (`Agents/CheckPendingProbes/p20_andor_matrix.lua`: 40 forms stored in a table so O2 cannot inline/fold them, 64 input tuples each) diverges at O0/O1/O2 for these direct-`return` forms:

| # | form |
|---|---|
| 1 | `(a or b) and c` |
| 4 | `(a or b) and (c and d)` |
| 7 | `a and (b or c) and d` |
| 9 | `(a or (b and c)) and d` |
| 12 | `(not a or b) and c` |
| 13 | `(a or not b) and c` |
| 14 | `(a == b or c) and d` |
| 15 | `(a ~= nil or b) and c` |
| 16 | `(a and b == c) or d` (LOADB-before-compare shape from `compileConditionValue`) |
| 33 | `return (a or b) and c, (a or c) and d` |
| 34 | `((a or b) or c) and d` |
| 35 | `(a or (b or c)) and d` |
| 38 | `(a or 1) and b` (O0 only; O1+ folds) |
| 39 | `(a or b) and 5` |

A second matrix (`Agents/CheckPendingProbes/p36_valmix.lua`) shows the same failure for common idioms (O0/O1/O2 unless noted):
- `return a ~= b and c or d`: the ternary idiom with a comparison head, e.g. `return x ~= nil and x or default`
- `return a and (b or c) or d`
- `return (not a) and (not b) or c`
- `return ((a or b) and c or d) and a`
- `return if a then (b or c) and d else b` (O0 only)

`a and b or c` with a plain-register head, `a == b or c == d`, `a == b and c == d`, `if a or b then c else d` and `(a == 1 or b == 2) and (c == 3 or d)` all match. Fission output for `a ~= b and c or d`:

```lua
local v4
if arg0 ~= arg1 then
    v4 = arg2
    if v4 then return v4 end
else
    v4 = arg3
    return v4
end
return v4      -- falsy c returns c instead of d
```

A context sweep of `a ~= b and c or d` covered `local v = …` followed by a call, a call argument, a table constructor, a global/upvalue write, a multi-assignment, a loop body, `return L(…)` and `return (…), 1`. All of these match. Only the single-value `return <expr>` fails, and it still fails when nested (`if a then return a ~= b and c or d end`).

The same expressions match when the value is stored somewhere other than straight into the returned register: `local t = {(a or b) and c}`, `t.k = ...`, global/upvalue/parameter assignment, call argument, `if`/`while` condition, and `local v = ...; v = v or d; return v`. All failing outputs have the identical shape (form 16 shown):

```lua
local v4
if arg0 then
    v4 = true
    if arg1 == arg2 then return v4 end
else
    v4 = arg3
    return v4
end
return v4   -- the a-truthy / b~=c path returns `true` instead of `d`
```

Likely location: `ASTLifter::LiftControlFlow`, the visited-block branch at `ASTLifter.cpp:1158-1206`. The inner test's fall-through reaches the pure `Z` block after the outer `else` already visited it. Here `canDup` is false (not the entry block), `IsDuplicablePureRegion` needs an `IfHeader`, and the `SharedTailRegion` case needs an effect in the region or in `walked`. That pure-only case expects a short-circuit value folder to have consumed the shape earlier, and it hasn't when the merge block is the `RETURN`, so the `else { break; }` at 1203 silently drops the edge.

### P2 — `elseif … continue` next to an arm with a nested `break` loses the shared loop-body tail (O0/O1/O2)

Compiler: `compileStatIf` with `alwaysTerminates(thenbody) == false` emits `JUMP endLabel` after the `then` arm. At O0 `foldJumps` does not thread it, so the `print("a")` block ends in its own `JUMP` to the tail. The `elseif b then …; continue` arm jumps to the loop's `contLabel` (`Compiler.cpp:3811-3823`, `4806-4824`).

Repro (`--opt 0`):

```lua
local function f(a, b)
    local i = 0
    while i < 3 do
        i += 1
        if a then
            if b then print("ab") break end
            print("a")
        elseif b then
            print("b")
            continue
        end
        print("tail" .. i)
    end
    return i
end
print(f(true, false))
```

CFG is correct: `B7` (after `print("a")`) → `B11` (tail), `B8` (`elseif b`) false → `B11`, `B10` = `Continue` → latch. Fission output:

```lua
if arg0 then
    if arg1 then print("ab") break else print("a") end
elseif arg1 then
    print("b")
else
    print("tail" .. v2)          -- tail moved into a synthetic else; B7 -> B11 edge dropped
end
```

`f(true, false)` prints `a tail1 a tail2 a tail3` originally and only `a a a` after decompilation. With `print` as the effect, O1/O2 match because jump threading changes the shape. Same result for the `for i = 1, 3` version, and with `if b then … break else print("a") end` as the first arm. Without the nested `break`, or without the `continue`, it matches.

`Agents/CheckPendingProbes/p25_arms.lua` swaps `print` for calls to a local logging closure. There it diverges at **O0, O1 and O2**, at debug levels 0 and 2 alike, for:
- numeric `for`, `while`, `repeat … until`, and generic `for` bodies of the shape `if a then if b then … break end …; elseif b then …; continue end; tail`
- the variant with the effect before the nested `if b then break end`

`Agents/CheckPendingProbes/p28_arms2.lua` (18 forms, inputs from `{false, true, nil}`³) extends this. It also diverges at O0/O1/O2 for:
- `… elseif b then … else …; continue end` (the `continue` in the `else`)
- `… elseif b then …; continue else … end`
- a first arm that is only `if c then …; break end` with no statement after it
- `if c then …; break else … end` followed by `elseif b then …; continue elseif c then …; break end`
- the `while true do … if i > 3 then break end …` spelling

A pure-arithmetic version (`r += 1` / `r += 10; continue` / `r += 100`, no calls) diverges at O0 and O2 but not O1.

The same matrix matches when the nested terminator is `return` or `continue` instead of `break`, when the `elseif` arm ends in `break`/`return` instead of `continue`, with a plain `else … continue`, and with an extra `elseif … continue` arm.

This looks like the same silent-drop fallback as P1: `B11` is lifted as the `elseif` else-arm first, and when the `then` arm's walk reaches it, the visited-block branch in `LiftControlFlow` (`ASTLifter.cpp:1158-1206`) exits through `else { break; }`. Neither P1 nor P2 raises an error or debug note when an edge into an already-lifted block is dropped. A diagnostic there would have surfaced both.

### P3 — O2 inlined argument aliasing a loop-carried local redeclares it inside the loop (debug level 2 only)

Compiler: `compileInlinedCall` with `LuauCompileMoveElision` (`Compiler.cpp:1210-1221`, `compileExprAutoTemp`). The argument of an inlined local function is a *written* local, so it cannot take the "reuse register" path at 1200. `compileExprAutoTemp` still returns the local's own register instead of copying it. The inlined parameter's debug local therefore lives in the same register as the caller's local, with `startpc` equal to the pc right after the caller's last write (`x -= 4`).

Repro (`--opt 2`, default debug level 2; `--debug 0/1` match):

```lua
local out = {}
local function L(s) out[#out + 1] = s end
local function f(b) local x = 1 if b then x = x + 10 end while x > 5 do x -= 4 L(x) end return x end
print(f(true), f(false), table.concat(out, ","))
```

Fission output (both the `f` body and the call sites inlined into the main chunk):

```lua
while 5 < v1_1 do
    local v1_1 = v1_1 - 4     -- new local shadows the loop-carried one: loop never terminates
    out[(#out + 1)] = v1_1
end
```

VM trace: decompiled program hits the harness timeout (status 2). `ASTLifter::BeginsDebugLocal` (`ASTLifterInlining.cpp:133-141`) treats any debug local that starts at the next pc on the target register as a declaration by the current instruction (`DeclaresLocal`, `:143-146`). That assumption does not hold for move-elided inline parameters, which alias an existing local's register without a defining instruction. The repro is saved as `Agents/CheckPendingProbes/p3_inline_alias_loop.lua`. It first showed up in a larger CFG matrix, where every later form then failed with the same timeout.

Shape sweep at O2, with `L(s)` inlined and `--debug 0` vs `--debug 2`. Debug 0 always matches. Debug 2 diverges for:
- `while x > 5 do x -= 4 L(x) end` (also with `local x = c`, and with `L(x) L(x)`)
- `repeat x -= 4 L(x) until x <= 5`

Debug 2 matches when the write follows the call (`while … do L(x) x -= 4 end`), inside a numeric `for` body, and inside an `if` arm. Roblox bytecode is usually shipped at debug level 1 (no locals), so this mostly affects `DecompileTestCode`/server users and non-stripped bytecode.

### P4 — auto-named parameters `arg{N}` capture same-named globals (all O/debug levels)

Compiler: a global read is `GETIMPORT`/`GETGLOBAL` by name; parameters are registers `0..numparams-1` with no name at debug < 2. Fission always names parameters `arg{N}` (`ControlFlowAnalyzer.hpp:370-372`, via `SetGlobalName`), even at debug level 2. The collision guard `globalAutoNameCollisions` only collects globals matching `IsAutoNameShaped` (`v\d+(_\d+)*`, `:228-243`, `:332-358`), and `GetVarName` only `_`-prefixes those (`:298-306`).

```lua
arg0 = "GA0"
local function p1(a, b) return a, b, arg0 end
print(p1(1, 2))            -- original: 1 2 GA0
```

Fission emits `local function p1(arg0, arg1) return arg0, arg1, arg0 end`, which prints `1 2 1`. Diverges at every O0/O1/O2 × debug 0/1/2 combination (`Agents/CheckPendingProbes/p4_param_global.lua`). Loop variables (`i/j/k/v/k2/v2`) and `uv_N` do avoid referenced globals: the loop is emitted as `for k3, v3 in …` when `k`, `v`, `k2`, `v2` are read. Only parameter names are unguarded.

### P5 — `_`-prefixed collision rename drops the `local` keyword and writes a global

When an auto name `v{N}` collides with a source global, `GetVarName` renames it to `_v{N}` (`ControlFlowAnalyzer.hpp:300-306`). In the reduced case below, the first write of that version is emitted as a plain assignment, so the decompiled program creates a new global:

```lua
v0, v1, v2 = 1, 2, 3
local function g(x) local y = x .. "!" return y end
print(g("a"), v0, v1, v2)
do local m = { 1 } end
print(rawget(_G, "_v0"), rawget(_G, "_v1"), rawget(_G, "_v2"))   -- original: nil nil nil
```

Fission output (O0 and O1):

```lua
local v1_2 = print
v1_2(g("a"), v0, v1, v2)
_v1 = { 1 }          -- no `local`: global _v1 created
_v1 = print          -- the later `print` cache also lands in the global
```

The decompiled program reports `_v1` as a function instead of `nil`. With `do local math = {floor = …} end` after globals `v0…v6` and `arg0`…, `rawget(_G, "_v2")` becomes the table (original `nil`) at O0/O1. The same register had an earlier version declared under a different name (`v1_2`). The declaration decision (`m_definedRegisters`, per register) and the per-version name (`_v1`) disagree, so the new name never gets a `local`. Without a colliding global (the name stays `v1`) the declaration is emitted correctly.

### P6 — `OptimizeIR` constant-propagates locals that closures mutate

`DecompilerFlags::OptimizeIR` (CLI `--optimize-ir`, Server `optimizeIR`) runs `Fission::ConstantPropagation` to a fixpoint (`Decompiler.cpp:809-817`). `Analysis/ConstantPropagation.hpp` never mentions `CAPTURE` or upvalues, so a local captured by reference (`CAPTURE REF`) is treated as a plain SSA constant across calls that write it through `SETUPVAL`.

```lua
local a = 0
local function set() a = 5 end
set()
print(a)          -- original 5
```

With `--optimize-ir` at O0/O1 (debug 0 and 2) the output is `…; anon_41_0(); print(0)`; without the flag it is correct. At O2 the call is inlined, so it matches. Other hits: `local a = 1 … set(5) return get(), a` returns `1` instead of `5`, and `for i = 1, 3 do fs[i] = function() cnt += i end end … print(cnt)` prints `0` instead of `6` (O0/O1/O2). The VM oracle (`--sem-file`) and the unit tests decompile with flags `0` and never enable `OptimizeIR`, so this pass has no semantic coverage.

### P7 — user-defined classes: self-reference and forward reference (`DebugLuauUserDefinedClasses`)

Compiler: `compileClassDeclaration` (`Compiler.cpp:1709-1831`). Class names are pre-allocated block locals (`classLocals`), so a method can reference its own class, or a class declared later in the block, through an upvalue of the same local.

Checked with a temporary Catch2 probe using the same compile → decompile → recompile → `RunLuauTrace` harness as `ClassDeclarations.cpp` "NEWCLASS preserves table exports…", O0/O1/O2 × debug 0/2:

| Source | Result |
|---|---|
| `class C function make(self) return C.new({}) end function tag(self) return 'c' end end return C.new({}):make():tag()` | debug 0: diverges at every O. Output uses an undefined global `upv0_2_2` for `C` (`return upv0_2_2.new({})`, top-level `upv0_2_2.new({})`) → `attempt to index nil with 'new'`. Debug 2 matches. |
| `class C function make(self) return C end end return C.new({}):make() == C` | same: debug 0 fails at O0/O1/O2 |
| `class A function f(self) return B end end class B function g(self) return 'b' end end return A.new({}):f().new({}):g()` | diverges at **all** O × debug. Output: `local B = nil` then `class A … return B … end` then `class B … end`. The `class B` declaration binds a *new* local, so `A.f` captures the `nil` forward declaration. |

Passing class shapes: fields filled via `new({...})`, a three-level `open class`/`extends` chain, vararg methods, methods mutating a captured upvalue, a class stored in a table.

### P8 — `AutoNameVariables` names locals after child/service strings and shadows globals used in scope

With `AutoNameVariables` (CLI `--decompile-test` and default mode, Server option), `RobloxTypeInferer::Infer(…, autoNameVariables)` (`Decompiler.cpp:877-880`) and the renamer passes name locals from `GetService`/`FindFirstChild`/`WaitForChild` string arguments. Nothing checks those names against globals, builtins or upvalues that are read in the local's scope. Flags-0 decompilation of the same inputs matches, which is why the fuzzer never sees it.

```lua
local folder = { FindFirstChild = function(self, c) return { Name = c } end, WaitForChild = … }
local function f()
    local a = folder:FindFirstChild("script")
    local b = folder:WaitForChild("print")
    print(a.Name, b.Name, script.Name)
end
```

Output:

```lua
local script = v0:FindFirstChild("script")
local print = v0:WaitForChild("print")
print(script.Name, print.Name, …, script.Name, …)   -- calls the table: "attempt to call a table value"
```

Also at O2: `local tostring = v0:FindFirstChild("tostring") return tostring.Name, tostring(1)`. The service form hits the same rename: `local p = game:GetService("Players")` becomes `local Players = …`, which captures a global `Players` read later in the function (`Agents/CheckPendingProbes/p8_autoname_global_shadow.lua`; diverges O0/O1/O2, debug 0/2). In real Roblox code, children named `script`, `workspace`, `game`, `Enum` or `Instance` and a later read of that global are plausible.

Verification gap: `--sem-file`, `--replay-dir` and the unit-test semantic helpers all decompile with flags `0`, so `AutoNameVariables`, `InferTypes`, `InferRobloxTypes` and `OptimizeIR` output (P6, P8) is never executed by any oracle. `Agents/CheckPendingProbes/run_emitted.sh` is a workaround: decompile via the CLI, then run the emitted source through `--sem-file` and compare its *original* trace with the source's.

### P9 — vanilla entry points render vector constants as Roblox `Vector3.new`

Compiler: with default options, `vector.create(1, 2, 3)` with constant arguments is folded into a vector constant (`LOADK`). Roblox compiles `Vector3.new(…)` to the same constant via `vectorLib`/`vectorCtor`. `SourceGenerator::Visit(VectorNode)` (`Generator.hpp:1038-1070`) always emits `Vector3.new(…)`, and `Decompiler.cpp:889-894` hoists `local __fissionVectorCtor = Vector3.new`. That is correct for `DecompileRobloxBytecode`. `DecompileTestCode`/`DecompileVanillaBytecode` (plain `Luau::compile` input) produce the same output, and it fails in a stock Luau VM with `attempt to index nil with 'new'`, because `Vector3` does not exist there.

The semantic oracle hides this: `SemanticOracle.hpp:236-251` installs a fake global `Vector3 = table.clone(vector)` with `new = vector.create` before running either program. Seen in default-CLI output for `vector.create(1, 2, 3)`: `local __fissionVectorCtor = Vector3.new … __fissionVectorCtor(1, 2, 3)`.

## Inverse-lowering gaps (semantically correct, not the natural inverse)

Each case below has a source form that compiles to the *same* bytecode as the compiler input, but Fission emits a different, less natural form.

| # | Compiler lowering | Fission output | Natural inverse (same bytecode) |
|---|---|---|---|
| N1 | Interpolated string → `LOADK "…%*…"` + `NAMECALL format` + `CALL` (`compileExprInterpString`, `Compiler.cpp:2535-2623`); literal `%` escaped to `%%`, constant string sub-expressions spliced into the format | `("v=%*"):format(v2)` | `` `v={v2}` `` whenever the format constant contains only `%*` / `%%` specifiers (undo `%%`, escape `{`, `}`, `` ` ``, `\`) |
| N2 | `for i = a, b do` → `LOADN step, 1` (`compileStatFor`, `:4308-4311`) | `for i = 1, 3, 1 do` | omit a constant step of `1` |
| N3 | `a > b` / `a >= b` → `JUMPIFLT/LE b, a` with operands swapped (`compileCompareJump`) | `if 3 < v2 then`, `while 1 < v2 do` | put the non-constant operand on the left (`v2 > 3`). `b < a` and `a > b` compile identically. |
| N4 | Normal call: callee (`GETIMPORT`/`GETTABLEKS`) is evaluated *before* the arguments (`compileExprCall`, `:1517-1528`) | `local v1_1 = print; v1_1(f(1))` (O0/O1) | `print(f(1))`. Inlining the callee back into call position keeps source evaluation order. |
| N5 | `t[#t + 1] = f()`: l-value index is computed before the RHS (`compileStatAssign` one-to-one path, `:4536-4552`) | `local v2 = #v1 + 1; v1[v2] = f(2)` | `v1[#v1 + 1] = f(2)` (same order: l-value first) |
| N6 | `export local x = …` / `export function f` (`LuauExportValueSyntax`, `compileExportTable`, `:378-440`, `compileStatLocal` `:4116-4128`): values go into a `DUPTABLE`/`SETTABLEKS` export table followed by `return table.freeze(t)` | formerly `local v2 = { x = 5, … }; v2.z = v1; …; return (table.freeze(v2))` plus a dead `local x = 5` | `export local` / `export function` declarations |
| N7 | Straight-line main chunk at O0/O1 | `local v2` hoisted to the top, then the whole body wrapped in `do … end` with fresh `local v2` shadows inside (e.g. `local function f(x) return x end print(f(1)) local t = {} t[#t + 1] = f(2) … local n = f(4) …`) | no hoisted declaration or `do` wrapper needed |
| N8 | `t[#t + 1] = v` inside an inlined body | `out[(#out + 1)] = v` | drop redundant parentheses in index expressions |
| N9 | `debugLevel 2` keeps local/parameter names in `locvars` (`alpha`, `beta`, `step`, `key`, …) | `arg0`, `v2`, `i`, `k`, `v`: debug names are only consulted for phi/declaration decisions (`ASTLifter.cpp:2796-2802`), never for naming | use `locvars[].varname` for parameters and locals when present |
| N10 | Function names at debug 0 are stripped | `local function anon_41_0(…)` | name from the assigned local or global when available (`local function a` is the only binding) |

Status (2026-09-25; regression `Check pending N1-N5, N8` checks the text and O0/O1/O2 VM parity):
- **N1 — resolved.** `SourceGenerator::TryEmitInterpolation` renders `("…"):format(args)` on a string literal as a backtick string when the pattern holds only `%*` and `%%` and the `%*` count matches the argument count (non-variadic, expression or `local x =` position). `EmitStringBody` escapes `` ` ``, `{` and `\`, and `{ ` guards an argument that starts with `{`.
- **N2 — resolved.** `Visit(ForNumericNode)` omits a literal step of `1`.
- **N3 — resolved.** `LiftCondition`'s `liftComparison` emits `x > k` / `x >= k` when the left operand is a literal and the right is not. The swap is exact: same `LT`/`LE` operands and error text.
- **N4 — resolved at O0/O1/O2.** `InliningReordersEffect` allows a callee lookup loaded straight into its call's base register to render as the call's callee. At O0, `ReadLocationChanged` previously used `CALL` operand 0's input SSA version as its output; the call appeared to consume itself, and the path scan falsely crossed later argument calls. It now uses the recorded result definition. A debug local initialized by a call stays materialized when that local is the receiver of a table store, preserving scoped names. `Check pending N4 O0` failed before the fix and passes with direct nested-call output; a caller whose argument mutates the global callee matches VM traces at O0/O1/O2.
- **N5 — resolved.** The fresh-table constructor rule no longer blocks inlining when the overwritten operand register belongs to an operand that itself inlines, giving `t[#t + 1] = f(2)`.
- **N7 — resolved** as a side effect of N5: the `do … end` wrapper was scoping two `local v2` bindings, and the first no longer exists.
- **N8 — resolved.** Index keys render at precedence 0; a key whose text starts with `[` gets a leading space, so no `t[[[`.
- **N6 — resolved for recognized export-table shapes.** Luau marks the main prototype `LPF_USES_EXPORT`; `ExportDeclarationRewriter` requires that flag, a local table initializer, and the terminal `table.freeze(table)` return. It turns fields into `export local` declarations, deferred function stores into `export function`, and exported field reads/writes into local references. Dead constant locals and immediate nil-then-write pairs are removed. Ordinary frozen tables lack the flag and stay table-based; a generated parameter that would shadow an export name keeps the table form. `Check pending N6` covers O0/O1/O2 × debug 0/2, both constant and deferred exports, sole-function exports, output syntax, ordinary-table isolation, and a shadowing fallback. The export sources are VM-oracle UNRUNNABLE, so verification is compiler acceptance and output inspection, not runtime parity. Full suite: 663/663; stress: 22/22; saved replay: 560 PASS, 0 fail, 2,408 unchecked.
- **N9 — resolved.** At debug level 2, parameters take their `locvars` name (`ControlFlowAnalyzer::PopulateNames`), numeric/generic `for` variables take theirs (`ASTLifter::LoopDebugName`), and every other local's SSA versions take the name of the debug local they initialize or write inside (`ASTLifter.cpp`, capture pre-pass). Versions are grouped through same-register phis, so a value diamond or loop-carried value takes one name. A group is left unnamed when two locals disagree, or when a value is read outside the local's range (an inlined call's result). A write counts as the initializer when it is the register's last write before `startpc`, which covers `local a, b = f(), g()`. Move-elided inline parameters that open on a register another local still owns are skipped. When one local shadows another in a different register (`do local x = x + 1 end`), the inner one gets a `_N` suffix; locals are processed in `startpc` order so the outer one keeps the plain name. Supporting fixes: a declaration-only `LOADNIL` that opens a debug local stays as a bare `local x`, so a later loop assignment does not become the declaration. `DeclarationHoister::CoalesceAdjacentDeclarations` merges `local x; x = e` for any name, not just `vN`. Tests that exercise the no-debug-info naming heuristics (`i/j/k`, `ok/result`, `count`, `part`, reverse-field and global back-propagation, `_vN` collisions, register-scope reuse) now compile at debug level 1. Tests that only used `vN`/`argN` incidentally now expect the source names. Regression test: `Check pending N9` in `CheckPendingRegressions.cpp`.
- **N10 — not recoverable.** Debug level 0 strips every name. `anon_*` is as good as a generated name.

## Minor / not compiler-reachable

- **INT64_MIN integer constant.** `SourceGenerator::Visit(IntegerLiteralNode)` (`Generator.hpp:1030-1036`) prints `-9223372036854775808i`, which the Luau parser rejects. The integer lexer cannot express 2^63, and `compileExprUnary`'s negative-integer special case (`Compiler.cpp:2318-2328`) only negates parseable literals. The compiler cannot emit this constant (builtin `integer.*` calls are not folded). Bytecode from another producer (e.g. Roblox) could, and the output would then not recompile. Rendering it as `(-9223372036854775807i - 1i)` would fail at runtime (integer arithmetic needs the library), so something like `integer.create(-2^63)` would be needed.
- **`GETUPVAL` remark reads the inactive union member.** `BytecodeLifter.cpp:473-477` writes `value.imm.n` and formats `value.imm.u`. The value is the same on little-endian hosts, and it only affects the IR remark. This is the same class as the old FASTCALL3 register write.
- **`SUBRK`/`DIVRK` constant operand typed `ImmediateInteger`.** `BytecodeLifter.cpp:1176-1186` stores the constant-table index `B` as `ImmediateInteger` instead of `ImmediateConstant`, unlike `ADDK`…`IDIVK`, where `C` is typed `ImmediateConstant`. Probes with `1 - x`, `10 / x`, `300 - x` and `0.5 - x` (including `__sub`/`__div` metamethod operand order) all match, so consumers handle it. Generic code that switches on operand type (e.g. constant-pool validation or JSON dumps) would treat it as a literal number.
- **Harness false positives.** `--sem-file` compares printed `pcall` error strings verbatim, so `chunk:N:` line prefixes always differ between original and decompiled sources. Probes must strip `^.-:%d+: ` before printing. Uncaught errors are compared the same way.

Status (2026-09-25): all four resolved.
- INT64_MIN renders as `integer.create(-9223372036854775808)`, with a hand-built-bytecode regression covering VM parity.
- `GETUPVAL` now formats `imm.n`.
- `SUBRK`/`DIVRK` operand 1 is typed `ImmediateConstant` via `imm.k`, and the `ConstantPropagation` workaround is gone.
- `RunLuauTrace` applies `NormalizeError` to prints and the trace, so `pcall` messages no longer differ by line number. The session's `p11_calls` probe now matches.

## Verified as matching (no action)

Lifter decode vs `Bytecode.h`: all 91 opcodes have a case. Operand fields and AUX layouts match, including `JUMPXEQK*` (`KV`/`KB`/`NOT` bit), `FASTCALL2/2K/3`, `*UDATAKS` (`AUX & 0xffff`), `NEWCLASS`, `CALLFB`, `LOADB` skip, and the `JUMP`/`JUMPX`/`JUMPBACK` offsets. The branch-target validation in `LiftFunctionBytecodeInternal` matches VM `pc + 1 + D` for all jump families. Numeric and generic `for` use the documented prep↔latch abstraction.

VM-parity probes at O0/O1/O2 that matched (session-local probe sources, not kept in the repo):
- relational operators, including NaN and `not (a < b)` vs `a >= b`, plus error messages for mixed types
- `not not x` and other `not` value forms
- associativity and metamethod order for `..`, `-`, `^`, `/`
- multret truncation (`(f())`, `{f(), x}`, `select('#', …)`, varargs)
- multiple and compound assignment order with `__index`/`__newindex`
- numeric and generic loops (float and negative step, captured loop variables, loop-variable writes, `for`-bound evaluation order and error messages)
- constant edge values (`-0`, `inf`, NaN, `2^53+1`, `%`/`//` sign rules)
- closures and upvalues (shared upvalues, shadowing, per-iteration capture, `break` out of a capturing loop)
- table constructors (`SETLIST` multret, mixed keys, duplicate keys, keyword and non-identifier keys, `GETTABLEN` boundaries 0/1/256/257)
- builtins and FASTCALL, including shadowed `math` and `select` variants
- side-effect-only lowering (`compileExprSide` for extra `local`/assignment values, `if X and false`, surplus inlined arguments)
- interpolated strings (semantics only, see N1)
- O2 inlining (early return, multret, loops with returns)
- O2 unrolled `for` with `break`/`continue`
- `repeat … until true` forward-jump idioms
- string literal rendering (CR/LF, leading newline, `]]`, NUL, high bytes)
- `NAMECALL` vs `o.m(o, …)` lookup order
- `SUBRK`/`DIVRK` operand order
- `getfenv`/`setfenv` programs (import and inlining disabled, env swaps mid-function)
- `pcall`/`xpcall` FASTPCALL forms with multret arguments
- 3-variable `pairs`/`ipairs` loops
- effectful `while`/`repeat` conditions
- table-store coalescing across observing calls, `setmetatable` and aliases
- ~50 simultaneously live locals and 20-deep nested calls
- function-level nested `if`/`elseif` with partial returns and value merges
- vectors, `buffer`, `integer` values
- renamer passes (`require`, `GetAttribute`, `Instance.new`, reverse-field, loop-variable, `pcall`/length naming) with colliding globals
- classes: fields via `new({…})`, 3-level `extends`, vararg methods, captured-upvalue mutation, class stored in a table
- the `AutoNameVariables` pipeline and the `--optimize-ir` pipeline over all non-failing probes above (via `run_emitted.sh`). Only P1/P4/P5/P6/P8 differ.

## Not probed

- `LOADKX` (more than 32767 constants in one function) and `JUMPX` / long-jump trampolines: generating those sources was not practical here. The earlier ledger's C13 exercised long jumps.
- Class exports combined with `export function`. Multi-value `export local a, b = f()`, exported tables mutated after export, and `export function` recursion/cross-reference produced identical traces at O0–O2. The verdict was only `UNRUNNABLE` because the module returns functions.
- `libraryMemberConstantCb` (Roblox `Enum.*` constant folding) and `Debug*` compiler flags other than classes and if-local.

A temporary Catch2 probe (since removed) also covered these, all matching at O0/O1/O2:
- Roblox profile `vectorLib = "Vector3"`, `vectorCtor = "new"`, `vectorType = "Vector3"`: constant and runtime `Vector3.new`, vector arithmetic and `==`, typed `Vector3` parameters, vectors built in loops. Checked through both `DecompileVanillaBytecode` and `DecompileRobloxBytecode`.
- Roblox-encoded round trips: a `Luau::BytecodeEncoder` applied opcode × 227 while skipping AUX words, then `DecompileRobloxBytecode`, on loops with `continue`, `pairs`, varargs, upvalues, builtins and interpolation.
- `DebugLuauIfLocalSyntax` `if local x = … then / elseif local x = … then / else`, including a closure capturing the if-local (debug 0/2). It lowers to `local x = …; if x then` (`compileStatIfLocal`, `Compiler.cpp:3674-3735`), and Fission emits the equivalent plain form.
