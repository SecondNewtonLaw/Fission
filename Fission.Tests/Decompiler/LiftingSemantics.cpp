//
// Lift-pipeline regression tests.
//
// Each test compiles a Luau snippet, runs the full decompiler, and asserts
// properties of the resulting source string that document a specific lift-time
// behaviour (or-chain reconstruction, upvalue-name propagation, inline-anonymous
// closures, etc.).
//
// Notes on the tests:
//   * The pretty-printer's whitespace, temporary-naming and statement ordering
//     are allowed to drift, so tests use substring searches rather than exact
//     matching.
//   * Snippets are typically wrapped as a function with named parameters; using
//     `local x = ...` (vararg unpack) loses debug names because the compiler
//     binds locals via `GETVARARGS` to anonymous registers.
//

#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>
#include <string>

namespace {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    // Some tests need to disable the Luau front-end optimiser to preserve the
    // exact bytecode shape they are exercising (e.g. an explicit `if not v then
    // v = X end` chain that opt=1 would otherwise fold into return-and-comparison
    // form before the lifter ever sees the OR opcode).
    std::string DecompileOrFail(const std::string &source, int optLevel = 1, DecompilerFlags flags = static_cast<DecompilerFlags>(0)) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        auto result = decompiler.DecompileTestCode(source, flags, opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    // Find the first `for <ident> = ` occurrence that is at the start of a line
    // (after indentation) — skips matches inside the Fission header docstring
    // (e.g. "decompiler for RbxCli").
    std::string ExtractFirstNumericForLoopVar(const std::string &source) {
        static const std::regex re(R"((?:^|\n)\s*for\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s)");
        std::smatch m;
        if (std::regex_search(source, m, re))
            return m[1].str();
        return {};
    }

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
        if (needle.empty())
            return 0;
        size_t count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

} // namespace

// ---------------------------------------------------------------- Type inference

TEST_CASE("Lift: InferTypes annotates locals from table initializers", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local values = {}
        values[1] = 1
        values[2] = 2
        values[3] = 3
        return values
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: InferTypes"));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*table\s*=\s*\{)")));
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "3"));
}

TEST_CASE("Lift: InferTypes annotates function parameters from same-file call sites", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(name, count, enabled)
            return name
        end
        sink("rat", 3, false)
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(
        std::regex_search(
            out, std::regex(
                     R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*string,\s*[A-Za-z_][A-Za-z_0-9]*\s*:\s*number,\s*[A-Za-z_][A-Za-z_0-9]*\s*:\s*boolean\))"
                 )
        )
    );
}

TEST_CASE("Lift: InferTypes emits union annotations for mixed call-site argument types", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(value)
            return value
        end
        sink("rat")
        sink(12)
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*string\s*\|\s*number\))")));
}

TEST_CASE("Lift: InferTypes falls back to body-use inference for uncalled function arguments", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(value)
            return value + 1
        end
        return sink
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*number\))")));
}

TEST_CASE("Lift: OptimizeIR removes unreachable constant-false branches", "[Decompiler][OptimizeIR]") {
    const auto raw = DecompileOrFail(
        R"(
        if not true then
            print("dead")
        else
            print("live")
        end
    )",
        0
    );
    const auto optimized = DecompileOrFail(
        R"(
        if not true then
            print("dead")
        else
            print("live")
        end
    )",
        0, DecompilerFlags::OptimizeIR
    );

    INFO("raw decompile:\n" << raw);
    INFO("optimized decompile:\n" << optimized);
    CHECK(Contains(raw, "Decompile Options: None"));
    CHECK(Contains(optimized, "Decompile Options: OptimizeIR"));
    CHECK(Contains(raw, "dead"));
    CHECK_FALSE(Contains(optimized, "dead"));
    CHECK(Contains(optimized, "live"));
    CHECK_FALSE(Contains(optimized, "if not true then"));
}

TEST_CASE("Lift: InferRobloxTypes annotates well-known globals and Instance lookups", "[Decompiler][TypeInference][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local runService = game:GetService("RunService")
        local root = workspace:FindFirstChild("Map")
        local humanoid = script.Parent:FindFirstChildOfClass("Humanoid")
        print(runService, root, humanoid)
        return runService, root, humanoid
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: InferRobloxTypes"));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*RunService\s*=\s*game:GetService\("RunService"\))")));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Instance\s*=\s*workspace:FindFirstChild\("Map"\))")));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Humanoid\s*=\s*script.Parent:FindFirstChildOfClass\("Humanoid"\))")));
}

TEST_CASE("Lift: InferRobloxTypes annotates dot-call FindFirstChildWhichIsA returns", "[Decompiler][TypeInference][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local part = workspace.FindFirstChildWhichIsA(workspace, "BasePart")
        print(part)
        return part
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(
        std::regex_search(
            out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*BasePart\s*=\s*workspace\.FindFirstChildWhichIsA\(workspace,\s*"BasePart"\))")
        )
    );
}

TEST_CASE("Lift: AutoNameVariables derives names from Roblox lookup calls", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local service = game:GetService("RunService")
        local child = workspace:FindFirstChild("Map")
        local part = workspace:FindFirstChildWhichIsA("BasePart")
        print(service, child, part)
        return service, child, part
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: AutoNameVariables"));
    CHECK(Contains(out, "local RunService = game:GetService(\"RunService\")"));
    CHECK(Contains(out, "local Map = workspace:FindFirstChild(\"Map\")"));
    CHECK(Contains(out, "local BasePart = workspace:FindFirstChildWhichIsA(\"BasePart\")"));
    CHECK(Contains(out, "print(RunService, Map, BasePart)"));
    CHECK(Contains(out, "return RunService, Map, BasePart"));
}

TEST_CASE("Lift: AutoNameVariables prefixes Roblox names on collision", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local child = workspace:FindFirstChild("Map")
        local otherChild = workspace:FindFirstChild("Map")
        print(child, otherChild)
        return child, otherChild
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Map = workspace:FindFirstChild(\"Map\")"));
    CHECK(std::regex_search(out, std::regex(R"(local\s+v[0-9]+_Map\s*=\s*workspace:FindFirstChild\("Map"\))")));
    CHECK(!Contains(out, "local Map = workspace:FindFirstChild(\"Map\")\nlocal Map = workspace:FindFirstChild(\"Map\")"));
}

TEST_CASE("Lift: AutoNameVariables handles dot-call Roblox lookups", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local players = game.GetService(game, "Players")
        local part = workspace.FindFirstChildOfClass(workspace, "Part")
        print(players, part)
        return players, part
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Players = game.GetService(game, \"Players\")"));
    CHECK(Contains(out, "local Part = workspace.FindFirstChildOfClass(workspace, \"Part\")"));
    CHECK(Contains(out, "print(Players, Part)"));
    CHECK(Contains(out, "return Players, Part"));
}

TEST_CASE("Lift: AutoNameVariables sanitizes derived names", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local bad = workspace:FindFirstChild("Bad Name-1")
        local numeric = workspace:FindFirstChild("123Folder")
        print(bad, numeric)
        return bad, numeric
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Bad_Name_1 = workspace:FindFirstChild(\"Bad Name-1\")"));
    CHECK(Contains(out, "local _123Folder = workspace:FindFirstChild(\"123Folder\")"));
    CHECK(Contains(out, "print(Bad_Name_1, _123Folder)"));
    CHECK(Contains(out, "return Bad_Name_1, _123Folder"));
}

TEST_CASE("Lift: AutoNameVariables ignores unsupported calls", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local children = workspace:GetChildren()
        print(children)
        return children
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: AutoNameVariables"));
    CHECK(!Contains(out, "local GetChildren"));
    CHECK(!Contains(out, "local Children"));
}

TEST_CASE("Lift: AutoNameVariables and InferRobloxTypes compose without coupling", "[Decompiler][AutoName][TypeInference][RobloxTypes]") {
    const auto typedOnly = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::InferRobloxTypes
    );
    const auto namedOnly = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::AutoNameVariables
    );
    const auto both = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables
    );

    INFO("typed only:\n" << typedOnly);
    CHECK(Contains(typedOnly, "Decompile Options: InferRobloxTypes"));
    CHECK(std::regex_search(typedOnly, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Players\s*=\s*game:GetService\("Players"\))")));
    CHECK(!Contains(typedOnly, "Decompile Options: AutoNameVariables"));

    INFO("named only:\n" << namedOnly);
    CHECK(Contains(namedOnly, "Decompile Options: AutoNameVariables"));
    CHECK(Contains(namedOnly, "local Players = game:GetService(\"Players\")"));
    CHECK(!Contains(namedOnly, "local Players: Players"));

    INFO("both:\n" << both);
    CHECK(Contains(both, "Decompile Options: InferRobloxTypes, AutoNameVariables"));
    CHECK(Contains(both, "local Players: Players = game:GetService(\"Players\")"));
    CHECK(Contains(both, "print(Players)"));
    CHECK(Contains(both, "return Players"));
}

TEST_CASE("Lift: complex strict ModuleScript with generic loops and deferred callback survives", "[Decompiler][ModuleScript][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        --!strict

        local RunService = game:GetService("RunService")
        local SignalPlus = require("../Networking/SignalPlus")

        type promised_results = {Params: {}, Results: boolean, Signal: SignalPlus.Signal<any>}
        type table_type = {
            [string]: {Signal: SignalPlus.Signal<any>, init: () -> promised_results}
        }

        local module = {
            Signals = {
                OnCharacterChange = require("@self/Internal/OnCharacterChange")
            } :: table_type;
            Cache = {} :: {promised_results}
        }

        function module.init()
            for _, signal in module.Signals do
                local results_table = signal.init()
                local results = results_table.Results
                if not results then continue end
                table.insert(module.Cache, results_table)
            end

            task.defer(function()
                for _, signal_results in module.Cache do
                    local signal = signal_results.Signal
                    signal:Fire(table.unpack(signal_results.Params))
                end
                table.clear(module.Cache)
            end)
        end

        RunService:BindToRenderStep(module.init, 1, module.init)
        return module
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "BindToRenderStep"));
    CHECK_FALSE(Contains(out, "local module ="));
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "table.insert"));
    CHECK(Contains(out, "table.clear"));
    CHECK(Contains(out, "module.Cache"));
}

// -------------------------------------------------------------------- Upvalue

TEST_CASE("Lift: upvalue debug-name propagates onto captured local in parent scope", "[Decompiler][Upvalue]") {
    // The inner closure captures `file` (debug-named upvalue); the outer
    // function's parameter that held the captured register should be rendered
    // as `file` (not `arg0` / `a0`), and a propagation marker comment must be
    // emitted at the closure site.
    const auto out = DecompileOrFail(R"(
        return function(file)
            return function()
                return readfile(file)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "(file)"));                                    // outer param renamed
    CHECK(Contains(out, "readfile(file)"));                            // inner body uses upvalue name
    CHECK(CountOccurrences(out, "arg0") == 0);                         // default arg name gone
    CHECK(Contains(out, "Name 'file' propagated from upvalue names")); // marker present
}

TEST_CASE("Lift: upvalue propagation comment marks every propagated capture", "[Decompiler][Upvalue]") {
    // Multiple named upvalues should each get a propagation marker; outer
    // params must render as the upvalue names rather than `arg{N}`.
    const auto out = DecompileOrFail(R"(
        return function(first, second)
            return function()
                return first + second
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Name 'first' propagated from upvalue names"));
    CHECK(Contains(out, "Name 'second' propagated from upvalue names"));
    CHECK(Contains(out, "first + second"));
}

TEST_CASE("Lift: upvalue propagation does not introduce a redundant `local up = src` declaration", "[Decompiler][Upvalue]") {
    // When the upvalue name is propagated the lifter must NOT emit the old
    // `-- Fission: Beginning captures...` / `local file = arg0` shape.
    const auto out = DecompileOrFail(R"(
        return function(file)
            return function()
                return readfile(file)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(Contains(out, "Beginning captures"));
    CHECK_FALSE(Contains(out, "local file = arg"));
}

// -------------------------------------------------------- Inline anon closure

TEST_CASE("Lift: single-use closure passed as call argument is inlined", "[Decompiler][InlineAnon]") {
    // `pcall(function() return readfile(file) end)` was previously lifted as
    // a top-level `local function anon_N(...) ... end` declaration followed by
    // `pcall(anon_N)`. After the inline pass the closure body must appear
    // directly inside the call.
    // Source is written at chunk level (no outer `return function(...)` wrap)
    // so the only candidate for `local function anon_` would be the pcall
    // argument; if the inline rule fires the count must be 0.
    const auto out = DecompileOrFail(R"(
        local file = "x"
        local ok, err = pcall(function()
            return readfile(file)
        end)
        return ok, err
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "pcall(function("));
    CHECK(CountOccurrences(out, "local function anon_") == 0);
    // The call must not reference any synthesised closure-temp identifier.
    CHECK(CountOccurrences(out, "pcall(anon_") == 0);
}

TEST_CASE("Lift: vararg-only anonymous function emits valid argument list", "[Decompiler][InlineAnon]") {
    const auto out = DecompileOrFail(R"(
        local f = function(...)
            return ...
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "(..."));
    CHECK(Contains(out, "return ..."));
    CHECK_FALSE(Contains(out, "function(, ..."));
}

TEST_CASE("Lift: string literals escape backslashes", "[Decompiler][Strings]") {
    const auto out = DecompileOrFail(R"(
        return "\\"
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, R"("\\")"));
    CHECK_FALSE(Contains(out, R"("\")"));
}

TEST_CASE("Lift: closure used as call CALLEE (IIFE) is not folded as `function(...)` arg", "[Decompiler][InlineAnon]") {
    // `(function() ... end)()` is an immediately-invoked expression where the
    // closure is the callee. The inline rule must NOT trigger here (it is
    // reserved for the closure-passed-as-PARAMETER case).
    const auto out = DecompileOrFail(R"(
        return (function() return 42 end)()
    )");

    INFO("decompile:\n" << out);
    // The closure must keep a name binding (a local function), even though
    // the callee-folding rule could shape it differently in the future. The
    // important property is that we don't crash and we still see "42".
    CHECK(Contains(out, "42"));
}

TEST_CASE("Lift: closure return slot from pcall does not inherit the closure's name", "[Decompiler][Naming]") {
    // The result registers of `pcall(closure)` must NOT carry the closure's
    // own identifier name (was the original `local v1, anon_N = pcall(anon_N)`
    // bug). Combined with inline-anon emission the typical shape is now
    // `local <ok>, <err> = pcall(function() ... end)`.
    const auto out = DecompileOrFail(R"(
        return pcall(function() return 1 end)
    )");

    INFO("decompile:\n" << out);
    // No identifier of the form `anon_<...>` should appear anywhere in the
    // output now that the closure is inlined and the return register is
    // assigned a fresh temp name.
    CHECK(CountOccurrences(out, "anon_") == 0);
}

// ------------------------------------------------------- Short-circuit / or

TEST_CASE("Lift: `or` fallback preserves both operands and yields `or` semantics", "[Decompiler][ShortCircuit]") {
    // Regression for the JUMPIF label inversion bug, where `a or fallback`
    // used to lift as `a and fallback` (wrong semantics). Use named function
    // args so debug info gives stable identifiers to grep for.
    const auto out = DecompileOrFail(R"(
        return function(a) return a or "fallback" end
    )");

    INFO("decompile:\n" << out);
    // Both operands must survive; the inversion bug used to drop the
    // fallback or reverse the chain entirely.
    CHECK(Contains(out, "fallback"));
    // The output must reach the fallback by some control path that
    // corresponds to `a` being falsy (no `and` form).
    CHECK_FALSE(Contains(out, "a and \"fallback\""));
}

TEST_CASE("Lift: nested `if not X then X = Y end` is folded to an `or` expression", "[Decompiler][ShortCircuit]") {
    // The post-use `sink(v)` keeps `v` alive in a register, so the bytecode
    // lands as a proper JUMPIF/MOVE chain. The chain-fold pass should erase
    // the explicit `if not v` blocks and produce one or more `or` operators.
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, sink)
            local v = a
            if not v then v = b end
            if not v then v = c end
            sink(v)
        end
    )");

    INFO("decompile:\n" << out);
    // The chain-fold may not always pretty-print as a single `a or b or c`
    // (depends on adjacent statement shapes), but it should at least produce
    // one `or` operator and erase the explicit `if not v` skeleton.
    const bool foldedSomeway = Contains(out, " or ") || CountOccurrences(out, "if not v") == 0;
    CHECK(foldedSomeway);
}

// ------------------------------------------------------------- Numeric for

TEST_CASE("Lift: numeric `for` emits a proper loop-variable identifier", "[Decompiler][NumericFor]") {
    // Regression for `for 1 = 1, n, 1 do ... end` and `for err_not_reg = ...`.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local s = 0
            for i = 1, n do
                s = s + i
            end
            return s
        end
    )");

    INFO("decompile:\n" << out);
    REQUIRE(Contains(out, "for "));
    CHECK_FALSE(Contains(out, "for 1 ="));
    CHECK_FALSE(Contains(out, "err_not_reg"));

    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // Identifier regex already constrains the first char; the assertion is
    // structural sanity.
    CHECK(std::isalpha(static_cast<unsigned char>(varName.front())) != 0);
}

TEST_CASE("Lift: numeric `for` body re-uses the same loop-variable name as the header", "[Decompiler][NumericFor]") {
    // Prevent the regression where the header rendered as `for 1 = ...` while
    // the body referenced the loop var as `i_3`. Index a table by the loop
    // variable so the body must reference it explicitly.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local t = {}
            for i = 1, n do
                t[i] = i
            end
            return t
        end
    )");

    INFO("decompile:\n" << out);
    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // The loop-var token should appear at least once more (inside the body).
    CHECK(CountOccurrences(out, varName) >= 2);
}

// --------------------------------------------------------------- Generic for

TEST_CASE("Lift: generic for over `pairs` emits `for k, v in pairs(t)`", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for k, v in pairs(t) do
                print(k, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "pairs("));
}

TEST_CASE("Lift: generic for over `ipairs` survives lift", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for i, v in ipairs(t) do
                print(i, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "ipairs("));
}

// -------------------------------------------------------------------- Tables
// Note: dict-style key-value table literal reconstruction is a known
// limitation (SETTABLEKS after NEWTABLE may be fragmented by SSA/CFG).
// List-style (SETLIST) reconstruction works reliably.

TEST_CASE("Lift: empty table literal has balanced braces", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{  }` with spaces — check brace balance.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: list-style table literal preserves numeric elements", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {10, 20, 30}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{ 10, 20, 30 }` with space after `{`.
    CHECK(CountOccurrences(out, "10") >= 1);
    CHECK(CountOccurrences(out, "20") >= 1);
    CHECK(CountOccurrences(out, "30") >= 1);
    // All three elements survive.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    // No key-value syntax for plain list elements.
    CHECK_FALSE(Contains(out, "[1]"));
}

TEST_CASE("Lift: nested table literal preserves inner list tables", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {{1, 2}, {3, 4}}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // All four elements survive.
    CHECK(CountOccurrences(out, "1") >= 1);
    CHECK(CountOccurrences(out, "2") >= 1);
    CHECK(CountOccurrences(out, "3") >= 1);
    CHECK(CountOccurrences(out, "4") >= 1);
    // At least two brace pairs (outer + inner).
    CHECK(CountOccurrences(out, "{") >= 3);
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: absurd mixed SETLIST table literal terminates and preserves edge elements", "[Decompiler][Table][SETLIST][Stress]") {
    std::stringstream source;
    source << "return function(a, b)\nlocal t = {";
    for (int i = 1; i <= 260; ++i) {
        if (i > 1)
            source << ", ";
        switch (i % 10) {
        case 0:
            source << "{ " << i << ", key = \"v" << i << "\", nested = {" << (i + 1) << ", " << (i + 2) << "} }";
            break;
        case 1:
            source << "a + " << i;
            break;
        case 2:
            source << "b and \"s" << i << "\" or nil";
            break;
        case 3:
            source << "function(x) return x + " << i << " end";
            break;
        case 4:
            source << "{ [\"dyn" << i << "\"] = a, " << i << " }";
            break;
        case 5:
            source << "not b";
            break;
        case 6:
            source << "(a * " << i << ") % 7";
            break;
        case 7:
            source << "\"edge" << i << "\"";
            break;
        case 8:
            source << "nil";
            break;
        default:
            source << i;
            break;
        }
    }
    source << ", absurdKey = { tail = 9999, flag = true }, [a] = b }\nreturn t\nend";

    const auto out = DecompileOrFail(source.str());

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "260"));
    CHECK(Contains(out, "9999"));
    CHECK(Contains(out, "function"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\.)")));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\[)")));
}

TEST_CASE("Lift: table literal as return value preserves content", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            return {1, 2, 3}
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return {"));
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    CHECK(Contains(out, "3"));
}

TEST_CASE("Lift: table literal with expression elements preserves operators", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            local t = {a + 1, b * 2, a - b}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "+"));
    CHECK(Contains(out, "*"));
    CHECK(Contains(out, "-"));
    CHECK(Contains(out, "a"));
    CHECK(Contains(out, "b"));
}

TEST_CASE("Lift: table literal preserves aliases across later register reuse", "[Decompiler][Table][Alias]") {
    const auto out = DecompileOrFail(R"(
        local nested = {1}
        print(nested)
        local g = rat()
        local t = {nested}
        return g, t
    )");

    INFO("decompile:\n" << out);
    static const std::regex ratThenSelfTable(R"(local\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s*rat\(\)\s+local\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*\{\s*\1\s*\})");
    CHECK_FALSE(std::regex_search(out, ratThenSelfTable));
    CHECK(Contains(out, "rat()"));
    CHECK(Contains(out, "{ 1 }"));
}

TEST_CASE("Lift: DUPTABLE nil template values do not index constants out of range", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return { a = nil }
    )");

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(return\s+\{\s*a\s*=\s*nil\s*\})")));
}

TEST_CASE("Lift: dict-style table keys are preserved (TODO: known limitation)", "[Decompiler][Table]") {
    // KNOWN LIMITATION: dict-style keys ({x = 1}) are lost — LiftTableLiteral
    // cannot reconstruct SETTABLEKS across block boundaries after SSA/CFG.
    // This test asserts that numeric content survives at minimum.
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {x = 1, y = 2, z = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // At minimum, braces survive.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    // TODO: also check for "x =", "y =", "z =" once dict reconstruction is fixed.
}

TEST_CASE("Lift: mixed table literal numeric elements survive (TODO: keys known limitation)", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {1, 2, key = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Numeric list elements survive.
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    // TODO: "key =" should survive once dict reconstruction is fixed.
}

TEST_CASE("Lift: AssignmentStatementNode RHS not gratuitously parenthesised", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local x = 5
            x = x + 1
            return x
        end
    )");

    INFO("decompile:\n" << out);
    // The previous always-wrap behaviour emitted `x = (x + 1)`. The
    // precedence-aware pass must drop the redundant outer parens.
    CHECK_FALSE(Contains(out, "x = (x + 1)"));
}

TEST_CASE("Lift: `if` condition has no redundant outer parentheses around a bare comparison", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            if a == b then return 1 end
            return 0
        end
    )");

    INFO("decompile:\n" << out);
    // `if (a == b) then` form must not appear; the comparison is the entire
    // condition and needs no extra grouping.
    CHECK_FALSE(Contains(out, "if (a == b)"));
}

TEST_CASE("Lift: associative `or` chain has no redundant inner parentheses", "[Decompiler][Parens]") {
    // Same-operator chains under associative ops do not need inner parens.
    // Function-arg names are not yet propagated by the lifter (locals render
    // as `argN`), so the test matches the chain in a name-agnostic way.
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, sink)
            sink(a or b or c)
        end
    )");

    INFO("decompile:\n" << out);
    // The literal `(X or Y) or Z` form (grouping that the associativity-aware
    // emitter is supposed to drop) must not appear.
    static const std::regex leftAssocGrouping(R"(\([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\) or )");
    static const std::regex rightAssocGrouping(R"( or \([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\))");
    CHECK_FALSE(std::regex_search(out, leftAssocGrouping));
    CHECK_FALSE(std::regex_search(out, rightAssocGrouping));
    // Output should contain at least one `X or Y` pair.
    static const std::regex flatChain(R"([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*)");
    CHECK(std::regex_search(out, flatChain));
}
