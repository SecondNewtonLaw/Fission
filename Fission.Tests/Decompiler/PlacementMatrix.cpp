#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string Tagged(std::string expression, char tag) {
        expression.replace(expression.find('$'), 1, 1, tag);
        return expression;
    }

    std::string BuildCase(std::string body) {
        return R"LUA(local events = {}
local function mark(tag, value)
    table.insert(events, tag)
    return value
end
local function receiver(tag)
    mark(tag .. ":receiver", 0)
    return {
        [1] = 11,
        value = 11,
        get = function(self)
            return mark(tag .. ":method", 11)
        end,
    }
end
local function sink(...)
    return ...
end
)LUA" + body + R"LUA(
return table.concat(events, ","), value
)LUA";
    }

    void CheckParity(const std::string &name, const std::string &source, bool expectError = false) {
        INFO("matrix case: " << name);
        std::string originalBytecode;
        const bool compiles = fuzz::LuauCompiles(source, &originalBytecode);
        CHECK(compiles);
        if (!compiles)
            return;
        const auto result = fuzz::FullDecompile(source);
        CHECK(result.code == DecompileResult::Success);
        if (result.code != DecompileResult::Success)
            return;
        INFO(result.output);
        std::string reconstructedBytecode;
        const bool recompiles = fuzz::LuauCompiles(result.output, &reconstructedBytecode);
        CHECK(recompiles);
        if (!recompiles)
            return;
        CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.output, &source));
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            INFO("fixture " << i);
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], kOptions);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            CHECK(original.status == (expectError ? fuzz::SemTrace::Status::Error : fuzz::SemTrace::Status::Ok));
            if (expectError)
                CHECK(original.trace.find("error: A") != std::string::npos);
            CHECK(reconstructed.status == original.status);
            CHECK(reconstructed.trace == original.trace);
        }
    }
} // namespace

TEST_CASE("Placement matrix preserves evaluation order and multiplicity", "[Decompiler][PlacementMatrix]") {
    fuzz::EnableLuauFlags();
    const std::vector<std::pair<std::string, std::string>> producers{
        {"call", R"(mark("$", 11))"},           {"field", R"(receiver("$").value)"},  {"index", R"(receiver("$")[1])"},
        {"arithmetic", R"(mark("$", 10) + 1)"}, {"method", R"(receiver("$"):get())"}, {"closure", R"((function() return mark("$", 11) end)())"},
    };

    for (const auto &[producerName, producer] : producers) {
        const std::string a = Tagged(producer, 'A');
        const std::string b = Tagged(producer, 'B');
        const std::vector<std::pair<std::string, std::string>> contexts{
            {"call-arguments", "local value = sink(" + a + ", " + b + ")"},
            {"table-list", "local value = { " + a + ", " + b + " }"},
            {"table-key-value", "local value = { [" + a + "] = " + b + " }"},
            {"multi-assignment", "local left, right = " + a + ", " + b + "\nlocal value = left + right"},
            {"binary", "local value = (" + a + ") + (" + b + ")"},
            {"materialized-call-barrier", "local first = " + a + "\nmark(\"X\", 0)\nlocal value = sink(first, " + b + ")"},
            {"materialized-table-barrier", "local first = " + a + "\nmark(\"X\", 0)\nlocal value = { first, " + b + " }"},
        };
        for (const auto &[contextName, body] : contexts)
            CheckParity(producerName + "/" + contextName, BuildCase(body));
    }
}

TEST_CASE("Placement matrix preserves first observable failure", "[Decompiler][PlacementMatrix]") {
    fuzz::EnableLuauFlags();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"call", "local function fail(tag) error(tag) end\nlocal value = tostring(fail(\"A\"), fail(\"B\"))"},
        {"table", "local function fail(tag) error(tag) end\nlocal value = { fail(\"A\"), fail(\"B\") }"},
        {"key-value", "local function fail(tag) error(tag) end\nlocal value = { [fail(\"A\")] = fail(\"B\") }"},
        {"materialized", "local function fail(tag) error(tag) end\nlocal first = fail(\"A\")\nprint(\"barrier\")\nlocal value = { first, fail(\"B\") }"},
    };
    for (const auto &[name, body] : cases)
        CheckParity(name, body, true);
}

TEST_CASE("Control-flow matrix keeps loop-owned statements inside loops", "[Decompiler][PlacementMatrix][ControlFlow]") {
    fuzz::EnableLuauFlags();
    const std::vector<std::pair<std::string, std::string>> cases{
        {"nested-break", R"LUA(local events = {}
local function mark(tag, value) table.insert(events, tag) return value end
while mark("outer", true) do
    if mark("guard", true) then
        mark("body", 0)
        if mark("break", true) then break end
    end
end
return table.concat(events, ","))LUA"},
        {"nested-continue", R"LUA(local events = {}
local function mark(tag, value) table.insert(events, tag) return value end
local i = 0
while mark("outer", i < 2) do
    i += 1
    if mark("guard", i == 1) then continue end
    mark("tail", 0)
end
return table.concat(events, ","))LUA"},
        {"repeat-in-while", R"LUA(local events = {}
local function mark(tag, value) table.insert(events, tag) return value end
local i = 0
while mark("outer", i < 2) do
    repeat
        mark("repeat", 0)
    until mark("until", true)
    i += 1
end
return table.concat(events, ","))LUA"},
        {"while-in-repeat", R"LUA(local events = {}
local function mark(tag, value) table.insert(events, tag) return value end
local i = 0
repeat
    while mark("inner", i < 1) do i += 1 end
    mark("tail", 0)
until mark("until", true)
return table.concat(events, ","))LUA"},
    };
    for (const auto &[name, source] : cases)
        CheckParity(name, source);
}
