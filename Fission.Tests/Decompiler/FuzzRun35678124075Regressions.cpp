#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string Decompile(const std::string &source) {
        fuzz::EnableLuauFlags();
        const auto result = fuzz::FullDecompile(source);
        REQUIRE(result.code == DecompileResult::Success);
        INFO(result.output);
        return result.output;
    }
}

TEST_CASE("Fuzz run 35678124075 preserves first-error evaluation order", "[Decompiler][FuzzRegress][Semantics]") {
    const std::string source = R"LUA(local v0 = -{ 100i, false } and true
local v1 = "a-b"
local v2 = v1()
local v3 = v1(({ 166i, ["y"] = v0, ["a-b"] = true }).field)
return "value")LUA";
    const std::string output = Decompile(source);
    const auto prelude = Luau::compile("", kOptions);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source, kOptions), prelude);
    const auto reconstructed = fuzz::RunLuauTrace(Luau::compile(output, kOptions), prelude);

    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Fuzz run 35678124075 keeps reused multi-local bindings in scope", "[Decompiler][FuzzRegress][ForwardRef]") {
    const std::string source = R"LUA(local v0, v1, v2 = 139, (-(-table)), ipairs()
v1 -= ...
v0, v2 = 638, v2[tostring]:method(tostring[49.25], (if v2 then pairs else false))
return 598)LUA";
    const std::string output = Decompile(source);
    std::string bytecode;

    REQUIRE(fuzz::LuauCompiles(output, &bytecode));
    CHECK(fuzz::ClassifyForwardRef(output, &source) == "OTHER");
}

TEST_CASE("Fuzz run 35678124075 keeps statements before repeat termination", "[Decompiler][FuzzRegress][Recompile]") {
    const std::string source = R"LUA(local v0 = obj:get(968, nil)
for g1_0 in v0(v0()) do
end
string((169):method(math), #("x").field)
while -(if function(...)
    return table
end then v0["value"] else -(-function(p1, p2, p3, ...)
    return tostring, pairs
end)) do
    local v1 = nil
    local function f2()
        v0(select)
        f2 = not table
        v1(obj, false)
        return "value", nil
    end
    if next:set() then
        break
    end
end
repeat
until #...
return if false then obj:method(v0) else v0:set(true))LUA";
    const std::string output = Decompile(source);
    std::string bytecode;

    CHECK(fuzz::LuauCompiles(output, &bytecode));
}
