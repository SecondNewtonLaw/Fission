#include "IntegrationTestSupport.hpp"

#include <string>

// Chaos Machine sample (2026-11-01): a while condition built from 36 nested
// if-expressions that all share one opaque arithmetic term,
//
//     while (if (0b01 + 8 + 0 + 0/0 ^ 0b01) then
//                (if (...) then ... (if (...) then true else false) ...)
//            else false) do break end
//
// The lifter's value-term recogniser is deliberately shallow (an 8-deep climb,
// 16-block regions, 24 headers), and the sample is deeper than all of them.
// Luau removes the whole loop at O1/O2 because the condition is pure and the
// body only breaks; at O0 the diamonds survive, so that is where the inversion
// actually runs. Recording both keeps the hostile shape from silently
// regressing.
namespace {
    constexpr int kDepth = 36;

    std::string NestedCondition(const std::string &term) {
        std::string source = "while (if (" + term + ") then ";
        for (int level = 1; level < kDepth; ++level)
            source += "(if (" + term + ") then ";
        source += "true";
        for (int level = 0; level < kDepth; ++level)
            source += " else false)";
        return source;
    }

    std::string ChaosSample() { return NestedCondition("0b01 + 8 + 0 + 0/0 ^ 0b01") + " do break end"; }

    // Same shape, but every level calls an observable function, so the trace
    // proves the number and order of condition evaluations survive lifting.
    std::string ObservableSample() {
        std::string condition = "while (if (bump(1) + 8 + 0 + 0/0 ^ 1) then ";
        for (int level = 2; level <= kDepth; ++level)
            condition += "(if (bump(" + std::to_string(level) + ") + 8 + 0 + 0/0 ^ 1) then ";
        condition += "true";
        for (int level = 0; level < kDepth; ++level)
            condition += " else false)";

        std::string source = "local function bump(k)\n"
                             "    print(\"cond\", k)\n"
                             "    return 1\n"
                             "end\n"
                             "local function run()\n"
                             "    local n = 0\n"
                             "    ";
        source += condition;
        source += " do\n"
                  "        n += 1\n"
                  "        print(\"iter\", n)\n"
                  "        if n >= 2 then break end\n"
                  "    end\n"
                  "    return n\n"
                  "end\n"
                  "print(\"result\", run())\n";
        return source;
    }
} // namespace

TEST_CASE("Chaos machine nested if-expression condition decompiles and recompiles", "[Decompiler][ChaosMachine][Integration]") {
    fuzz::EnableLuauFlags();
    const std::string source = ChaosSample();
    for (const int optimization : {0, 1, 2}) {
        for (const int debug : {0, 1, 2}) {
            INFO("O" << optimization << " D" << debug << "\n" << source);
            const Luau::CompileOptions options{optimization, debug};
            const auto originalBytecode = Luau::compile(source, options);
            REQUIRE_FALSE(originalBytecode.empty());
            REQUIRE(originalBytecode.front() != '\0');

            Decompiler decompiler;
            const auto result = decompiler.DecompileVanillaBytecode(originalBytecode, DecompilerFlags::AutoNameVariables);
            REQUIRE(result.resultCode == DecompileResult::Success);
            INFO("decompiled:\n" << result.decompilationOutput);

            const auto recompiled = Luau::compile(result.decompilationOutput, options);
            REQUIRE_FALSE(recompiled.empty());
            REQUIRE(recompiled.front() != '\0');
        }
    }
}

TEST_CASE("Chaos machine nested if-expression condition preserves evaluation order", "[Decompiler][ChaosMachine][Integration]") {
    fuzz::EnableLuauFlags();
    const std::string source = ObservableSample();
    for (const int optimization : {0, 1, 2}) {
        for (const int debug : {0, 1, 2}) {
            for (const DecompilerFlags irFlags : {static_cast<DecompilerFlags>(0), DecompilerFlags::OptimizeIR}) {
                INFO("O" << optimization << " D" << debug << " IR=" << (irFlags != static_cast<DecompilerFlags>(0)) << "\n" << source);
                const Luau::CompileOptions options{optimization, debug};
                const auto originalBytecode = Luau::compile(source, options);
                REQUIRE_FALSE(originalBytecode.empty());
                REQUIRE(originalBytecode.front() != '\0');

                Decompiler decompiler;
                const auto result = decompiler.DecompileVanillaBytecode(originalBytecode, irFlags | DecompilerFlags::AutoNameVariables);
                REQUIRE(result.resultCode == DecompileResult::Success);
                integration_test::CheckOutput(originalBytecode, result.decompilationOutput, options);
            }
        }
    }
}
