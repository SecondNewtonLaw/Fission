//
// Created by Dottik on 22/9/2026.
//

#include "Decompiler.hpp"
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
    return decompiler.DecompileTestCode(source, enabled ? DecompilerFlags::FissionDebugNotes : static_cast<DecompilerFlags>(0),
                                        Luau::CompileOptions{1, 2});
}

TEST_CASE("Debug notes retain final shared-loop decisions", "[Decompiler][DebugNotes]") {
    std::ifstream input(std::filesystem::path(FISSION_SOURCE_DIR) / "Fission.Fuzzing/regressions/nested_repeat_shared_header.lua");
    REQUIRE(input.good());
    const auto result = DecompileForNotes(std::string(std::istreambuf_iterator<char>(input), {}));
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.debugNotes.find("preserve inner latch") != std::string::npos);
    CHECK(result.debugNotes.find("reconstruct outer repeat") != std::string::npos);
    CHECK(result.debugNotes.find("(_start) B1:") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[0] from B0 = R1#") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[1] from B5 = R1#") != std::string::npos);
    CHECK(result.debugNotes.find("phi R1 input[2] from B14 = R1#") != std::string::npos);
    CHECK(result.debugNotes.size() < 16384);
}

TEST_CASE("Debug notes identify functions and lift each closure body once", "[Decompiler][DebugNotes]") {
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

TEST_CASE("Debug notes name the instruction blocking effect movement", "[Decompiler][DebugNotes]") {
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

TEST_CASE("Debug note budgets retain decisions and reset cleanly", "[Decompiler][DebugNotes]") {
    FissionDebugNotes notes;
    notes.Reset(true);
    for (int i = 0; i < 100; ++i)
        notes.Add(FissionDebugStage::CFA, "routine {}", i);
    notes.AddDecision(FissionDebugStage::CFA, "final loop decision");
    for (int i = 0; i < 100; ++i)
        notes.Add(FissionDebugStage::CFA, "later routine {}", i);
    CHECK(notes.Render().find("final loop decision") != std::string::npos);
    CHECK(notes.Render().find("169 additional notes omitted") != std::string::npos);

    std::vector<std::string> blockNotes{"SSA: retain other stage"};
    notes.AddBlock(FissionDebugStage::CFA, "F0 (loop)", 1, blockNotes, "partition detail", false);
    REQUIRE(blockNotes.size() == 2);
    CHECK(blockNotes.back() == "CFA: partition detail");
    for (int i = 0; i < 100; ++i)
        notes.AddBlock(FissionDebugStage::CFA, "F0 (loop)", 1, blockNotes, std::format("decision {}", i));
    CHECK(blockNotes.size() == 8);
    CHECK(blockNotes.front() == "SSA: retain other stage");
    CHECK(blockNotes.back() == "CFA: decision 99");
    CHECK(notes.Render().find("F0 (loop) B1: decision 99") != std::string::npos);
    CHECK(notes.Render().find("final loop decision") == std::string::npos);

    notes.Reset(true);
    notes.AddDecision(FissionDebugStage::AST, std::string(4096, 'x'));
    CHECK(notes.Render().size() < 600);
    CHECK(notes.Render().find("...") != std::string::npos);
    notes.Reset(false);
    notes.AddDecision(FissionDebugStage::AST, "disabled");
    notes.AddBlock(FissionDebugStage::CFA, "F0 (loop)", 1, blockNotes, "disabled");
    CHECK(notes.Render().empty());
    CHECK(blockNotes.size() == 8);
    notes.Reset(true);
    CHECK(notes.Render().empty());
}
