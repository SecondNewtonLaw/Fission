//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "ControlFlowRewriterTestSupport.hpp"
#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
#include <string>

using namespace control_flow_regression;

static const std::string kAccessorDriver = R"LUA(
    local f = __integration_subject()
    local obj = {
        attributes={AccuracyDeviation=2,Pellets=3,Damage=4,["Max HP"]=5,["Super Long Name"]=6,["3D Offset"]=7,["2Handed"]=8,["Damage/Sec"]=9},
        GetFullName=function() return "root.name" end,
        GetHumanoid=function() return "humanoid" end,
        GetTime=function() return 17 end,
        GetAttribute=function(self,key) print("get",key); return self.attributes[key] end,
        SetAttribute=function(self,key,value) print("set",key,value); self.attributes[key]=value end
    }
    print(f(obj,3)); print(f(obj,7)); print(obj.attributes.Damage)
)LUA";

TEST_CASE("Regress: path require is renamed to its module leaf", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local svc = game:GetService("ReplicatedStorage")
        local mod = require(svc.Core.Widget)
        print(mod)
        print(mod)
        return mod
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    // The auto-named local becomes `local Widget = require(...)`.
    CHECK(ContainsRegex(out, std::regex(R"(const\s+Widget\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// A string-variant require has no path to mine, so the binding is left untouched.
TEST_CASE("Regress: string require is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local mod = require("SomeStringModule")
        print(mod)
        print(mod)
        return mod
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "require(\"SomeStringModule\")"));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+SomeStringModule\b)")));
    CHECK(Recompiles(out));
}

// Two requires resolving to the same leaf are ambiguous -> neither is renamed.
TEST_CASE("Regress: duplicate require leaf is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.Dup)
        local b = require(svc.Other.Dup)
        print(a, b)
        print(a, b)
        return a, b
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    // Path keys still read `.Dup`, but no binding is renamed to `local Dup`.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+Dup\s*=\s*require\()")));
    CHECK(CountOccurrences(out, "require(") >= 2);
    CHECK(Recompiles(out));
}

// A leaf that matches a built-in Luau/Roblox global is not used as a name: it
// would shadow the global. The clashing require is left auto-named; an adjacent
// non-clashing one still renames.
TEST_CASE("Regress: require leaf colliding with a Luau global is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.task)
        local b = require(svc.Core.Inventory)
        print(a, b)
        print(a, b)
        return a, b
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    // `task` is a built-in -> never bound as a local name.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+task\s*=\s*require\()")));
    // The non-clashing module still renames.
    CHECK(ContainsRegex(out, std::regex(R"(const\s+Inventory\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// GetterRenamer: local = obj:GetXxx() -> xxx

// A local bound to a `Get<Property>` call is renamed to the property: the `Get`
// prefix is dropped and the first remaining letter lower-cased.
TEST_CASE("Regress: getter-bound local is renamed to its property", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(self)
            local a = self:GetFullName()
            local b = self:GetHumanoid()
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+fullName\s*=\s*\w+:GetFullName\()")));
    CHECK(ContainsRegex(out, std::regex(R"(const\s+humanoid\s*=\s*\w+:GetHumanoid\()")));
    CHECK(Recompiles(out));
}

// A getter whose property name is a built-in global is left auto-named:
// `GetTime` would yield `time`, which would shadow the `time` global.
TEST_CASE("Regress: getter property colliding with a global is not renamed", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(self)
            local a = self:GetTime()
            print(a)
            print(a)
            return a
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+time\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:GetTime\()")));
    CHECK(Recompiles(out));
}

// AttributeRenamer: local = obj:GetAttribute("X") -> x ; obj:SetAttribute("X", v) -> v named x

// A local bound to `:GetAttribute("Name")` is renamed after the attribute string, lower-first-cased.
TEST_CASE("Regress: GetAttribute-bound local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj)
            local a = obj:GetAttribute("AccuracyDeviation")
            local b = obj:GetAttribute("Pellets")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+accuracyDeviation\s*=\s*\w+:GetAttribute\("AccuracyDeviation"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(const\s+pellets\s*=\s*\w+:GetAttribute\("Pellets"\))")));
    CHECK(Recompiles(out));
}

// The value written by `:SetAttribute("Name", v)` is renamed after the attribute string, at its
// declaration and every reference.
TEST_CASE("Regress: SetAttribute value local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj, base)
            local v = base * 2
            obj:SetAttribute("Damage", v)
            return v
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+damage\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// When a GetAttribute read and a SetAttribute write name the same attribute in one scope, the read owns
// the name: the read local becomes `damage`, and the written value keeps its auto-name (two locals cannot
// share `damage`). Without the get-over-set priority both would collide and neither would be renamed.
TEST_CASE("Regress: GetAttribute claims the name over a same-attribute SetAttribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj, base)
            local d = obj:GetAttribute("Damage")
            local nv = base + 1
            obj:SetAttribute("Damage", nv)
            print(d)
            print(d)
            return d, nv
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+damage\s*=\s*\w+:GetAttribute\("Damage"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// A multi-word attribute name is folded to camelCase: `"Max HP"` -> `maxHp`,
// `"Super Long Name"` -> `superLongName`.
TEST_CASE("Regress: multi-word attribute name is camelCased", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj)
            local a = obj:GetAttribute("Max HP")
            local b = obj:GetAttribute("Super Long Name")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+maxHp\s*=\s*\w+:GetAttribute\("Max HP"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(const\s+superLongName\s*=\s*\w+:GetAttribute\("Super Long Name"\))")));
    CHECK(Recompiles(out));
}

// A name that folds to a leading digit is salvaged by spelling the digit out: `"3D Offset"` folds to
// `3DOffset`, then the leading `3` becomes `three` -> `threeDOffset`.
TEST_CASE("Regress: leading-digit attribute name spells the digit out", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj)
            local a = obj:GetAttribute("3D Offset")
            local b = obj:GetAttribute("2Handed")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+threeDOffset\s*=\s*\w+:GetAttribute\("3D Offset"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(const\s+twoHanded\s*=\s*\w+:GetAttribute\("2Handed"\))")));
    CHECK(Recompiles(out));
}

// An attribute name whose folded form is still not a legal identifier (punctuation the space-fold does
// not remove) leaves the auto-name untouched.
TEST_CASE("Regress: attribute name that stays illegal after folding is not renamed", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function f(obj)
            local a = obj:GetAttribute("Damage/Sec")
            print(a)
            print(a)
            return a
        end
        return f
    )",
        1, 2, kAccessorDriver
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(:GetAttribute\("Damage/Sec"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+damageSec\s*=)")));
    CHECK(Recompiles(out));
}

// PropertyRenamer: local = obj.Property -> property

TEST_CASE("Integration: property aliases preserve reads and mutations", "[Decompiler][Property][Regression][Integration]") {
    integration_test::Check(R"LUA(local player = { Character = { id = 8 } }
local value = player.Character
print(value.id)
value.id += 1
print(value.id, player.Character.id))LUA");
}

TEST_CASE("Integration: reassigned property values retain each observation", "[Decompiler][Property][Regression][Integration]") {
    integration_test::Check(R"LUA(local player = { First = 3, Second = 9 }
local value = player.First
print(value)
value = player.Second
print(value))LUA");
}

TEST_CASE("Integration: metatable owners retain method identity", "[Decompiler][Class][Naming][Regression][Integration]") {
    integration_test::Check(R"LUA(local Owner = {}
Owner.__index = Owner
function Owner:Get() return self.value end
local item = setmetatable({ value = 3 }, Owner)
print(item:Get(), rawequal(getmetatable(item), Owner)))LUA");
}

TEST_CASE("Integration: method aliases preserve receiver mutations", "[Decompiler][Class][Naming][Regression][Integration]") {
    integration_test::Check(R"LUA(local controller = { count = 0 }
function controller:initialize(n) self.count += n; return self end
local alias = controller
alias:initialize(4)
controller:initialize(3)
print(rawequal(alias, controller), controller.count))LUA");
}

// End-to-end: a chain of single-source property reads is renamed.
TEST_CASE("Regress: property reads are renamed end-to-end", "[Decompiler][Property][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function clean(plr)
            local c = plr.Character
            local h = c.Humanoid
            print(c.Humanoid, h)
            print(c.Humanoid, h)
            return c, h
        end
        return clean
    )",
        1, 2, R"DRIVER(local f=__integration_subject(); local c,h=f({Character={Humanoid="humanoid"}}); print(c.Humanoid,h))DRIVER"
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+character\s*=\s*\w+\.Character)")));
    CHECK(ContainsRegex(out, std::regex(R"(const\s+humanoid\s*=\s*character\.Humanoid)")));
    CHECK(Recompiles(out));
}

// Output-quality cleanups

TEST_CASE("Integration: self assignments retain index callbacks", "[Decompiler][Cleanup][Regression][Integration]") {
    integration_test::Check(R"LUA(local calls = 0
local function nextValue() calls += 1; return calls end
local value = nextValue()
value = value
print(value, calls)
local log = {}
local object = setmetatable({}, {
    __index = function() log[#log + 1] = "read"; return 7 end,
    __newindex = function(_, _, value) log[#log + 1] = "write:" .. value end,
})
object.x = object.x
print(table.concat(log, ",")))LUA");
}

// A literal call-callee gets exactly one wrapping paren layer, not two.
TEST_CASE("Regress: literal method call is not double-parenthesized", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        return ("hello"):rep(3)
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\("hello"\):rep\()")));
    CHECK_FALSE(Contains(out, "((")); // no double-paren
    CHECK(Recompiles(out));
}

// A keyed table value is emitted without redundant wrapping parens.
TEST_CASE("Regress: keyed table value has no wrapping parens", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        return {[0] = "s", [1] = "ms"}
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*"s")")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*\(")")));
    CHECK(Recompiles(out));
}

// A constant with high (non-ASCII) bytes round-trips as the raw UTF-8 text, not
// a re-escaped `\\206` sequence (the deserializer no longer pre-escapes).
TEST_CASE("Regress: high-byte string constant is not double-escaped", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188s\"", 1, 2, ""); // source escapes -> bytes CE BC 73 ("μs")

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));    // raw UTF-8 bytes present
    CHECK_FALSE(Contains(out, "\\206")); // not re-emitted as a decimal escape
    CHECK(Recompiles(out));
}

// Mixed string: a valid UTF-8 sequence stays raw (readable) while invalid/lone
// high bytes are escaped as \ddd, so the emitted file is always valid UTF-8 and
// still round-trips to the identical bytes.
TEST_CASE("Regress: invalid UTF-8 bytes are escaped, valid stays raw", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188\\255x\"", 1, 2, ""); // CE BC (mu, valid) + FF (invalid) + x

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));   // valid 2-byte sequence kept raw
    CHECK(Contains(out, "\\255"));      // lone 0xFF escaped to decimal
    CHECK_FALSE(Contains(out, "\xFF")); // no raw invalid byte leaked into the file
    CHECK(Recompiles(out));
}

// A computed local stored into a field is named after that field.
TEST_CASE("Regress: reverse-field naming names a local after its field", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local function make(x)
            local self = {}
            local v = x * 2 + 1
            self.health = v
            print(v)
            return self
        end
        return make
    )",
        1, 1, R"DRIVER(local f=__integration_subject(); print(f(3).health,f(-2).health))DRIVER"
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+health\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(\.health\s*=\s*health)")));
    CHECK(Recompiles(out));
}

// SETLIST array-element forward references
// An array element that indexes a populated inner table (built between the outer
// NEWTABLE and its SETLIST) must not be folded into the constructor: the inner
// table's `local` is emitted after the outer one, so the fold either drops it or
// reads nil. When the outer table is a real local, the SETLIST is emitted as
// `t[i] = elem` assignments after every contributing local is declared.
TEST_CASE("Regress: SETLIST array element indexing an inner table is declared before use", "[Decompiler][Table][SetList][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local t = { 43, ({ "a", "b", f = "" })[2] }
        t[1] = 5
        return t
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    // the inner table survives (it was being dropped entirely).
    CHECK(ContainsRegex(out, std::regex(R"(\(\{\s*"a",\s*"b",\s*f\s*=\s*""\s*\}\)\[2\])")));
    // every auto-named local is declared before it is used.
    CHECK(NoForwardReference(out));
    // the inner table is NOT folded into the outer constructor as a bare `vN[2]` element.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\{[^{}]*\bv\d+\[\d+\][^{}]*\})")));
    CHECK(Recompiles(out));
}

// The same defect when the indexed inner table is the FIRST element and the outer
// table is multi-use (so it is a real local, not inlined into a return).
TEST_CASE("Regress: SETLIST first element indexing an inner table is sound", "[Decompiler][Table][SetList][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        local t = { ({ "a", "b", f = "" })[2], 43 }
        print(t[1], t[2])
        print(t[1], t[2])
        return t
    )",
        1, 2, ""
    );

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(const\s+t\s*=\s*\{\s*\(\{\s*"a",\s*"b",\s*f\s*=\s*""\s*\}\)\[2\],\s*43\s*\})")));
    CHECK(NoForwardReference(out));
    CHECK(Recompiles(out));
}

// Guard against over-deferral (a quality regression): the safe, common cases must
// STILL fold into a single `{ ... }` constructor. An inner table built and indexed
// inline as a return value is consumed at its last use, where everything already
// exists; it folds. Arithmetic over params/earlier-locals folds. Nested constructors
// fold. None of these should spill into `t[i] = elem` statements.
TEST_CASE("Regress: SETLIST safe elements still fold into a constructor", "[Decompiler][Table][SetList][Regression]") {
    SECTION("return-form inner-table index folds") {
        const auto out = DecompileOrFail("return { 43, ({ 10, 20, 30 })[2] }", 1, 2, "");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s*\{\s*43,\s*\(\{\s*10,\s*20,\s*30\s*\}\)\[2\]\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("arithmetic over params folds") {
        const auto out = DecompileOrFail(
            "local function f(a, b) return { a + 1, b * 2, a - b } end return f", 1, 2,
            R"DRIVER(local f=__integration_subject(); print(f(3,4)); print(f(-2,7)))DRIVER"
        );
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*a\s*\+\s*1,\s*b\s*\*\s*2,\s*a\s*-\s*b\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("array of nested constructors folds and stays declared-before-use") {
        const auto out = DecompileOrFail(
            R"(
            local t = { { x = 1 }, { y = 2 } }
            t[1].x = 9
            return t
        )",
            1, 2, ""
        );
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*\{\s*x\s*=\s*1\s*\},\s*\{\s*y\s*=\s*2\s*\}\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

TEST_CASE("Regress: computed key after SETLIST stays in constructor", "[Decompiler][Table][SetList][Regression]") {
    const auto out = DecompileOrFail(
        "local function f(g) local t = { 1, 2, [g()] = 3 } print(t) return t end return f", 1, 2,
        R"DRIVER(local f=__integration_subject(); local n=0; local t=f(function() n+=1; print("key",n); return "key" end); print(t[1],t[2],t.key,n))DRIVER"
    );
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\{\s*1,\s*2,\s*\[\w+\(\)\]\s*=\s*3\s*\})")));
    CHECK(Recompiles(out));
}

// A SETLIST with count == 0 is variadic: the array gathers every source register up to the stack top,
// whose last element is a multret call. The size must come from that multret producer, not be treated
// as one element -- otherwise all but the first element are dropped and each emitted as a stray call.
// And no element may also leak as a standalone `local` beside the folded constructor (the SETLIST
// counts its base register twice). Regression: TestService.LuauLSP_Settings (`{ a(), b(), ..., n() }`).
TEST_CASE("Regress: variadic SETLIST keeps every element and does not duplicate", "[Decompiler][Table][SetList][Regression]") {
    SECTION("table of calls with a multret tail") {
        const auto out = DecompileOrFail(
            R"(
            local function f(g)
                return { g(1), g(2), g(3), g(4), g(5) }
            end
            return f
        )",
            1, 2,
            R"DRIVER(local f=__integration_subject(); local t=f(function(n) print("call",n); return n,n+10,n+100 end); print(#t,t[1],t[5],t[6],t[7]))DRIVER"
        );
        INFO("decompile:\n" << out);
        // every element survives...
        for (int i = 1; i <= 5; ++i) {
            const std::string call = "g(" + std::to_string(i) + ")";
            INFO("element " << call);
            CHECK(CountOccurrences(out, call) == 1); // present exactly once -- not dropped, not duplicated
        }
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("nested array assigned into a field (LuauLSP_Settings shape)") {
        const auto out = DecompileOrFail(
            R"(
            local t = {}
            t.include = {
                game:GetService("Workspace"),
                game:GetService("Players"),
                game:GetService("Lighting"),
                game:GetService("TestService"),
            }
            return t
        )",
            1, 2, ""
        );
        INFO("decompile:\n" << out);
        CHECK(CountOccurrences(out, R"(GetService("Workspace"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("Players"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("Lighting"))") == 1);
        CHECK(CountOccurrences(out, R"(GetService("TestService"))") == 1);
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// Variadic / multret tails (SSA implicit-use completeness)
// A RETURN / SETLIST / CALL whose operand count is 0 is variadic: the value list runs from its base
// register up to a multret producer (a call returning all results, or `...`). The SSA builder must
// record every register in that range as a use -- recording only the first (the old `count==0 -> 1`)
// silently dropped every trailing value (`return 1, 2, f()` lifted to `return 1`). These guard that
// whole class so it cannot regress again.
