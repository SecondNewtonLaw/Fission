// Created by Dottik on 24/9/2026.
// Co-Created using Codex.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "LiftingSemanticsTestSupport.hpp"
#include "SSABuilder.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>
#include <string>

TEST_CASE("Compiler audit: unread local keeps global read effects", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::CheckSameTraceWithPrelude(
        "local unused = watched\nprint('after')",
        R"LUA(setmetatable(getfenv(), {
    __index = function(_, key)
        if key == "watched" then print("read watched") end
    end
}))LUA"
    );
}

TEST_CASE("Compiler audit: global self-assignment keeps environment callbacks", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::CheckSameTraceWithPrelude(
        "watched = watched\nprint('after')",
        R"LUA(setmetatable(getfenv(), {
    __index = function(_, key)
        if key == "watched" then print("read watched"); return 7 end
    end,
    __newindex = function(_, key, value)
        if key == "watched" then print("write watched", value) end
    end
}))LUA"
    );
}

TEST_CASE("Compiler audit: GETIMPORT retains its full constant index", "[Decompiler][CompilerAudit][IR]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    std::string source;
    for (int i = 0; i < 270; ++i)
        source += "print('filler" + std::to_string(i) + "')\n";
    source += "return math.pi";

    Luau::CompileOptions options{};
    options.optimizationLevel = 1;
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    Fission::InstructionDecoder decoder{};
    BytecodeLifter lifter{&decoder};
    const auto lifted = lifter.LiftDeserializedBytecode(*decoded);

    bool sawHighImport = false;
    const auto &raw = decoded->lpMainFunction->instructions;
    REQUIRE(lifted.instructions.size() == raw.size());
    for (size_t pc = 0; pc < raw.size(); ++pc) {
        if (raw[pc].GetOpCode() != LOP_GETIMPORT || raw[pc].GetD() <= 255)
            continue;
        sawHighImport = true;
        REQUIRE(lifted.instructions[pc].operation == LiftedOperation::GETIMPORT);
        CHECK(lifted.instructions[pc].operands[1].value.imm.k == raw[pc].GetD());
    }
    REQUIRE(sawHighImport);
}

TEST_CASE("Compiler audit: deep table literals keep their values", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    constexpr int depth = 110;
    std::string source = "local t = " + std::string(depth, '{') + "7" + std::string(depth, '}') + "\n";
    source += "for i = 1, " + std::to_string(depth) + " do t = t[1] end\nreturn t";
    const auto bytecode = Luau::compile(source);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    const auto decompiled = lifting_semantics_test::DecompileOrFail(source);
    lifting_semantics_test::CheckSameTrace(source, decompiled, 1);
}

TEST_CASE("Compiler audit: closing captured locals preserves multret tail", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = "local x = 1\nlocal function capture() return x end\nx = 2\nreturn 5, math.modf(3.5)";
    Luau::CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 2;
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    const auto &instructions = decoded->lpMainFunction->instructions;
    bool foundProducerThenClose = false;
    for (size_t i = 0; i + 2 < instructions.size(); ++i)
        if (instructions[i].GetOpCode() == LOP_CALL && instructions[i + 1].GetOpCode() == LOP_CLOSEUPVALS && instructions[i + 2].GetOpCode() == LOP_RETURN) {
            foundProducerThenClose = true;
            break;
        }
    REQUIRE(foundProducerThenClose);
    const auto decompiled = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    INFO("decompiled:\n" << decompiled);
    lifting_semantics_test::CheckSameTrace(source, decompiled, 1);
}

TEST_CASE("Compiler audit: unused table with NaN key still raises", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = "local unused = { [0 / 0] = 1 }\nreturn 7";
    Luau::CompileOptions options{};
    options.optimizationLevel = 1;
    options.debugLevel = 2;
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    const auto decompiled = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    const auto prelude = Luau::compile("", options);
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto output = fuzz::RunLuauTrace(Luau::compile(decompiled, options), prelude);
    INFO("decompiled:\n" << decompiled);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(output.status == original.status);
    CHECK(output.trace == original.trace);
}

TEST_CASE("Compiler audit: integer parameter type survives deserialization", "[Decompiler][CompilerAudit][Types]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    Luau::CompileOptions options{};
    options.typeInfoLevel = 1;
    options.debugLevel = 2;
    const auto bytecode = Luau::compile("return function(value: integer) return value end", options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->lpMainFunction->subfunctions.size() == 1);
    auto *function = decoded->lpMainFunction->subfunctions[0];
    REQUIRE(function != nullptr);
    REQUIRE_FALSE(function->typeinfo.empty());
    CHECK(Deserializer::TryGetTypeName(function, 0) == "integer");
    const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    CHECK(output.find(": integer") != std::string::npos);
    CHECK(lifting_semantics_test::CompilesOk(output));
}

TEST_CASE("Compiler audit: global lookup stays before intervening call", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string prelude = R"LUA(local state = 0
trigger = function() state = 1 end
setmetatable(getfenv(), {
    __index = function(_, key)
        if key == "watched" then print("read", state); return state end
    end
}))LUA";
    const auto check = [&](const std::string &source, auto opcode) {
        Luau::CompileOptions options{};
        options.debugLevel = 2;
        const auto bytecode = Luau::compile(source, options);
        Deserializer deserializer{};
        const auto decoded = deserializer.Deserialize(bytecode);
        REQUIRE(decoded.has_value());
        bool found = false;
        for (const auto &instruction : decoded->lpMainFunction->instructions)
            found |= instruction.GetOpCode() == opcode;
        REQUIRE(found);
        lifting_semantics_test::CheckSameTraceWithPrelude(source, prelude);
    };
    check("local x = watched\ntrigger()\nreturn x", LOP_GETIMPORT);
    check("watched = nil\nlocal x = watched\ntrigger()\nreturn x", LOP_GETGLOBAL);
}

TEST_CASE("Compiler audit: generic loop latch reads generator state and control", "[Decompiler][CompilerAudit][SSA]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function step(state, index)
    if index < 2 then return index + 1, index + 1 end
end
local sum = 0
for key, value in step, {}, 0 do sum += value end
return sum)LUA";
    Luau::CompileOptions options{};
    options.optimizationLevel = 1;
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    Fission::InstructionDecoder decoder{};
    BytecodeLifter lifter{&decoder};
    auto lifted = lifter.LiftDeserializedBytecode(*decoded);
    ControlFlowAnalyzer cfa{};
    auto analyzed = cfa.DetermineBasicBlocks(&lifted);
    cfa.OptimizeGraph(analyzed);
    cfa.PruneUnreachable(analyzed);
    cfa.IdentifyStructures(analyzed);
    SSABuilder ssa{};
    ssa.Build(analyzed);

    bool found = false;
    for (const auto &instruction : lifted.instructions) {
        if (instruction.operation != LiftedOperation::FORGLOOP)
            continue;
        found = true;
        const auto use = analyzed.implicitUses.find(&instruction);
        REQUIRE(use != analyzed.implicitUses.end());
        REQUIRE(use->second.size() == 3);
        CHECK(use->second[0] == instruction.operands[0].ssaVersion);
    }
    REQUIRE(found);
}
