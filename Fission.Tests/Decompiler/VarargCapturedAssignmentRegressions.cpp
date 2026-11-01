//
// Regression coverage for the fuzz-found captured vararg assignment divergence.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <string>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

static void CheckTraceMatches(const std::string &source, fuzz::SemTrace::Status expectedStatus, const std::string &expectedTrace) {
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
    INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
    REQUIRE(original.status == expectedStatus);
    CHECK(original.trace == expectedTrace);
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

// `f1 = ...` writes the captured local after its closure was created. The write has no SSA reader
// (the closure reads the upvalue cell), so it must not be treated as a dead GETVARARGS result.
TEST_CASE("Regress fuzz: a vararg write to a captured local is not dropped", "[Decompiler][Regression][Semantics][Capture][Vararg]") {
    EnableLuauFFlagsOnce();
    CheckTraceMatches(
        R"LUA(
local function f1()
    print("f1")
end
local v0
v0, f1 = function()
    f1()
end, ...
v0()
)LUA",
        fuzz::SemTrace::Status::Error, "error: attempt to call a nil value\n"
    );
}

TEST_CASE("A vararg write to a captured local stays visible to its closure", "[Decompiler][Regression][Semantics][Capture][Vararg]") {
    EnableLuauFFlagsOnce();
    CheckTraceMatches(
        R"LUA(
local function run(...)
    local function f1()
        print("old")
    end
    local v0
    v0, f1 = function()
        f1()
    end, ...
    v0()
end
run(function()
    print("new")
end)
)LUA",
        fuzz::SemTrace::Status::Ok, "\"new\"\n"
    );
}
