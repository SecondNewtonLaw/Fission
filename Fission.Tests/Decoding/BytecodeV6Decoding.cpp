//
// Created by Dottik on 04/17/2026.
//
#include "Deserializer.hpp"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("V6", "[BytecodeDecoder]") {
    /*
        V6 bytecode for `print'Hello, world!'`

          06 03 02 05   70 72 69 6e   74 0d 48 65   6c 6c 6f 2c   │ ····print·Hello, │
          20 77 6f 72   6c 64 21 00   01 02 00 00   01 02 00 06   │  world!········· │
          41 00 00 00   0c 00 01 00   00 00 00 40   05 01 02 00   │ A··········@···· │
          15 00 02 01   16 00 01 00   03 03 01 04   00 00 00 40   │ ···············@ │
          03 02 00 01   00 01 18 00   00 00 00 00   00 01 00 00   │ ················ │
          00 00 00

     */
    std::vector<uint8_t> bytecodeBinary = {0x06, 0x03, 0x02, 0x05, 0x70, 0x72, 0x69, 0x6e, 0x74, 0x0d, 0x48, 0x65, 0x6c, 0x6c, 0x6f, 0x2c, 0x20,
                                           0x77, 0x6f, 0x72, 0x6c, 0x64, 0x21, 0x00, 0x01, 0x02, 0x00, 0x00, 0x01, 0x02, 0x00, 0x06, 0x41, 0x00,
                                           0x00, 0x00, 0x0c, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x40, 0x05, 0x01, 0x02, 0x00, 0x15, 0x00, 0x02,
                                           0x01, 0x16, 0x00, 0x01, 0x00, 0x03, 0x03, 0x01, 0x04, 0x00, 0x00, 0x00, 0x40, 0x03, 0x02, 0x00, 0x01,
                                           0x00, 0x01, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
    std::string bytecode;
    bytecode.resize(bytecodeBinary.size());
    memcpy(bytecode.data(), bytecodeBinary.data(), bytecodeBinary.size());

    Deserializer deserializer{};
    DeserializedBytecode *deserialized;
    auto dOpt = deserializer.Deserialize(bytecode);
    REQUIRE(dOpt);
    deserialized = &*dOpt;

    INFO("Checking Deserialized Metadata");
    REQUIRE(deserialized->bytecodeVersion == 6);
    REQUIRE(deserialized->lpMainFunction != nullptr);
    REQUIRE(deserialized->typesVersion == 3); // v6 bytecode uses tt v3
    REQUIRE(deserialized->functions.size() == 1);
    REQUIRE(deserialized->functions.data() == deserialized->lpMainFunction);

    REQUIRE(deserialized->stringTable.size() == 2);           // method + string used.
    REQUIRE(deserialized->stringTable[0] == "print");         // first string used
    REQUIRE(deserialized->stringTable[1] == "Hello, world!"); // second string used

    INFO("Checking runtime userdata mappings");
    for (size_t i = 0; i < deserialized->userdataMappings.size(); i++) {
        REQUIRE(deserialized->userdataMappings.at(i) == 0); // no ud mappings.
    }

    INFO("Checking lpMain metadata");
    REQUIRE(deserialized->lpMainFunction->bIsMain);
    REQUIRE(deserialized->lpMainFunction->bytecodeId == 0);
    REQUIRE(deserialized->lpMainFunction->isvararg);
    REQUIRE(deserialized->lpMainFunction->nups == 0);
    REQUIRE(deserialized->lpMainFunction->numparams == 0);
    REQUIRE(deserialized->lpMainFunction->upvalueNames.empty());
}