#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string CheckSemanticParity(const std::string &source) {
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        const auto originalBytecode = Luau::compile(source, kOptions);
        const auto reconstructedBytecode = Luau::compile(result.output, kOptions);
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], kOptions);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
        return result.output;
    }
} // namespace

TEST_CASE("Fuzz run 35687267534 keeps outer while body before repeat tail", "[Decompiler][FuzzRegress][Structure]") {
    const auto result = fuzz::FullDecompile(R"LUA(repeat
    while function(...)
        pairs(nil)
        return tostring, "value"
    end do
        ipairs(nil, nil)
        repeat
        until nil
        local v0 = (if false then nil else 518)
    end
    (nil)(string)
    for g0_0, g0_1 in tonumber(next, obj) do
        math("x")
        local function f2(p3)
            p3()
            return
        end
        g0_1, g0_1 = "x", t
    end
until {  }
pairs -= select:run()
t[""](math.field, {  })
return {  })LUA");
    REQUIRE(result.code == DecompileResult::Success);
    const auto &output = result.output;
    const auto closureBody = output.find("pairs(nil)");
    REQUIRE(closureBody != std::string::npos);
    CHECK(output.find("pairs(nil)", closureBody + 1) == std::string::npos);
}

TEST_CASE("Nested repeats sharing a header preserve the outer condition", "[Decompiler][FuzzRegress][Semantics]") {
    const std::string source = R"LUA(local function mark(value)
    print(value)
    return value
end

local count = 0
repeat
    repeat
        count += 1
    until mark(count >= 2)
    count += 1
until if mark(false) then false else mark(count >= 5)

print(count)
return count)LUA";
    CheckSemanticParity(source);

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, DecompilerFlags::CaptureCFGGraph | DecompilerFlags::FissionDebugNotes, kOptions);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("CFG bytes: " << result.cfgGraph.size());
    CHECK(result.cfgGraph.find("<B>WHY</B>") != std::string::npos);
    CHECK(result.cfgGraph.find("back-edge reaches B1 through inner-loop exit B6") != std::string::npos);
    CHECK(result.cfgGraph.find("preserve inner latch B4") != std::string::npos);
}
