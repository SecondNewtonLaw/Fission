// Created by Dottik on 24/9/2026.
// Co-Created using Codex.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Deserializer.hpp"
#include "Luau/Bytecode.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>

TEST_CASE("Deserializer: legacy type metadata leaves instruction cursor intact", "[BytecodeDecoder][Types][Regression]") {
    std::string proto{char(1), char(0), char(0), char(0), char(0)};
    proto.append({char(2), char(LBC_TYPE_FUNCTION), char(0), char(1)});
    const std::uint32_t ret = static_cast<std::uint32_t>(LOP_RETURN) | (1u << 16);
    for (int shift = 0; shift < 32; shift += 8)
        proto.push_back(static_cast<char>(ret >> shift));
    proto.append(7, '\0');

    std::string bytecode{char(12), char(1), char(0), char(1), static_cast<char>(proto.size())};
    bytecode += proto;
    bytecode.push_back('\0');

    const auto vm = fuzz::RunLuauTrace(bytecode, Luau::compile(""));
    REQUIRE(vm.status == fuzz::SemTrace::Status::Ok);
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->lpMainFunction->instructions.size() == 1);
    CHECK(decoded->lpMainFunction->instructions[0].GetOpCode() == LOP_RETURN);
}

TEST_CASE("Deserializer: normalized v1 function signature exposes parameter type", "[BytecodeDecoder][Types][Regression]") {
    std::string proto{char(1), char(1), char(0), char(0), char(0)};
    proto.append({char(3), char(LBC_TYPE_FUNCTION), char(1), char(LBC_TYPE_NUMBER), char(1)});
    const std::uint32_t ret = static_cast<std::uint32_t>(LOP_RETURN) | (2u << 16);
    for (int shift = 0; shift < 32; shift += 8)
        proto.push_back(static_cast<char>(ret >> shift));
    proto.append(7, '\0');

    std::string bytecode{char(12), char(1), char(0), char(1), static_cast<char>(proto.size())};
    bytecode += proto;
    bytecode.push_back('\0');

    const auto vm = fuzz::RunLuauTrace(bytecode, Luau::compile(""));
    REQUIRE(vm.status == fuzz::SemTrace::Status::Ok);
    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->lpMainFunction->uTypeVersion == 1);
    REQUIRE_FALSE(decoded->lpMainFunction->typeinfo.empty());
    CHECK(Deserializer::TryGetTypeName(decoded->lpMainFunction, 0) == "number");
}
