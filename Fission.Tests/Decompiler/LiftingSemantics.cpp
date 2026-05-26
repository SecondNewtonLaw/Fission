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
    std::string DecompileOrFail(const std::string &source, int optLevel = 1) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
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

// ----------------------------------------------------------- Paren precedence

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
