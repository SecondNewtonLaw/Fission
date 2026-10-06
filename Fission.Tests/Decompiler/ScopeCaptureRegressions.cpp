//
// Created by Dottik on 21/9/2026.
//

#include "IntegrationTestSupport.hpp"

TEST_CASE("Integration: captured bindings survive later declarations", "[Decompiler][Scope][Regression][Integration]") {
    SECTION("named closure") {
        integration_test::Check(R"LUA(local saved
do local value = tonumber("12"); local function read() return value end; saved = read end
do local value = tonumber("42"); print(saved(), value) end
print(saved()))LUA");
    }
    SECTION("renamed captured binding") {
        integration_test::Check(R"LUA(local object = { Health = 5 }
local value = object.Health
local saved = function() return value end
do local value = tonumber("9"); print(saved(), value) end
print(saved(), object.Health))LUA");
    }
    SECTION("recursive binding") {
        integration_test::Check(R"LUA(local function first() return first end
local saved = first
do local first = tonumber("9"); print(saved() == saved, first) end
print(saved() == saved))LUA");
    }
}
