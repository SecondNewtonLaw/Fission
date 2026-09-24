// Assert lift semantics with structural matches that tolerate formatting and temporary-name changes.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "LiftingSemanticsTestSupport.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include "Rewriters/DeclarationHoister.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/LoopVariableRenamer.hpp"
#include <catch2/catch_test_macros.hpp>
#include <cctype>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace lifting_semantics_test {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    // Some tests need to disable the Luau front-end optimiser to preserve the
    // exact bytecode shape they are exercising (e.g. an explicit `if not v then
    // v = X end` chain that opt=1 would otherwise fold into return-and-comparison
    // form before the lifter ever sees the OR opcode).
    std::string DecompileOrFail(const std::string &source, int optLevel, DecompilerFlags flags) {
        EnableLuauFFlagsOnce();
        Decompiler decompiler{};
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        auto result = decompiler.DecompileTestCode(source, flags, opts);
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    // Decompile already-compiled, non-Roblox (identity-decoder) Luau bytecode.
    std::string DecompileVanillaOrFail(const std::string &bytecode) {
        Decompiler decompiler{};
        auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    // Find the first `for <ident> = ` occurrence that is at the start of a line
    // (after indentation); skips matches inside the Fission header docstring
    // (e.g. "decompiler for RbxCli").
    std::string ExtractFirstNumericForLoopVar(const std::string &source) {
        static const std::regex re(R"((?:^|\n)\s*for\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s)");
        std::smatch m;
        if (std::regex_search(source, m, re))
            return m[1].str();
        return {};
    }

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    std::string DecodeBase64(std::string_view input) {
        static constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string output;
        int value = 0;
        int bits = -8;
        for (const unsigned char c : input) {
            if (std::isspace(c))
                continue;
            if (c == '=')
                break;
            const auto digit = alphabet.find(c);
            if (digit == std::string_view::npos)
                throw std::runtime_error("invalid base64 fixture");
            value = (value << 6) | static_cast<int>(digit);
            bits += 6;
            if (bits >= 0) {
                output.push_back(static_cast<char>((value >> bits) & 0xff));
                bits -= 8;
            }
        }
        return output;
    }

    std::string DecompileItemSpawnOrFail() {
        std::ifstream fixture{FISSION_SOURCE_DIR "/Samples/EncodedRoblox/ITEM_SPAWN_GUARDS.txt", std::ios::binary};
        REQUIRE(fixture);
        std::stringstream encoded;
        encoded << fixture.rdbuf();

        Decompiler decompiler{};
        const auto result = decompiler.DecompileRobloxBytecode(DecodeBase64(encoded.str()), static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompile:\n" << result.decompilationOutput);
        INFO("IR:\n" << result.irOutput);
        return result.decompilationOutput;
    }

    size_t LineIndentContaining(const std::string &source, const std::string_view needle) {
        const auto position = source.find(needle);
        if (position == std::string::npos)
            return std::string::npos;
        const auto newline = source.rfind('\n', position);
        const auto lineStart = newline == std::string::npos ? 0 : newline + 1;
        return source.find_first_not_of(' ', lineStart) - lineStart;
    }

    // Recompile the decompiled source; proves the output is valid Luau (the byte-0 sentinel is the
    // Luau compiler's error marker).
    bool CompilesOk(const std::string &source) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 1;
        const std::string bc = Luau::compile(source, opts);
        return !bc.empty() && bc[0] != '\0';
    }

    void CheckSameTrace(const std::string &source, const std::string &decompiled, int optLevel) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = optLevel;
        opts.debugLevel = 2;
        const auto prelude = Luau::compile("", opts);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, opts), prelude);
        const auto optimized = fuzz::RunLuauTrace(Luau::compile(decompiled, opts), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(optimized.status == original.status);
        CHECK(optimized.trace == original.trace);
    }

    void CheckSameTraceWithPrelude(const std::string &source, const std::string &preludeSource) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const auto output = DecompileOrFail(source, opts.optimizationLevel);
        const auto prelude = Luau::compile(preludeSource, opts);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, opts), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(output, opts), prelude);
        INFO("decompiled output:\n" << output);
        INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    }

    size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
        if (needle.empty())
            return 0;
        size_t count = 0;
        size_t pos = 0;
        while ((pos = haystack.find(needle, pos)) != std::string::npos) {
            ++count;
            pos += needle.size();
        }
        return count;
    }

} // namespace lifting_semantics_test

using namespace lifting_semantics_test;

// Type inference

TEST_CASE("Lift: InferTypes omits generic table annotations", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local values = {}
        values[1] = 1
        values[2] = 2
        values[3] = 3
        return values
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: InferTypes"));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*table\s*=\s*\{)")));
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "3"));
}

TEST_CASE("Lift: type inference omits bare nil annotations", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local value = nil
        print(value)
        return value
    )",
        0, DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK_FALSE(std::regex_search(out, std::regex(R"(:\s*nil\b)")));
}

TEST_CASE("Lift: identical elseif bodies merge with ordered or", "[Decompiler][ShortCircuit]") {
    const auto out = DecompileOrFail(R"(
        local function f(x)
            if x == 1 then
                print("hit")
            elseif x == 2 then
                print("hit")
            elseif x == 3 then
            end
            return math.abs(x)
        end
        return f
    )");
    INFO(out);
    CHECK(CountOccurrences(out, "print(\"hit\")") == 1);
    CHECK(Contains(out, " or "));
}

TEST_CASE("Lift: InferTypes annotates function parameters from same-file call sites", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(name, count, enabled)
            return name
        end
        sink("rat", 3, false)
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(
        std::regex_search(
            out, std::regex(
                     R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*string,\s*[A-Za-z_][A-Za-z_0-9]*\s*:\s*number,\s*[A-Za-z_][A-Za-z_0-9]*\s*:\s*boolean\))"
                 )
        )
    );
}

TEST_CASE("Lift: InferTypes emits union annotations for mixed call-site argument types", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(value)
            return value
        end
        sink("rat")
        sink(12)
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*string\s*\|\s*number\))")));
}

TEST_CASE("Lift: InferTypes falls back to body-use inference for uncalled function arguments", "[Decompiler][TypeInference]") {
    const auto out = DecompileOrFail(
        R"(
        local function sink(value)
            return value + 1
        end
        return sink
    )",
        0, DecompilerFlags::InferTypes
    );

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(function\s+sink\([A-Za-z_][A-Za-z_0-9]*\s*:\s*number\))")));
}

TEST_CASE("Lift: OptimizeIR removes unreachable constant-false branches", "[Decompiler][OptimizeIR]") {
    const auto raw = DecompileOrFail(
        R"(
        if not true then
            print("dead")
        else
            print("live")
        end
    )",
        0
    );
    const auto optimized = DecompileOrFail(
        R"(
        if not true then
            print("dead")
        else
            print("live")
        end
    )",
        0, DecompilerFlags::OptimizeIR
    );

    INFO("raw decompile:\n" << raw);
    INFO("optimized decompile:\n" << optimized);
    CHECK(Contains(raw, "Decompile Options: None"));
    CHECK(Contains(optimized, "Decompile Options: OptimizeIR"));
    CHECK(Contains(raw, "dead"));
    CHECK_FALSE(Contains(optimized, "dead"));
    CHECK(Contains(optimized, "live"));
    CHECK_FALSE(Contains(optimized, "if not true then"));
}

TEST_CASE("Lift: OptimizeIR propagates arithmetic constants into branches", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local a = 6
        local b = a + 3
        local c = b * 4
        if c == 36 then
            return 7
        end
        return 8
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "if "));
    CHECK_FALSE(Contains(optimized, " + "));
    CHECK_FALSE(Contains(optimized, " * "));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR folds reverse constant arithmetic", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local a = 3
        local b = 4
        return 10 - a, 12 / b
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 7, 3"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR joins equal constants through unknown branches", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local value
        if tonumber("1") == 1 then
            value = 5
        else
            value = 5
        end
        return value + 2
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 7"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR preserves changing loop values", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local value = 0
        while value < 3 do
            value += 1
        end
        return value
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "while "));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR preserves dynamic boolean control flow", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local function step(values)
            local nextValue = values[1] + values[2]
            local descending = values[2] <= 0
            local keepGoing = if descending then values[3] <= nextValue else not descending and nextValue <= values[3]
            values[1] = nextValue
            if not keepGoing then
                return 1
            end
            return 2
        end
        return step({ 5, 1, 10 }), step({ 5, -1, 0 })
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR folds constant bit32 xor", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local xor = bit32.bxor
        local a = 170
        local b = 204
        return xor(a, b)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "bxor("));
    CHECK(Contains(optimized, "102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR does not duplicate single-result builtins", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local first, second = bit32.bxor(170, 204)
        return first, second
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102, 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR resolves table aliases across calls", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        local function run(self)
            return self[118](170, 204)
        end
        return run(operations)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 102"));
    CHECK_FALSE(Contains(optimized, "[118](170"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR propagates method receivers", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        function operations:run()
            return self[118](170, 204)
        end
        return operations:run()
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 102"));
    CHECK_FALSE(Contains(optimized, "[118](170"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR propagates captured table aliases", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        local function make()
            return function()
                return operations[118](170, 204)
            end
        end
        return make()()
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 102"));
    CHECK_FALSE(Contains(optimized, "[118](170"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR invalidates fields written through callees", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        local function run(self)
            self[118] = function()
                return 9
            end
            return self[118](170, 204)
        end
        return run(operations)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR tracks table identity through setmetatable", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = setmetatable({}, {})
        operations[118] = bit32.bxor
        local function run(self)
            return self[118](170, 204)
        end
        return run(operations)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR requires table initialization to dominate", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        if tonumber("1") == 1 then
            operations[118] = bit32.bxor
        end
        local function run(self)
            return self[118](170, 204)
        end
        return run(operations)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR invalidates escaped tables", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        local marker = tostring(operations)
        local function run(self)
            return self[118](170, 204), #marker > 0
        end
        return run(operations)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR joins ambiguous table aliases", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local first = { [118] = bit32.bxor }
        local second = { [118] = function()
            return 9
        end }
        local selected = if tonumber("1") == 1 then first else second
        local function run(self)
            return self[118](170, 204)
        end
        return run(selected)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR does not specialize returned closures", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        operations[118] = bit32.bxor
        return function()
            return operations[118](170, 204)
        end
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "[118](170"));
}

TEST_CASE("Lift: OptimizeIR requires captured tables to be initialized first", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = {}
        local function run()
            return operations[118](170, 204)
        end
        operations[118] = bit32.bxor
        return run()
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR invalidates mutable captures", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local operations = { [118] = bit32.bxor }
        local function run()
            return operations[118](170, 204)
        end
        operations = { [118] = function()
            return 9
        end }
        return run()
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "return 102"));
}

TEST_CASE("Lift: OptimizeIR respects shadowed bit32 tables", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local bit32 = { bxor = function()
            return 9
        end }
        return bit32.bxor(170, 204)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "bxor"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR respects reassigned global bit32", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        bit32 = { bxor = function()
            return 9
        end }
        return bit32.bxor(170, 204)
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK(Contains(optimized, "bxor"));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: OptimizeIR removes constant-false dispatcher loops", "[Decompiler][OptimizeIR]") {
    const std::string source = R"(
        local state = 2
        while state <= 0 do
            error("dead dispatcher")
        end
        return 4
    )";
    const auto optimized = DecompileOrFail(source, 0, DecompilerFlags::OptimizeIR);

    INFO("optimized decompile:\n" << optimized);
    CHECK(CompilesOk(optimized));
    CHECK_FALSE(Contains(optimized, "dead dispatcher"));
    CHECK_FALSE(Contains(optimized, "while "));
    CheckSameTrace(source, optimized);
}

TEST_CASE("Lift: loop condition follows branch-to-body polarity", "[Decompiler][ControlFlow][Loop]") {
    EnableLuauFFlagsOnce();
    Luau::BytecodeBuilder bb{};
    const uint32_t main = bb.beginFunction(0, true);
    bb.emitAD(LOP_LOADN, 0, 1);
    bb.emitAD(LOP_LOADN, 1, 0);
    const size_t header = bb.emitLabel();
    const size_t branch = bb.emitLabel();
    bb.emitAD(LOP_JUMPIFNOTLE, 0, 0);
    bb.emitAux(1);
    bb.emitABC(LOP_RETURN, 0, 2, 0);
    const size_t body = bb.emitLabel();
    bb.emitAD(LOP_LOADN, 0, 0);
    const size_t backEdge = bb.emitLabel();
    bb.emitAD(LOP_JUMPBACK, 0, 0);
    REQUIRE(bb.patchJumpD(branch, body));
    REQUIRE(bb.patchJumpD(backEdge, header));
    bb.endFunction(2, 0);
    bb.setMainFunction(main);
    bb.finalize();

    const std::string bytecode = bb.getBytecode();
    const auto out = DecompileVanillaOrFail(bytecode);
    INFO("decompile:\n" << out);
    CHECK(CompilesOk(out));
    CHECK(Contains(out, "while not ("));

    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const auto prelude = Luau::compile("", opts);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(out, opts), prelude);
    REQUIRE(decompiled.status == fuzz::SemTrace::Status::Ok);
    CHECK(decompiled.trace == "return: 0\n");
}
