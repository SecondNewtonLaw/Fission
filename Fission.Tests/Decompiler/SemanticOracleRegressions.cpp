#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>

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
