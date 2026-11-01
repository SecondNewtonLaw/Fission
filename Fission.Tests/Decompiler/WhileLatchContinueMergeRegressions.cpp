//
// Regression coverage for the fuzz-found while/repeat latch merge divergence.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <cstring>
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

// A while/repeat latch jumps back to its header instead of the loop exit. The merge analysis must
// still recognise a conditional jump into that latch as `continue`, or the code after the `if`
// gets folded into one arm and the other path loses it.
TEST_CASE("Regress fuzz: while latch continue keeps the code after the if on the fallthrough path", "[Decompiler][Regression][Semantics][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
while 0 do
    if table[ipairs] then
        if string.field then
            table()
            if obj then
                continue
            end
        end
    else
    end
    for g0_0 in obj() do
    end
    for i0 = math[print], true, nil do
    end
    if print.field then
        break
    end
end
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto prelude = Luau::compile("obj = { x = 1 }\n");
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
    INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Error);
    CHECK(original.trace == "error: attempt to call a table value\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

// The same loop shape with a successful run: the fallthrough path still executes the code after
// the if, and the conditional continue skips it.
TEST_CASE("While loop with a conditional continue keeps the post-if body on both paths", "[Decompiler][Regression][Semantics][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function run(tbl, key, str, obj, pr)
    local n = 0
    while n < 3 do
        n = n + 1
        if tbl[key] then
            if str.field then
                n = n + 10
                if obj then
                    continue
                end
            end
        else
        end
        n = n + 1
        if pr.field then
            break
        end
    end
    return n
end
print(run({}, nil, { field = true }, false, { field = false }))
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
    INFO("original: " << original.trace << "decompiled: " << decompiled.trace);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    CHECK(original.trace == "4\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}
