//
// Created by Dottik on 5/10/2026.
//
#pragma once

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include <catch2/catch_test_macros.hpp>
#include <initializer_list>
#include <string>

namespace integration_test {
    inline void
    CheckOutput(const std::string &originalBytecode, const std::string &output, const Luau::CompileOptions &options, const std::string &environment = "") {
        INFO("decompiled:\n" << output);
        const auto reconstructedBytecode = Luau::compile(output, options);
        REQUIRE_FALSE(reconstructedBytecode.empty());
        REQUIRE(reconstructedBytecode.front() != '\0');
        const auto prelude = Luau::compile(environment, options);
        REQUIRE_FALSE(prelude.empty());
        REQUIRE(prelude.front() != '\0');
        const auto expected = fuzz::RunLuauTrace(originalBytecode, prelude);
        REQUIRE(expected.status == fuzz::SemTrace::Status::Ok);
        REQUIRE(expected.comparable);
        REQUIRE_FALSE(expected.trace.empty());
        const auto actual = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
        CHECK(actual.status == expected.status);
        CHECK(actual.comparable);
        CHECK(actual.trace == expected.trace);
    }

    inline void Check(std::initializer_list<std::string> sources) {
        fuzz::EnableLuauFlags();
        for (int optimization : {0, 1, 2}) {
            for (int debug : {0, 2}) {
                Decompiler decompiler;
                for (const auto &source : sources) {
                    INFO("O" << optimization << " D" << debug << "\n" << source);
                    const Luau::CompileOptions options{optimization, debug};
                    const auto originalBytecode = Luau::compile(source, options);
                    REQUIRE_FALSE(originalBytecode.empty());
                    REQUIRE(originalBytecode.front() != '\0');
                    const auto result = decompiler.DecompileVanillaBytecode(originalBytecode, DecompilerFlags::OptimizeIR | DecompilerFlags::AutoNameVariables);
                    REQUIRE(result.resultCode == DecompileResult::Success);
                    CheckOutput(originalBytecode, result.decompilationOutput, options);
                }
            }
        }
    }
    inline void Check(const std::string &source) { Check(std::initializer_list<std::string>{source}); }
} // namespace integration_test
