//
// Created by Dottik on 21/9/2026.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
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

namespace control_flow_regression {

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

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    bool ContainsRegex(const std::string &haystack, const std::regex &pattern) { return std::regex_search(haystack, pattern); }

    // true if `src` recompiles to valid bytecode (first byte 0 = Luau error marker).
    bool Recompiles(const std::string &src) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string bc = Luau::compile(src, opts);
        return !bc.empty() && bc[0] != '\0';
    }

    std::string FirstBetween(const std::string &haystack, const std::string &begin, const std::string &end) {
        const size_t beginPos = haystack.find(begin);
        if (beginPos == std::string::npos)
            return {};

        const size_t bodyPos = beginPos + begin.size();
        const size_t endPos = haystack.find(end, bodyPos);
        if (endPos == std::string::npos)
            return {};

        return haystack.substr(bodyPos, endPos - bodyPos);
    }

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

    // No auto-named local `vN` is textually used before its `local vN` declaration; the
    // forward-reference bug (a value folded into a constructor whose own `local` is emitted, or
    // dropped, afterwards reads nil). Comment blocks are stripped first. Names that never appear
    // in a `local` declaration are globals/params and are ignored.
    bool NoForwardReference(const std::string &decompiled) {
        const std::string s = std::regex_replace(decompiled, std::regex(R"(--\[\[[\s\S]*?\]\])"), "");
        const std::regex localRe(R"(\blocal\s+(v\d+)\b)");
        std::set<std::string> names;
        for (std::sregex_iterator it(s.begin(), s.end(), localRe), e; it != e; ++it)
            names.insert((*it)[1].str());
        for (const auto &name : names) {
            std::smatch dm;
            if (!std::regex_search(s, dm, std::regex("\\blocal\\s+(" + name + ")\\b")))
                continue;
            const auto declTokPos = static_cast<size_t>(dm.position(1));
            std::smatch tm;
            if (std::regex_search(s, tm, std::regex("\\b" + name + "\\b")) && static_cast<size_t>(tm.position(0)) < declTokPos)
                return false; // first appearance of vN is a use, before its declaration
        }
        return true;
    }

} // namespace control_flow_regression

using namespace control_flow_regression;

// ScopeBlockIntroducer: do-end scoping of register-reuse phases
// Register reuse (hence whether the lifter emits a `local vN` redefinition) is too
// allocation-dependent to force from source reliably, so the wrap/guard logic is unit-tested
// directly on a hand-built statement list; an end-to-end test then checks recompile-safety.
namespace scope_regression {
    std::shared_ptr<VariableDeclarationNode> LocalDecl(const std::string &name, const std::string &valueName) {
        auto lhs = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name));
        auto rhs = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(valueName));
        return std::make_shared<VariableDeclarationNode>(lhs, rhs);
    }
    std::shared_ptr<ExpressionStatementNode> UseStmt(const std::string &name) {
        return std::make_shared<ExpressionStatementNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)));
    }
    bool IsDoBlock(const std::shared_ptr<Statement> &s) {
        auto b = std::dynamic_pointer_cast<BlockStatementNode>(s);
        return b && b->bEmitAsDoBlock;
    }
} // namespace scope_regression

using namespace scope_regression;

// A redefinition whose prior lifetime is self-contained splits into two `do ... end` scopes.
TEST_CASE("Scope: self-contained redefinition is split into do-blocks", "[Decompiler][Scope][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "x")); // local v0 = x
    stmts.push_back(UseStmt("v0"));        // v0
    stmts.push_back(LocalDecl("v0", "y")); // local v0 = y  (redefinition; old v0 dead)
    stmts.push_back(UseStmt("v0"));        // v0

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 2);
    CHECK(IsDoBlock(stmts[0]));
    CHECK(IsDoBlock(stmts[1]));
    auto b0 = std::dynamic_pointer_cast<BlockStatementNode>(stmts[0]);
    CHECK(b0->body.size() == 2); // the decl + its use
}

TEST_CASE("Scope: crossing local moves with the phase that consumes it", "[Decompiler][Scope][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "x"));   // local v0 = x
    stmts.push_back(LocalDecl("keep", "a")); // local keep = a   (escapes the first phase)
    stmts.push_back(UseStmt("v0"));          // v0
    stmts.push_back(LocalDecl("v0", "y"));   // local v0 = y     (redefinition)
    stmts.push_back(UseStmt("keep"));        // keep  <- used AFTER the redefinition
    stmts.push_back(UseStmt("v0"));          // v0

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 2);
    CHECK_FALSE(IsDoBlock(stmts[0]));
    CHECK(IsDoBlock(stmts[1]));
    auto second = std::dynamic_pointer_cast<BlockStatementNode>(stmts[1]);
    REQUIRE(second);
    auto keep = std::dynamic_pointer_cast<VariableDeclarationNode>(second->body[0]);
    REQUIRE(keep);
    CHECK(keep->value != nullptr);
}

TEST_CASE("Scope: crossing result does not exhaust registers across reused temporaries", "[Decompiler][Scope][Regression]") {
    const auto assign = [](const std::string &left, const std::string &right) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(left)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(right))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("result", "seed"));
    for (size_t i = 0; i < 205; ++i) {
        stmts.push_back(LocalDecl("v1", "source"));
        stmts.push_back(assign("resultSink", "result"));
        stmts.push_back(assign("valueSink", "v1"));
    }

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator generator{};
    const std::string source = generator.GenerateSource(&root);
    INFO("rendered source:\n" << source);
    CHECK(Recompiles(source));
}

TEST_CASE("Scope: phase boundary keeps a producer with its redeclared consumer", "[Decompiler][Scope][Regression]") {
    const auto assign = [](const std::string &left, const std::string &right) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(left)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(right))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts{LocalDecl("persistent", "seed"), LocalDecl("v2", "seed")};
    for (size_t i = 0; i < 205; ++i) {
        stmts.push_back(LocalDecl("v3", "source"));
        stmts.push_back(LocalDecl("v2", "v3"));
        stmts.push_back(assign("persistent", "v2"));
    }

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator generator{};
    const std::string source = generator.GenerateSource(&root);
    INFO("rendered source:\n" << source);
    CHECK(Recompiles(source));
}

TEST_CASE("Scope: crossing redeclarations reuse the outer binding", "[Decompiler][Scope][Regression]") {
    const auto assign = [](const std::string &left, const std::string &right) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(left)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(right))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts{LocalDecl("persistent", "seed"), LocalDecl("v0", "seed"), LocalDecl("v1", "seed")};
    for (size_t i = 0; i < 205; ++i) {
        stmts.push_back(LocalDecl("v0", "source"));
        stmts.push_back(LocalDecl("v1", "other"));
        stmts.push_back(assign("persistent", "v0"));
    }

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator generator{};
    const std::string source = generator.GenerateSource(&root);
    INFO("rendered source:\n" << source);
    CHECK(Recompiles(source));
}

TEST_CASE("Scope: demoted redeclaration keeps the original binding visible", "[Decompiler][Scope][Regression]") {
    std::vector<std::shared_ptr<Statement>> stmts{
        LocalDecl("v0", "first"), LocalDecl("v1", "a"), LocalDecl("v1", "b"), LocalDecl("v0", "second"),
        LocalDecl("v2", "a"), LocalDecl("v2", "b"), LocalDecl("v3", "a"), LocalDecl("v3", "b"), UseStmt("v0")
    };

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE_FALSE(stmts.empty());
    CHECK_FALSE(IsDoBlock(stmts.front()));
    auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(stmts.front());
    REQUIRE(declaration);
    CHECK(std::dynamic_pointer_cast<IdentifierExpressionNode>(declaration->identifier)->identifier->name == "v0");
}

TEST_CASE("Scope: bare crossing redeclaration is not demoted to null assignment", "[Decompiler][Scope][Regression]") {
    const auto assign = [](const std::string &left, const std::string &right) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(left)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(right))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts{
        LocalDecl("v0", "first"), LocalDecl("v1", "a"),
        std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), nullptr),
        LocalDecl("v1", "b"), LocalDecl("v1", "c"), assign("sink", "v0")
    };

    ScopeBlockIntroducer{}.Run(stmts);

    size_t nullAssignments = 0;
    const auto inspect = [&](const auto &self, const auto &list) -> void {
        for (const auto &stmt : list) {
            if (const auto assignment = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt))
                nullAssignments += assignment->right == nullptr;
            if (const auto block = std::dynamic_pointer_cast<BlockStatementNode>(stmt))
                self(self, block->body);
        }
    };
    inspect(inspect, stmts);
    CHECK(nullAssignments == 0);
}

TEST_CASE("Scope: repeat condition keeps its body binding visible", "[Decompiler][Scope][Regression]") {
    auto repeat = std::make_shared<RepeatStatementNode>();
    repeat->body = std::make_shared<BlockStatementNode>();
    repeat->body->body = {LocalDecl("v0", "first"), LocalDecl("v0", "second")};
    repeat->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0"));
    std::vector<std::shared_ptr<Statement>> stmts{repeat};

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(repeat->body->body.size() == 2);
    CHECK_FALSE(IsDoBlock(repeat->body->body.back()));
}

TEST_CASE("Scope: repeat-carried redeclarations do not exhaust locals", "[Decompiler][Scope][Regression]") {
    auto repeat = std::make_shared<RepeatStatementNode>();
    repeat->body = std::make_shared<BlockStatementNode>();
    repeat->body->body.push_back(LocalDecl("v0", "seed"));
    for (size_t i = 0; i < 205; ++i)
        repeat->body->body.push_back(LocalDecl("v0", "v0"));
    repeat->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0"));
    std::vector<std::shared_ptr<Statement>> stmts{repeat};

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator generator{};
    const std::string source = generator.GenerateSource(&root);
    INFO("rendered source:\n" << source);
    CHECK(CountOccurrences(source, "local v0") == 1);
    CHECK(Recompiles(source));
}

TEST_CASE("Scope: reused temporary cuts keep a crossing table binding", "[Decompiler][Scope][Regression]") {
    const auto callDecl = [](const std::string &name, const std::string &callee) {
        auto call = std::make_shared<CallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(callee)),
            std::vector<std::shared_ptr<Expression>>{},
            std::vector<std::shared_ptr<Expression>>{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name))},
            false,
            false
        );
        return std::make_shared<ExpressionStatementNode>(call);
    };
    const auto callAssign = [](const std::string &name, const std::string &callee) {
        auto call = std::make_shared<CallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(callee)),
            std::vector<std::shared_ptr<Expression>>{},
            std::vector<std::shared_ptr<Expression>>{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name))},
            false,
            false
        );
        call->bIsLocalDeclaration = false;
        return std::make_shared<ExpressionStatementNode>(call);
    };
    const auto assign = [](const std::string &name, const std::string &value) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(value))
        );
    };
    const auto memberAssign = [](const std::string &owner, const std::string &field, const std::string &value) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<MemberExpressionNode>(
                std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(owner)),
                field
            ),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(value))
        );
    };
    const auto member = [](const std::string &owner, const std::string &field) {
        return std::make_shared<MemberExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(owner)),
            field
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts{
        callDecl("v2", "outer"), callDecl("v2", "color"), memberAssign("sink", "stroke", "v2"), assign("v2", "font"), callDecl("v2", "v2"),
        memberAssign("sink", "custom", "v2"), callAssign("v2", "shadow"), callDecl("v3", "one"), memberAssign("v2", "a", "v3"),
        callDecl("v3", "two"), memberAssign("v2", "b", "v3"), callDecl("v3", "three"), memberAssign("v2", "c", "v3"),
        std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{
            member("v2", "tag"), member("v2", "a"), member("v2", "b"), member("v2", "c")
        })
    };
    RootNode beforeRoot{stmts};
    SourceGenerator beforeGenerator{};
    const std::string before = beforeGenerator.GenerateSource(&beforeRoot);

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode afterRoot{stmts};
    SourceGenerator afterGenerator{};
    const std::string after = afterGenerator.GenerateSource(&afterRoot);
    const std::string prelude =
        "outer = function() return { tag = 'outer' } end shadow = function() return { tag = 'shadow' } end "
        "color = function() return 0 end font = function() return { tag = 'font' } end sink = {} "
        "one = function() return 1 end two = function() return 2 end three = function() return 3 end";
    INFO("rendered source:\n" << after);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(before), Luau::compile(after), {Luau::compile(prelude)});
    CHECK(verdict.original.trace == "return: \"shadow\"\t1\t2\t3\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Source: non-finite numbers preserve their values", "[Decompiler][Source][Regression]") {
    auto result = std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{
        std::make_shared<NumberLiteralNode>(std::numeric_limits<double>::infinity()),
        std::make_shared<NumberLiteralNode>(-std::numeric_limits<double>::infinity()),
        std::make_shared<NumberLiteralNode>(std::numeric_limits<double>::quiet_NaN()),
    });
    RootNode root{{result}};
    SourceGenerator generator{};
    const std::string source = generator.GenerateSource(&root);

    INFO("rendered source:\n" << source);
    REQUIRE(Recompiles(source));
    const auto verdict = fuzz::CompareSemantics(Luau::compile("return 1 / 0, -1 / 0, 0 / 0"), Luau::compile(source), {Luau::compile("")});
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Source: constant overflow preserves call argument errors", "[Decompiler][Source][Regression]") {
    const std::string source = "select(893 ^ 373, {})";
    const auto output = DecompileOrFail(source);
    INFO("rendered source:\n" << output);
    REQUIRE(Recompiles(source));
    REQUIRE(Recompiles(output));
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    REQUIRE(original.trace == "error: invalid argument #1 to 'select' (index out of range)\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

// End-to-end: with the pass on, a real decompile of register-reuse-shaped code recompiles.
TEST_CASE("Scope: pass keeps decompiled output recompilable", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
        local function f()
            local keep = workspace.Part
            local a = keep:Clone()
            a.Parent = workspace
            local b = keep:Clone()
            b.Parent = workspace
            return keep
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return"));
    CHECK(Recompiles(out));
}

TEST_CASE("Scope: module initializer keeps crossing locals outside later scopes", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(R"(
        local service = game:GetService("ReplicatedStorage")
        local cache = require("../Internal/Cache")
        local lookups = {
            Table = require("../Utilities/TableLookup"),
            Value = require("../Utilities/ValueLookup"),
            Key = require("../Utilities/KeyLookup"),
        }
        local module = {}
        function module.Create(id, key, value, lookup, sync)
            local entry = cache[id]
            if lookup then
                local kind = if lookup.Table then "Table" elseif lookup.Value then "Value" else "Key"
                local lookupFn = lookups[kind]
                local found = lookupFn and lookupFn.TableLookup(id, cache[id], lookup.Name, 1)
                if found then found[key] = value else entry[key] = value end
            else
                entry[key] = value
            end
            if not sync then return end
            send({Identity = id, indexkey = tostring(key), dataValue = value})
        end
        return module
    )");

    INFO("decompile:\n" << out);
    CHECK(out.starts_with("--[["));
    const auto firstScope = out.find("\ndo\n");
    REQUIRE(out.find("local cache") != std::string::npos);
    REQUIRE(out.find("local lookups") != std::string::npos);
    CHECK(out.find("local cache") < firstScope);
    CHECK(out.find("local lookups") < firstScope);
    CHECK(Recompiles(out));
}

TEST_CASE("Regress: lookup guard keeps its fallback for non-string keys", "[Decompiler][ControlFlow][Regression]") {
    const std::string source = R"(
        local function create(entry, key, value, lookup, found)
            if lookup and type(key) == "string" then
                if found then
                    found[key] = value
                else
                    entry[key] = value
                end
            else
                entry[key] = value
            end
        end
        local numeric = {}
        create(numeric, 7, 11, {Table = true}, nil)
        local plain = {}
        create(plain, "name", 12, nil, nil)
        return numeric[7], plain.name
    )";
    const auto out = DecompileOrFail(source);
    INFO("decompile:\n" << out);
    REQUIRE(Recompiles(out));
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(out), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 11\t12\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

// End-to-end through the SourceGenerator: a redefinition list renders as two `do ... end` scopes
// and the rendered source recompiles.
TEST_CASE("Scope: redefinition renders as do-end and recompiles", "[Decompiler][Scope][Regression]") {
    auto assign = [](const std::string &lhs, const std::string &rhs) {
        return std::make_shared<AssignmentStatementNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(lhs)),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(rhs))
        );
    };
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(LocalDecl("v0", "g1")); // local v0 = g1
    stmts.push_back(assign("h1", "v0"));    // h1 = v0
    stmts.push_back(LocalDecl("v0", "g2")); // local v0 = g2  (redefinition)
    stmts.push_back(assign("h2", "v0"));    // h2 = v0

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode root{stmts};
    SourceGenerator gen{};
    const std::string src = gen.GenerateSource(&root);

    INFO("rendered:\n" << src);
    CHECK(ContainsRegex(src, std::regex(R"((?:^|\n)\s*do\s*(?:\n|$))")));
    CHECK(ContainsRegex(src, std::regex(R"((?:^|\n)\s*end\s*(?:\n|$))")));
    CHECK(CountOccurrences(src, "do") >= 2);
    CHECK(Recompiles(src));
}

// Real lifted call-result locals are NameCall/Call nodes (rets + isLocal) wrapped in an
// ExpressionStatement, NOT VariableDeclaration. The first cut of this feature only knew
// VariableDeclaration and produced ZERO do-blocks on real output; this guards that exact shape.
TEST_CASE("Scope: ExpressionStatement-wrapped call-result redefinition is scoped", "[Decompiler][Scope][Regression]") {
    auto callDecl = [](const std::string &ret, const std::string &obj) {
        std::vector<std::shared_ptr<Expression>> args;
        std::vector<std::shared_ptr<Expression>> rets{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(ret))};
        auto nc = std::make_shared<NameCallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(obj)), std::make_shared<StringLiteralNode>("m"), args, rets, false, false
        );
        nc->bIsLocalDeclaration = true; // `local ret = obj:m()`
        return std::make_shared<ExpressionStatementNode>(nc);
    };
    std::vector<std::shared_ptr<Statement>> stmts;
    stmts.push_back(callDecl("v0", "a")); // local v0 = a:m()
    stmts.push_back(UseStmt("v0"));
    stmts.push_back(callDecl("v0", "b")); // local v0 = b:m()  (redefinition via call rets)
    stmts.push_back(UseStmt("v0"));

    ScopeBlockIntroducer{}.Run(stmts);

    REQUIRE(stmts.size() == 2);
    CHECK(IsDoBlock(stmts[0]));
    CHECK(IsDoBlock(stmts[1]));
}

// RequireRenamer: path-style require(...) -> module leaf name

// A path-style `require(A.B.Mod)` bound to an auto-named local is renamed to the
// module's leaf name, and every reference follows.
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
    )");

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
        CHECK(ContainsRegex(out, std::regex(R"(\{\s*arg0\s*\+\s*1,\s*arg1\s*\*\s*2,\s*arg0\s*-\s*arg1\s*\})")));
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
        // every element survives (the param `g` is auto-named arg0 at flags=0)...
        for (int i = 1; i <= 5; ++i) {
            const std::string call = "arg0(" + std::to_string(i) + ")";
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
TEST_CASE("Regress: variadic RETURN keeps every value", "[Decompiler][Variadic][Return][Regression]") {
    SECTION("multret call tail: return 1, 2, g()") {
        const auto out = DecompileOrFail("local function f(g) return 1, 2, g() end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+1,\s*2,\s*arg0\(\))")));
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
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\(\))")));
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
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0,\s*arg1,\s*arg2)")));
        CHECK(Recompiles(out));
    }
    SECTION("multret in non-tail position is truncated to one value: return g(), b") {
        const auto out = DecompileOrFail("local function f(g, b) return g(), b end return f");
        INFO("decompile:\n" << out);
        CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\(\),\s*arg1)")));
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
    )");

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
    )");

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
    )");

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
    )");

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
    )");
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
    )");
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
        )");
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
        )");
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
        )");
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
    )");
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
    )");
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
    )");
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
    )");
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
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+:setValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bself\.value\s*=)")));
    // member-read method -> colon + self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+:getValue\b)")));
    CHECK(ContainsRegex(out, std::regex(R"(\breturn\s+self\.value\b)")));
    // free function (no receiver use) -> stays dot, no self
    CHECK(ContainsRegex(out, std::regex(R"(\bfunction\s+v\d+\.freeFn\b)")));
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
    )");
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
        CHECK(ContainsRegex(out, std::regex(R"(\blocal\s+v\d+\s*=\s*\{)"))); // table is a local
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
        )");
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
        )");
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
        )");
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
        )");
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
