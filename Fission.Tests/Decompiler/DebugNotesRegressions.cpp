//
// Created by Dottik on 22/9/2026.
//

#include "IntegrationTestSupport.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

static DecompilationResult DecompileForNotes(const std::string &source, bool enabled = true) {
    for (auto *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
    Decompiler decompiler{};
    const Luau::CompileOptions options{1, 2};
    const auto bytecode = Luau::compile(source, options);
    REQUIRE_FALSE(bytecode.empty());
    REQUIRE(bytecode.front() != '\0');
    const auto result = decompiler.DecompileVanillaBytecode(bytecode, enabled ? DecompilerFlags::FissionDebugNotes : static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    integration_test::CheckOutput(bytecode, result.decompilationOutput, options);
    return result;
}

TEST_CASE("Debug notes retain final shared-loop decisions", "[Decompiler][DebugNotes][Integration]") {
    std::ifstream input(std::filesystem::path(FISSION_SOURCE_DIR) / "Fission.Fuzzing/regressions/nested_repeat_shared_header.lua");
    REQUIRE(input.good());
    const auto result = DecompileForNotes(std::string(std::istreambuf_iterator<char>(input), {}));
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.debugNotes.find("preserve inner latch") != std::string::npos);
    CHECK(result.debugNotes.find("reconstruct outer repeat") != std::string::npos);
    CHECK(result.debugNotes.find("(_start) B1:") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[0] from B0 = R1#") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[1] from B8 = R1#") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[2] from B2 = R1#") != std::string::npos);
    CHECK(result.debugNotes.size() < 16384);
}

TEST_CASE("Debug notes identify functions and lift each closure body once", "[Decompiler][DebugNotes][Integration]") {
    const std::string source = "local function mark(value) print(value) return value end\nreturn mark(4)";
    const auto result = DecompileForNotes(source);
    const auto plain = DecompileForNotes(source, false);
    REQUIRE(result.resultCode == DecompileResult::Success);
    REQUIRE(plain.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput == plain.decompilationOutput);
    size_t lifts = 0;
    for (size_t at = 0; (at = result.debugNotes.find(": lifting ", at)) != std::string::npos; ++at)
        ++lifts;
    CHECK(lifts == 2);
    CHECK(result.debugNotes.find("F0 (mark) B0:") != std::string::npos);
    CHECK(result.debugNotes.find("F1 (_start) B0:") != std::string::npos);
    CHECK(plain.debugNotes.empty());
}

TEST_CASE("Debug notes name the instruction blocking effect movement", "[Decompiler][DebugNotes][Integration]") {
    const auto result = DecompileForNotes(R"LUA(local function mark(tag, value)
    print(tag)
    return value
end
local value = mark("first", 3)
print("between")
return value + tonumber("1"))LUA");
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.debugNotes.find("keep _") != std::string::npos);
    CHECK(result.debugNotes.find("barrier _") != std::string::npos);
    CHECK(result.debugNotes.find("CALL") != std::string::npos);
    CHECK(result.debugNotes.find("R") != std::string::npos);
}

TEST_CASE("Debug note budgets survive large requests and reset on reuse", "[Decompiler][DebugNotes][Integration]") {
    fuzz::EnableLuauFlags();
    Decompiler decompiler;
    const Luau::CompileOptions options{1, 2};
    const auto run = [&](const std::string &source, bool enabled) {
        const auto bytecode = Luau::compile(source, options);
        REQUIRE_FALSE(bytecode.empty());
        REQUIRE(bytecode.front() != '\0');
        const auto result = decompiler.DecompileVanillaBytecode(bytecode, enabled ? DecompilerFlags::FissionDebugNotes : static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
        integration_test::CheckOutput(bytecode, result.decompilationOutput, options);
        return result;
    };
    std::string source = "local total = 0\n";
    for (int i = 0; i < 80; ++i)
        source += "if tonumber(\"" + std::to_string(i) + "\") % 2 == 0 then total += 1 else total += 2 end\n";
    source += "print(total)\n";
    const auto large = run(source, true);
    CHECK(large.debugNotes.find("additional notes omitted") != std::string::npos);
    CHECK(large.debugNotes.find("[CFA]") != std::string::npos);
    CHECK(large.debugNotes.find("[SSA]") != std::string::npos);
    CHECK(large.debugNotes.find("[AST]") != std::string::npos);
    CHECK(large.debugNotes.size() < 65536);

    const auto small = run("print(17)", true);
    CHECK_FALSE(small.debugNotes.empty());
    CHECK(small.debugNotes.find("additional notes omitted") == std::string::npos);
    CHECK(small.debugNotes.size() < large.debugNotes.size());
    const auto disabled = run("print(23)", false);
    CHECK(disabled.debugNotes.empty());
    CHECK(run("print(17)", true).debugNotes == small.debugNotes);
}
