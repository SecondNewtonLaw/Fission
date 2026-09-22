//
// Created by Dottik on 22/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace ExpressionLifetimeRegressions {
    void CheckSemanticParity(const std::string &source) {
        fuzz::EnableLuauFlags();
        std::string originalBytecode;
        REQUIRE(fuzz::LuauCompiles(source, &originalBytecode));
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);

        std::string reconstructedBytecode;
        REQUIRE(fuzz::LuauCompiles(result.output, &reconstructedBytecode));
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            std::string prelude;
            REQUIRE(fuzz::LuauCompiles(fuzz::kSemPreludes[i], &prelude));
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            REQUIRE(original.status != fuzz::SemTrace::Status::LoadFailed);
            REQUIRE(original.status != fuzz::SemTrace::Status::Timeout);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
}

TEST_CASE("Expression lifetime evaluates reused method results once", "[Decompiler][ExpressionLifetime]") {
    ExpressionLifetimeRegressions::CheckSemanticParity(R"LUA(local v0 = "a-b";
v0 = (not t:get());
v0((-{ "x", [v0] = next, [nil] = math }));
return math:method();
)LUA");
}

TEST_CASE("Expression lifetime preserves table reads before overwrite", "[Decompiler][ExpressionLifetime]") {
    ExpressionLifetimeRegressions::CheckSemanticParity(R"LUA(local v0 = ({  })[(if next then nil else true)]
local v1 = nil
local v2 = function()
    return v1(699, false)
end
v0()
local v3 = nil
return function()
    return - -30.428571428571427
end
)LUA");
}
