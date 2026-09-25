//
// Created by Dottik on 21/9/2026.
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

    std::string DecompileOrFail(const std::string &source, int optLevel, int debugLevel) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = debugLevel;
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
    std::vector<std::shared_ptr<Statement>> stmts{LocalDecl("v0", "first"),  LocalDecl("v1", "a"), LocalDecl("v1", "b"),
                                                  LocalDecl("v0", "second"), LocalDecl("v2", "a"), LocalDecl("v2", "b"),
                                                  LocalDecl("v3", "a"),      LocalDecl("v3", "b"), UseStmt("v0")};

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
        LocalDecl("v0", "first"),
        LocalDecl("v1", "a"),
        std::make_shared<VariableDeclarationNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("v0")), nullptr),
        LocalDecl("v1", "b"),
        LocalDecl("v1", "c"),
        assign("sink", "v0")
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
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(callee)), std::vector<std::shared_ptr<Expression>>{},
            std::vector<std::shared_ptr<Expression>>{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name))}, false, false
        );
        return std::make_shared<ExpressionStatementNode>(call);
    };
    const auto callAssign = [](const std::string &name, const std::string &callee) {
        auto call = std::make_shared<CallExpressionNode>(
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(callee)), std::vector<std::shared_ptr<Expression>>{},
            std::vector<std::shared_ptr<Expression>>{std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name))}, false, false
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
            std::make_shared<MemberExpressionNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(owner)), field),
            std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(value))
        );
    };
    const auto member = [](const std::string &owner, const std::string &field) {
        return std::make_shared<MemberExpressionNode>(std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(owner)), field);
    };
    std::vector<std::shared_ptr<Statement>> stmts{
        callDecl("v2", "outer"),
        callDecl("v2", "color"),
        memberAssign("sink", "stroke", "v2"),
        assign("v2", "font"),
        callDecl("v2", "v2"),
        memberAssign("sink", "custom", "v2"),
        callAssign("v2", "shadow"),
        callDecl("v3", "one"),
        memberAssign("v2", "a", "v3"),
        callDecl("v3", "two"),
        memberAssign("v2", "b", "v3"),
        callDecl("v3", "three"),
        memberAssign("v2", "c", "v3"),
        std::make_shared<ReturnStatementNode>(
            std::vector<std::shared_ptr<Expression>>{member("v2", "tag"), member("v2", "a"), member("v2", "b"), member("v2", "c")}
        )
    };
    RootNode beforeRoot{stmts};
    SourceGenerator beforeGenerator{};
    const std::string before = beforeGenerator.GenerateSource(&beforeRoot);

    ScopeBlockIntroducer{}.Run(stmts);

    RootNode afterRoot{stmts};
    SourceGenerator afterGenerator{};
    const std::string after = afterGenerator.GenerateSource(&afterRoot);
    const std::string prelude = "outer = function() return { tag = 'outer' } end shadow = function() return { tag = 'shadow' } end "
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
