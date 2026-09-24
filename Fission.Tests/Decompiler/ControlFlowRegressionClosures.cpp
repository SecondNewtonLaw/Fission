//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "ControlFlowTestSupport.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeadLocalEliminator.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/PropertyRenamer.hpp"
#include "Rewriters/ReverseFieldRenamer.hpp"
#include "Rewriters/ScopeAwareRenamer.hpp"
#include "Rewriters/ScopeBlockIntroducer.hpp"
#include "Rewriters/SelfAssignmentEliminator.hpp"
#include "SourceGenerator/Generator.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
#include <string>

using namespace control_flow_test;

TEST_CASE("Regress: phi-merged call result is not re-declared with local", "[Decompiler][Phi][Regression]") {
    // `cs and cs:IsA(...) or false` lowers so the method-call result lands directly
    // in the merge register (no temp), exercising the phi-hoist of a Call/NameCall
    // return target. `print` after forces the value to escape the branches.
    const auto out = DecompileOrFail(
        R"(
        return function(cs)
            local isSubj = cs and cs:IsA("VehicleSeat") or false
            print(isSubj)
        end
    )",
        2
    );

    INFO("decompile:\n" << out);
    // The method-call branch must NOT re-declare the merge variable with `local`
    // (that would shadow the hoisted merge variable and lose the value).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+\w+\s*=\s*\w+:IsA\()")));
}

// Comparison materialised into a boolean register
// Known bug: `local x = a ~= b` (a comparison stored as a boolean value, not
// used directly as a branch condition) lowered to a garbled
// `if a == b then local x = x end` with a self-assign and no real value.
// It must reconstruct the comparison expression itself.

TEST_CASE("Regress: comparison stored as boolean value is reconstructed", "[Decompiler][ShortCircuit][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        return function(self)
            local x = self.occlusionMode ~= "Invisicam"
            print(x)
        end
    )",
        2
    );

    INFO("decompile:\n" << out);
    // No self-assign garbage.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\b(v\d+|arg\d+)\s*=\s*\1\b)")));
    // The comparison must be materialised as a value.
    CHECK(ContainsRegex(out, std::regex(R"(=\s*\w+\.occlusionMode\s*~=\s*"Invisicam")")));
}

// Numeric for-loop with both `continue` and `break`
// Known bug: the break edge inlined the post-loop code (and leaked loop control
// registers), and the `continue` path was emitted as a spurious `break`. The
// loop must keep one real `break`, no stray return, and the post-loop statement
// must appear after the loop.

TEST_CASE("Regress: numeric for with continue and break", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t, limit)
            for i = 1, #t do
                local v = t[i]
                if v > limit then
                    continue
                end
                if v == 0 then
                    break
                end
                print(v)
            end
            print("done")
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*=)")));
    // The body's guarded statement and the real break both survive.
    CHECK(ContainsRegex(out, std::regex(R"(\bbreak\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(print\(v\d+\))")));
    // The post-loop statement must be present (not swallowed into the break path).
    CHECK(ContainsRegex(out, std::regex(R"(print\("done"\))")));
    // Exactly one break; the `continue` path must NOT have become a second break.
    size_t breakCount = 0;
    for (size_t p = out.find("break"); p != std::string::npos; p = out.find("break", p + 1))
        ++breakCount;
    CHECK(breakCount == 1);
}

// `x = X and (...)` must reassign, not shadow with a nested `local`
// Known bug: `local s = a and (b or c)` lowered to `local s = a; if s then
// local s ... end`; the inner `local s` shadowed the outer, so the post-merge
// read saw `a` instead of the computed value. The inner store must reassign.

TEST_CASE("Regress: and-chain conditional reassign does not shadow", "[Decompiler][Phi][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(cm)
            local activeSensor = cm.ActiveController and
                ((cm.ActiveController:IsA("GroundController") and cm.GroundSensor) or
                 (cm.ActiveController:IsA("ClimbController") and cm.ClimbSensor))
            if activeSensor and activeSensor.SensedPart then
                return activeSensor.SensedPart
            end
            return nil
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // The sensor variable is declared once (from ActiveController) and reassigned.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+(\w+)\s*=\s*\w+\.ActiveController)")));
    // The conditional update must be a bare reassignment, never a shadowing `local`.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(then\s*\n\s*local\s+\w+\s*\n)")));
}

// `~=` / `==` equality-jump branch polarity
// Known bug: JUMPXEQK with the not-flag clear (compiled from a `~=` source test)
// had ifStatementTrue/False swapped, so `if x ~= "b" then return 10 end` came
// back as `if x == "b" then return 10`, inverting the branch.

TEST_CASE("Regress: not-equal if-branch keeps correct polarity", "[Decompiler][Branch][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function g(x)
            if x ~= "b" then
                return 10
            end
            return 20
        end
        return g
    )");

    INFO("decompile:\n" << out);
    // The inverted form `if x == "b" then return 10` must NOT appear.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(==\s*"b"\s*then\s*return\s+10)")));
    // 10 must be the `~=` result (either guarded by ~=, or 20 guarded by ==).
    const bool correctPolarity =
        ContainsRegex(out, std::regex(R"(~=\s*"b"\s*then\s*return\s+10)")) || ContainsRegex(out, std::regex(R"(==\s*"b"\s*then\s*return\s+20)"));
    CHECK(correctPolarity);
}

// Module-table constructor must not absorb non-inlinable field values
// Known bug: `local t = {}; function t.foo() ... end` was reconstructed as a
// fabricated literal `{ foo = v5 }` (every field aliased to a single reused
// register) and the closure leaked as a standalone global `anon_# = function`.
// The table must stay empty and the closure inline into the field store.

// A table-field closure on a module table (`local t = {}; function t.foo()`) is
// sugared back to dot-declaration form `function t.foo(...) ... end` by the
// ClassMethodRewriter, NOT left as a `t.foo = function()`/`t.foo = foo` pair.
// Dot form has no `self` slot, so the parameter list is untouched and the
// rewrite is always recompilable.
TEST_CASE("Regress: module table-field closure is sugared to dot declaration", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = {}
        function t.foo()
            return 42
        end
        return t
    )");

    INFO("decompile:\n" << out);
    // Closure body survives.
    CHECK(ContainsRegex(out, std::regex(R"(return\s+42)")));
    // Emitted as a dot declaration bound straight to the field.
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+\.foo\s*\()")));
    // Not split into a separate decl + field assignment.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\.foo\s*=)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+function\s+foo\b)")));
}

// Dot sugar on a module table must NOT absorb a `self` slot: every declared
// parameter stays in the visible signature (unlike the colon path). A two-arg
// module function keeps two args.
TEST_CASE("Regress: module dot-method keeps all parameters", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local m = {}
        function m.add(a, b)
            return a + b
        end
        return m
    )");

    INFO("decompile:\n" << out);
    // Two distinct params survive in the signature; no `self`, no param shift.
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+\.add\s*\(\s*\w+\s*,\s*\w+\s*\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\bself\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:add\b)")));
}

// ClassMethodRewriter: a class table (`X.__index = X`) with a colon-method
// field closure is sugared back to `function X:method(...)`, absorbing the
// implicit first parameter as `self` and shifting the remaining params down.
TEST_CASE("Regress: class colon-method is sugared to method syntax", "[Decompiler][Class][Regression]") {
    const auto out = DecompileOrFail(R"(
        local Animal = {}
        Animal.__index = Animal
        function Animal:speak(volume)
            return self.name, volume
        end
        return Animal
    )");

    INFO("decompile:\n" << out);
    // Emitted in colon form; the implicit self is absorbed and the one explicit
    // param is shifted down into the visible signature (two args -> one).
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+:speak\s*\(\s*\w+\s*\))")));
    // The body references self (the absorbed first arg), not a leaked arg0 name.
    CHECK(ContainsRegex(out, std::regex(R"(\bself\b)")));
    // The class table marker survives the rewrite.
    CHECK(ContainsRegex(out, std::regex(R"(\.__index\s*=)")));
    // Not left as a plain field-assigned function literal.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\.speak\s*=\s*function\b)")));
}

// A table-constructor scan must not absorb a SETLIST/SETTABLE targeting a *later*
// table that merely reuses the same register. `setmetatable({}, mt)` followed by
// `self.list = {1,2,3}` must keep the empty `{}` empty.
TEST_CASE("Regress: empty constructor does not absorb a reused-register table", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(mt)
            local self = setmetatable({}, mt)
            self.list = {1, 2, 3}
            return self
        end
        return f
    )");

    INFO("decompile:\n" << out);
    // The list assignment keeps its real values, not fabricated register names.
    CHECK(ContainsRegex(out, std::regex(R"(\.list\s*=\s*\{\s*1\s*,\s*2\s*,\s*3\s*\})")));
    // The metatable's table must stay empty: the list must NOT have been absorbed
    // into the setmetatable argument (the reused-register bug).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(setmetatable\(\s*\{\s*1\s*,)")));
    // An empty constructor `{}` is present (the metatable subject).
    CHECK(ContainsRegex(out, std::regex(R"(\{\s*\})")));
}

// A constructor with only inlinable (constant) values must still coalesce into
// a single `{ ... }` literal; the guard above must not over-trigger.
TEST_CASE("Regress: constant table constructor still coalesces", "[Decompiler][Table][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local t = { a = 1, b = 2, c = "x" }
            return t
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\{[^}]*a\s*=\s*1[^}]*\})")));
}

// Nested REF upvalue capture must alias, not redefine
// A variable captured by reference (mutated across scopes) is ONE shared variable.
// Rendering the capture as `local uv_N = source` makes a by-value copy: writes
// inside the closure no longer alias the outer variable. It must keep a single
// consistent name across all nesting levels with no redefinition.
TEST_CASE("Regress: nested REF upvalue capture aliases instead of redefining", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no debug names -> exercises the autogenerated-name path
    const std::string source = R"(
        local value = _G.globalValue
        local function f()
            print(value)
            local function f1()
                print(value)
                value = _G.globalValue
            end
            f1()
            value = _G.globalValue
        end
        f()
        value = _G.globalValue
        return f
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // No by-value capture copy (`local uv_N = ...`); that would desync the writes.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+uv_\d+\s*=)")));
    // The shared variable is declared once, BEFORE the closure that captures it.
    std::smatch valueDecl;
    REQUIRE(std::regex_search(out, valueDecl, std::regex(R"(local\s+(\w+)\s*=\s*_G\.globalValue)")));
    const size_t closurePos = out.find("local function f");
    REQUIRE(closurePos != std::string::npos);
    CHECK(static_cast<size_t>(valueDecl.position(0)) < closurePos);
    // Exactly one declaration of it; the trailing write is an assignment, not a 2nd local.
    CHECK(CountOccurrences(out, "local " + valueDecl[1].str() + " ") == 1);
}

// A LUA_TINTEGER constant only reaches the bytecode via a library-member-constant
// fold callback (plain literals are f64). Inject one at compile time so the whole
// pipeline (compile -> deserialize -> lift -> source) is exercised.
TEST_CASE("Constants: integer constant decompiles with the `i` suffix", "[Decompiler][Constants]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    static const char *const knownLibraries[] = {"Integers", nullptr};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 2; // library-K folding needs O2
    opts.debugLevel = 1;
    opts.librariesWithKnownMembers = knownLibraries; // enables the member-constant fold for `Integers`
    opts.libraryMemberConstantCb = [](const char *library, const char *member, Luau::CompileConstant *constant) {
        if (std::strcmp(library, "Integers") == 0 && std::strcmp(member, "Big") == 0)
            Luau::setCompileConstantInteger64(constant, 82199292);
    };
    const std::string source = R"(
        return function()
            return Integers.Big
        end
    )";
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "82199292i"));
}

TEST_CASE("Constants: vector constants emit in expr and table positions, not silent nil", "[Decompiler][Constants]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 2;
    opts.debugLevel = 1;
    const std::string source = R"(
        return function()
            local v = vector.create(1, 2, 3)
            local t = { vector.create(4, 5, 6), vector.create(7, 8, 9) }
            return v, t
        end
    )";
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Vector3.new(1, 2, 3)")); // LOADK vector
    CHECK(Contains(out, "Vector3.new(4, 5, 6)")); // vector inside a constant table
    CHECK(Contains(out, "Vector3.new(7, 8, 9)"));
    CHECK_FALSE(Contains(out, "nil")); // no constant silently dropped to nil
}

// OmitFissionComments suppresses informational comments but keeps warnings
TEST_CASE("Option: OmitFissionComments drops info comments, keeps warnings", "[Decompiler][Options]") {
    EnableLuauFFlagsOnce();
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    const std::string source = R"(
        local function outer()
            local shared = {}
            local function inner()
                shared.flag = true
                local other = {}
                other.x = 1
                print(other)
                return other
            end
            return inner
        end
        return outer
    )";

    // Default: Fission's informational comments are present. (Fresh Decompiler per
    // call; the generator's buffer is a reused member that accumulates otherwise.)
    Decompiler d1{};
    const auto withInfo = d1.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(withInfo.resultCode == DecompileResult::Success);
    CHECK(Contains(withInfo.decompilationOutput, "Fission ~~ Function Information"));
    CHECK(Contains(withInfo.decompilationOutput, "Fission: INFO:"));

    // With the flag: per-function info blocks and INFO/capture notes are gone.
    Decompiler d2{};
    const auto noInfo = d2.DecompileTestCode(source, DecompilerFlags::OmitFissionComments, opts);
    REQUIRE(noInfo.resultCode == DecompileResult::Success);
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Fission ~~ Function Information"));
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Fission: INFO:"));
    CHECK_FALSE(Contains(noInfo.decompilationOutput, "Beginning captures"));
    // The code itself still decompiles (the function is still there).
    CHECK(Contains(noInfo.decompilationOutput, "function"));
}

// A nested function's own vN must not shadow a captured upvalue of the same name
// A nested function reuses low register numbers, so its own `vN` can equal a
// captured upvalue that (after aliasing) reads as the enclosing scope's `vN`.
// Rendered inline, the inner `local vN` then shadows the upvalue. The inner's own
// auto-name must be disambiguated (suffixed) so it stays distinct.
TEST_CASE("Regress: nested function own vN does not shadow captured upvalue vN", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names; the collision surfaces
    const std::string source = R"(
        local function outer()
            local shared = {}
            local function inner()
                shared.flag = true
                local other = {}
                other.x = 1
                print(other)
                return other
            end
            return inner
        end
        return outer
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // The captured `shared` and the inner's own `other` must not share a name: no
    // `vN` may be assigned a field and then re-declared `local vN` in the same body.
    std::smatch m;
    const bool shadowed = std::regex_search(out, m, std::regex(R"((v\d+)\.flag\s*=\s*true[\s\S]*?local\s+\1\b)"));
    CHECK_FALSE(shadowed);
    // A Fission INFO note documents the rename that resolved the collision.
    CHECK(ContainsRegex(out, std::regex(R"(Fission: INFO:[^\n]*has been suffixed to avoid shadowing)")));
}

// Sibling closures capturing by value must not collide on uv_N names
// Each closure numbers its upvalues from 0, so emitting `local uv_N = source`
// capture copies in the shared parent scope makes siblings clash (closure A's
// `local uv_0 = t1` vs closure B's `local uv_0 = t1`), and a closure's own uv_0
// reference can bind to the wrong sibling's local. VAL captures must alias the
// source variable directly; no copy, no uv_N.
TEST_CASE("Regress: sibling VAL captures alias sources without uv_N collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // autogenerated names; collision would surface
    const std::string source = R"(
        local function outer()
            local t1 = {}
            local t2 = {}
            local t3 = {}
            local function a()
                t1[1] = t2
                return t3
            end
            local function b()
                t2[1] = t3
                return t1
            end
            a()
            b()
            return a, b
        end
        return outer
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // No capture-copy locals and no generic uv_N identifiers at all.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+uv_\d+\s*=)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\buv_\d+\b)")));
}

// LCT_UPVAL capture chaining: a closure that captures one of its parent's
// upvalues must reference the parent's upvalue, not a colliding fresh uv_0
// Bug (real Opera-GX script): an inner closure captured the parent's upvalue
// index N (CAPTURE mode 2 / LCT_UPVAL), but the lifter named the inner upvalue
// uv_0; colliding with the parent's own uv_0 (a different value). e.g. a pcall
// closure rendered `uv_0:GetCampaignEligibilityAsync(...)` where uv_0 was the
// PlaceId table, not the AdService it actually captured. Compiled WITHOUT upvalue
// debug names (debugLevel 1), so names are autogenerated and the collision shows.
TEST_CASE("Regress: LCT_UPVAL capture resolves to the parent upvalue, no uv_0 collision", "[Decompiler][Closure][Regression]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1; // no upvalue debug names -> autogenerated uv_N, collision visible
    const std::string source = R"(
        local cache = {}
        local Service = {}
        function Service:Async() return true end
        return function()
            if cache[1] == nil then
                local ok = pcall(function()
                    return Service:Async()
                end)
                return ok
            end
            return false
        end
    )";
    auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
    REQUIRE(result.resultCode == DecompileResult::Success);
    const auto out = std::move(result.decompilationOutput);

    INFO("decompile:\n" << out);
    // The method is invoked on the captured Service upvalue.
    CHECK(Contains(out, ":Async("));
    // `cache` is the parent's uv_0 (used for the `cache[1]` index). The Async call
    // must NOT be made on uv_0; that is the collision bug.
    CHECK_FALSE(Contains(out, "uv_0:Async"));
}

// Generalized iteration `for k,v in t do` must not emit the implicit nils
// Luau lowers `for k, v in t do` to the iterator triple [t, nil, nil] (LOAD nil,
// LOAD nil, FORGPREP). The lifter emitted all three slots -> `for k, v in t, nil,
// nil do`, which is non-idiomatic and not portable to plain Lua. When the state
// and control values are both nil they are implicit and must be dropped.
TEST_CASE("Regress: generalized for-in does not emit trailing nil iterator args", "[Decompiler][Loop][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local sum = 0
            for k, v in t do
                sum = sum + v
            end
            return sum
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, " in "));
    // The generalized form iterates the value directly: `for a, b in arg0 do`.
    CHECK(ContainsRegex(out, std::regex(R"(for\s+\w+\s*,\s*\w+\s+in\s+\w+\s+do)")));
    // The implicit `nil, nil` state/control must NOT be materialized.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(in\s+\w+\s*,\s*nil\s*,\s*nil)")));
    CHECK_FALSE(Contains(out, ", nil, nil"));
}

// Production safety boundary: malformed bytecode must not crash the host
// In a PRODUCTION_BUILD libassert's DEBUG_ASSERT is stripped and ASSERT aborts;
// a malformed/hostile stream would crash the whole host (RbxCli). The safety
// boundary (Decompiler::CommonDecompilerEntry) installs a throwing libassert
// handler and catches everything, so a bad chunk returns a failure result
// instead of taking the process down. If this test ever crashes the runner, the
// boundary regressed. (Reaching the end of the loop IS the no-crash proof.)
TEST_CASE("Safety: malformed/truncated bytecode degrades gracefully, no crash", "[Decompiler][Safety]") {
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};

    // Pure garbage of various shapes.
    const std::string garbage[] = {
        std::string(""),
        std::string("\x01\x02\x03"),
        std::string("\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF", 8),
        std::string("\x06not-real-bytecode-just-bytes"),
    };
    for (const auto &g : garbage) {
        const auto r = decompiler.DecompileRobloxBytecode(g, static_cast<DecompilerFlags>(0));
        CHECK(r.resultCode != DecompileResult::Success);
    }

    // Every proper prefix of a real compiled chunk exercises the bounds checks
    // deep inside deserialize/lift. None may crash; none is a complete chunk.
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    const std::string good = Luau::compile("local t = {} for i = 1, 10 do t[i] = i * i end return t", opts);
    REQUIRE(good.size() > 8);
    for (size_t len = 1; len < good.size(); ++len) {
        const auto r = decompiler.DecompileRobloxBytecode(good.substr(0, len), static_cast<DecompilerFlags>(0));
        CHECK(r.resultCode != DecompileResult::Success);
    }
}

TEST_CASE("Regress: dead `local` from an or-step is eliminated, the for survives", "[Decompiler][Loop][Regression]") {
    // `#"67" or 9` lifts with a phi-consumed step register, which forced a standalone `local vN = #"67"`
    // that the for-step then inlined anyway. The dead binding must be dropped, leaving one `#"67"`.
    const auto out = DecompileOrFail(
        R"(
        local a = ({})
        while 1 do for i = 1, 1, #"67" or 9 do table.insert(a, i) break end end
    )",
        0
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, "table.insert"));
    // the length expression appears once (the for-step), not also as a dead local.
    CHECK(CountOccurrences(out, "#\"67\"") == 1);
}
