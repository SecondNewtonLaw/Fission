//
// Created by Dottik on 21/9/2026.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "ControlFlowRewriterTestSupport.hpp"
#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <limits>
#include <regex>
#include <set>
#include <string>

namespace control_flow_regression {
    const std::string kNamingEnvironment = R"LUA(
        game = {GetService = function(_, name)
            print("service", name)
            return {Core={Widget="Core.Widget",Dup="Core.Dup",task="Core.task",Inventory="Core.Inventory"},Other={Dup="Other.Dup"}}
        end}
        __integration_cache = {alpha={},beta={}}
        local lookup = {TableLookup=function(id,entry,name) print("lookup",id,name); return name=="hit" and entry or nil end}
        require = function(path)
            print("require",path)
            if path=="../Internal/Cache" then return __integration_cache end
            if string.find(path,"Lookup",1,true) then return lookup end
            return path
        end
        Instance = {new=function(name) print("new",name); return {ClassName=name} end}
        Color3 = {fromRGB=function(r,g,b) return vector.create(r/255,g/255,b/255) end}
        MyClass = {new=function() return {init=function(self) print("init"); self.initialized=true end} end}
        local player = {GetMouse=function() print("mouse"); return "mouse" end}
        game.Players = {LocalPlayer=player, GetPropertyChangedSignal=function() print("signal"); return player end}
        workspace = {clones={}}
        workspace.Part = {Clone=function()
            local value={}
            workspace.clones[#workspace.clones+1]=value
            print("clone",#workspace.clones)
            return value
        end}
        send = function(value) print("send",value.Identity,value.indexkey,value.dataValue) end
        math.randomseed(0)
    )LUA";

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::string DecompileOrFail(const std::string &source, int optLevel, int debugLevel, const std::string &driver, const std::string &environment) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = debugLevel;
        auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        integration_test::CheckSource(source, result.decompilationOutput, opts, driver, environment);
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

    // Auto-named bindings must be declared before first use.
    // Strip comment blocks; ignore globals and parameters.
    bool NoForwardReference(const std::string &decompiled) {
        const std::string s = std::regex_replace(decompiled, std::regex(R"(--\[\[[\s\S]*?\]\])"), "");
        const std::regex localRe(R"(\b(?:local|const)\s+(v\d+)\b)");
        std::set<std::string> names;
        for (std::sregex_iterator it(s.begin(), s.end(), localRe), e; it != e; ++it)
            names.insert((*it)[1].str());
        for (const auto &name : names) {
            std::smatch dm;
            if (!std::regex_search(s, dm, std::regex("\\b(?:local|const)\\s+(" + name + ")\\b")))
                continue;
            const auto declTokPos = static_cast<size_t>(dm.position(1));
            std::smatch tm;
            if (std::regex_search(s, tm, std::regex("\\b" + name + "\\b")) && static_cast<size_t>(tm.position(0)) < declTokPos)
                return false; // first appearance of vN is a use, before its declaration
        }
        return true;
    }

} // namespace control_flow_regression

TEST_CASE("Integration: initializer reads preserve shadowed bindings", "[Decompiler][Regression][Const][Integration]") {
    integration_test::Check(R"LUA(local value = tonumber("9")
local function first() return value end
do
    local value = first() + 1
    local function inner() return value end
    print(first(), inner(), value)
end
print(first(), value))LUA");
}

using namespace control_flow_regression;

TEST_CASE("Integration: reused bindings preserve phase outputs", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local value = tonumber("1")
print(value)
do
    local value = tonumber("2")
    print(value)
end
print(value))LUA");
}

TEST_CASE("Integration: crossing values keep producer order", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local calls = 0
local function nextValue() calls += 1; return calls end
local persistent = nextValue()
do local value = nextValue(); print(persistent, value) end
do local value = nextValue(); print(persistent, value) end
print(persistent, calls))LUA");
}

TEST_CASE("Integration: many temporary phases retain crossing values", "[Decompiler][Scope][Regression][Integration]") {
    std::string source = "local calls = 0; local function step() calls += 1; return calls end\nlocal saved = step(); local total = 0\n";
    for (int phase = 0; phase < 205; ++phase)
        source += "do local temporary = step(); total += saved + temporary end\n";
    source += "print(saved, total, calls)\n";
    integration_test::Check(source);
}

TEST_CASE("Integration: phase consumers use matching producers", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local calls = 0
local function nextValue() calls += 1; return calls end
do local producer = nextValue(); local consumer = producer * 2; print(consumer) end
do local producer = nextValue(); local consumer = producer * 3; print(consumer) end
print(calls))LUA");
}

TEST_CASE("Integration: phase assignments update surviving binding", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local persistent = 1
do local value = tonumber("2"); persistent = value; print(persistent) end
do local value = tonumber("3"); persistent = value; print(persistent) end
print(persistent))LUA");
}

TEST_CASE("Integration: later scopes preserve captured values", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local read
do local value = tonumber("7"); read = function() return value end end
do local value = tonumber("9"); print(read(), value) end
print(read()))LUA");
}

TEST_CASE("Integration: uninitialized shadowing preserves nil and outer value", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local value = tonumber("23")
print(value)
do
    local value
    print(value)
    value = 7
    print(value)
end
print(value))LUA");
}

TEST_CASE("Integration: repeat conditions see body bindings", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local calls = 0
local total = 0
repeat
    local value = calls + 1
    calls = value
    do local temporary = value * 2; total += temporary end
until value == 3
print(calls, total))LUA");
}

TEST_CASE("Integration: many repeat phases preserve carried state", "[Decompiler][Scope][Regression][Integration]") {
    std::string source = "local calls = 0; local function step(value) calls += 1; return value + 1 end\nlocal state = 0\nrepeat\nlocal value = state\n";
    for (int phase = 0; phase < 205; ++phase)
        source += "do local temporary = step(value); value = temporary end\n";
    source += "state = value\nuntil value > 0\nprint(state, calls)\n";
    integration_test::Check(source);
}

TEST_CASE("Integration: temporary phases preserve table identity", "[Decompiler][Scope][Regression][Integration]") {
    std::string source = "local persistent = { value = 0 }; local alias = persistent\n";
    for (int phase = 1; phase <= 205; ++phase)
        source += "do local temporary = { value = " + std::to_string(phase) + " }; persistent.value += temporary.value; alias = persistent end\n";
    source += "print(rawequal(alias, persistent), persistent.value)\n";
    integration_test::Check(source);
}

TEST_CASE("Source: non-finite numbers preserve their values", "[Decompiler][Source][Integration]") {
    fuzz::EnableLuauFlags();
    Luau::BytecodeBuilder builder;
    const auto main = builder.beginFunction(0, false);
    const auto positive = builder.addConstantNumber(std::numeric_limits<double>::infinity());
    const auto negative = builder.addConstantNumber(-std::numeric_limits<double>::infinity());
    const auto nan = builder.addConstantNumber(std::numeric_limits<double>::quiet_NaN());
    builder.emitAD(LOP_LOADK, 0, static_cast<int16_t>(positive));
    builder.emitAD(LOP_LOADK, 1, static_cast<int16_t>(negative));
    builder.emitAD(LOP_LOADK, 2, static_cast<int16_t>(nan));
    builder.emitABC(LOP_RETURN, 0, 4, 0);
    builder.endFunction(3, 0);
    builder.setMainFunction(main);
    builder.finalize();
    Decompiler decompiler;
    const auto result = decompiler.DecompileVanillaBytecode(builder.getBytecode());
    REQUIRE(result.resultCode == DecompileResult::Success);
    integration_test::CheckOutput(
        builder.getBytecode(), result.decompilationOutput, Luau::CompileOptions{1, 2}, "",
        "local positive,negative,nan=__integration_subject(); print(positive==math.huge,negative==-math.huge,nan~=nan)"
    );
}

TEST_CASE("Source: constant overflow preserves call argument errors", "[Decompiler][Source][Regression]") {
    const std::string source = "select(893 ^ 373, {})";
    const auto output = DecompileOrFail(source, 1, 2, "print(pcall(__integration_subject))");
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
    const auto out = DecompileOrFail(
        R"(
        local function f()
            local keep = workspace.Part
            local a = keep:Clone()
            a.Parent = workspace
            local b = keep:Clone()
            b.Parent = workspace
            return keep
        end
        return f
    )",
        1, 2, "local f=__integration_subject(); print(f()==workspace.Part,workspace.clones[1].Parent==workspace,workspace.clones[2].Parent==workspace)"
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return"));
    CHECK(Recompiles(out));
}

TEST_CASE("Scope: module initializer keeps crossing locals outside later scopes", "[Decompiler][Scope][Regression]") {
    const auto out = DecompileOrFail(
        R"(
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
    )",
        1, 2,
        R"DRIVER(local m=__integration_subject(); m.Create("alpha","x",7,{Table=true,Name="hit"},false); print(__integration_cache.alpha.x); m.Create("beta","x",9,{Value=true,Name="miss"},true); print(__integration_cache.beta.x); m.Create("alpha","x",11,nil,true); print(__integration_cache.alpha.x))DRIVER"
    );

    INFO("decompile:\n" << out);
    CHECK(out.starts_with("--[["));
    const auto firstScope = out.find("\ndo\n");
    REQUIRE(out.find("const cache") != std::string::npos);
    REQUIRE(out.find("const lookups") != std::string::npos);
    CHECK(out.find("const cache") < firstScope);
    CHECK(out.find("const lookups") < firstScope);
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

TEST_CASE("Integration: nested bindings preserve outer values", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local value = tonumber("1")
do local value = tonumber("2"); print(value) end
do local value = tonumber("3"); print(value) end
print(value))LUA");
}

TEST_CASE("Integration: scoped multiple results retain their bindings", "[Decompiler][Scope][Regression][Integration]") {
    integration_test::Check(R"LUA(local function pair(n) return n, n + 1 end
local a, b = pair(1)
print(a, b)
do local a, b = pair(3); print(a, b) end
print(a, b))LUA");
}

// RequireRenamer: path-style require(...) -> module leaf name

// A path-style `require(A.B.Mod)` bound to an auto-named local is renamed to the
// module's leaf name, and every reference follows.
