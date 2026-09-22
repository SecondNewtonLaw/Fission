#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"

#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("Semantic oracle normalizes runtime addresses in printed strings", "[Fuzz][SemanticOracle]") {
    const std::string bytecode = Luau::compile("print(tostring(function() end)) return 1");
    const std::string prelude = Luau::compile("");
    const auto verdict = fuzz::CompareSemantics(bytecode, bytecode, {prelude});

    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
    CHECK(verdict.original.trace == "\"function: <address>\"\nreturn: 1\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
}
