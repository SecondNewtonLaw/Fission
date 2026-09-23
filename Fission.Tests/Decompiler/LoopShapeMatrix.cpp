//
// Created by Dottik on 23/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include <array>
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
        "while g % 3 ~= 0 do g += 1 if g % 2 == 0 then continue end h += 1 end",
        "repeat g += 1 if g % 2 == 1 then continue end h += 1 until g % 3 == 0",
    };
    constexpr const char *kTails[] = {
        "g += 1",
        "g += 1 print(g, h)",
        "g += 1 if h % 2 == 0 then print(\"even\", g) else print(\"odd\", h) end",
        "g += 1 if g % 4 == 0 then continue end print(g)",
    };
    // An empty prefix makes the inner loop open the outer body, sharing its header.
    constexpr const char *kPrefixes[] = {"", "h += 1\n"};
} // namespace

TEST_CASE("Loop shapes: nested loop matrix keeps semantics", "[Decompiler][LoopShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *outer : kOuterLoops)
        for (const char *prefix : kPrefixes)
            for (const char *inner : kInnerLoops)
                for (const char *tail : kTails) {
                    const std::string body = Fill(Fill(outer, "{INNER}", std::string(prefix) + inner), "{TAIL}", tail);
                    const std::string source = "local g, h = 0, 0\n" + body + "\nprint(\"end\", g, h)";
                    if (const auto decompiled = Divergence(source); !decompiled.empty())
                        failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
                }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}

namespace {
    constexpr const char *kBranchLoops[] = {
        "while g < 30 do\n{BODY}\nend",
        "while true do\n{BODY}\nif g > 30 then break end\nend",
        "repeat\n{BODY}\nuntil g > 30",
        "while g < 30 or h < 2 do\n{BODY}\nend",
        "for i = 1, 12 do\n{BODY}\nend",
        "for _, v in ipairs({ 1, 2, 3, 4, 5, 6 }) do\n{BODY}\nend",
    };
    // Every body advances `g` first so each loop terminates whichever branch it takes.
    constexpr const char *kBranchBodies[] = {
        "g += 1 if g % 2 == 0 then g += 1 elseif g % 3 == 0 then h += 1 else g += 2 end",
        "g += 1 if g % 2 == 0 then g += 1 if h > 3 then break end end h += 1",
        "g += 1 if g % 5 == 0 then g += 1 continue end h += 1",
        "g += 1 if g > 20 then return g end h += 1",
        "g += 1 if g % 2 == 0 or h % 3 == 0 then g += 1 else h += 1 end",
        "g += 1 if g % 2 == 0 and h % 3 == 0 then break elseif g % 7 == 0 then continue end h += 1",
        "g += 1 local t = g % 4 if t == 0 then g += 3 elseif t == 1 then g += 2 elseif t == 2 then h += 1 else h += 2 end",
        "g += 1 if (g % 2 == 0 and h > 1) or g % 5 == 0 then h += 2 end print(g, h)",
    };
    constexpr const char *kNestings[] = {"{LOOP}", "for k = 1, 2 do\n{LOOP}\nh += k\nend"};

    constexpr const char *kValueTemplates[] = {
        "A and B",
        "A or B",
        "A and B or C",
        "(A or B) and C",
        "not A or B",
        "A and (B or C)",
        "(A and B) or (C and D)",
        "if A then B else C",
        "if A then B elseif C then D else nil",
        "not (A and B)",
    };
    // Operands mix locals, side-effecting calls, table reads, constants and comparisons.
    constexpr std::array<std::array<const char *, 4>, 4> kValueOperands = {{
        {"x", "f(2)", "t.k", "nil"},
        {"f(false)", "x", "f(3)", "(x == 0)"},
        {"(x == 0)", "nil", "x", "f(4)"},
        {"t.k", "f(false)", "(x == 0)", "x"},
    }};
    constexpr const char *kValueContexts[] = {
        "local v = {E}\nprint(v)",
        "print({E})",
        "if {E} then print(\"yes\") else print(\"no\") end",
        "local n = 0\nwhile ({E}) and n < 2 do n += 1 print(\"loop\", n) end",
        "return {E}",
        "t.r = {E}\nprint(t.r)",
    };

    // Each loop exposes an iteration value `i`.
    constexpr const char *kCaptureLoops[] = {
        "for i = 1, 4 do\n{BODY}\nend",
        "for _, i in ipairs({ 1, 2, 3, 4 }) do\n{BODY}\nend",
        "local i = 0\nwhile i < 4 do\ni += 1\n{BODY}\nend",
        "local i = 0\nrepeat\ni += 1\n{BODY}\nuntil i >= 4",
        "local i = 0\nwhile true do\ni += 1\nif i > 4 then break end\n{BODY}\nend",
    };
    constexpr const char *kCaptureBodies[] = {
        "local v = i * 2 fns[#fns + 1] = function() return v end",
        "fns[#fns + 1] = function() return i end",
        "local v = i fns[#fns + 1] = function() v += 1 return v end v += 10",
        "local v = i if v % 2 == 0 then fns[#fns + 1] = function() return v end end",
        "local v = i fns[#fns + 1] = function() return v end if v > 2 then break end",
        "local v = i local g = function() return v end v = v * 3 fns[#fns + 1] = g",
        "local v = i if v % 2 == 0 then continue end fns[#fns + 1] = function() return v + 1 end",
    };

    constexpr const char *kMergeStatements[] = {
        "if x then a = 1 else b = 2 end",
        "if x then a, b = b, a end",
        "if x == 0 then a = a + b elseif x then b = a - b else a, b = b, a end",
        "for i = 1, 5 do if i % 2 == 0 then a, b = b, a + i else a = a * 2 end end",
        "while a < 50 do a, b = b, a + b end",
        "local c = a if x then c = b end a = c + 1",
        "if x then local t = a a = b b = t end",
        "repeat a, b = b + 1, a until a > 10 or (x and b > 5)",
        "if x then a = a + 1 end if not x then b = b + 1 end a, b = a + b, a - b",
        "local f = function() return a + b end if x then a = 10 end b = f()",
    };
    constexpr const char *kMergeWrappers[] = {"{S}", "if x ~= false then\n{S}\nend"};

    std::string FillOperands(std::string expression, const std::array<const char *, 4> &operands) {
        std::string out;
        for (const char c : expression) {
            if (c >= 'A' && c <= 'D')
                out += operands[c - 'A'];
            else
                out += c;
        }
        return out;
    }
} // namespace

TEST_CASE("Value shapes: short-circuit values keep semantics in every context", "[Decompiler][ValueShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *context : kValueContexts)
        for (const char *expression : kValueTemplates)
            for (const auto &operands : kValueOperands) {
                const std::string body = Fill(context, "{E}", FillOperands(expression, operands));
                const std::string source = "local calls = 0\n"
                                           "local function f(n) calls += 1 print(\"f\", n, calls) return n end\n"
                                           "local t = { k = 4 }\n"
                                           "local function probe(x)\n" +
                                           body + "\nend\nprint(probe(nil)) print(probe(false)) print(probe(0)) print(probe(3))";
                if (const auto decompiled = Divergence(source); !decompiled.empty())
                    failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
            }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}

TEST_CASE("Capture shapes: closures keep each iteration's bindings", "[Decompiler][CaptureShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *loop : kCaptureLoops)
        for (const char *body : kCaptureBodies) {
            const std::string source =
                "local fns = {}\n" + Fill(loop, "{BODY}", body) + "\nfor n, fn in ipairs(fns) do print(n, fn()) end\nprint(\"end\", #fns)";
            if (const auto decompiled = Divergence(source); !decompiled.empty())
                failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
        }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}

TEST_CASE("Merge shapes: values merged across branches and loops keep semantics", "[Decompiler][MergeShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *wrapper : kMergeWrappers)
        for (const char *statement : kMergeStatements) {
            const std::string source = "local function probe(x)\nlocal a, b = 1, 2\n" + Fill(wrapper, "{S}", statement) +
                                       "\nreturn a, b\nend\nprint(probe(nil)) print(probe(false)) print(probe(0)) print(probe(3))";
            if (const auto decompiled = Divergence(source); !decompiled.empty())
                failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
        }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}

TEST_CASE("Loop shapes: branches inside loops keep semantics", "[Decompiler][LoopShapes][Semantics]") {
    fuzz::EnableLuauFlags();
    std::vector<std::string> failures;
    for (const char *nesting : kNestings)
        for (const char *loop : kBranchLoops)
            for (const char *body : kBranchBodies) {
                const std::string code = Fill(nesting, "{LOOP}", Fill(loop, "{BODY}", body));
                const std::string source = "local g, h = 0, 0\n" + code + "\nprint(\"end\", g, h)";
                if (const auto decompiled = Divergence(source); !decompiled.empty())
                    failures.push_back(source + "\n--- decompiled ---\n" + decompiled);
            }
    for (const auto &failure : failures)
        UNSCOPED_INFO(failure);
    CHECK(failures.empty());
}
