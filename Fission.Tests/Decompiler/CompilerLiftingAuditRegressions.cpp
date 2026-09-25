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

TEST_CASE("Compiler short-circuit fallback remains reachable after false primary", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function k(a, b, c, d)
    return (a and b) and c or d
end
print(k(true, true, false, 7))
print(k(false, true, true, 8))
return k(true, true, 5, 9))LUA";
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
        lifting_semantics_test::CheckSameTrace(source, output, optLevel);
    }
}

TEST_CASE("Compiler shared short-circuit fallback runs after a false first value", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function nested(x, y, z)
    return (x and y) or (z and 42) or "fallback"
end
print(nested(true, false, false))
print(nested(true, nil, true))
print(nested(true, 5, false)))LUA";
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
        INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(source, output, optLevel);
    }
}

TEST_CASE("Compiler variadic table list preserves all results after computed fields", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function key() print("key"); return "x" end
local function value() print("value"); return 1 end
local function make(...)
    local t = {[key()] = value(), 10, [key()] = value(), ...}
    print(t.x, t[1], t[2], t[3], t[4])
end
local function pass(...) return ... end
local function makeCall(...)
    local t = {[key()] = value(), 10, [key()] = value(), pass(...)}
    print(t.x, t[1], t[2], t[3], t[4])
end
make(10, false)
make()
make(10, nil, false)
makeCall(10, false))LUA";
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
        INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(source, output, optLevel);
    }
}

TEST_CASE("Compiler variadic table list preserves results after global keys", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function value() print("value"); return 1 end
local function make(...)
    local t = {[keyA] = value(), 10, [keyB] = value(), ...}
    print(t.x, t.y, t[1], t[2], t[3])
end
keyA = "x"
keyB = "y"
make(10, false))LUA";
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
        INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(source, output, optLevel);
    }
}

TEST_CASE("Compiler variadic table list preserves results after conditional keys", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local function make(c, ...)
    local t = {[c and "x" or "y"] = 1, 10, [c and "a" or "b"] = 2, ...}
    print(t.x, t.y, t.a, t.b, t[1], t[2], t[3])
end
make(true, 10, false)
make(false, 10, false))LUA";
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
        INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(source, output, optLevel);
    }
}

TEST_CASE("Compiler arithmetic chains remain recompilable", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    std::string source = "local x = 0\ncold = true\nif cold then\n";
    for (int i = 0; i < 10000; ++i)
        source += "x += 1\n";
    source += "end\nreturn x\n";
    const auto output = lifting_semantics_test::DecompileOrFail(source, 0);
    REQUIRE(lifting_semantics_test::CompilesOk(output));
    lifting_semantics_test::CheckSameTrace(source, output, 0);
}

TEST_CASE("Compiler method debug name does not shadow visible bindings", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string capturesOuter = R"LUA(local function count()
    print("outer")
    return 3
end
local obj = {}
function obj:count()
    return count()
end
print(obj:count(), count()))LUA";
    const std::string usesOuterLater = R"LUA(local function count()
    return 3
end
local obj = {}
function obj:count()
    return 4
end
print(obj:count(), count()))LUA";
    const std::string shadowsGlobal = R"LUA(local obj = {}
function obj:print()
    print("inner")
    return 4
end
print("outer")
print(obj:print()))LUA";
    for (int optLevel : {0, 1}) {
        for (const auto &source : {capturesOuter, usesOuterLater}) {
            const auto output = lifting_semantics_test::DecompileOrFail(source, optLevel);
            INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
            lifting_semantics_test::CheckSameTrace(source, output, optLevel);
        }
    }
    for (int optLevel : {0, 1, 2}) {
        const auto output = lifting_semantics_test::DecompileOrFail(shadowsGlobal, optLevel);
        INFO("optimization level: " << optLevel << "\ndecompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(shadowsGlobal, output, optLevel);
    }
}

TEST_CASE("Compiler vector constant does not bind to a captured Vector3 local", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = R"LUA(local v = Vector3.new(1, 2, 3)
local Vector3 = 5
local function read() return Vector3 end
Vector3 += 1
print(read(), v))LUA";
    Luau::CompileOptions options{};
    options.optimizationLevel = 2;
    options.debugLevel = 2;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    bool foundVector = false;
    for (const auto &constant : decoded->lpMainFunction->constants)
        foundVector |= constant.kType == LUA_TVECTOR;
    REQUIRE(foundVector);
    const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    INFO("decompiled:\n" << output);
    REQUIRE(lifting_semantics_test::Contains(output, "local __fissionVectorCtor = Vector3.new"));
    REQUIRE(lifting_semantics_test::Contains(output, "__fissionVectorCtor(1, 2, 3)"));
    const auto prelude = Luau::compile("", options);
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output, options), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Compiler vector.create constant survives a Vector3 global write", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    const std::string source = "Vector3 = 5\nlocal v = vector.create(1, 2, 3)\nprint(v)";
    Luau::CompileOptions options{};
    options.optimizationLevel = 2;
    options.debugLevel = 2;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    bool foundVector = false;
    for (const auto &constant : decoded->lpMainFunction->constants)
        foundVector |= constant.kType == LUA_TVECTOR;
    REQUIRE(foundVector);
    const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    INFO("decompiled:\n" << output);
    const auto captureAt = output.find("local __fissionVectorCtor = Vector3.new");
    REQUIRE(captureAt != std::string::npos);
    REQUIRE(captureAt < output.find("Vector3 = 5"));
    REQUIRE(lifting_semantics_test::Contains(output, "print(__fissionVectorCtor(1, 2, 3))"));
    const auto prelude = Luau::compile("", options);
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output, options), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Compiler vector constant survives a Vector3 write through _G", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    Luau::CompileOptions options{};
    options.optimizationLevel = 2;
    options.debugLevel = 2;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";
    const auto prelude = Luau::compile("", options);
    for (const std::string &source : {
             std::string{"_G.Vector3 = 5\nlocal v = vector.create(1, 2, 3)\nprint(v)"},
             std::string{R"LUA(local function key(n)
    if n == 0 then return "Vector3" end
    return key(n - 1)
end
_G[key(1)] = 5
print(vector.create(1, 2, 3)))LUA"},
         }) {
        const auto bytecode = Luau::compile(source, options);
        REQUIRE_FALSE(bytecode.empty());
        REQUIRE(bytecode[0] != '\0');
        Deserializer deserializer{};
        const auto decoded = deserializer.Deserialize(bytecode);
        REQUIRE(decoded.has_value());
        bool foundVector = false;
        for (const auto &constant : decoded->lpMainFunction->constants)
            foundVector |= constant.kType == LUA_TVECTOR;
        REQUIRE(foundVector);
        const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
        INFO("source:\n" << source << "\ndecompiled:\n" << output);
        const auto captureAt = output.find("local __fissionVectorCtor = Vector3.new");
        REQUIRE(captureAt != std::string::npos);
        REQUIRE(captureAt < output.find("_G"));
        if (source.find("local function key") != std::string::npos)
            REQUIRE(lifting_semantics_test::Contains(output, "_G["));
        const auto original = fuzz::RunLuauTrace(bytecode, prelude);
        const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output, options), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(reconstructed.status == original.status);
        CHECK(reconstructed.trace == original.trace);
    }
}

TEST_CASE("Compiler nonfinite vector components survive source generation", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    Luau::CompileOptions options{};
    options.optimizationLevel = 2;
    options.debugLevel = 2;
    options.vectorLib = "Vector3";
    options.vectorCtor = "new";
    options.vectorType = "Vector3";
    const auto bytecode = Luau::compile("print(vector.create(math.huge, -math.huge, math.sqrt(-1)))", options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode[0] != '\0');
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    bool foundVector = false;
    for (const auto &constant : decoded->lpMainFunction->constants)
        foundVector |= constant.kType == LUA_TVECTOR;
    REQUIRE(foundVector);
    const auto output = lifting_semantics_test::DecompileVanillaOrFail(bytecode);
    INFO("decompiled:\n" << output);
    const auto prelude = Luau::compile("", options);
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output, options), prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Inlined early return preserves the natural loop exit", "[Decompiler][CompilerAudit][Semantics]") {
    const std::string body = R"LUA(print((function()
    local x = 0
    if flag then
        while x < 1 do
            if early then return x end
            x += 1
        end
    end
    print("after short", x)
    return x
end)()))LUA";
    for (const std::string prefix : {"flag = true\n", "early = true\nflag = true\n", "flag = false\n"}) {
        const auto source = prefix + body;
        const auto output = lifting_semantics_test::DecompileOrFail(source, 2);
        INFO("decompiled:\n" << output);
        lifting_semantics_test::CheckSameTrace(source, output, 2);
    }

    std::string longBody = body;
    const auto returnAt = longBody.find("    return x\n");
    REQUIRE(returnAt != std::string::npos);
    for (int i = 0; i < 20; ++i)
        longBody.insert(returnAt, "    print(\"tail\", x)\n");
    const auto longSource = "flag = true\n" + longBody;
    const auto longOutput = lifting_semantics_test::DecompileOrFail(longSource, 2);
    INFO("decompiled long tail:\n" << longOutput);
    lifting_semantics_test::CheckSameTrace(longSource, longOutput, 2);

    std::string branchedBody = body;
    const auto branchAt = branchedBody.find("    return x\n");
    REQUIRE(branchAt != std::string::npos);
    for (int i = 0; i < 8; ++i)
        branchedBody.insert(branchAt, "    if branch" + std::to_string(i) + " then print(\"yes\", x) else print(\"no\", x) end\n");
    const auto branchedSource = "flag = true\n" + branchedBody;
    const auto branchedOutput = lifting_semantics_test::DecompileOrFail(branchedSource, 2);
    INFO("decompiled branched tail:\n" << branchedOutput);
    lifting_semantics_test::CheckSameTrace(branchedSource, branchedOutput, 2);
}

TEST_CASE("Compiler long alias chain snapshots an upvalue before mutation", "[Decompiler][CompilerAudit][Semantics]") {
    lifting_semantics_test::EnableLuauFFlagsOnce();
    std::string source = "local u = 1\nlocal function f()\nlocal a0 = u\n";
    for (int i = 1; i <= 65; ++i)
        source += "local a" + std::to_string(i) + " = a" + std::to_string(i - 1) + "\n";
    source += "u = 2\nreturn a65\nend\nreturn f()";
    const auto output = lifting_semantics_test::DecompileOrFail(source, 0);
    INFO("decompiled:\n" << output);
    lifting_semantics_test::CheckSameTrace(source, output, 0);
}
