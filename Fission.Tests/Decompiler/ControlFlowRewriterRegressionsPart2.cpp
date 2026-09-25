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

TEST_CASE("Regress: path require is renamed to its module leaf", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local mod = require(svc.Core.Widget)
        print(mod)
        print(mod)
        return mod
    )");

    INFO("decompile:\n" << out);
    // The auto-named local becomes `local Widget = require(...)`.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+Widget\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// A string-variant require has no path to mine, so the binding is left untouched.
TEST_CASE("Regress: string require is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local mod = require("SomeStringModule")
        print(mod)
        print(mod)
        return mod
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "require(\"SomeStringModule\")"));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+SomeStringModule\b)")));
    CHECK(Recompiles(out));
}

// Two requires resolving to the same leaf are ambiguous -> neither is renamed.
TEST_CASE("Regress: duplicate require leaf is not renamed", "[Decompiler][Require][Regression]") {
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.Dup)
        local b = require(svc.Other.Dup)
        print(a, b)
        print(a, b)
        return a, b
    )");

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
    const auto out = DecompileOrFail(R"(
        local svc = game:GetService("ReplicatedStorage")
        local a = require(svc.Core.task)
        local b = require(svc.Core.Inventory)
        print(a, b)
        print(a, b)
        return a, b
    )");

    INFO("decompile:\n" << out);
    // `task` is a built-in -> never bound as a local name.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+task\s*=\s*require\()")));
    // The non-clashing module still renames.
    CHECK(ContainsRegex(out, std::regex(R"(local\s+Inventory\s*=\s*require\()")));
    CHECK(Recompiles(out));
}

// GetterRenamer: local = obj:GetXxx() -> xxx

// A local bound to a `Get<Property>` call is renamed to the property: the `Get`
// prefix is dropped and the first remaining letter lower-cased.
TEST_CASE("Regress: getter-bound local is renamed to its property", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(self)
            local a = self:GetFullName()
            local b = self:GetHumanoid()
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+fullName\s*=\s*\w+:GetFullName\()")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+humanoid\s*=\s*\w+:GetHumanoid\()")));
    CHECK(Recompiles(out));
}

// A getter whose property name is a built-in global is left auto-named:
// `GetTime` would yield `time`, which would shadow the `time` global.
TEST_CASE("Regress: getter property colliding with a global is not renamed", "[Decompiler][Getter][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(self)
            local a = self:GetTime()
            print(a)
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+time\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:GetTime\()")));
    CHECK(Recompiles(out));
}

// AttributeRenamer: local = obj:GetAttribute("X") -> x ; obj:SetAttribute("X", v) -> v named x

// A local bound to `:GetAttribute("Name")` is renamed after the attribute string, lower-first-cased.
TEST_CASE("Regress: GetAttribute-bound local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("AccuracyDeviation")
            local b = obj:GetAttribute("Pellets")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+accuracyDeviation\s*=\s*\w+:GetAttribute\("AccuracyDeviation"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+pellets\s*=\s*\w+:GetAttribute\("Pellets"\))")));
    CHECK(Recompiles(out));
}

// The value written by `:SetAttribute("Name", v)` is renamed after the attribute string, at its
// declaration and every reference.
TEST_CASE("Regress: SetAttribute value local is renamed to the attribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj, base)
            local v = base * 2
            obj:SetAttribute("Damage", v)
            return v
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+damage\s*=)")));
    CHECK(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// When a GetAttribute read and a SetAttribute write name the same attribute in one scope, the read owns
// the name: the read local becomes `damage`, and the written value keeps its auto-name (two locals cannot
// share `damage`). Without the get-over-set priority both would collide and neither would be renamed.
TEST_CASE("Regress: GetAttribute claims the name over a same-attribute SetAttribute", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj, base)
            local d = obj:GetAttribute("Damage")
            local nv = base + 1
            obj:SetAttribute("Damage", nv)
            print(d)
            print(d)
            return d, nv
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+damage\s*=\s*\w+:GetAttribute\("Damage"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:SetAttribute\("Damage",\s*damage\))")));
    CHECK(Recompiles(out));
}

// A multi-word attribute name is folded to camelCase: `"Max HP"` -> `maxHp`,
// `"Super Long Name"` -> `superLongName`.
TEST_CASE("Regress: multi-word attribute name is camelCased", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("Max HP")
            local b = obj:GetAttribute("Super Long Name")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+maxHp\s*=\s*\w+:GetAttribute\("Max HP"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+superLongName\s*=\s*\w+:GetAttribute\("Super Long Name"\))")));
    CHECK(Recompiles(out));
}

// A name that folds to a leading digit is salvaged by spelling the digit out: `"3D Offset"` folds to
// `3DOffset`, then the leading `3` becomes `three` -> `threeDOffset`.
TEST_CASE("Regress: leading-digit attribute name spells the digit out", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("3D Offset")
            local b = obj:GetAttribute("2Handed")
            print(a, b)
            print(a, b)
            return a, b
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+threeDOffset\s*=\s*\w+:GetAttribute\("3D Offset"\))")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+twoHanded\s*=\s*\w+:GetAttribute\("2Handed"\))")));
    CHECK(Recompiles(out));
}

// An attribute name whose folded form is still not a legal identifier (punctuation the space-fold does
// not remove) leaves the auto-name untouched.
TEST_CASE("Regress: attribute name that stays illegal after folding is not renamed", "[Decompiler][Attribute][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f(obj)
            local a = obj:GetAttribute("Damage/Sec")
            print(a)
            print(a)
            return a
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(:GetAttribute\("Damage/Sec"\))")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(local\s+damageSec\s*=)")));
    CHECK(Recompiles(out));
}

// PropertyRenamer: local = obj.Property -> property

namespace property_regression {
    std::shared_ptr<MemberExpressionNode> MemberRead(const std::string &tbl, const std::string &key) {
        return std::make_shared<MemberExpressionNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(tbl)), key);
    }
    std::string DeclName(const std::shared_ptr<Statement> &s) {
        auto vd = std::dynamic_pointer_cast<VariableDeclarationNode>(s);
        if (!vd)
            return {};
        auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(vd->identifier);
        return (id && id->identifier) ? id->identifier->name : std::string{};
    }
} // namespace property_regression

using namespace property_regression;

// A local with a single source that is a property read is renamed to the
// property (first letter lower-cased), references included.
TEST_CASE("Regress: single-source property local renamed to property", "[Decompiler][Property][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "Character")
        )
    ); // local v0 = plr.Character
    stmts.push_back(UseStmt("v0"));

    PropertyRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "character");
    auto use = std::dynamic_pointer_cast<IdentifierExpressionNode>(std::dynamic_pointer_cast<ExpressionStatementNode>(stmts[1])->expression);
    REQUIRE(use);
    CHECK(use->identifier->name == "character");
}

// A local that is reassigned (more than one value source) keeps its auto-name --
// the property name would no longer describe its contents.
TEST_CASE("Regress: reassigned property local is not renamed", "[Decompiler][Property][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "First"))
    ); // local v0 = plr.First
    stmts.push_back(UseStmt("v0"));
    stmts.push_back(
        std::make_shared<AssignmentStatementNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), MemberRead("plr", "Second"))
    ); // v0 = plr.Second
    stmts.push_back(UseStmt("v0"));

    PropertyRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "v0"); // untouched: two sources
}

TEST_CASE("Regress: class marker does not rename its owner to __index", "[Decompiler][Class][Naming][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), std::make_shared<TableLiteralNode>()
        )
    );
    stmts.push_back(
        std::make_shared<AssignmentStatementNode>(MemberRead("v0", "__index"), std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")))
    );

    ReverseFieldRenamer{}.Run(stmts);

    CHECK(DeclName(stmts[0]) == "v0");
}

TEST_CASE("Regress: scoped rename updates method receiver", "[Decompiler][Class][Naming][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(
        std::make_shared<VariableDeclarationNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), std::make_shared<TableLiteralNode>()
        )
    );
    auto method = std::make_shared<FunctionDeclarationNode>(
        "v0:initialize", 0, std::unordered_map<int32_t, std::shared_ptr<FunctionArgumentExpression>>{}, false, std::make_shared<BlockStatementNode>(), false
    );
    stmts.push_back(method);

    ScopeAwareRenamer::Run(stmts, [](const auto &) { return std::vector<std::pair<std::string, std::string>>{{"v0", "controller"}}; });

    CHECK(DeclName(stmts[0]) == "controller");
    CHECK(method->functionName == "controller:initialize");
}

// End-to-end: a chain of single-source property reads is renamed.
TEST_CASE("Regress: property reads are renamed end-to-end", "[Decompiler][Property][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function clean(plr)
            local c = plr.Character
            local h = c.Humanoid
            print(c, h)
            print(c, h)
            return c, h
        end
        return clean
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+character\s*=\s*\w+\.Character)")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+humanoid\s*=\s*character\.Humanoid)")));
    CHECK(Recompiles(out));
}

// Output-quality cleanups

// A no-op `x = x` reassignment is dropped; surrounding statements survive.
TEST_CASE("Regress: self-assignment is eliminated", "[Decompiler][Cleanup][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    auto keepL = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("keep"));
    auto keepR = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("src"));
    stmts.push_back(std::make_shared<AssignmentStatementNode>(keepL, keepR)); // keep = src
    auto selfL = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("foo"));
    auto selfR = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("foo"));
    stmts.push_back(std::make_shared<AssignmentStatementNode>(selfL, selfR)); // foo = foo  (dropped)

    SelfAssignmentEliminator{}.Run(stmts);

    REQUIRE(stmts.size() == 1);
    auto asn = std::dynamic_pointer_cast<AssignmentStatementNode>(stmts[0]);
    REQUIRE(asn);
    auto l = std::dynamic_pointer_cast<IdentifierExpressionNode>(asn->left);
    REQUIRE(l);
    CHECK(l->identifier->name == "keep");
}

// A literal call-callee gets exactly one wrapping paren layer, not two.
TEST_CASE("Regress: literal method call is not double-parenthesized", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        return ("hello"):rep(3)
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\("hello"\):rep\()")));
    CHECK_FALSE(Contains(out, "((")); // no double-paren
    CHECK(Recompiles(out));
}

// A keyed table value is emitted without redundant wrapping parens.
TEST_CASE("Regress: keyed table value has no wrapping parens", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        return {[0] = "s", [1] = "ms"}
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*"s")")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\[0\]\s*=\s*\(")")));
    CHECK(Recompiles(out));
}

// A constant with high (non-ASCII) bytes round-trips as the raw UTF-8 text, not
// a re-escaped `\\206` sequence (the deserializer no longer pre-escapes).
TEST_CASE("Regress: high-byte string constant is not double-escaped", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188s\""); // source escapes -> bytes CE BC 73 ("μs")

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));    // raw UTF-8 bytes present
    CHECK_FALSE(Contains(out, "\\206")); // not re-emitted as a decimal escape
    CHECK(Recompiles(out));
}

// Mixed string: a valid UTF-8 sequence stays raw (readable) while invalid/lone
// high bytes are escaped as \ddd, so the emitted file is always valid UTF-8 and
// still round-trips to the identical bytes.
TEST_CASE("Regress: invalid UTF-8 bytes are escaped, valid stays raw", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail("return \"\\206\\188\\255x\""); // CE BC (mu, valid) + FF (invalid) + x

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "\xCE\xBC"));   // valid 2-byte sequence kept raw
    CHECK(Contains(out, "\\255"));      // lone 0xFF escaped to decimal
    CHECK_FALSE(Contains(out, "\xFF")); // no raw invalid byte leaked into the file
    CHECK(Recompiles(out));
}

// A computed local stored into a field is named after that field.
TEST_CASE("Regress: reverse-field naming names a local after its field", "[Decompiler][Cleanup][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function make(x)
            local self = {}
            local v = x * 2 + 1
            self.health = v
            print(v)
            return self
        end
        return make
    )", 1, 1);

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+health\s*=)")));
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
    const auto out = DecompileOrFail(R"(
        local t = { 43, ({ "a", "b", f = "" })[2] }
        t[1] = 5
        return t
    )");

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
    const auto out = DecompileOrFail(R"(
        local t = { ({ "a", "b", f = "" })[2], 43 }
        print(t)
        print(t)
        return t
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\s*=\s*\{\s*\(\{\s*"a",\s*"b",\s*f\s*=\s*""\s*\}\)\[2\],\s*43\s*\})")));
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
        const auto out = DecompileOrFail("return { 43, ({ 10, 20, 30 })[2] }");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s*\{\s*43,\s*\(\{\s*10,\s*20,\s*30\s*\}\)\[2\]\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("arithmetic over params folds") {
        const auto out = DecompileOrFail("local function f(a, b) return { a + 1, b * 2, a - b } end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*a\s*\+\s*1,\s*b\s*\*\s*2,\s*a\s*-\s*b\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
    SECTION("array of nested constructors folds and stays declared-before-use") {
        const auto out = DecompileOrFail(R"(
            local t = { { x = 1 }, { y = 2 } }
            t[1].x = 9
            return t
        )");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*\{\s*x\s*=\s*1\s*\},\s*\{\s*y\s*=\s*2\s*\}\s*\})")));
        CHECK(NoForwardReference(out));
        CHECK(Recompiles(out));
    }
}

// A SETLIST with count == 0 is variadic: the array gathers every source register up to the stack top,
// whose last element is a multret call. The size must come from that multret producer, not be treated
// as one element -- otherwise all but the first element are dropped and each emitted as a stray call.
// And no element may also leak as a standalone `local` beside the folded constructor (the SETLIST
// counts its base register twice). Regression: TestService.LuauLSP_Settings (`{ a(), b(), ..., n() }`).
TEST_CASE("Regress: variadic SETLIST keeps every element and does not duplicate", "[Decompiler][Table][SetList][Regression]") {
    SECTION("table of calls with a multret tail") {
        const auto out = DecompileOrFail(R"(
            local function f(g)
                return { g(1), g(2), g(3), g(4), g(5) }
            end
            return f
        )");
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
        const auto out = DecompileOrFail(R"(
            local t = {}
            t.include = {
                game:GetService("Workspace"),
                game:GetService("Players"),
                game:GetService("Lighting"),
                game:GetService("TestService"),
            }
            return t
        )");
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
