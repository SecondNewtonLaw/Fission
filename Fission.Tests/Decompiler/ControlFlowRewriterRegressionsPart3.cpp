//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "ControlFlowRewriterTestSupport.hpp"
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

using namespace control_flow_regression;
using namespace scope_regression;

TEST_CASE("Regress: variadic RETURN keeps every value", "[Decompiler][Variadic][Return][Regression]") {
    SECTION("multret call tail: return 1, 2, g()") {
        const auto out = DecompileOrFail("local function f(g) return 1, 2, g() end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+1,\s*2,\s*g\(\))")));
        CHECK(Recompiles(out));
    }
    SECTION("vararg tail: return 1, ...") {
        const auto out = DecompileOrFail("local function f(...) return 1, ... end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+1,\s*\.\.\.)")));
        CHECK(Recompiles(out));
    }
    SECTION("pure multret: return g()") {
        const auto out = DecompileOrFail("local function f(g) return g() end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+g\(\))")));
        CHECK(Recompiles(out));
    }
    SECTION("pure vararg: return ...") {
        const auto out = DecompileOrFail("local function f(...) return ... end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+\.\.\.)")));
        CHECK(Recompiles(out));
    }
    SECTION("fixed multi-return unaffected: return a, b, c") {
        const auto out = DecompileOrFail("local function f(a, b, c) return a, b, c end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+a,\s*b,\s*c)")));
        CHECK(Recompiles(out));
    }
    SECTION("multret in non-tail position is truncated to one value: return g(), b") {
        const auto out = DecompileOrFail("local function f(g, b) return g(), b end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+g\(\),\s*b)")));
        CHECK(Recompiles(out));
    }
}

// Auto-name vs program-global collision
// The decompiler names registers `v<reg>`. A program global shaped the same way (`v6`) would be
// overwritten by a `local v6`, and on recompile later reads of the global would rebind to the local.
// GetVarName prefixes the register's local (`_v6`) so the global is preserved, and emits a FISSION INFO.
TEST_CASE("Regress: register auto-name does not overwrite a same-named global", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local a, b, c, d, e, f = tonumber(), tonumber(), tonumber(), tonumber(), tonumber(), tonumber()
        local g = not v6()
        v681(631)
        return #("a-b"), select(nil, 31)[{ ["data"] = g }], g, a, b, c, d, e, f
    )", 1, 1);

    INFO("decompile:\n" << out);
    // the colliding register local is prefixed (the global `v6` is referenced as a bare call).
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+_v\d+\b)")));
    // the global `v6` survives as a global (not shadowed before its use).
    CHECK(NoForwardReference(out));
    CHECK(Recompiles(out));
}

// Back-propagated names are lower-first-cased
// A local stored into a global (SETGLOBAL) is named after the global, lower-first-cased so it cannot
// shadow it: `BlahBlah = v0` -> `local blahBlah = ...; BlahBlah = blahBlah`.
TEST_CASE("Regress: SETGLOBAL back-propagates a lower-first global name", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            BlahBlah = a
            print(a)
            return a
        end
        return f
    )", 1, 1);

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+blahBlah\b)")));        // lower-first local
    CHECK(ContainsRegex(out, std::regex(R"(\bBlahBlah\s*=\s*blahBlah\b)"))); // global preserved, no shadow
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\blocal\s+BlahBlah\b)")));  // never a same-cased local
    CHECK(Recompiles(out));
}

// A local stored into a capitalized field is named after it, lower-first-cased.
TEST_CASE("Regress: reverse-field back-prop lowers a capitalized field name", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            o.Health = a
            print(a)
            return a
        end
        return f
    )", 1, 1);

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+health\b)")));      // lower-first local
    CHECK(ContainsRegex(out, std::regex(R"(\.Health\s*=\s*health\b)"))); // field key keeps its casing
    CHECK(Recompiles(out));
}

// Shadow safety: a global that is already lower-first must NOT be back-propagated; a `local score`
// would shadow the `score` global on its later reads. The engine's used-name gate must refuse it.
TEST_CASE("Regress: lower-first global is not back-propagated (would shadow)", "[Decompiler][Naming][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(o)
            local a = o:compute()
            score = a
            print(a)
            return a
        end
        return f
    )", 1, 1);

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\blocal\s+score\b)"))); // would shadow the global `score`
    CHECK(ContainsRegex(out, std::regex(R"(\bscore\s*=\s*\w)")));        // the global assignment survives
    CHECK(Recompiles(out));
}

// Constructor-result naming
// A local bound to a constructor call is named after what it builds. `Instance.new("X")` names after
// the class string; any other `Type.new(...)` / `Type.from*(...)` names after the type. Lower-first so
// the local never shadows the type/global. Only the lifter's auto-names are retargeted.
TEST_CASE("Feature: Instance.new names the local after the class string", "[Decompiler][Naming][Constructor]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local a = Instance.new("Part")
            a.Anchored = true
            print(a)
            return a
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+part\s*=\s*Instance\.new)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: Instance.new lower-firsts a PascalCase class string", "[Decompiler][Naming][Constructor]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local a = Instance.new("ScreenGui")
            a.Enabled = true
            print(a)
            return a
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+screenGui\b)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: datatype constructor names the local after the type", "[Decompiler][Naming][Constructor]") {
    SECTION("Vector3.new") {
        const auto out = DecompileOrFail(R"(
            local function f(x)
                local a = Vector3.new(x, x, x)
                print(a)
                return a + a
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+vector3\s*=\s*Vector3\.new)")));
        CHECK(Recompiles(out));
    }
    SECTION("Color3.fromRGB (from* factory)") {
        const auto out = DecompileOrFail(R"(
            local function f()
                local a = Color3.fromRGB(1, 2, 3)
                print(a)
                return a, a
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+color3\s*=\s*Color3\.fromRGB)")));
        CHECK(Recompiles(out));
    }
    SECTION("user OOP class .new()") {
        const auto out = DecompileOrFail(R"(
            local function f()
                local a = MyClass.new()
                a:init()
                return a
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+myClass\s*=\s*MyClass\.new)")));
        CHECK(Recompiles(out));
    }
}

// Loop-variable naming (nesting-aware)
// Numeric-for loop vars become i / j / k by nesting depth; siblings reuse i (disjoint scopes).
TEST_CASE("Feature: nested numeric for loops use i, j, k", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b, c)
            for x = 1, a do
                for y = 1, b do
                    for z = 1, c do
                        print(x, y, z)
                    end
                end
            end
            return a
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+i\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+j\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+k\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bprint\(i,\s*j,\s*k\))")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: sibling numeric for loops both reuse i", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a, b)
            for x = 1, a do print(x) end
            for y = 1, b do print(y) end
            return a
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(CountOccurrences(out, "for i =") == 2); // both siblings -> i
    CHECK(Recompiles(out));
}

// Generic-for: ipairs -> i, v; pairs (and others) -> k, v.
TEST_CASE("Feature: generic for ipairs uses i, v", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for a, b in ipairs(t) do print(a, b) end
            return t
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+i,\s*v\s+in\s+ipairs\b)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Feature: generic for pairs uses k, v", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            for a, b in pairs(t) do print(a, b) end
            return t
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfor\s+k,\s*v\s+in\s+pairs\b)")));
    CHECK(Recompiles(out));
}

// Collision safety: a loop var must not be renamed to a name the body already uses (a global `i`),
// which it would otherwise capture. Bump past it; the global write survives.
TEST_CASE("Feature: loop var does not capture a same-named global in its body", "[Decompiler][Naming][Loop]") {
    const auto out = DecompileOrFail(R"(
        local function f(a)
            for x = 1, a do
                i = x
            end
            return a
        end
        return f
    )");
    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\bfor\s+i\s*=)"))); // would capture the global `i`
    CHECK(ContainsRegex(out, std::regex(R"(\bi\s*=\s*\w)")));        // the global assignment survives
    CHECK(Recompiles(out));
}

// self recovery for module-table methods
// A module-table field closure whose first parameter is used as a receiver (`p.field`, `p:m()`) is a
// method: emit colon syntax with `self` (`function T:m(...)`). A free function (first param not a
// receiver) stays dot syntax with its parameters intact.
TEST_CASE("Feature: module-table method recovers self (colon form)", "[Decompiler][Naming][Self]") {
    const auto out = DecompileOrFail(R"(
        local T = {}
        function T:setValue(v)
            self.value = v
        end
        function T:getValue()
            return self.value
        end
        function T.freeFn(a, b)
            return a + b
        end
        return T
    )");
    INFO("decompile:\n" << out);
    // member-write method -> colon + self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+\w+:setValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bself\.value\s*=)")));
    // member-read method -> colon + self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+\w+:getValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\breturn\s+self\.value\b)")));
    // free function (no receiver use) -> stays dot, no self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+\w+\.freeFn\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:freeFn\b)")));
    CHECK(Recompiles(out));
}

// Class methods (the `X.__index = X` pattern) already recover self; guard it stays that way.
TEST_CASE("Feature: class method keeps self", "[Decompiler][Naming][Self]") {
    const auto out = DecompileOrFail(R"(
        local C = {}
        C.__index = C
        function C:setX(v)
            self.x = v
        end
        return C
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+\w+:setX\b)"))); // table keeps auto-name under flags=0
    CHECK(ContainsRegex(out, std::regex(R"(\bself\.x\s*=)")));
    CHECK(Recompiles(out));
}

// Length / count naming
// A local bound to a `#expr` length is named `count`.
TEST_CASE("Feature: length operator names the local count", "[Decompiler][Naming][Length]") {
    const auto out = DecompileOrFail(R"(
        local function f(t)
            local n = #t
            print(n)
            return n + n
        end
        return f
    )", 1, 1);
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+count\s*=\s*#)")));
    CHECK(Recompiles(out));
}

// Scope-restore: a register used only as a branch-local temporary must not
// keep its slot "defined" past the merge. A later instruction reusing the
// slot (a merge-block NEWTABLE) was emitted as a bare `v2 = {}` -- no `local`
// -- leaking the value to a global. (Repro: MouseOverModule, R2 is a namecall
// arg inside the `or` branch, then a fresh table in the merge block.)
TEST_CASE("Regress: reused branch-temp slot is re-declared local after the merge", "[Decompiler][Naming][Regression]") {
    // a leaking bare assignment of an auto-name to a table literal: `v2 = { ... }` with no `local`.
    const std::regex leak(R"((?:^|\n)[ \t]*v\d+[ \t]*=[ \t]*\{)");

    SECTION("if-branch temp then merge-block table") {
        const auto out = DecompileOrFail(R"(
            local function f(c, obj)
                if c then
                    obj:Method("temparg")
                end
                local t = {}
                t.x = 1
                print(t)
                return t
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK_FALSE(ContainsRegex(out, leak));                               // no global leak
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+\w+\s*=\s*\{)"))); // table is a local
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }

    SECTION("`or` with namecall arg then table (MouseOverModule shape)") {
        const auto out = DecompileOrFail(R"(
            local v = game.Players.LocalPlayer or game.Players:GetPropertyChangedSignal("LocalPlayer")
            local mouse = v:GetMouse()
            local t = {}
            t.handler = function() end
            return t
        )");
        INFO("decompile:\n" << out);
        CHECK_FALSE(ContainsRegex(out, leak));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// pcall / xpcall result naming
// `local ok, result = pcall(f)` -- the first protected-call return is always a boolean status, the
// second its result/error. Conventional, never-wrong names. Reused across scopes (each scope gets its
// own ok/result); a second pcall in the same scope bumps to ok2/result2 so distinct registers never
// collapse to one name.
TEST_CASE("Feature: pcall result names ok / result", "[Decompiler][Naming][Pcall]") {
    SECTION("multi-return pcall") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local a, b = pcall(g, 1, 2)
                if a then print(b) end
                return a
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*,\s*result\s*=\s*pcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("single-return pcall") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local a = pcall(g)
                if a then print("y") end
                return a
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*=\s*pcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("xpcall names the same way") {
        const auto out = DecompileOrFail(R"(
            local function f(g, h)
                local a, b = xpcall(g, h)
                return a, b
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*,\s*result\s*=\s*xpcall\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("two pcalls in one scope stay distinct (no aliasing)") {
        const auto out = DecompileOrFail(R"(
            local function f(g, h)
                local a = pcall(g)
                local b = pcall(h)
                if a then print("a") end
                if b then print("b") end
                return a, b
            end
            return f
        )", 1, 1);
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok\s*=\s*pcall\b)")));
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+ok2\s*=\s*pcall\b)")));
        // the two statuses must remain two distinct names through the return
        CHECK(ContainsRegex(out, std::regex(R"(\breturn\s+ok\s*,\s*ok2\b)")));
        CHECK(Recompiles(out));
    }
    SECTION("existing ok in scope is not captured") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                local ok = 5
                local a, b = pcall(g)
                return ok, a, b
            end
            return f
        )");
        INFO("decompile:\n" << out);
        CHECK(Recompiles(out));
        CHECK(NoForwardReference(out));
    }
}

TEST_CASE("Tail-duplicated table re-inlines consumed elements", "[Decompiler][Regression][DoubleLift]") {
    // A run-once repeat-until followed by a while-with-closure-condition makes the lifter
    // duplicate the tail region, lifting the table-literal instructions twice. The first lift
    // inlines the nil/select elements and marks their defs processed. A second lift must re-inline
    // pure element defs instead of emitting undeclared register names.
    const auto out = DecompileOrFail(R"(
        repeat
            next ..= (-(-select));
            tonumber = ((if 168.75 then string else 296) == { data = "hello", [529] = false, k = math, y = 852 });
        until (#(select).field)
        while function(p0, p1, p2)
            ipairs(true);
            return ;
        end do
            if 106 then
                local v0, v1, v2 = tostring, tostring, false;
                v1(true, v2);
                v1 += tonumber;
            end
        end
        t.field(f0[{ nil, x = math, ["hello"] = false, select }], ipairs(false));
    )");
    INFO("decompile:\n" << out);
    // every lifted copy of the constructor must carry the literal elements, never a register name
    const std::regex faithful(R"(\{\s*nil,\s*x\s*=\s*math,\s*hello\s*=\s*false,\s*select\s*\})");
    const size_t copies = static_cast<size_t>(std::distance(std::sregex_iterator(out.begin(), out.end(), faithful), std::sregex_iterator{}));
    CHECK(copies >= 1);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\{\s*v\d+,\s*x\s*=\s*math)")));
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: numeric-for body cannot poison its start closure", "[Decompiler][Loop][Regression][ForwardReference]") {
    const auto out = DecompileOrFail(R"(
        for g1_0 in pairs.field() do
        end
        for i0 = function(p0)
            p0()
            p0()
            p0(610, nil)
            return nil, 973
        end, ... do
            while (if false then i0 else true) do
                if "value" then
                    continue
                end
            end
            tostring(82, math)
            table = math(nil, next)
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(Contains(out, "for i = v2, ..., 1"));
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: numeric-for conditional step closure is merged", "[Decompiler][Loop][Regression][ForwardReference]") {
    const auto out = DecompileOrFail(R"(
        local function f0(p1, p2, p3)
            f0(print, next)
            return true, string
        end
        for i1 = ((57)), (if ... then f0.field else table.field), (if (t * f0) then (#true) else function(...)
            return
        end) do
            if (tostring.field) then
                break
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bv\d+\s*=.*\bfunction\s*\()")));
    const bool emitsUndeclaredStep = Contains(out, "for i = 57, v1, v2") && !Contains(out, "local v2");
    CHECK_FALSE(emitsUndeclaredStep);
    CHECK(Recompiles(out));
}

// ClassMethodRewriter must not splice a `local function F` into `T.m = F` dot sugar
// when an EARLIER statement (declared before the assignment) still captures F -- doing
// so drops the `local function F` decl and leaves that earlier closure referencing an
// unbound global. The forward read-scan alone missed the backward capture; the fix adds
// an IsIdentifierReadBetween check over (decl, assignment).
TEST_CASE("Regress: module dot-sugar keeps closure captured by an earlier function", "[Decompiler][Class][Regression]") {
    const auto out = DecompileOrFail(R"(
        local t = {}
        local function helper(x)
            return x + 1
        end
        local function wrapper()
            return helper(5)
        end
        t.run = helper
        return t, wrapper
    )");

    INFO("decompile:\n" << out);
    // Preserve helper as a local function instead of consuming it into `t.run`.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+function\s+helper\s*\()")));
    // wrapper still references the bound local `helper`.
    CHECK(ContainsRegex(out, std::regex(R"(return\s+helper\s*\(\s*5\s*\))")));
    // The rewrite did NOT fire: no dot-declaration of run absorbing helper's body.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(function\s+\w+\.run\s*\()")));
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: deep truthiness OR chain lifts without native recursion", "[Decompiler][Regression][StackDepth]") {
    std::string source = "local text = ...\nif ";
    for (int i = 0; i < 96; ++i) {
        if (i)
            source += " or ";
        source += "string.match(text, \"pattern" + std::to_string(i) + "\")";
    }
    source += " then return true end\nreturn false";

    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    CHECK(Contains(out, " or "));
    CHECK(Recompiles(out));
}

TEST_CASE("SSA: deep dominator tree renames iteratively", "[SSA][Regression][StackDepth]") {
    std::string source = "local x = ...\n";
    for (int i = 0; i < 320; ++i)
        source += "if x == " + std::to_string(i) + " then x += 1 end\n";
    source += "return x";

    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    CHECK(Recompiles(out));
}
