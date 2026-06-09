#include "Decompiler.hpp"
#include "Luau/Common.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <regex>
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

static std::string DecompileOrFail(const std::string &source) {
    EnableLuauFFlagsOnce();

    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);

    REQUIRE(result.resultCode == DecompileResult::Success);
    return std::move(result.decompilationOutput);
}

static bool ContainsRegex(const std::string &source, const std::regex &pattern) { return std::regex_search(source, pattern); }

static size_t CountRegex(const std::string &source, const std::regex &pattern) {
    return static_cast<size_t>(std::distance(std::sregex_iterator(source.begin(), source.end(), pattern), std::sregex_iterator{}));
}

TEST_CASE("Roundtrip: simple return", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return 42");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*return\s+42\s*$)")));
}

TEST_CASE("Roundtrip: while loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*v\d+\s*\+=\s*1[\s\S]*return\s+v\d+)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"((?:while|repeat)[\s\S]*\breturn\s+v\d+[\s\S]*(?:until|end))")));
}

TEST_CASE("Roundtrip: nested calls", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return function(a, b, c) return math.max(a, math.min(b, c)) end");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\s*=\s*arg1)")));
    CHECK(ContainsRegex(out, std::regex(R"(local\s+v\d+\s*=\s*arg2)")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+math\.max\(arg0,\s*math\.min\((?:arg1|v\d+),\s*(?:arg2|v\d+)\)\))")));
}

TEST_CASE("Roundtrip: table literal", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(return { 1, 2, 3, "hello" })");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+\{\s*1,\s*2,\s*3,\s*"hello"\s*\})")));
}

TEST_CASE("Roundtrip: function declaration", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local function add(a, b)
            return a + b
        end
        return add(1, 2)
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(local\s+function\s+add\([^)]*\)[\s\S]*return\s+[^\n]*\+[^\n]*[\s\S]*end)")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+add\(1,\s*2\))")));
}

TEST_CASE("Roundtrip: numeric for loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local s = 0
        for i = 1, 10 do
            s = s + i
        end
        return s
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*for\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*1,\s*10,\s*1\s+do)")));
    CHECK(ContainsRegex(out, std::regex(R"((?:^|\n)\s*v\d+\s*\+=\s*[A-Za-z_][A-Za-z_0-9]*)")));
}

TEST_CASE("Roundtrip: variable assignment with binary expression", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail("return function(a, b) return a + b end");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(return\s+arg0\s*\+\s*arg1)")));
}

TEST_CASE("Roundtrip: if statement", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local x = math.random()
        if x > 0.5 then
            return 1
        end
        return 0
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(if\s+0\.5\s*>=\s*math\.random\(\)\s+then\s+return\s+0\s+else\s+return\s+1\s+end)")));
}

TEST_CASE("Roundtrip: repeat-until loop", "[Decompiler][Roundtrip]") {
    const auto out = DecompileOrFail(R"(
        local i = 0
        repeat
            i = i + 1
        until i >= 10
        return i
    )");

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(repeat\s+v\d+\s*\+=\s*1\s+until\s*\(10\s*<=\s*v\d+\)\s+return\s+v\d+)")));
    CHECK(CountRegex(out, std::regex(R"((?:^|\n)\s*return\b)")) == 1u);
}
