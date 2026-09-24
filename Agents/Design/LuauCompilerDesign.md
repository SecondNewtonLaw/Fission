# Luau Compiler Lowering

How the Luau compiler turns source into bytecode, written as the reference for inverting it. Decompilation is the inverse of these shapes: when a CFA, SSA or lifter rule is in doubt, check it against this document and the compiler source, not against intuition.

**Version**: Luau 0.738 (`GIT_TAG` in `CMakeLists.txt`). Find the exact tree through `Luau_SOURCE_DIR` in any build's `CMakeCache.txt` (currently `F:/cpm_cache/luau/1735`). Function names below are stable across versions; line numbers are not.

| File | Role |
|---|---|
| `Compiler/src/Compiler.cpp` | AST → bytecode for every statement and expression |
| `Compiler/src/ConstantFolding.cpp` | Which expressions become constants before emission |
| `Compiler/src/ValueTracking.cpp` | `written`/`init` per local, `Written`/`Mutable` per global |
| `Compiler/src/Builtins.cpp` | Which calls are builtins (FASTCALL candidates) |
| `Compiler/src/CostModel.cpp` | Inline and unroll cost heuristics |
| `Compiler/src/Utils.h` | `alwaysTerminates` |
| `Bytecode/src/BytecodeBuilder.cpp` | `foldJumps`, `expandJumps`, `undoEmit`, jump patching |
| `Common/include/Luau/Bytecode.h` | Opcode encodings and semantics |
| `VM/src/lvmexecute.cpp` | Ground truth for what an opcode does |

`Bytecode/src/Sccp.cpp`, `BytecodeGraph.cpp` and `BytecodeCallInliner.h` are not called by `Luau::compile`. `CMPPROTO` comes only from that bytecode-level inliner and never appears in compiled output.

Notation: "jumps to L" means the patched target label L. Labels are instruction indices. "After X" is the instruction following X, including its AUX word.

## 1. Compile options and flags

- **`optimizationLevel`** (default 1). A `--!optimize N` hot comment overrides it. `--!native`, or any function carrying `@native`, forces level 2.
  - O0 has no constant folding, no GETIMPORT, no FASTCALL and no jump folding.
  - O1 is what Roblox ships.
  - O2 adds inlining, loop unrolling, builtin constant folding and a few register tricks (§12).
- **`debugLevel`** (default 1; Roblox ships 1).
  - At 2, constant-local elision is disabled (§2), `gatherConstUpvals` re-adds constant upvalues, and **upvalue names** are emitted.
  - Fission reads function names and upvalue names but not debug local names.
  - At debug 2, upvalue names separate a captured local from other variables sharing its register. At debug 1 that separation must come from Fission's own naming, so naming bugs hide at debug 2. Always test both levels.
  - Harness settings. The CLI and fuzzer take `--opt N` and `--debug N`:

    | Harness | Default settings |
    |---|---|
    | Fuzzer (`kOpt`, `kDebug`) | O1 / debug 2 |
    | CLI `--decompile-test` | O1 / debug 2 |
    | SSA/CLI smoke paths | debug 1 |
- **FFlags**: every Fission harness sets every `Luau*` bool flag to true (`Debug*` flags stay off). Flags that change emitted shapes:

  | Flag | Effect |
  |---|---|
  | `LuauCompileMoveElision` | Drops a `MOVE` when an operand can be read straight from a local; inlined results go straight to the target (§3, §12) |
  | `LuauCompileCleanBlockDeadClose` | No `CLOSEUPVALS` after a block that always terminates |
  | `LuauCompileContinueEagerClose` | Repeat `continue` closes fewer locals (§8) |
  | `LuauCompileLoopUnrollZero` | O2: a numeric for with trip count 0 emits nothing |
  | `LuauCompileIifeInline` | O2: an immediately-invoked function expression is inlined regardless of cost |
  | `LuauCompileConcatTargetTop` | Concat operands are compiled with the TempTop register discipline |
  | `LuauEmitCallFeedback` | `CALLFB` instead of `CALL` inside nested functions (§10) |
  | `LuauCompileFastpcall` | `FASTPCALL` for `pcall`/`xpcall` (§10) |
  | `LuauOptimizeExportTable` | Export tables built through `DUPTABLE` templates |

  `DebugLuauIfLocalSyntax` (`if local x = e then`) and `DebugLuauUserDefinedClasses` stay off in the harness. Production Roblox bytecode may have a different flag set, so treat every flag-gated shape as possible input.

## 2. What never reaches the bytecode

These are unrecoverable by design. The decompiler emits an equivalent program and must not try to recover them.

- **Constant folding** (`foldConstants`, O1+). The compiler folds:
  - literals and unary `-`, `not`, `#` on constants;
  - arithmetic on numbers;
  - `..` of strings;
  - `==`/`~=` on any constants, and `<`/`<=`/`>`/`>=` on numbers;
  - interpolated strings whose parts are all constant strings;
  - if-expressions with a constant condition.

  `and`/`or` fold as soon as the **left** side is constant (`true and x` becomes `x`, even when `x` is unknown). `x and false` is **not** folded.
- **Constant locals.** A local that is never written and whose initializer is constant takes no register at all: every use becomes the constant (`compileStatLocal`, O1+ with debug ≤ 1). This includes trailing locals with no value (`local a, b = 1` makes `b` the constant nil).
- **Constant tables.** A local table that is never mutated and never escapes (`buildTableConstantMap`), and whose items are all `name = constant`, has its field reads folded: `t.x` becomes the constant. The table is still built.
- **Alias locals.** `local b = a`, where neither local is ever written, makes `b` share `a`'s register with no `MOVE` (O1+).
- **Dead code.**
  - A block stops compiling after its first always-terminating statement (`alwaysTerminates`: return, break, continue, or an `if` with both arms terminating, or with a constant condition choosing a terminating arm). Code after `return` in the same block does not exist.
  - `if <false> then … end` emits only the else body.
  - `if <true> then A else B end` emits only `A`.
  - `if x and <false> then` evaluates `x` for side effects, then emits the else body.
  - `while <false>` emits nothing.
- **Side-effect-only expressions** (`compileExprSide`): a local, global, `...`, function expression or constant used only for effect emits nothing. This applies to extra values in `local a = 1, g` and extra call arguments of an inlined call. Note that a global read's `__index` is dropped.
- **Transparent wrappers.** Parentheses, type assertions (`::`) and instantiations emit nothing, except that parentheses truncate a multi-value call to one value.

## 3. Registers and locals

- Allocation is a stack.
  - `allocReg` bumps `regTop`, and `RegScope` restores it at scope exit.
  - Locals keep their register until their block ends.
  - Temporaries live above all live locals.
- A local's register is its only home. There is no spilling and no reuse while it is live.
- **Direct local reads.** `compileExprAuto` returns a local's own register instead of copying it. An operand that is a local is therefore read when the consuming instruction executes, not when the source mentions it. For example, in `a + f()` with `f` writing `a` through an upvalue, `ADD` reads the new `a`. The bytecode defines the semantics, and re-emitting `a + f()` compiles identically.
- **Move elision.** `compileExprAutoTemp` undoes a trailing `MOVE target, src` and uses `src` directly unless `src` is captured (`regCaptured`). The places it applies:
  - fastcall arguments;
  - inline arguments;
  - `GETTABLEKS` object;
  - single-value `return`.
- **Direct assignment to locals.** `x = e` compiles `e` straight into `x`'s register, and `local x = e` into the freshly allocated register. There is no temporary, and the last instruction of `e` writes the local.
- **Captures** (`compileExprFunction`, `CAPTURE` after `NEWCLOSURE`):

  | Capture | When |
  |---|---|
  | `LCT_VAL reg` | Local never written anywhere |
  | `LCT_REF reg` | Local written somewhere (shared upvalue cell) |
  | `LCT_UPVAL idx` | Variable lives in an enclosing function |

  Inlining can capture a constant, in which case the compiler loads it into a temporary and uses `LCT_VAL`.
- **Self-reference.** `NEWCLOSURE` stores the closure into its target register before processing captures, so `local function f` capturing `f`'s own register by `VAL` is a self-reference.
- **Closure sharing.** `DUPCLOSURE` (a shared closure constant) is used instead of `NEWCLOSURE` when every upvalue is immutable and is either top-level or itself a shareable closure, at O1+ without `setfenv`.
- **Closing captured locals.** Captured locals that are written get `LCT_REF`. `closeLocals` emits a single `CLOSEUPVALS minReg` at:
  - block exit (skipped after a terminating block when the flag is on);
  - `break`/`continue`, down to the loop's local offset;
  - `return` (closing all locals);
  - repeat exits (twice, §8).
- **Multiple assignment** (`compileStatAssign`):
  - `a, t.x = …` evaluates every l-value's subexpressions left to right, then the right-hand sides left to right, then assigns.
  - Right-hand sides go directly into local targets, except for a local that is read after it is assigned (a conflict), which gets a temporary.
  - Non-local targets are stored left to right. The conflicting locals are then filled with `MOVE`s.
  - `a, b = b, a` therefore becomes `MOVE tmp, b; MOVE b, a; MOVE a, tmp` (or similar). Its statements cannot be split naively.
  - When there are more targets than values, the trailing targets receive nil (`LOADNIL`) or the last call's extra results.
- **Compound assignment.**
  - `x op= e` on a local is `OPK x, x, k` or `OP x, x, r`.
  - On an index it is `GET* tmp; OP tmp; SET* tmp`, and the table/key expressions are evaluated once.
  - `x ..= e` is `MOVE r0, x; e… into r1..; CONCAT x, r0, rn`.
- **Local functions.** `local function f` reserves the register (`pushLocal`) before compiling the closure. `function t.a.b()` compiles the closure into a temporary, then `SETTABLEKS`. `function g()` for a global uses `SETGLOBAL`.

## 4. Expressions

| Source | Lowering |
|---|---|
| `nil`, `true`, `false` | `LOADNIL`, `LOADB r, v, 0` |
| number | `LOADN` when it is an int16 and not `-0.0`, otherwise `LOADK`/`LOADKX` |
| integer (`LuauIntegerType2`) | `LOADK` of an integer constant; `-1000i` folds to one constant |
| string | `LOADK`/`LOADKX` |
| local | its register, or `MOVE target, reg` when a copy is needed (omitted at O1+ when `target == reg`) |
| upvalue | `GETUPVAL` |
| global | `GETIMPORT` when never written anywhere in the module (O1+); `GETGLOBAL` otherwise |
| `a.b.c` from a global | `GETIMPORT` with a 2- or 3-part import id when every part's constant index is < 1024 and the root global is `Default` (never written and not `_G`) |
| `e.name` | `GETTABLEKS target, obj, hash; AUX k` |
| `e[1..256]` constant | `GETTABLEN target, obj, idx-1` |
| `e["str"]` constant | `GETTABLEKS` |
| `e[k]` | `GETTABLE target, obj, key` (object evaluated before key) |
| `a op k` (k a number constant ≤ 255) | `OPK target, a, k` |
| `k - a`, `k / a` | `SUBRK`/`DIVRK target, k, a` |
| `k + a`, `k * a` (O2, `a` typed number) | `ADDK`/`MULK target, a, k`, operands swapped |
| `a op b` | `OP target, a, b` (left compiled first) |
| `a .. b .. c` | one `CONCAT target, r0, r2`: the **right spine** is flattened, the left is not, so `(a..b)..c` is two `CONCAT`s |
| `-x`, `not x`, `#x` | `MINUS`, `NOT`, `LENGTH` |
| comparison as value | §6 |
| `and` / `or` as value | §6 |
| if-expression | §6 |
| `` `a{x}b` `` | `LOADK fmt; x… into r+2..; NAMECALL r, r, "format"; CALL r, n+2, 2`. Constant string parts are merged into `fmt` (with `%` escaped) and pass no argument |
| `...` | `GETVARARGS target, n+1`, or `B = 0` for multi-value |
| `function … end` | `NEWCLOSURE`/`DUPCLOSURE` + `CAPTURE`s |
| table constructor | §11 |

`MOVE` elision means an operand register may be a local's own register even when the source wrote a more complex expression that folded down to that local.

## 5. Conditions (`compileConditionValue`)

`compileConditionValue(node, target, skipJump, onlyTruth)` emits jumps that are taken when `node`'s truthiness equals `onlyTruth`. The other case falls through. With a `target`, the value is also left in `target` on the jumping path; on the fall-through path `target` is unspecified.

| Node | Emission |
|---|---|
| **Constant** | Nothing when its truthiness differs from `onlyTruth`. Otherwise it computes the value into `target` when there is one, then emits an unconditional `JUMP` |
| **`a and b`, `a or b`** | See the two cases below the table |
| **Comparison** | With a target, `LOADB target, onlyTruth` first, then one compare-jump built with `not_ = !onlyTruth` (below) |
| **`not x`** | Recurses on `x` with inverted polarity, **only without a target** |
| **Parentheses** | Transparent |
| **Anything else** | Calls, indexing, locals, if-expressions, `not` with a target, `and`/`or` nested inside another operator, … It is computed into one register (`compileExprTemp` into `target`, or `compileExprAuto`, which may be a local's own register), then a single `JUMPIF` (onlyTruth) or `JUMPIFNOT` on it |

The `and`/`or` row has two cases:

- **`onlyTruth == (op is and)`**:
  1. The left side goes into a local else-list with `!onlyTruth` and no target.
  2. The right side is compiled with the caller's polarity and target.
  3. The else-list is patched to just after the right side.
- **Otherwise**: both sides are compiled into the caller's skip list with the caller's polarity and target.

**Compare-jump** (`compileCompareJump`):

- **Operand order.** The left operand is evaluated first, then the right, so evaluation order always matches the source.
- **`>` / `>=`.** These emit `JUMPIF[NOT]LT/LE right, left`: the operands are swapped in the instruction, not in the evaluation.
- **Constants in `==` / `~=`.** A constant on either side (moved to the right) with type nil/boolean/number/string gives `JUMPXEQKNIL/KB/KN/KS reg; AUX k | NOT<<31`. The NOT bit is set when `(op is ==) == not_`. Vector and integer constants use the register form.
- **Negated forms.** `not_` selects `JUMPIFNOT*`. `JUMPIFNOTLT a, b` means "not (a < b)", which differs from `a >= b` under NaN and `__lt`/`__le`. A decompiler that flips a comparison to swap arms **must** emit `not (a < b)`, never `a >= b`.

**Resulting shape.** A condition is a tree of forward jumps with exactly two exits (the skip list and the fall-through). Its leaves are single-register tests or compare-jumps. A leaf whose value comes from a branching sub-expression (an if-expression, or jump-form `and`/`or` evaluated as a value) is a single-entry region whose paths join at the test through a phi for that register. `DetectGuardRegion` and value terms implement the inverse.

## 6. Values with control flow

- **Comparison as value** (`compileExprBinary`). The compare jump is taken when the comparison is **true**:

  ```
  <compare-jump → L1>; LOADB r, 0, 1 (skips next); L1: LOADB r, 1
  ```
- **`and`/`or` as value** (`compileExprAndOr`):
  - **Constant left side**: only the selected side is compiled.
  - **Left side is not `isConditionFast`**, where condition-fast means constant, comparison, `and`/`or`, or a group of those:
    - right side is a local: `AND`/`OR target, left, right`;
    - right side is a constant with index ≤ 255: `ANDK`/`ORK target, left, k`;
    - otherwise the jump form below.
  - **Jump form**:
    1. `reg` is `target` when it is a temporary, otherwise a fresh register.
    2. `compileConditionValue(left, &reg, skip, onlyTruth = !isAnd)` runs.
    3. The right side is compiled into `reg`.
    4. The skip label comes after the right side.
    5. `MOVE target, reg` follows when `reg` was fresh. A non-temporary target such as `a = a > 1 or a + 2` uses a fresh `reg` so that the left side cannot clobber `a`.
  - A nested condition-fast left side splits into several jumps. For example, `(a and b) or c` as a value:
    - `a` is tested with no target (`JUMPIFNOT a → c-part`), so its value is never stored;
    - `b` goes into `reg` then `JUMPIF reg → end`;
    - `c` goes into `reg`.
- **If-expression** (`compileExprIfElse`):
  - **Constant condition**: only the selected branch.
  - **Condition is a local `v`**:
    - `if v then v else e` becomes `OR`/`ORK target, v, e`;
    - `if v then e else v` becomes `AND`/`ANDK target, v, e` (when `e` is a local or a constant).
  - **General**: a false-jumping condition with no target, then the true value into `target`, `JUMP end`, the else label, the false value into `target`, the end label.
- **The target is written on every path.** Every path writes `target` (or the shared `reg`). With `LuauCompileMoveElision` a branch may instead write into a local's register directly when the whole expression is being assigned to that local.

## 7. If statement (`compileStatIf`)

In order of precedence:

1. **Constant-false condition**: only the else body.
2. **`x and <constant false>`**: `x` for side effects, then the else body.
3. **`if local`** (`DebugLuauIfLocalSyntax`): the value goes into a fresh register, `JUMPIFNOT reg → else`, and the local lives only in the then-arm.
4. **Break fast path**: `if c then break end` with no else, and no captured locals since the loop start. The condition is compiled with `onlyTruth = true` and its jumps are patched **directly to the loop's break target**. No `JUMP`, no body.
5. **Continue fast path**: same shape with `continue` (a single statement), when no locals captured since `localOffsetContinue`. The jumps go to the continue target.
6. **General**:
   - The false jumps go to else/end. The then body **falls through** from the condition, so it is laid out immediately after the condition code.
   - With an else body and a non-empty false-jump list:
     - then-arm always terminates: no `JUMP`, and the else label follows the then body directly;
     - otherwise: `JUMP end`, else label, else body, end label.
   - A constant-true condition has an empty false-jump list, so the else body is not emitted.
   - `elseif` is an `if` nested as the sole statement of the else body.

## 8. Loops

Every loop pushes a `Loop{localOffset, localOffsetContinue}`. `break`/`continue` statements push `LoopJump`s, which `patchLoopJumps` resolves when the loop ends.

- **Break target**: `endLabel`.
- **Continue target**: `contLabel`.

### While (`compileStatWhile`)

```
loopLabel: <condition, false jumps → end>
           <body>
contLabel = backLabel: JUMPBACK loopLabel
end:
```

- **Constant-false condition**: no code at all.
- **`while true`**: the condition emits nothing, so the loop is `body; JUMPBACK`.
- **Targets**: `continue` goes to the `JUMPBACK`; `break` goes to `end`.
- **Test placement**: the test is at the top. There is no loop rotation.

### Repeat (`compileStatRepeat`)

```
loopLabel: <body>
           [CLOSEUPVALS: locals declared after the first continue, once continue is used]
contLabel: <condition, true jumps → skipLabel>
           [CLOSEUPVALS: body locals]
           JUMPBACK loopLabel
skipLabel: [CLOSEUPVALS: body locals, again]
endLabel:
```

- **Until-condition scope**: the condition can read body locals, so body locals are closed only after it.
- **Exit targets**:
  - `break` goes to **endLabel**; the until-exit goes to **skipLabel**.
  - The two coincide unless body locals are captured.
- **`continue`** goes to `contLabel`.
  - With `LuauCompileContinueEagerClose`, `localOffsetContinue` freezes at the first `continue`: locals declared before it are not closed by it, because the until-condition may still reference them.
  - Using a local declared after the first `continue` in the until-condition is a compile error.
- **`until <constant true>`**: no condition and **no `JUMPBACK`**. The body runs once, and `break`/`continue` become forward jumps to the end.
- **`until <constant false>`**: the condition emits nothing, so the loop is `body; [CLOSEUPVALS]; JUMPBACK`. That is identical to `while true` except for the `continue` target.
- **Equivalence with `while true`**: `while true do … if x then break end end` and `repeat … until x` can produce the same bytecode.

### Numeric for (`compileStatFor`)

```
           from → R+2; to → R+0; step → R+1 (LOADN 1 when omitted)    -- evaluation order: from, to, step
forLabel:  FORNPREP R → endLabel
loopLabel: [MOVE var, R+2 when the loop variable is assigned in the body]
           <body>
           [CLOSEUPVALS]
contLabel: FORNLOOP R → loopLabel
endLabel:
```

- **Jump targets**: `FORNPREP` skips to **after `FORNLOOP`**, and `FORNLOOP` jumps to **after `FORNPREP`** (the VM applies D to the incremented pc).
- **Loop control**: `continue` goes to `FORNLOOP` and `break` goes to `endLabel`.
- **Loop variable**: it is `R+2` itself unless it is written in the body. `R+0`/`R+1` are never visible to user code.
- **Unrolling at O2**: constant bounds may be fully unrolled (§12).

### Generic for (`compileStatForIn`)

```
           values → R..R+2 (compileExprListTemp, 3 targets)
skipLabel: FORGPREP[_NEXT|_INEXT] R → backLabel
loopLabel: <body>                                    -- variables in R+3.., at least 2 registers reserved
           [CLOSEUPVALS]
contLabel = backLabel: FORGLOOP R → loopLabel; AUX nvars | (0x80000000 for ipairs)
endLabel:
```

- **Prep opcode**: at O1+ with ≤ 2 variables:
  - `ipairs(t)` uses `FORGPREP_INEXT`;
  - `pairs(t)` or `next, t` uses `FORGPREP_NEXT` (the latter only without fenv);
  - otherwise `FORGPREP`.
- **Prep behaviour**: every prep jumps to `FORGLOOP` unconditionally. The first iteration's test is the `FORGLOOP`.
- **Loop control**: `continue` goes to `FORGLOOP` and `break` goes to `endLabel`.

### Break / continue (general form)

`CLOSEUPVALS` (when there are captured locals since the loop's offset), then `JUMP`, patched later. The if fast paths in §7 emit no `JUMP` of their own.

### Invariants the CFA may rely on (before `foldJumps`)

- Every while/repeat back edge is a `JUMPBACK`, and every loop is the interval `[loopLabel, back-edge]`. The natural exit is the instruction after the back edge (the `FORNLOOP`/`FORGLOOP`/`JUMPBACK`).
- Every numeric for back edge is `FORNLOOP`, and every generic for back edge is `FORGLOOP`.
- Inner and outer loops can share a header pc when the outer loop has no top test (`while true` or `repeat`) and its body starts with a loop.
- `break`, `continue` and conditions only ever jump **forward**. `JUMPBACK`/`FORNLOOP`/`FORGLOOP` are the only backward jumps, except `expandJumps` trampolines (§13).

## 9. Return

- **Multiple values**: `return a, b` where the registers are consecutive locals becomes `RETURN a, 3` with no copies.
- **Single value**:
  - with move elision, `compileExprAutoTemp` may return a local's register;
  - otherwise values are computed into fresh consecutive registers, and a trailing call or `...` gives `B = 0` (multi-value).
- **Closing**: `closeLocals(0)` comes first, so `CLOSEUPVALS` may precede `RETURN`.
- **Implicit return**: the implicit final `RETURN 0, 1` is omitted when the function body always terminates.
- **Returns inside inlined bodies** become `JUMP`s (§12).

## 10. Calls

- **Plain call** (`compileExprCall`):
  - The function goes into `regs`, then arguments into `regs+1..`; the last argument may be multi-value (`B = 0`).
  - Then `CALL regs, nargs+1, nresults+1` (`C = 0` for a multi-value result).
  - The results are moved to the target unless the target is at the top of the stack (`targetTop`), in which case the call is placed so that results land in place.
  - A statement call has `C = 1` (no results).
- **Method call**:

  ```
  NAMECALL regs, obj, hash; AUX k
  <args into regs+2..>
  CALL regs, nargs+2, …
  ```

  `obj` is the local's own register when possible.
- **Builtin fast calls** (O1+):

  ```
  <args> (registers or constants; locals used in place)
  FASTCALLn bfid, arg0, skip; [AUX]
  <fallback setup: MOVE/LOADK into regs+1.., then the function into regs>
  CALL regs, …
  ```

  - `FASTCALL1/2/2K/3` is used for 1–3 arguments when the last argument is not multi-value. At O2 a none-safe builtin with matching arity still qualifies when the last argument is multi-value. `FASTCALL3` is used only when one of the arguments is a local.
  - On success, execution skips the fallback **and** the `CALL`. The fallback setup is emitted after the arguments, so the argument registers may be locals read in place.
  - Generic `FASTCALL bfid, 0, skip` is used for other arities: arguments are compiled normally, then `FASTCALL`, then the function load, then `CALL`.
  - `select(n, ...)` uses `FASTCALL1 LBF_SELECT_VARARG` followed by `GETVARARGS`.
  - `bit32.extract(x, f, w)` with constant `f`/`w` uses `FASTCALL2K LBF_BIT32_EXTRACTK` with a packed constant.
  - Which calls count as builtins: a global, or a library member of a `Default` global, or a local alias of either that is never written (`getBuiltin`).
- **`pcall` / `xpcall`** (`LuauCompileFastpcall`, O1+, global not written):
  - Arguments first, then `FASTPCALL id, nexplicit, skip`, then the function load, then `CALL`.
  - `FASTPCALL` performs the protected call and skips the `CALL` on success.
- **`CALLFB`** (`LuauEmitCallFeedback`): replaces `CALL` inside nested functions (not main) when the call is not a builtin, not `FASTPCALL`, and neither the arguments nor the results are multi-value. The AUX word is a feedback slot. The semantics are identical to `CALL`.

## 11. Tables (`compileExprTable`)

- **Empty `{}`**: `NEWTABLE target, hashSizeLog, 0; AUX arraySize`, with sizes predicted from later assignments (`predictTableShapes`).
- **All items are `name = value` (1 ≤ n ≤ `kMaxLength`)**:
  - Emits `DUPTABLE target, k`, where `k` is a template of the keys.
  - Since bytecode v7 the template also carries constant values (`LBC_CONSTANT_TABLE_WITH_CONSTANTS`): a field whose value is a constant emits **no store**. The value exists only in the template, and the decompiler must read it back from the constant.
  - For duplicate keys the last value wins. Once a key has a non-constant value, every later item with that key is emitted as a store, in order.
- **Otherwise**: `NEWTABLE` with the array size (list items, excluding a trailing `...`, plus `[1]..[n]` keys when every key is such an index) and the hash size.
- **Order of items** after construction:
  - List items accumulate in chunk registers and flush with `SETLIST t, r0, n+1; AUX startIndex` every 16 items, **and before every keyed item**, so insertion order is preserved.
  - A trailing multi-value item flushes with `C = 0`.
  - A keyed item is stored with `SETTABLEKS`/`SETTABLEN`/`SETTABLE` right after its key and value are computed.
  - A later branch can change a captured computed key before the final `SETLIST C=0`. Reconstruction must use the key value stored earlier. ASTLifter saves a pre-materialized key beside the table declaration or an effectful call key at its `SETTABLE` site. It keeps the latter store out of constructor folding, then rebuilds the final literal with every result of the last call. O2 inlining can turn a call key into a literal `LOAD`; that literal remains the key even if its temporary register is reused.
- **Temporary target**: when the target is not a temporary, the table is built in a fresh register and then moved.

## 12. O2-only transformations

- **Inlining** (`tryCompileInlinedCall`) applies to a call to a local function, or to a constant-table member function. The callee must:
  - not be vararg, not use `self`, and not be recursive in the current inline stack;
  - be at depth < 5, in a module without `getfenv`/`setfenv`;
  - fall within the cost threshold (IIFEs bypass the threshold);
  - not be called for multiple results.

  Lowering:
  - Arguments are evaluated first:
    - constant arguments emit nothing and are folded into the body;
    - arguments that are unwritten locals reuse the local's register;
    - others get fresh registers, or the call's target register when the function returns that parameter (move elision).
  - The body is compiled in place with constants re-folded, so branches on constant arguments disappear.
  - Each `return` becomes `<values into target>; CLOSEUPVALS?; JUMP returnLabel`. The last return's `JUMP` is removed when it would jump to the next instruction.
  - On the fall-through path the target registers get `LOADNIL`.

  The result is forward jumps and joins with no source `if` behind them. It also produces **loops with extra exits**: a `return` inside an inlined loop jumps past the loop's natural exit to the return label.
- **Unrolling** (`tryCompileUnrolledFor`) applies to a numeric for with constant bounds, trip count ≤ 25 (boosted by the cost model up to 300), and a loop variable that is not written.
  - The body is emitted once per iteration with the loop variable folded to a constant.
  - Each iteration's `continue` jumps to the next copy, and `break` jumps past the last copy.
  - With `LuauCompileLoopUnrollZero`, trip count 0 emits nothing.
- **Builtin constant folding** (when fenv is unused): `math.pi`, `math.floor(3.5)` and similar fold to constants.
- **Multi-value call to fixed call**: a call to a function known to return exactly one value (`returnsOne`), or a builtin with one result, is compiled with a fixed result count even in multi-value positions.
- **Constant on the left**: `k + x` and `k * x` with `x` typed number become `ADDK`/`MULK x, k`.

## 13. BytecodeBuilder post-pass

These run after each function is emitted and change the CFG the decompiler sees.

- **`foldJumps`** (O1+, skipped when any jump is long). For **every** patched jump (conditional jumps, `JUMP`, `JUMPBACK`, `FORNPREP`, `FORNLOOP`, `FORGPREP*`, `FORGLOOP`):
  - **Threading.** The target is followed through chains of **forward unconditional `JUMP`s**, and the jump is retargeted to the final destination. Conditional exits therefore skip intermediate join blocks. The skipped `JUMP`s stay in place and may become unreachable.
  - **`JUMP` to `RETURN`.** An unconditional `JUMP` whose final target is a `RETURN` is **replaced by a copy of that `RETURN`**. Returns are duplicated into the arms; for example, the `JUMP end` over an else body becomes a `RETURN` when a `RETURN` follows the `if`. Only exact `RETURN` targets qualify: a join that computes a value or closes upvalues first is not copied.
  - **Back edges can become forward.** A back edge whose target is a forward `JUMP` is threaded forward:
    - `while true do break end`: the `JUMPBACK` targets the break `JUMP` at the header, so it is threaded to the loop exit and becomes a **forward `JUMPBACK`**;
    - `for i = a, b do break end` does the same to `FORNLOOP`.

    A loop whose first body statement is an unconditional `break` or `JUMP` has no backward edge at all.
- **Zero-offset jumps.** A `JUMP` with `D = 0` falls through; one example is the `JUMP end` over an empty else. `JUMPIF`/`JUMPIFNOT` with `D = 0` and compare-jumps with an offset to the next instruction also occur. A compare-jump still evaluates its comparison and can raise.
- **`expandJumps`** handles jumps whose offset is ≥ 32767/3 in functions with any long jump. The jump is replaced by a trampoline, and a conditional jump therefore becomes a **backward jump by two instructions** into a `JUMPX`:

  ```
  JUMP +1        -- forward path skips the JUMPX
  JUMPX target   -- 24-bit offset
  OP … -2        -- the original jump, now targeting the JUMPX
  ```

  `JUMPX` is also a valid interrupt point.

## 14. Implications for Fission

| Compiler behaviour | Fission counterpart | Invariant to hold |
|---|---|---|
| Condition = forward-jump tree, 2 exits, value-region leaves (§5) | `DetectOrChain`, `DetectGuardRegion`, value terms | Any tree of pure tests lifts to one boolean expression; a leaf that computes a value is part of the condition only through its phi register |
| Then-arm falls through from the condition (§7) | `FindMergeBlock`, IfHeader arm selection | The fall-through successor of the last test is the then-arm |
| Break/continue fast path jumps straight to the loop target (§7) | Break/continue edge classification | A condition exit may *be* a loop exit/continue target, with no JUMP block |
| Repeat has two exits when locals are captured (§8) | `exitAfterLatch`, repeat detection | `skipLabel` and `endLabel` are both loop exits |
| While/repeat back edge = `JUMPBACK`; for back edge = `FORNLOOP`/`FORGLOOP` (§8) | `IdentifyStructures` | One latch per loop; exit = instruction after the back edge, reached through NOP/JUMP chains |
| `foldJumps` threading and RETURN cloning (§13) | CFA edges, merge detection | Exits may skip join blocks; arms may end in cloned `RETURN`s; `JUMPBACK`/`FORNLOOP` may point forward |
| `expandJumps` trampolines (§13) | CFA back-edge handling | A conditional `OP -2` → `JUMPX` is not a loop |
| FOR targets are pc+1-relative (§8) | CFG FOR edges | Fission builds them from raw D, which gives the known `EXTRA_REACHING` over-approximation in the SSA oracle |
| Locals read in place; move elision (§3) | SSA, `ShouldInline`, effect ordering | An operand register may be a local read at instruction time; inlining an earlier read past a write to that register changes meaning |
| Multiple assignment conflicts (§3) | Statement lifting | `a, b = b, a` needs its temporaries; do not split into sequential assignments |
| `JUMPIFNOT*` compare (§5) | Condition negation | Negate as `not (a < b)`, never as `a >= b` |
| Constant fields live in `DUPTABLE` templates (§11) | Table reconstruction | Read template constants; a field without a store still exists |
| `SETLIST` flush before keyed items (§11) | Table coalescing | Item order in the constructor matches store order |
| Final `SETLIST C=0` (§11) | AST table reconstruction | Keep the trailing call's full result tuple; preserve earlier keyed and fixed-array stores, including the stored value of a computed key that later changes |
| FASTCALL skip region (§10) | Lifter/CFA | The fallback setup and `CALL` are one call; the skip is not a branch |
| O2 inlined returns (§12) | `joinedExit` (CFA), loop-exit phi predeclare, guarded for natural exit | An exit from the body can bypass natural-exit code and join at the return label. CFA uses the instruction after the latch to find the natural exit even when `break` also reaches it, then finds the shared join. While/repeat lift with break arms. Numeric/generic for lift the natural path under a flag that early exits set; result locals are declared before the loop. |
| `AND`/`OR` read both registers (§6) | `ShouldInline` | Never inline an effectful or raising def into the right operand of register-form `and`/`or`: that position becomes lazy |
| A register reused by a new local keeps its default name (§3) | `InputRebound` | A pure def is not inlined past a non-call write that reuses a direct operand's name |
| `x and function() end or y` (§6) | `DetectOrChain` `truthyClosure` | A tested closure folds to `true` only when the test is its sole user |
| Debug level (§1) | Fuzzer, naming | Debug 1 removes upvalue names and elides constant locals. Fuzz and test with `--debug 1` too |
| `SETLIST` elements are fresh temporaries (§11) | `LiftSetListElement`, `elementForwardRefs` | Only a phi-defined element needs the materialized local; a call element can always inline into the constructor |
| `local function f` captures its own register (§3) | Closure handlers | The self-capture alias is the closure's own name unless its value flows into a phi |
| VAL-captured locals are never reassigned (§3) | Capture pre-pass | Without debug names, a captured version whose register is reused gets its own name so loop closures stay per-iteration |

**Probes**: each section above has a semantic probe family (compile → decompile → recompile, traces compared). The last full run, on 2026-09-23, matched at O1/O2 × debug 1/2. On 2026-09-24, targeted O2 regressions also matched for numeric and generic for loops with inlined returns, a break plus an inlined nil return, and nested for loops. Remaining exceptions:
- **An if-expression condition term that tests a function literal, inside a compound condition whose body cannot be duplicated** (for example, it holds an infinite loop). The value-term folder cannot render the closure. Fuzz-only shape.
- **Expressions nested more than 64 deep.** `LiftExpression` rejects them as hostile input, for example 65+ chained method calls. Valid Luau, but rare.
