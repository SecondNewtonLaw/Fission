//
// Created by Dottik on 2/6/2026.
//
// Complex control-flow stress tests (deep if/elseif chains, nested loops, break/continue).
//

#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
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

    bool ContainsRegex(const std::string &haystack, const std::regex &pattern) { return std::regex_search(haystack, pattern); }

    size_t CountSubstr(const std::string &hay, const std::string &needle) {
        size_t n = 0;
        for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size()))
            ++n;
        return n;
    }

    // Count statement keywords as whole words (rough, but good enough for body checks).
    size_t CountWord(const std::string &hay, const std::string &word) {
        size_t n = 0;
        std::regex re("\\b" + word + "\\b");
        for (auto it = std::sregex_iterator(hay.begin(), hay.end(), re); it != std::sregex_iterator(); ++it)
            ++n;
        return n;
    }

} // namespace

// =========================================================================
// if / elseif chains
// =========================================================================

// Plain 4-way `==` chain: every body present exactly once, else present.
TEST_CASE("CCF: four-way elseif chain preserves every branch once", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(x)
            if x == 1 then
                fa()
            elseif x == 2 then
                fb()
            elseif x == 3 then
                fc()
            else
                fd()
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "fa(") == 1);
    CHECK(CountSubstr(out, "fb(") == 1);
    CHECK(CountSubstr(out, "fc(") == 1);
    CHECK(CountSubstr(out, "fd(") == 1);
}

// An assignment-bodied elseif chain compiles to nested *negated* ifs; the AST
// flattener must invert them back into a readable `elseif` chain rather than a
// `local v = a and b or c` lowers to a diamond (`if not a then v=c else v=b; if v
// then ... end end`); the short-circuit folder must collapse it back to the single
// expression rather than leaving the nested-if form.
TEST_CASE("CCF: a-and-b-or-c folds to a short-circuit expression", "[Decompiler][ShortCircuit]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b, c)
            local x = a and b or c
            print(x)
        end
        return f
    )",
        2);
    INFO("decompile:\n" << out);
    // One folded expression, not a staircase of ifs assigning x.
    CHECK(ContainsRegex(out, std::regex(R"(=\s*\w+\s+and\s+\w+\s+or\s+\w+)")));
    CHECK(CountWord(out, "if") == 0);
}

// The same, with a method-call middle term (the ShouldUseVehicleCamera isSubj
// shape): `cs and cs:IsA("X") or false`.
TEST_CASE("CCF: and-call-or folds with a namecall middle term", "[Decompiler][ShortCircuit]") {
    const auto out = DecompileOrFail(R"(
        local function f(self, cs)
            self.x = cs and cs:IsA("VehicleSeat") or false
        end
        return f
    )",
        2);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(and\s+\w+:IsA\("VehicleSeat"\)\s+or\s+false)")));
}

// staircase of `if v ~= A then if v ~= B then ...`.
TEST_CASE("CCF: assignment elseif chain is flattened to elseif", "[Decompiler][ControlFlow][If]") {
    // A movement-mode-style chain: branches assign a local consumed by a LATER
    // conditional block (mirrors ActivateCameraController). This yields the nested
    // negated-if shape the flattener targets, without tail-duplicating post-chain
    // code into every branch.
    const auto out = DecompileOrFail(R"(
        local function f(x, flag)
            local mode = 0
            if x == 1 then
                mode = 10
            elseif x == 2 then
                mode = 20
            elseif x == 3 then
                mode = 30
            elseif x == 4 then
                mode = 40
            else
                mode = 99
            end
            if flag then
                use(mode)
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    // The chain is surfaced as `elseif` rather than a staircase of nested ifs
    // (without flattening there would be zero `elseif`s).
    CHECK(CountWord(out, "elseif") >= 2);
    // The leading links use positive equality (`== 1`, `== 2`), not negated tests.
    CHECK(ContainsRegex(out, std::regex(R"(elseif\s+\w+\s*==\s*2)")));
    for (const char *v : {"10", "20", "30", "40", "99"})
        CHECK(CountSubstr(out, v) >= 1);
}

// The report's ActivateCameraController dispatch: an `or`-grouped elseif must
// only fire for its three values, NOT become a fallthrough that also runs for
// the unhandled `else` (warn) case.
TEST_CASE("CCF: or-grouped elseif does not leak into the else branch", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local creator = nil
            if t == "Scriptable" then
                return
            elseif t == "Custom" then
                creator = pickCustom()
            elseif t == "Track" then
                creator = pickTrack()
            elseif t == "Attach" or t == "Watch" or t == "Fixed" then
                creator = legacy()
            else
                warn("unhandled", t)
            end
            use(creator)
        end
        return f
    )");
    INFO("decompile:\n" << out);
    // legacy() is reachable only from the or-group, so it appears exactly once
    // and warn() exactly once; neither duplicated nor merged.
    CHECK(CountSubstr(out, "legacy(") == 1);
    CHECK(CountSubstr(out, "warn(") == 1);
    CHECK(CountSubstr(out, "pickCustom(") == 1);
    CHECK(CountSubstr(out, "pickTrack(") == 1);
    // The or-group's three string comparisons all survive.
    CHECK(ContainsRegex(out, std::regex(R"("Attach")")));
    CHECK(ContainsRegex(out, std::regex(R"("Watch")")));
    CHECK(ContainsRegex(out, std::regex(R"("Fixed")")));
}

// elseif chain with trailing NO-OP branches (report cat 10: DevTouchMovementMode
// etc.). The no-ops must not steal the action from a sibling branch.
TEST_CASE("CCF: elseif chain with no-op tail branches", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(name)
            if name == "Mode" then
                doMode()
            elseif name == "Occlusion" then
                doOcclusion()
            elseif name == "Zoom" then
                doZoom()
            elseif name == "A" then
            elseif name == "B" then
            elseif name == "C" then
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "doMode(") == 1);
    CHECK(CountSubstr(out, "doOcclusion(") == 1);
    CHECK(CountSubstr(out, "doZoom(") == 1);
    // The Occlusion action must be guarded by its own name, not run for "Mode".
    CHECK(ContainsRegex(out, std::regex(R"("Occlusion")")));
}

// Nested CameraMode handling (report cat 10 CameraMode sub-tree): if inside the
// first elseif arm, with its own elseif/else.
TEST_CASE("CCF: nested if inside an elseif arm", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(prop, mode)
            if prop == "CameraMode" then
                if mode == "LockFirstPerson" then
                    lock()
                elseif mode == "Classic" then
                    classic()
                else
                    warn("bad", mode)
                end
            elseif prop == "Zoom" then
                zoom()
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "lock(") == 1);
    CHECK(CountSubstr(out, "classic(") == 1);
    CHECK(CountSubstr(out, "warn(") == 1);
    CHECK(CountSubstr(out, "zoom(") == 1);
}

// Three-deep nested if/else, each level branching both ways.
TEST_CASE("CCF: three-deep nested if/else keeps all leaves", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b, c)
            if a then
                if b then
                    if c then leaf1() else leaf2() end
                else
                    if c then leaf3() else leaf4() end
                end
            else
                if b then leaf5() else leaf6() end
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    for (int i = 1; i <= 6; ++i)
        CHECK(CountSubstr(out, "leaf" + std::to_string(i) + "(") == 1);
}

// elseif chain that returns a different value per branch (phi at the join).
TEST_CASE("CCF: elseif chain returning per-branch values", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(x)
            local r
            if x == 1 then
                r = "one"
            elseif x == 2 then
                r = "two"
            elseif x == 3 then
                r = "three"
            else
                r = "other"
            end
            return r
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"("one")")));
    CHECK(ContainsRegex(out, std::regex(R"("two")")));
    CHECK(ContainsRegex(out, std::regex(R"("three")")));
    CHECK(ContainsRegex(out, std::regex(R"("other")")));
    // Each value assigned/returned exactly once (no duplicated branch).
    CHECK(CountSubstr(out, "\"three\"") == 1);
}

// Mixed `and`/`or` compound conditions across elseif arms.
TEST_CASE("CCF: compound boolean conditions in elseif arms", "[Decompiler][ControlFlow][If]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b, c)
            if a and b then
                p()
            elseif a or c then
                q()
            elseif not a and not b and not c then
                r()
            else
                s()
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    // Each branch body must be reachable. (An `or` condition may legitimately
    // emit its body once per disjunct, so assert presence, not an exact count.)
    CHECK(CountSubstr(out, "p(") >= 1);
    CHECK(CountSubstr(out, "q(") >= 1);
    CHECK(CountSubstr(out, "r(") >= 1);
    CHECK(CountSubstr(out, "s(") >= 1);
}

// =========================================================================
// Loops with nested control flow
// =========================================================================

// Nested while inside while; inner break exits only the inner loop.
TEST_CASE("CCF: nested while with inner break", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(n, m)
            local i = 0
            while i < n do
                local j = 0
                while j < m do
                    if cond(i, j) then
                        break
                    end
                    inner(i, j)
                    j = j + 1
                end
                outer(i)
                i = i + 1
            end
            done()
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "inner(") == 1);
    CHECK(CountSubstr(out, "outer(") == 1);
    CHECK(CountSubstr(out, "done(") == 1);
    // Exactly one break (the inner one); it must not be duplicated to the outer.
    CHECK(CountWord(out, "break") == 1);
}

// repeat-until with an if/break inside the body. A `repeat ... until` with a
// mid-body break is rendered as the equivalent `while true do ... if c break end`
// — the key requirement is that the mid-body `break` keeps its position so `step`
// is NOT run on the break iteration (it used to fold the break into the loop
// condition and re-order it ahead of `step`).
TEST_CASE("CCF: repeat-until with conditional break", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local x = 0
            repeat
                x = x + 1
                if x == 5 then
                    break
                end
                step(x)
            until x >= 10
            return x
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(while|repeat)")));
    CHECK(CountSubstr(out, "step(") == 1);
    // The `x == 5` break must appear BEFORE `step`, so step is skipped on break.
    CHECK(ContainsRegex(out, std::regex(R"(==\s*5[\s\S]*break[\s\S]*step\()")));
    // Both the mid-break and the loop's exit test survive as breaks.
    CHECK(CountWord(out, "break") == 2);
}

// while containing a nested repeat-until.
// The statements after the inner `repeat ... until` (`tail(i)`, `i = i + 1`) must
// stay in the while body, after the repeat — not be pulled into the repeat body.
// (Was a degenerate loopExit==latch making the exit block look like the body.)
TEST_CASE("CCF: while with a nested repeat-until", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(n)
            local i = 0
            while i < n do
                local k = 0
                repeat
                    k = k + 1
                    body(i, k)
                until k >= 3
                tail(i)
                i = i + 1
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(repeat)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bbody\b)")));
    // `tail` (a post-repeat statement) must appear AFTER the `until`, not before.
    CHECK(ContainsRegex(out, std::regex(R"(until[\s\S]*tail)")));
}

// numeric for with a nested while and a conditional continue/return.
TEST_CASE("CCF: numeric-for with nested while and early return", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for i = 1, #t do
                local v = t[i]
                while v > 0 do
                    if v == 13 then
                        return i
                    end
                    v = v - 1
                end
                mark(i)
            end
            return -1
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(while)")));
    CHECK(CountSubstr(out, "mark(") == 1);
}

// while-true with two break conditions (compound loop exit).
TEST_CASE("CCF: while-true with multiple break conditions", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b)
            while true do
                work()
                if a() then
                    break
                end
                if b() then
                    break
                end
                more()
            end
            after()
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "work(") == 1);
    CHECK(CountSubstr(out, "more(") == 1);
    // after() is post-loop and must appear exactly once (not inlined per break).
    CHECK(CountSubstr(out, "after(") == 1);
    // At least one real break survives; the canonical form may fold the first exit
    // into the loop condition (`while not a() do`), leaving the rest as `break`.
    CHECK(CountWord(out, "break") >= 1);
}

// Deeply nested loop + conditionals: for > while > if/elseif with break.
TEST_CASE("CCF: for over while over elseif with break", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(rows)
            for i = 1, rows do
                local j = 0
                while j < 10 do
                    if j == 3 then
                        a()
                    elseif j == 7 then
                        b()
                        break
                    else
                        c()
                    end
                    j = j + 1
                end
                d(i)
            end
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(CountSubstr(out, "a(") == 1);
    CHECK(CountSubstr(out, "b(") == 1);
    CHECK(CountSubstr(out, "c(") == 1);
    CHECK(CountSubstr(out, "d(") == 1);
    CHECK(CountWord(out, "break") == 1);
}

// repeat-until with a compound `or` exit condition.
TEST_CASE("CCF: repeat-until with compound or condition", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local x = 0
            repeat
                x = x + 1
                tick(x)
            until x >= 10 or stop()
            return x
        end
        return f
    )");
    INFO("decompile:\n" << out);
    // A `repeat ... until A or B` may come back as an equivalent `while` form;
    // accept either as long as it is a loop and the body and exit survive.
    CHECK(ContainsRegex(out, std::regex(R"(repeat|while)")));
    CHECK(CountSubstr(out, "tick(") == 1);
    CHECK(CountSubstr(out, "stop(") == 1);
}

// Generic for (pairs) with a nested if that conditionally continues.
TEST_CASE("CCF: generic-for with conditional skip", "[Decompiler][ControlFlow][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for k, v in pairs(t) do
                if v == nil then
                    continue
                end
                if v == "stop" then
                    break
                end
                visit(k, v)
            end
            finish()
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(pairs\()")));
    CHECK(CountSubstr(out, "visit(") == 1);
    CHECK(CountSubstr(out, "finish(") == 1);
    CHECK(CountWord(out, "break") == 1);
}
