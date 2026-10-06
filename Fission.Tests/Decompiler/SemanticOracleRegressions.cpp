#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Semantic oracle executes exported closures through a real caller", "[Fuzz][SemanticOracle][Integration]") {
    const auto prelude = Luau::compile("");
    const auto driver = Luau::compile("local f=__integration_subject(); print(f(7))");
    const auto original = fuzz::RunLuauTrace(Luau::compile("return function(value) return value*2 end"), prelude, std::chrono::milliseconds(1000), driver);
    const auto different = fuzz::RunLuauTrace(Luau::compile("return function(value) return value*3 end"), prelude, std::chrono::milliseconds(1000), driver);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(different.status == fuzz::SemTrace::Status::Ok);
    CHECK(original.comparable);
    CHECK(different.comparable);
    CHECK(original.trace == "14\n");
    CHECK(different.trace == "21\n");
}

TEST_CASE("Semantic oracle normalizes runtime addresses in printed strings", "[Fuzz][SemanticOracle]") {
    const std::string bytecode = Luau::compile("print(tostring(function() end)) return 1");
    const std::string prelude = Luau::compile("");
    const auto verdict = fuzz::CompareSemantics(bytecode, bytecode, {prelude});

    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "\"function: <address>\"\nreturn: 1\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
}

TEST_CASE("Semantic oracle runs Roblox Vector3 constants", "[Fuzz][SemanticOracle]") {
    const std::string prelude = Luau::compile("");
    const auto verdict = fuzz::CompareSemantics(
        Luau::compile("return vector.create(1, 2, 3), vector.zero, vector.create(1, 0, 0)"),
        Luau::compile("return Vector3.new(1, 2, 3), Vector3.zero, Vector3.xAxis"), {prelude}
    );

    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "return: vec(1,2,3)\tvec(0,0,0)\tvec(1,0,0)\n");
}
