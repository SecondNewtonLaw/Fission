//
// Created by Dottik on 23/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

namespace {
    constexpr Luau::CompileOptions kOptions{fuzz::kOpt, fuzz::kDebug};

    std::string Fill(std::string text, const std::string &key, const std::string &value) {
        const auto at = text.find(key);
        return at == std::string::npos ? text : text.replace(at, key.size(), value);
    }

    // Empty when the decompiled program traces like the original on every fixture, else the decompiled source.
    std::string Divergence(const std::string &source) {
        const auto result = fuzz::FullDecompile(source);
        if (result.code != DecompileResult::Success)
            return "<decompile failed>";
        const auto originalBytecode = Luau::compile(source, kOptions);
        const auto reconstructedBytecode = Luau::compile(result.output, kOptions);
        for (size_t i = 0; i < fuzz::kSemPreludeCount; ++i) {
            const auto prelude = Luau::compile(fuzz::kSemPreludes[i], kOptions);
            const auto original = fuzz::RunLuauTrace(originalBytecode, prelude);
            const auto reconstructed = fuzz::RunLuauTrace(reconstructedBytecode, prelude);
            if (reconstructed.status != original.status || reconstructed.trace != original.trace)
                return result.output;
        }
        return {};
    }

    // Outer loops without a leading test share their header with an inner loop that opens the body.
    constexpr const char *kOuterLoops[] = {
        "while true do\n{INNER}\n{TAIL}\nif g > 12 then break end\nend",
        "repeat\n{INNER}\n{TAIL}\nuntil g > 12",
        "repeat\n{INNER}\n{TAIL}\nuntil g > 12 or h > 60",
        "while g < 13 do\n{INNER}\n{TAIL}\nend",
        "while g < 13 or h < 0 do\n{INNER}\n{TAIL}\nend",
        "for i = 1, 4 do\n{INNER}\n{TAIL}\nend",
    };
    constexpr const char *kInnerLoops[] = {
        "while g % 3 ~= 0 do g += 1 h += 1 end",
        "while g % 3 ~= 0 or h < 2 do g += 1 h += 1 end",
        "while g % 5 ~= 0 and h < 50 do g += 1 h += 1 end",
        "repeat g += 1 h += 1 until g % 3 == 0",
        "repeat g += 1 h += 1 until g % 4 == 0 or h > 30",
        "repeat g += 1 h += 1 if h > 45 then break end until g % 2 == 0",
        "while true do g += 1 h += 1 if g % 3 == 0 then break end end",
    };
    constexpr const char *kTails[] = {
        "g += 1",
        "g += 1 print(g, h)",
        "g += 1 if h % 2 == 0 then print(\"even\", g) else print(\"odd\", h) end",
    };
} // namespace

TEST_CASE("Loop shapes: nested loop matrix keeps semantics", "[Decompiler][LoopShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *outer : kOuterLoops)
        for (const char *inner : kInnerLoops)
            for (const char *tail : kTails) {
                const std::string source = "local g, h = 0, 0\n" + Fill(Fill(outer, "{INNER}", inner), "{TAIL}", tail) + "\nprint(\"end\", g, h)";
                if (const auto decompiled = Divergence(source); !decompiled.empty())
                    failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
            }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}
