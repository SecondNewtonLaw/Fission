//
// Created by Dottik on 24/9/2026.
// Co-Created using Codex.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "AbstractSyntaxTree/ASTNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "ControlFlowTestSupport.hpp"
#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
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

TEST_CASE("Regress: phi-merged call result is not re-declared with local", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        return function(cs)
            local isSubj = cs and cs:IsA("VehicleSeat") or false
            print(isSubj)
        end
    )LUA",
        R"DRIVER(local f=__integration_subject(); f(nil); for _,answer in {false,true} do f({IsA=function(_,name) print(name); return answer end}) end)DRIVER"
    );
}

TEST_CASE("Regress: comparison stored as boolean value is reconstructed", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        return function(self)
            local x = self.occlusionMode ~= "Invisicam"
            print(x)
        end
    )LUA",
        R"DRIVER(local f=__integration_subject(); f({occlusionMode="Invisicam"}); f({occlusionMode="Zoom"}); f({}))DRIVER"
    );
}

TEST_CASE("Regress: numeric for with continue and break", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(local f=__integration_subject(); f({},3); f({1,7,2,0,3},3); f({1,2},3))DRIVER"
    );
}

TEST_CASE("Regress: and-chain conditional reassign does not shadow", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f({})); for _,kind in {"GroundController","ClimbController","Other"} do print(f({ActiveController={IsA=function(_,name) print(name); return name==kind end},GroundSensor={SensedPart="ground"},ClimbSensor={SensedPart="climb"}})) end)DRIVER"
    );
}

TEST_CASE("Regress: not-equal if-branch keeps correct polarity", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function g(x)
            if x ~= "b" then
                return 10
            end
            return 20
        end
        return g
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f("a"),f("b"),f("c")))DRIVER"
    );
}

TEST_CASE("Regress: module table-field closure is sugared to dot declaration", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local t = {}
        function t.foo()
            return 42
        end
        return t
    )LUA",
        R"DRIVER(print(__integration_subject().foo()))DRIVER"
    );
}

TEST_CASE("Regress: module dot-method keeps all parameters", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local m = {}
        function m.add(a, b)
            return a + b
        end
        return m
    )LUA",
        R"DRIVER(local m=__integration_subject(); print(m.add(2,3),m.add(-4,7)))DRIVER"
    );
}

TEST_CASE("Regress: class colon-method is sugared to method syntax", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local Animal = {}
        Animal.__index = Animal
        function Animal:speak(volume)
            return self.name, volume
        end
        return Animal
    )LUA",
        R"DRIVER(local Animal=__integration_subject(); local a=setmetatable({name="cat"},Animal); print(a:speak(3)); print(a:speak(7)))DRIVER"
    );
}

TEST_CASE("Regress: empty constructor does not absorb a reused-register table", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(mt)
            local self = setmetatable({}, mt)
            self.list = {1, 2, 3}
            return self
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); local a,b=f({}),f({}); a.list[1]=9; print(a.list[1],b.list[1],a.list[3]))DRIVER"
    );
}

TEST_CASE("Regress: constant table constructor still coalesces", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f()
            local t = { a = 1, b = 2, c = "x" }
            return t
        end
        return f
    )LUA",
        R"DRIVER(local t=__integration_subject()(); print(t.a,t.b,t.c))DRIVER"
    );
}

TEST_CASE("Regress: nested REF upvalue capture aliases instead of redefining", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(_G.globalValue=3; local f=__integration_subject(); _G.globalValue=7; f(); _G.globalValue=11; f())DRIVER"
    );
}

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
    integration_test::CheckSource(source, out, opts, "print(__integration_subject()())");
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
    CHECK(Contains(out, "local __fissionVectorCtor = vector.create"));
    CHECK(Contains(out, "__fissionVectorCtor(1, 2, 3)")); // LOADK vector
    CHECK(Contains(out, "__fissionVectorCtor(4, 5, 6)")); // vector inside a constant table
    CHECK(Contains(out, "__fissionVectorCtor(7, 8, 9)"));
    CHECK_FALSE(Contains(out, "nil")); // no constant silently dropped to nil
    integration_test::CheckSource(source, out, opts, "local f=__integration_subject(); local v,t=f(); print(v,t[1],t[2])");
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
    const std::string driver = "local outer=__integration_subject(); local first,second=outer(),outer(); print(first().x,second().x)";
    integration_test::CheckSource(source, withInfo.decompilationOutput, opts, driver);
    integration_test::CheckSource(source, noInfo.decompilationOutput, opts, driver);
}

// A nested function's own vN must not shadow a captured upvalue of the same name
// A nested function reuses low register numbers, so its own `vN` can equal a
// captured upvalue that (after aliasing) reads as the enclosing scope's `vN`.
// Rendered inline, the inner `local vN` then shadows the upvalue. The inner's own
// auto-name must be disambiguated (suffixed) so it stays distinct.
TEST_CASE("Regress: nested function own vN does not shadow captured upvalue vN", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(local outer=__integration_subject(); local a,b=outer(),outer(); print(a().x,b().x,a().x))DRIVER"
    );
}

TEST_CASE("Regress: sibling VAL captures alias sources without uv_N collision", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(local outer=__integration_subject(); local a,b=outer(); local t3=a(); local t1=b(); print(t1[1][1]==t3,a()==t3,b()==t1))DRIVER"
    );
}

TEST_CASE("Regress: LCT_UPVAL capture resolves to the parent upvalue, no uv_0 collision", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
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
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f(),f()))DRIVER"
    );
}

TEST_CASE("Regress: generalized for-in does not emit trailing nil iterator args", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(
        local function f(t)
            local sum = 0
            for k, v in t do
                sum = sum + v
            end
            return sum
        end
        return f
    )LUA",
        R"DRIVER(local f=__integration_subject(); print(f({}),f({2,3,4}),f({-3,7})))DRIVER"
    );
}

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

TEST_CASE("Regress: dead `local` from an or-step is eliminated, the for survives", "[Decompiler][Regression][Integration]") {
    integration_test::Check(
        R"LUA(local a = ({})
        for attempt=1,3 do for i = 1, 1, #"67" or 9 do table.insert(a, i) break end end
return a[1],a[2],a[3])LUA",
        R"DRIVER(print(__integration_subject()))DRIVER"
    );
}
