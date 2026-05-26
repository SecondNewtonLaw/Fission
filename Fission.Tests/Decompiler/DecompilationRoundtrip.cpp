#include "Decompiler.hpp"
#include "Luau/Common.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

TEST_CASE("Roundtrip: simple return", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode("return 42");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("return") != std::string::npos);
}

TEST_CASE("Roundtrip: while loop", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        return i
    )");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("while") != std::string::npos);
    CHECK(result.decompilationOutput.find("do") != std::string::npos);
    CHECK(result.decompilationOutput.find("end") != std::string::npos);
}

TEST_CASE("Roundtrip: nested calls", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode("local a = math.max(1, math.min(2, 3))");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("math.max") != std::string::npos);
    CHECK(result.decompilationOutput.find("math.min") != std::string::npos);
}

TEST_CASE("Roundtrip: table literal", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(local a = { 1, 2, 3, "hello" })");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("{") != std::string::npos);
}

TEST_CASE("Roundtrip: function declaration", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(
        local function add(a, b)
            return a + b
        end
        return add(1, 2)
    )");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("function") != std::string::npos);
    CHECK(result.decompilationOutput.find("add") != std::string::npos);
}

TEST_CASE("Roundtrip: numeric for loop", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(
        local s = 0
        for i = 1, 10 do
            s = s + i
        end
        return s
    )");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("for") != std::string::npos);
    CHECK(result.decompilationOutput.find("do") != std::string::npos);
}

TEST_CASE("Roundtrip: variable assignment with binary expression", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode("local x = 1 + 2");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("local") != std::string::npos);
}

TEST_CASE("Roundtrip: if statement", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(
        local x = math.random()
        if x > 0.5 then
            return 1
        end
        return 0
    )");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("if") != std::string::npos);
    CHECK(result.decompilationOutput.find("then") != std::string::npos);
}

TEST_CASE("Roundtrip: repeat-until loop", "[Decompiler][Roundtrip]") {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(R"(
        local i = 0
        repeat
            i = i + 1
        until i >= 10
        return i
    )");

    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("return") != std::string::npos);
}
