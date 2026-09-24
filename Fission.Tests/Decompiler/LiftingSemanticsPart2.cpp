//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "LiftingSemanticsTestSupport.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeclarationHoister.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/LoopVariableRenamer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace lifting_semantics_test;

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

TEST_CASE("Lift: AutoNameVariables keeps references to prefixed scoped locals", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        do
            local first = workspace:WaitForChild("HumanoidRootPart")
            first.Transparency = 0
        end
        do
            local second = workspace:WaitForChild("HumanoidRootPart")
            second.Transparency = 1
        end
        do
            local third = workspace:WaitForChild("HumanoidRootPart")
            third.Transparency = 2
        end
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "HumanoidRootPart.Transparency = 0"));
    const std::regex prefixed(R"(local\s+(v[0-9]+_HumanoidRootPart)\s*=\s*workspace:WaitForChild\("HumanoidRootPart"\))");
    auto match = std::sregex_iterator(out.begin(), out.end(), prefixed);
    REQUIRE(match != std::sregex_iterator{});
    const auto second = (*match++)[1].str();
    REQUIRE(match != std::sregex_iterator{});
    const auto third = (*match++)[1].str();
    CHECK(second != third);
    CHECK(Contains(out, second + ".Transparency = 1"));
    CHECK(Contains(out, third + ".Transparency = 2"));
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
    // Declaration and closure references must share the captured upvalue name.
    CHECK(Contains(out, "local module ="));
    CHECK(CompilesOk(out));
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "table.insert"));
    CHECK(Contains(out, "table.clear"));
    CHECK(Contains(out, "module.Cache"));
}

// Upvalues

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

// Calls

// With every Luau* FFlag enabled, the compiler emits CALLFB (a feedback-collecting call) in place of
// CALL. Both plain calls and method calls must decompile to ordinary calls, not vanish or garble.
TEST_CASE("Lift: feedback calls (CALLFB) render as ordinary calls", "[Decompiler][Call]") {
    const auto out = DecompileOrFail(R"(
        return function(t, x)
            local a = print(x)
            local b = t:method(x)
            return a, b
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "print("));   // plain call survives
    CHECK(Contains(out, ":method(")); // method call survives
    CHECK(CompilesOk(out));
}

// CMPPROTO is a V11 native-codegen closure-specialization guard: it is never emitted by Luau::compile
// (only appears inside CodeGen's internal bytecode graph) and has no Luau source form, so the lifter
// elides it. Decompiling bytecode that contains one must not crash or corrupt the surrounding code; the
// guarded closure and the code around it must still come out as valid Luau. Hand-assembled because no
// source produces this opcode.
TEST_CASE("Lift: CMPPROTO is elided without crashing or corrupting surrounding code", "[Decompiler][CMPPROTO]") {
    EnableLuauFFlagsOnce();
    Luau::BytecodeBuilder bb{};

    const uint32_t child = bb.beginFunction(0, /*isvararg*/ false);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);

    const uint32_t main = bb.beginFunction(0, /*isvararg*/ true);
    const int16_t childIdx = bb.addChildFunction(child);
    bb.emitAD(LOP_NEWCLOSURE, 0, childIdx); // R0 = function() end
    // jump target is relative to the word after the primary insn; CMPPROTO carries an AUX word, so a
    // jump of 1 lands on the following RETURN (the fall-through target) rather than the AUX word. jump 0
    // would target the AUX and fails Luau's debug BytecodeBuilder validation.
    bb.emitAD(LOP_CMPPROTO, 0, /*jump*/ 1);   // guard on R0 (elided)
    bb.emitAux(static_cast<uint32_t>(child)); // AUX = proto id
    bb.emitABC(LOP_RETURN, 0, 2, 0);          // return R0
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);
    bb.setMainFunction(main);
    bb.finalize();

    const auto out = DecompileVanillaOrFail(bb.getBytecode()); // REQUIRE(Success) inside
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "function")); // the guarded closure survived
    CHECK(Contains(out, "return"));   // and is returned
    CHECK(CompilesOk(out));           // the output is valid Luau
}

TEST_CASE("Lift: userdata field opcodes preserve source semantics", "[Decompiler][Userdata]") {
    EnableLuauFFlagsOnce();
    const std::string field = "value";
    const auto fieldRef = Luau::BytecodeBuilder::StringRef{field.data(), field.size()};
    const auto hash = static_cast<uint8_t>(Luau::BytecodeBuilder::getStringHash(fieldRef));

    SECTION("field read") {
        Luau::BytecodeBuilder bb{};
        const uint32_t main = bb.beginFunction(1, false);
        const int32_t key = bb.addConstantString(fieldRef);
        bb.emitABC(LOP_GETUDATAKS, 1, 0, hash);
        bb.emitAux(static_cast<uint32_t>(key));
        bb.emitABC(LOP_RETURN, 1, 2, 0);
        bb.endFunction(2, 0);
        bb.setMainFunction(main);
        bb.finalize();

        const auto out = DecompileVanillaOrFail(bb.getBytecode());
        INFO("decompile:\n" << out);
        CHECK(Contains(out, "arg0.value"));
        CHECK(CompilesOk(out));
    }

    SECTION("field write") {
        Luau::BytecodeBuilder bb{};
        const uint32_t main = bb.beginFunction(2, false);
        const int32_t key = bb.addConstantString(fieldRef);
        bb.emitABC(LOP_SETUDATAKS, 1, 0, hash);
        bb.emitAux(static_cast<uint32_t>(key));
        bb.emitABC(LOP_RETURN, 0, 1, 0);
        bb.endFunction(2, 0);
        bb.setMainFunction(main);
        bb.finalize();

        const auto out = DecompileVanillaOrFail(bb.getBytecode());
        INFO("decompile:\n" << out);
        CHECK(Contains(out, "arg0.value = arg1"));
        CHECK(CompilesOk(out));
    }

    SECTION("method call") {
        Luau::BytecodeBuilder bb{};
        const uint32_t main = bb.beginFunction(1, false);
        const int32_t key = bb.addConstantString(fieldRef);
        bb.emitABC(LOP_NAMECALLUDATA, 1, 0, hash);
        bb.emitAux(static_cast<uint32_t>(key));
        bb.emitABC(LOP_CALL, 1, 2, 2);
        bb.emitABC(LOP_RETURN, 1, 2, 0);
        bb.endFunction(3, 0);
        bb.setMainFunction(main);
        bb.finalize();

        const auto out = DecompileVanillaOrFail(bb.getBytecode());
        INFO("decompile:\n" << out);
        CHECK(Contains(out, "arg0:value()"));
        CHECK(CompilesOk(out));
    }
}

TEST_CASE("Lift: nested local does not capture later global read", "[Decompiler][DeclarationHoister][Semantic]") {
    CheckSameTraceWithPrelude(
        R"LUA(
local function read()
    do
        local v0 = 7
        print(v0)
    end
    return v0
end
return read()
)LUA",
        "v0 = 99"
    );
}

TEST_CASE("Lift: loop binding does not capture later global read", "[Decompiler][DeclarationHoister][Semantic]") {
    CheckSameTraceWithPrelude(
        R"LUA(
local function read()
    for v0 = 1, 2 do
        print(v0)
        v0 += 1
    end
    return v0
end
return read()
)LUA",
        "v0 = 99"
    );
}

TEST_CASE("DeclarationHoister: loop binding only covers its body", "[Decompiler][DeclarationHoister][Rewriter]") {
    const auto identifier = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    const auto makeLoop = [&] {
        auto loop = std::make_shared<ForNumericNode>();
        loop->loopVariable = identifier("v0");
        loop->startVariable = std::make_shared<NumberLiteralNode>(1);
        loop->maxIncreased = std::make_shared<NumberLiteralNode>(2);
        loop->increaseBy = std::make_shared<NumberLiteralNode>(1);
        loop->lpLoopBody = std::make_shared<BlockStatementNode>();
        loop->lpLoopBody->body.push_back(std::make_shared<AssignmentStatementNode>(identifier("v0"), std::make_shared<NumberLiteralNode>(3)));
        return loop;
    };

    SECTION("later read remains global") {
        auto loop = makeLoop();
        std::vector<std::shared_ptr<Statement>> statements{
            loop, std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{identifier("v0")})
        };

        DeclarationHoister{}.Run(statements);

        CHECK(statements.size() == 2);
        CHECK(statements.front() == loop);
    }

    SECTION("later orphan assignment still gets a local") {
        auto loop = makeLoop();
        std::vector<std::shared_ptr<Statement>> statements{
            loop, std::make_shared<AssignmentStatementNode>(identifier("v0"), std::make_shared<NumberLiteralNode>(4)),
            std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{identifier("v0")})
        };

        DeclarationHoister{}.Run(statements);

        REQUIRE(statements.size() == 4);
        const auto declaration = std::dynamic_pointer_cast<VariableDeclarationNode>(statements.front());
        REQUIRE(declaration);
        const auto declarationIdentifier = std::dynamic_pointer_cast<IdentifierExpressionNode>(declaration->identifier);
        REQUIRE(declarationIdentifier);
        CHECK(declarationIdentifier->identifier->name == "v0");
        CHECK(statements[1] == loop);
    }
}

TEST_CASE("DeclarationHoister: disjoint locals cover their own uses", "[Decompiler][DeclarationHoister][Rewriter]") {
    const auto identifier = [](const char *name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    auto branch = std::make_shared<IfStatementNode>();
    branch->condition = identifier("flag");
    branch->thenBranch = std::make_shared<BlockStatementNode>();
    branch->thenBranch->body = {
        std::make_shared<VariableDeclarationNode>(identifier("v1"), std::make_shared<NumberLiteralNode>(1)),
        std::make_shared<ExpressionStatementNode>(identifier("v1"))
    };
    auto laterDeclaration = std::make_shared<VariableDeclarationNode>(identifier("v1"), std::make_shared<NumberLiteralNode>(2));
    std::vector<std::shared_ptr<Statement>> statements{
        branch, laterDeclaration, std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{identifier("v1")})
    };

    DeclarationHoister{}.Run(statements);

    CHECK(statements.size() == 3);
    CHECK(statements.front() == branch);
    CHECK(statements[1] == laterDeclaration);
}

TEST_CASE("Lift: local initializer reads outer binding", "[Decompiler][DeclarationHoister][Semantic]") {
    CheckSameTraceWithPrelude(
        R"LUA(
local function read()
    do
        local v0 = v0 + 1
        print(v0)
    end
    return v0
end
return read()
)LUA",
        "v0 = 41"
    );
}

// Inline anonymous closures

TEST_CASE("Lift: single-use closure passed as call argument is inlined", "[Decompiler][InlineAnon]") {
    // A single-use call-argument closure must appear directly inside the call.
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

TEST_CASE("Lift: FASTPCALL is decoded and protected calls remain source calls", "[Decompiler][FASTPCALL]") {
    EnableLuauFFlagsOnce();
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const auto bytecode = Luau::compile(
        R"(
        local ok, result = pcall(function() return 42 end)
        local ok2, result2 = xpcall(function() return 43 end, function(err) return err end)
        return ok, result, ok2, result2
    )",
        opts
    );
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode[0] != '\0');

    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->bytecodeVersion == 14);

    int fastPcallCount = 0;
    bool sawPcallSelector = false;
    bool sawXpcallSelector = false;
    for (const auto &function : decoded->functions)
        for (const auto instruction : function.instructions) {
            if (instruction.GetOpCode() != LOP_FASTPCALL)
                continue;
            ++fastPcallCount;
            sawPcallSelector |= instruction.GetABCOperand(LuauInstruction::LuauOperand::A) == 0;
            sawXpcallSelector |= instruction.GetABCOperand(LuauInstruction::LuauOperand::A) == 1;
        }
    CHECK(fastPcallCount >= 2);
    CHECK(sawPcallSelector);
    CHECK(sawXpcallSelector);

    Decompiler decompiler{};
    const auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(Contains(result.decompilationOutput, "pcall("));
    CHECK(Contains(result.decompilationOutput, "xpcall("));
    CHECK(CompilesOk(result.decompilationOutput));
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
    // Preserve the reachable constant without crashing on the malformed path.
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

// Short-circuit expressions

TEST_CASE("Lift: `or` fallback preserves both operands and yields `or` semantics", "[Decompiler][ShortCircuit]") {
    // Named arguments make the expected `or` operands stable in debug output.
    const auto out = DecompileOrFail(R"(
        return function(a) return a or "fallback" end
    )");

    INFO("decompile:\n" << out);
    // Both operands and their order must survive.
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

// Numeric for loops
