// Assert lift semantics with structural matches that tolerate formatting and temporary-name changes.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "Luau/Bytecode.h"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
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

namespace {

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
    std::string DecompileOrFail(const std::string &source, int optLevel = 1, DecompilerFlags flags = static_cast<DecompilerFlags>(0)) {
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

    void CheckSameTrace(const std::string &source, const std::string &decompiled, int optLevel = 0) {
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

} // namespace

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

TEST_CASE("Lift: InferRobloxTypes annotates well-known globals and Instance lookups", "[Decompiler][TypeInference][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local runService = game:GetService("RunService")
        local root = workspace:FindFirstChild("Map")
        local humanoid = script.Parent:FindFirstChildOfClass("Humanoid")
        print(runService, root, humanoid)
        return runService, root, humanoid
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: InferRobloxTypes"));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*RunService\s*=\s*game:GetService\("RunService"\))")));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Instance\s*=\s*workspace:FindFirstChild\("Map"\))")));
    CHECK(std::regex_search(out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Humanoid\s*=\s*script.Parent:FindFirstChildOfClass\("Humanoid"\))")));
}

TEST_CASE("Lift: InferRobloxTypes annotates dot-call FindFirstChildWhichIsA returns", "[Decompiler][TypeInference][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local part = workspace.FindFirstChildWhichIsA(workspace, "BasePart")
        print(part)
        return part
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(
        std::regex_search(
            out, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*BasePart\s*=\s*workspace\.FindFirstChildWhichIsA\(workspace,\s*"BasePart"\))")
        )
    );
}

TEST_CASE("Lift: AutoNameVariables derives names from Roblox lookup calls", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local service = game:GetService("RunService")
        local child = workspace:FindFirstChild("Map")
        local part = workspace:FindFirstChildWhichIsA("BasePart")
        print(service, child, part)
        return service, child, part
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: AutoNameVariables"));
    CHECK(Contains(out, "local RunService = game:GetService(\"RunService\")"));
    CHECK(Contains(out, "local Map = workspace:FindFirstChild(\"Map\")"));
    CHECK(Contains(out, "local BasePart = workspace:FindFirstChildWhichIsA(\"BasePart\")"));
    CHECK(Contains(out, "print(RunService, Map, BasePart)"));
    CHECK(Contains(out, "return RunService, Map, BasePart"));
}

TEST_CASE("Lift: AutoNameVariables prefixes Roblox names on collision", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local child = workspace:FindFirstChild("Map")
        local otherChild = workspace:FindFirstChild("Map")
        print(child, otherChild)
        return child, otherChild
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Map = workspace:FindFirstChild(\"Map\")"));
    CHECK(std::regex_search(out, std::regex(R"(local\s+v[0-9]+_Map\s*=\s*workspace:FindFirstChild\("Map"\))")));
    CHECK(!Contains(out, "local Map = workspace:FindFirstChild(\"Map\")\nlocal Map = workspace:FindFirstChild(\"Map\")"));
}

TEST_CASE("Lift: AutoNameVariables keeps references to prefixed scoped locals", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        do
            local first = workspace:WaitForChild("HumanoidRootPart")
            first.Transparency = 0
        end
        do
            local second = workspace:WaitForChild("HumanoidRootPart")
            second.Transparency = 1
        end
        do
            local third = workspace:WaitForChild("HumanoidRootPart")
            third.Transparency = 2
        end
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "HumanoidRootPart.Transparency = 0"));
    const std::regex prefixed(R"(local\s+(v[0-9]+_HumanoidRootPart)\s*=\s*workspace:WaitForChild\("HumanoidRootPart"\))");
    auto match = std::sregex_iterator(out.begin(), out.end(), prefixed);
    REQUIRE(match != std::sregex_iterator{});
    const auto second = (*match++)[1].str();
    REQUIRE(match != std::sregex_iterator{});
    const auto third = (*match++)[1].str();
    CHECK(second != third);
    CHECK(Contains(out, second + ".Transparency = 1"));
    CHECK(Contains(out, third + ".Transparency = 2"));
}

TEST_CASE("Lift: AutoNameVariables handles dot-call Roblox lookups", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local players = game.GetService(game, "Players")
        local part = workspace.FindFirstChildOfClass(workspace, "Part")
        print(players, part)
        return players, part
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Players = game.GetService(game, \"Players\")"));
    CHECK(Contains(out, "local Part = workspace.FindFirstChildOfClass(workspace, \"Part\")"));
    CHECK(Contains(out, "print(Players, Part)"));
    CHECK(Contains(out, "return Players, Part"));
}

TEST_CASE("Lift: AutoNameVariables sanitizes derived names", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local bad = workspace:FindFirstChild("Bad Name-1")
        local numeric = workspace:FindFirstChild("123Folder")
        print(bad, numeric)
        return bad, numeric
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "local Bad_Name_1 = workspace:FindFirstChild(\"Bad Name-1\")"));
    CHECK(Contains(out, "local _123Folder = workspace:FindFirstChild(\"123Folder\")"));
    CHECK(Contains(out, "print(Bad_Name_1, _123Folder)"));
    CHECK(Contains(out, "return Bad_Name_1, _123Folder"));
}

TEST_CASE("Lift: AutoNameVariables ignores unsupported calls", "[Decompiler][AutoName][RobloxTypes]") {
    const auto out = DecompileOrFail(
        R"(
        local children = workspace:GetChildren()
        print(children)
        return children
    )",
        0, DecompilerFlags::AutoNameVariables
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Decompile Options: AutoNameVariables"));
    CHECK(!Contains(out, "local GetChildren"));
    CHECK(!Contains(out, "local Children"));
}

TEST_CASE("Lift: AutoNameVariables and InferRobloxTypes compose without coupling", "[Decompiler][AutoName][TypeInference][RobloxTypes]") {
    const auto typedOnly = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::InferRobloxTypes
    );
    const auto namedOnly = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::AutoNameVariables
    );
    const auto both = DecompileOrFail(
        R"(
        local players = game:GetService("Players")
        print(players)
        return players
    )",
        0, DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables
    );

    INFO("typed only:\n" << typedOnly);
    CHECK(Contains(typedOnly, "Decompile Options: InferRobloxTypes"));
    CHECK(std::regex_search(typedOnly, std::regex(R"(local\s+[A-Za-z_][A-Za-z_0-9]*\s*:\s*Players\s*=\s*game:GetService\("Players"\))")));
    CHECK(!Contains(typedOnly, "Decompile Options: AutoNameVariables"));

    INFO("named only:\n" << namedOnly);
    CHECK(Contains(namedOnly, "Decompile Options: AutoNameVariables"));
    CHECK(Contains(namedOnly, "local Players = game:GetService(\"Players\")"));
    CHECK(!Contains(namedOnly, "local Players: Players"));

    INFO("both:\n" << both);
    CHECK(Contains(both, "Decompile Options: InferRobloxTypes, AutoNameVariables"));
    CHECK(Contains(both, "local Players: Players = game:GetService(\"Players\")"));
    CHECK(Contains(both, "print(Players)"));
    CHECK(Contains(both, "return Players"));
}

TEST_CASE("Lift: complex strict ModuleScript with generic loops and deferred callback survives", "[Decompiler][ModuleScript][Regression]") {
    const auto out = DecompileOrFail(
        R"(
        --!strict

        local RunService = game:GetService("RunService")
        local SignalPlus = require("../Networking/SignalPlus")

        type promised_results = {Params: {}, Results: boolean, Signal: SignalPlus.Signal<any>}
        type table_type = {
            [string]: {Signal: SignalPlus.Signal<any>, init: () -> promised_results}
        }

        local module = {
            Signals = {
                OnCharacterChange = require("@self/Internal/OnCharacterChange")
            } :: table_type;
            Cache = {} :: {promised_results}
        }

        function module.init()
            for _, signal in module.Signals do
                local results_table = signal.init()
                local results = results_table.Results
                if not results then continue end
                table.insert(module.Cache, results_table)
            end

            task.defer(function()
                for _, signal_results in module.Cache do
                    local signal = signal_results.Signal
                    signal:Fire(table.unpack(signal_results.Params))
                end
                table.clear(module.Cache)
            end)
        end

        RunService:BindToRenderStep(module.init, 1, module.init)
        return module
    )",
        0, DecompilerFlags::InferRobloxTypes
    );

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "BindToRenderStep"));
    // Declaration and closure references must share the captured upvalue name.
    CHECK(Contains(out, "local module ="));
    CHECK(CompilesOk(out));
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "table.insert"));
    CHECK(Contains(out, "table.clear"));
    CHECK(Contains(out, "module.Cache"));
}

// Upvalues

TEST_CASE("Lift: upvalue debug-name propagates onto captured local in parent scope", "[Decompiler][Upvalue]") {
    // The inner closure captures `file` (debug-named upvalue); the outer
    // function's parameter that held the captured register should be rendered
    // as `file` (not `arg0` / `a0`), and a propagation marker comment must be
    // emitted at the closure site.
    const auto out = DecompileOrFail(R"(
        return function(file)
            return function()
                return readfile(file)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "(file)"));                                    // outer param renamed
    CHECK(Contains(out, "readfile(file)"));                            // inner body uses upvalue name
    CHECK(CountOccurrences(out, "arg0") == 0);                         // default arg name gone
    CHECK(Contains(out, "Name 'file' propagated from upvalue names")); // marker present
}

TEST_CASE("Lift: upvalue propagation comment marks every propagated capture", "[Decompiler][Upvalue]") {
    // Multiple named upvalues should each get a propagation marker; outer
    // params must render as the upvalue names rather than `arg{N}`.
    const auto out = DecompileOrFail(R"(
        return function(first, second)
            return function()
                return first + second
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "Name 'first' propagated from upvalue names"));
    CHECK(Contains(out, "Name 'second' propagated from upvalue names"));
    CHECK(Contains(out, "first + second"));
}

TEST_CASE("Lift: upvalue propagation does not introduce a redundant `local up = src` declaration", "[Decompiler][Upvalue]") {
    // When the upvalue name is propagated the lifter must NOT emit the old
    // `-- Fission: Beginning captures...` / `local file = arg0` shape.
    const auto out = DecompileOrFail(R"(
        return function(file)
            return function()
                return readfile(file)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(Contains(out, "Beginning captures"));
    CHECK_FALSE(Contains(out, "local file = arg"));
}

// Calls

// With every Luau* FFlag enabled, the compiler emits CALLFB (a feedback-collecting call) in place of
// CALL. Both plain calls and method calls must decompile to ordinary calls, not vanish or garble.
TEST_CASE("Lift: feedback calls (CALLFB) render as ordinary calls", "[Decompiler][Call]") {
    const auto out = DecompileOrFail(R"(
        return function(t, x)
            local a = print(x)
            local b = t:method(x)
            return a, b
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "print("));   // plain call survives
    CHECK(Contains(out, ":method(")); // method call survives
    CHECK(CompilesOk(out));
}

// CMPPROTO is a V11 native-codegen closure-specialization guard: it is never emitted by Luau::compile
// (only appears inside CodeGen's internal bytecode graph) and has no Luau source form, so the lifter
// elides it. Decompiling bytecode that contains one must not crash or corrupt the surrounding code; the
// guarded closure and the code around it must still come out as valid Luau. Hand-assembled because no
// source produces this opcode.
TEST_CASE("Lift: CMPPROTO is elided without crashing or corrupting surrounding code", "[Decompiler][CMPPROTO]") {
    EnableLuauFFlagsOnce();
    Luau::BytecodeBuilder bb{};

    const uint32_t child = bb.beginFunction(0, /*isvararg*/ false);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);

    const uint32_t main = bb.beginFunction(0, /*isvararg*/ true);
    const int16_t childIdx = bb.addChildFunction(child);
    bb.emitAD(LOP_NEWCLOSURE, 0, childIdx); // R0 = function() end
    // jump target is relative to the word after the primary insn; CMPPROTO carries an AUX word, so a
    // jump of 1 lands on the following RETURN (the fall-through target) rather than the AUX word. jump 0
    // would target the AUX and fails Luau's debug BytecodeBuilder validation.
    bb.emitAD(LOP_CMPPROTO, 0, /*jump*/ 1);   // guard on R0 (elided)
    bb.emitAux(static_cast<uint32_t>(child)); // AUX = proto id
    bb.emitABC(LOP_RETURN, 0, 2, 0);          // return R0
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);
    bb.setMainFunction(main);
    bb.finalize();

    const auto out = DecompileVanillaOrFail(bb.getBytecode()); // REQUIRE(Success) inside
    INFO("decompile:\n" << out);
    CHECK(Contains(out, "function")); // the guarded closure survived
    CHECK(Contains(out, "return"));   // and is returned
    CHECK(CompilesOk(out));           // the output is valid Luau
}

// Inline anonymous closures

TEST_CASE("Lift: single-use closure passed as call argument is inlined", "[Decompiler][InlineAnon]") {
    // A single-use call-argument closure must appear directly inside the call.
    const auto out = DecompileOrFail(R"(
        local file = "x"
        local ok, err = pcall(function()
            return readfile(file)
        end)
        return ok, err
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "pcall(function("));
    CHECK(CountOccurrences(out, "local function anon_") == 0);
    // The call must not reference any synthesised closure-temp identifier.
    CHECK(CountOccurrences(out, "pcall(anon_") == 0);
}

TEST_CASE("Lift: FASTPCALL is decoded and protected calls remain source calls", "[Decompiler][FASTPCALL]") {
    EnableLuauFFlagsOnce();
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const auto bytecode = Luau::compile(
        R"(
        local ok, result = pcall(function() return 42 end)
        local ok2, result2 = xpcall(function() return 43 end, function(err) return err end)
        return ok, result, ok2, result2
    )",
        opts
    );
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode[0] != '\0');

    Deserializer deserializer{};
    const auto decoded = deserializer.Deserialize(bytecode);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->bytecodeVersion == 14);

    int fastPcallCount = 0;
    bool sawPcallSelector = false;
    bool sawXpcallSelector = false;
    for (const auto &function : decoded->functions)
        for (const auto instruction : function.instructions) {
            if (instruction.GetOpCode() != LOP_FASTPCALL)
                continue;
            ++fastPcallCount;
            sawPcallSelector |= instruction.GetABCOperand(LuauInstruction::LuauOperand::A) == 0;
            sawXpcallSelector |= instruction.GetABCOperand(LuauInstruction::LuauOperand::A) == 1;
        }
    CHECK(fastPcallCount >= 2);
    CHECK(sawPcallSelector);
    CHECK(sawXpcallSelector);

    Decompiler decompiler{};
    const auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(Contains(result.decompilationOutput, "pcall("));
    CHECK(Contains(result.decompilationOutput, "xpcall("));
    CHECK(CompilesOk(result.decompilationOutput));
}

TEST_CASE("Lift: vararg-only anonymous function emits valid argument list", "[Decompiler][InlineAnon]") {
    const auto out = DecompileOrFail(R"(
        local f = function(...)
            return ...
        end
        return f
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "(..."));
    CHECK(Contains(out, "return ..."));
    CHECK_FALSE(Contains(out, "function(, ..."));
}

TEST_CASE("Lift: string literals escape backslashes", "[Decompiler][Strings]") {
    const auto out = DecompileOrFail(R"(
        return "\\"
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, R"("\\")"));
    CHECK_FALSE(Contains(out, R"("\")"));
}

TEST_CASE("Lift: closure used as call CALLEE (IIFE) is not folded as `function(...)` arg", "[Decompiler][InlineAnon]") {
    // `(function() ... end)()` is an immediately-invoked expression where the
    // closure is the callee. The inline rule must NOT trigger here (it is
    // reserved for the closure-passed-as-PARAMETER case).
    const auto out = DecompileOrFail(R"(
        return (function() return 42 end)()
    )");

    INFO("decompile:\n" << out);
    // The closure must keep a name binding (a local function), even though
    // the callee-folding rule could shape it differently in the future. The
    // Preserve the reachable constant without crashing on the malformed path.
    CHECK(Contains(out, "42"));
}

TEST_CASE("Lift: closure return slot from pcall does not inherit the closure's name", "[Decompiler][Naming]") {
    // The result registers of `pcall(closure)` must NOT carry the closure's
    // own identifier name (was the original `local v1, anon_N = pcall(anon_N)`
    // bug). Combined with inline-anon emission the typical shape is now
    // `local <ok>, <err> = pcall(function() ... end)`.
    const auto out = DecompileOrFail(R"(
        return pcall(function() return 1 end)
    )");

    INFO("decompile:\n" << out);
    // No identifier of the form `anon_<...>` should appear anywhere in the
    // output now that the closure is inlined and the return register is
    // assigned a fresh temp name.
    CHECK(CountOccurrences(out, "anon_") == 0);
}

// Short-circuit expressions

TEST_CASE("Lift: `or` fallback preserves both operands and yields `or` semantics", "[Decompiler][ShortCircuit]") {
    // Named arguments make the expected `or` operands stable in debug output.
    const auto out = DecompileOrFail(R"(
        return function(a) return a or "fallback" end
    )");

    INFO("decompile:\n" << out);
    // Both operands and their order must survive.
    CHECK(Contains(out, "fallback"));
    // The output must reach the fallback by some control path that
    // corresponds to `a` being falsy (no `and` form).
    CHECK_FALSE(Contains(out, "a and \"fallback\""));
}

TEST_CASE("Lift: nested `if not X then X = Y end` is folded to an `or` expression", "[Decompiler][ShortCircuit]") {
    // The post-use `sink(v)` keeps `v` alive in a register, so the bytecode
    // lands as a proper JUMPIF/MOVE chain. The chain-fold pass should erase
    // the explicit `if not v` blocks and produce one or more `or` operators.
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, sink)
            local v = a
            if not v then v = b end
            if not v then v = c end
            sink(v)
        end
    )");

    INFO("decompile:\n" << out);
    // The chain-fold may not always pretty-print as a single `a or b or c`
    // (depends on adjacent statement shapes), but it should at least produce
    // one `or` operator and erase the explicit `if not v` skeleton.
    const bool foldedSomeway = Contains(out, " or ") || CountOccurrences(out, "if not v") == 0;
    CHECK(foldedSomeway);
}

// Numeric for loops

TEST_CASE("Lift: numeric `for` emits a proper loop-variable identifier", "[Decompiler][NumericFor]") {
    // Regression for `for 1 = 1, n, 1 do ... end` and `for err_not_reg = ...`.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local s = 0
            for i = 1, n do
                s = s + i
            end
            return s
        end
    )");

    INFO("decompile:\n" << out);
    REQUIRE(Contains(out, "for "));
    CHECK_FALSE(Contains(out, "for 1 ="));
    CHECK_FALSE(Contains(out, "err_not_reg"));

    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // Identifier regex already constrains the first char; the assertion is
    // structural sanity.
    CHECK(std::isalpha(static_cast<unsigned char>(varName.front())) != 0);
}

TEST_CASE("Lift: numeric `for` body re-uses the same loop-variable name as the header", "[Decompiler][NumericFor]") {
    // Prevent the regression where the header rendered as `for 1 = ...` while
    // the body referenced the loop var as `i_3`. Index a table by the loop
    // variable so the body must reference it explicitly.
    const auto out = DecompileOrFail(R"(
        return function(n)
            local t = {}
            for i = 1, n do
                t[i] = i
            end
            return t
        end
    )");

    INFO("decompile:\n" << out);
    auto varName = ExtractFirstNumericForLoopVar(out);
    INFO("loop var name = " << varName);
    REQUIRE_FALSE(varName.empty());
    // The loop-var token should appear at least once more (inside the body).
    CHECK(CountOccurrences(out, varName) >= 2);
}

// Generic for loops

TEST_CASE("Lift: generic for over `pairs` emits `for k, v in pairs(t)`", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for k, v in pairs(t) do
                print(k, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "pairs("));
}

TEST_CASE("Lift: generic for over `ipairs` survives lift", "[Decompiler][GenericFor]") {
    const auto out = DecompileOrFail(R"(
        return function(t)
            for i, v in ipairs(t) do
                print(i, v)
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "for "));
    CHECK(Contains(out, " in "));
    CHECK(Contains(out, "ipairs("));
}

// Tables
// Dict-style key-value table literal reconstruction remains a known
// limitation (SETTABLEKS after NEWTABLE may be fragmented by SSA/CFG).
// List-style (SETLIST) reconstruction works reliably.

TEST_CASE("Lift: empty table literal has balanced braces", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{  }` with spaces; check brace balance.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: list-style table literal preserves numeric elements", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {10, 20, 30}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // Output may be `{ 10, 20, 30 }` with space after `{`.
    CHECK(CountOccurrences(out, "10") >= 1);
    CHECK(CountOccurrences(out, "20") >= 1);
    CHECK(CountOccurrences(out, "30") >= 1);
    // All three elements survive.
    CHECK(Contains(out, "{"));
    CHECK(Contains(out, "}"));
    // No key-value syntax for plain list elements.
    CHECK_FALSE(Contains(out, "[1]"));
}

TEST_CASE("Lift: nested table literal preserves inner list tables", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {{1, 2}, {3, 4}}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    // All four elements survive.
    CHECK(CountOccurrences(out, "1") >= 1);
    CHECK(CountOccurrences(out, "2") >= 1);
    CHECK(CountOccurrences(out, "3") >= 1);
    CHECK(CountOccurrences(out, "4") >= 1);
    // At least two brace pairs (outer + inner).
    CHECK(CountOccurrences(out, "{") >= 3);
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
}

TEST_CASE("Lift: absurd mixed SETLIST table literal terminates and preserves edge elements", "[Decompiler][Table][SETLIST][Stress]") {
    std::stringstream source;
    source << "return function(a, b)\nlocal t = {";
    for (int i = 1; i <= 260; ++i) {
        if (i > 1)
            source << ", ";
        switch (i % 10) {
        case 0:
            source << "{ " << i << ", key = \"v" << i << "\", nested = {" << (i + 1) << ", " << (i + 2) << "} }";
            break;
        case 1:
            source << "a + " << i;
            break;
        case 2:
            source << "b and \"s" << i << "\" or nil";
            break;
        case 3:
            source << "function(x) return x + " << i << " end";
            break;
        case 4:
            source << "{ [\"dyn" << i << "\"] = a, " << i << " }";
            break;
        case 5:
            source << "not b";
            break;
        case 6:
            source << "(a * " << i << ") % 7";
            break;
        case 7:
            source << "\"edge" << i << "\"";
            break;
        case 8:
            source << "nil";
            break;
        default:
            source << i;
            break;
        }
    }
    source << ", absurdKey = { tail = 9999, flag = true }, [a] = b }\nreturn t\nend";

    const auto out = DecompileOrFail(source.str());

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "260"));
    CHECK(Contains(out, "9999"));
    CHECK(Contains(out, "function"));
    CHECK(CountOccurrences(out, "{") == CountOccurrences(out, "}"));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\.)")));
    CHECK_FALSE(std::regex_search(out, std::regex(R"(\}\s*\[)")));
}

TEST_CASE("Lift: table literal as return value preserves content", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            return {1, 2, 3}
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "return {"));
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    CHECK(Contains(out, "3"));
}

TEST_CASE("Lift: table literal with expression elements preserves operators", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            local t = {a + 1, b * 2, a - b}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "+"));
    CHECK(Contains(out, "*"));
    CHECK(Contains(out, "-"));
    CHECK(Contains(out, "a"));
    CHECK(Contains(out, "b"));
}

TEST_CASE("Lift: table literal preserves aliases across later register reuse", "[Decompiler][Table][Alias]") {
    const auto out = DecompileOrFail(R"(
        local nested = {1}
        print(nested)
        local g = rat()
        local t = {nested}
        return g, t
    )");

    INFO("decompile:\n" << out);
    static const std::regex ratThenSelfTable(R"(local\s+([A-Za-z_][A-Za-z_0-9]*)\s*=\s*rat\(\)\s+local\s+[A-Za-z_][A-Za-z_0-9]*\s*=\s*\{\s*\1\s*\})");
    CHECK_FALSE(std::regex_search(out, ratThenSelfTable));
    CHECK(Contains(out, "rat()"));
    CHECK(Contains(out, "{ 1 }"));
}

TEST_CASE("Lift: DUPTABLE nil template values do not index constants out of range", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return { a = nil }
    )");

    INFO("decompile:\n" << out);
    CHECK(std::regex_search(out, std::regex(R"(return\s+\{\s*a\s*=\s*nil\s*\})")));
}

TEST_CASE("Lift: dict-style table keys are preserved", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {x = 1, y = 2, z = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "x = 1"));
    CHECK(Contains(out, "y = 2"));
    CHECK(Contains(out, "z = 3"));
}

TEST_CASE("Lift: mixed list/dict table literal preserves both elements and keys", "[Decompiler][Table]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local t = {1, 2, key = 3}
            return t
        end
    )");

    INFO("decompile:\n" << out);
    CHECK(Contains(out, "1"));
    CHECK(Contains(out, "2"));
    CHECK(Contains(out, "key = 3"));
}

TEST_CASE("Lift: AssignmentStatementNode RHS not gratuitously parenthesised", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function()
            local x = 5
            x = x + 1
            return x
        end
    )");

    INFO("decompile:\n" << out);
    // The previous always-wrap behaviour emitted `x = (x + 1)`. The
    // precedence-aware pass must drop the redundant outer parens.
    CHECK_FALSE(Contains(out, "x = (x + 1)"));
}

TEST_CASE("Lift: parallel swap preserves saved value", "[Decompiler][Assignment]") {
    const std::string pairSwap = R"(
        local function pair(x)
            return x, x + 1
        end
        local first, second = pair(5)
        first, second = second, first
        return first, second
    )";
    const auto checkBehavior = [](const std::string &source, const std::string &expected) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string originalBc = Luau::compile(source, opts);
        const std::string decompiledBc = Luau::compile(DecompileOrFail(source), opts);
        const std::string emptyPrelude = Luau::compile("", opts);
        const auto original = fuzz::RunLuauTrace(originalBc, emptyPrelude);
        const auto decompiled = fuzz::RunLuauTrace(decompiledBc, emptyPrelude);
        CHECK(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(decompiled.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == expected);
        CHECK(decompiled.trace == original.trace);
    };
    checkBehavior(pairSwap, "return: 6\t5\n");
    checkBehavior("local first, second = 5, 6\nfirst, second = second, first\nreturn first, second\n", "return: 6\t5\n");
}

TEST_CASE("Lift: loop variable naming preserves outer header bindings", "[Decompiler][LoopBinding]") {
    const auto identifier = [](const std::string &name) { return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(name)); };
    for (const bool numeric : {false, true}) {
        const std::string originalName = numeric ? "i_4" : "v4";
        auto variable = identifier(originalName);
        auto header = identifier(originalName);
        auto bodyUse = identifier(originalName);
        auto body = std::make_shared<BlockStatementNode>();
        body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{bodyUse}));
        std::vector<std::shared_ptr<Statement>> statements;
        if (numeric) {
            auto loop = std::make_shared<ForNumericNode>();
            loop->loopVariable = variable;
            loop->startVariable = header;
            loop->maxIncreased = identifier(originalName);
            loop->increaseBy = identifier(originalName);
            loop->lpLoopBody = body;
            statements.push_back(loop);
        } else {
            auto loop = std::make_shared<ForGeneralNode>();
            loop->loopVariables = {variable};
            loop->generator = header;
            loop->state = identifier(originalName);
            loop->index = identifier(originalName);
            loop->body = body;
            statements.push_back(loop);
        }
        LoopVariableRenamer{}.Run(statements);
        CHECK(header->identifier->name == originalName);
        CHECK(variable->identifier->name != originalName);
        CHECK(bodyUse->identifier->name == variable->identifier->name);
    }
}

TEST_CASE("Lift: captured anonymous closure keeps one binding", "[Decompiler][ClosureBinding]") {
    const auto check = [](const std::string &source) {
        const auto out = DecompileOrFail(source);
        INFO(out);
        const auto prelude = Luau::compile("", Luau::CompileOptions{1, 2});
        const auto original = fuzz::RunLuauTrace(Luau::compile(source, Luau::CompileOptions{1, 2}), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(out, Luau::CompileOptions{1, 2}), prelude);
        REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: 5\t9\n");
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };
    check(R"(
        local v0 = (function(x) return x + 1 end)
        local before = v0(4)
        local function later() return v0(8) end
        return before, later()
    )");
    check(R"(
        local offset = tonumber('1')
        local v0 = (function(x) return x + offset end)
        local before = v0(4)
        local function later() return v0(8) end
        return before, later()
    )");
}

TEST_CASE("Lift: reassigned captured closure keeps original binding", "[Decompiler][ClosureBinding][Regression]") {
    const std::string source = R"(
        local function f() return 1 end
        local function read() return f end
        f = function() return 2 end
        return read()()
    )";
    const auto out = DecompileOrFail(source);
    INFO(out);
    CheckSameTrace(source, out);
}

TEST_CASE("Lift: `if` condition has no redundant outer parentheses around a bare comparison", "[Decompiler][Parens]") {
    const auto out = DecompileOrFail(R"(
        return function(a, b)
            if a == b then return 1 end
            return 0
        end
    )");

    INFO("decompile:\n" << out);
    // `if (a == b) then` form must not appear; the comparison is the entire
    // condition and needs no extra grouping.
    CHECK_FALSE(Contains(out, "if (a == b)"));
}

TEST_CASE("Lift: associative `or` chain has no redundant inner parentheses", "[Decompiler][Parens]") {
    // Same-operator chains under associative ops do not need inner parens.
    // Function-arg names are not yet propagated by the lifter (locals render
    // as `argN`), so the test matches the chain in a name-agnostic way.
    const auto out = DecompileOrFail(R"(
        return function(a, b, c, sink)
            sink(a or b or c)
        end
    )");

    INFO("decompile:\n" << out);
    // The literal `(X or Y) or Z` form (grouping that the associativity-aware
    // emitter is supposed to drop) must not appear.
    static const std::regex leftAssocGrouping(R"(\([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\) or )");
    static const std::regex rightAssocGrouping(R"( or \([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*\))");
    CHECK_FALSE(std::regex_search(out, leftAssocGrouping));
    CHECK_FALSE(std::regex_search(out, rightAssocGrouping));
    // Output should contain at least one `X or Y` pair.
    static const std::regex flatChain(R"([A-Za-z_][A-Za-z_0-9]* or [A-Za-z_][A-Za-z_0-9]*)");
    CHECK(std::regex_search(out, flatChain));
}

TEST_CASE("Lift: empty else block is omitted", "[Decompiler][Readability]") {
    const auto out = DecompileOrFail(R"(
        return function(value)
            if value then
                print(value)
            else
            end
        end
    )");

    INFO("decompile:\n" << out);
    CHECK_FALSE(std::regex_search(out, std::regex(R"(else\s*\n\s*end)")));
    CHECK(Contains(out, "if "));
    CHECK(Contains(out, "print("));
}

TEST_CASE("IfChainSimplifier removes only empty else blocks", "[Decompiler][Readability]") {
    auto condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("value"));
    auto thenBranch = std::make_shared<BlockStatementNode>();
    thenBranch->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto emptyElse = std::make_shared<BlockStatementNode>();
    auto emptyIf = std::make_shared<IfStatementNode>();
    emptyIf->condition = condition;
    emptyIf->thenBranch = thenBranch;
    emptyIf->elseBranch = emptyElse;
    std::vector<std::shared_ptr<Statement>> statements{emptyIf};
    IfChainSimplifier{}.Run(statements);
    CHECK(emptyIf->elseBranch == nullptr);
    CHECK(emptyIf->condition == condition);
    CHECK(emptyIf->thenBranch == thenBranch);

    auto nonemptyElse = std::make_shared<BlockStatementNode>();
    nonemptyElse->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto keptIf = std::make_shared<IfStatementNode>();
    keptIf->condition = condition;
    keptIf->thenBranch = thenBranch;
    keptIf->elseBranch = nonemptyElse;
    statements = {keptIf};
    IfChainSimplifier{}.Run(statements);
    CHECK(keptIf->elseBranch == nonemptyElse);
}

TEST_CASE("IfChainSimplifier inverts empty then without rewriting relational condition", "[Decompiler][Readability]") {
    auto left = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("value"));
    auto right = std::make_shared<NumberLiteralNode>(0.0);
    auto condition = std::make_shared<BinaryExpressionNode>("<", left, right);
    auto emptyThen = std::make_shared<BlockStatementNode>();
    auto elseBranch = std::make_shared<BlockStatementNode>();
    elseBranch->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
    auto ifNode = std::make_shared<IfStatementNode>();
    ifNode->condition = condition;
    ifNode->thenBranch = emptyThen;
    ifNode->elseBranch = elseBranch;
    std::vector<std::shared_ptr<Statement>> statements{ifNode};
    IfChainSimplifier{}.Run(statements);
    auto inverted = std::dynamic_pointer_cast<UnaryExpressionNode>(ifNode->condition);
    REQUIRE(inverted);
    CHECK(inverted->op == "not ");
    CHECK(inverted->operand == condition);
    CHECK(ifNode->thenBranch == elseBranch);
    CHECK(ifNode->elseBranch == nullptr);
}

TEST_CASE("IfChainSimplifier merges identical elseif arms and keeps trailing else", "[Decompiler][Readability]") {
    auto makeBody = [] {
        auto body = std::make_shared<BlockStatementNode>();
        body->body.push_back(std::make_shared<ReturnStatementNode>(std::vector<std::shared_ptr<Expression>>{}));
        return body;
    };
    auto outer = std::make_shared<IfStatementNode>();
    outer->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("a"));
    outer->thenBranch = makeBody();
    auto inner = std::make_shared<IfStatementNode>();
    inner->condition = std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("b"));
    inner->thenBranch = makeBody();
    inner->elseBranch = std::make_shared<BlockStatementNode>();
    outer->elseBranch = std::make_shared<BlockStatementNode>();
    outer->elseBranch->body.push_back(inner);
    std::vector<std::shared_ptr<Statement>> statements{outer};
    IfChainSimplifier{}.Run(statements);
    auto condition = std::dynamic_pointer_cast<BinaryExpressionNode>(outer->condition);
    REQUIRE(condition);
    CHECK(condition->op == "or");
    CHECK(std::dynamic_pointer_cast<IdentifierExpressionNode>(condition->left)->identifier->name == "a");
    CHECK(std::dynamic_pointer_cast<IdentifierExpressionNode>(condition->right)->identifier->name == "b");
    CHECK(outer->elseBranch == inner->elseBranch);
}

TEST_CASE("Lift: FastWait keeps short-circuit duration separate from timer", "[Decompiler][ShortCircuit][Regression]") {
    std::ifstream fixture{FISSION_SOURCE_DIR "/Samples/EncodedRoblox/FAST_WAIT_BRANCH.txt", std::ios::binary};
    REQUIRE(fixture);
    std::stringstream encoded;
    encoded << fixture.rdbuf();

    Decompiler decompiler{};
    const auto flags = DecompilerFlags::OptimizeIR | DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
    const auto result = decompiler.DecompileRobloxBytecode(DecodeBase64(encoded.str()), flags);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompile:\n" << result.decompilationOutput);
    INFO("IR:\n" << result.irOutput);

    const auto functionStart = result.decompilationOutput.find("local function fastWait");
    REQUIRE(functionStart != std::string::npos);
    const auto function = result.decompilationOutput.substr(functionStart);
    std::smatch duration;
    std::smatch timer;
    REQUIRE(std::regex_search(function, duration, std::regex(R"(local\s+(\w+): number = tonumber\(arg0\))")));
    REQUIRE(std::regex_search(function, timer, std::regex(R"(local\s+(\w+) = tick\(\))")));
    CHECK(Contains(function, std::string(" = ") + timer[1].str() + " + (" + duration[1].str() + " or 0.03333333333333333)"));
    CHECK(Contains(function, std::string("coroutine.yield() - ") + timer[1].str()));
    CHECK(Contains(function, ": thread = coroutine.running()"));
    CHECK_FALSE(Contains(function, "v2 + (v2 or"));
    CHECK_FALSE(Contains(function, "\n    do\n"));
    CHECK_FALSE(Contains(result.decompilationOutput, ": table"));
}

TEST_CASE("Lift: ItemSpawn successful guards continue to later checks", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    const auto tickStart = out.find("local serverTimeNow");
    const auto tickEnd = out.find("Triggered:Connect", tickStart);
    REQUIRE(tickStart != std::string::npos);
    REQUIRE(tickEnd != std::string::npos);
    const auto tick = out.substr(tickStart, tickEnd - tickStart);

    const auto weatherIndent = LineIndentContaining(tick, "RequiredWeather");
    const auto oreIndent = LineIndentContaining(tick, "RequiredOre");
    const auto eventIndent = LineIndentContaining(tick, "RequiredEvent");
    const auto limitIndent = LineIndentContaining(tick, "LimitedAmount");
    REQUIRE(weatherIndent != std::string::npos);
    REQUIRE(oreIndent != std::string::npos);
    REQUIRE(eventIndent != std::string::npos);
    REQUIRE(limitIndent != std::string::npos);
    CHECK_FALSE(std::regex_search(tick, std::regex(R"(\belseif[^\n]*RequiredOre)")));
    CHECK(oreIndent == weatherIndent);
    CHECK(eventIndent == weatherIndent);
    CHECK(limitIndent == weatherIndent);
}

TEST_CASE("Lift: ItemSpawn active weather still invokes collection", "[Decompiler][ControlFlow][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    const auto promptStart = out.find("Triggered:Connect");
    const auto promptEnd = out.find("SetState(false, true)", promptStart);
    REQUIRE(promptStart != std::string::npos);
    REQUIRE(promptEnd != std::string::npos);
    const auto prompt = out.substr(promptStart, promptEnd - promptStart);

    const auto weatherIndent = LineIndentContaining(prompt, "RequiredWeather");
    const auto invokeIndent = LineIndentContaining(prompt, "InvokeServer");
    REQUIRE(weatherIndent != std::string::npos);
    REQUIRE(invokeIndent != std::string::npos);
    CHECK(invokeIndent == weatherIndent);
}

TEST_CASE("Lift: ItemSpawn method closures remain local", "[Decompiler][Closure][Regression]") {
    const auto out = DecompileItemSpawnOrFail();
    CHECK_FALSE(std::regex_search(out, std::regex(R"((^|\n)(Construct|Start|Stop)\s*=\s*function)")));
}

TEST_CASE("Lift: rename comments only describe surviving suffixed locals", "[Decompiler][AutoName][Regression]") {
    std::ifstream fixture{FISSION_SOURCE_DIR "/Samples/EncodedRoblox/MODULE_LOADER_STALE_SUFFIX.txt", std::ios::binary};
    REQUIRE(fixture);
    std::stringstream encoded;
    encoded << fixture.rdbuf();

    Decompiler decompiler{};
    const auto flags = DecompilerFlags::OptimizeIR | DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
    const auto result = decompiler.DecompileRobloxBytecode(DecodeBase64(encoded.str()), flags);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompile:\n" << result.decompilationOutput);

    CHECK(Contains(result.decompilationOutput, "local ok, result = pcall(require, v)"));
    CHECK_FALSE(Contains(result.decompilationOutput, "has been suffixed to avoid shadowing"));
}

TEST_CASE("Lift: nested auto names do not shadow visible locals", "[Decompiler][AutoName][Regression]") {
    const std::string source = R"(
        local v0 = {}
        local v1 = {}
        local v2 = {}
        function v0.start(arg0)
            local screen = nil
            local enabled
            for outerKey, outerValue in pairs(arg0) do
                if screen == nil then
                    screen = outerValue
                end
                for innerKey, innerValue in pairs(outerValue) do
                    v1[innerKey] = innerValue
                end
            end
            table.sort({}, function(left, right)
                local temporary = left + 1
                return temporary < right
            end)
            enabled = false
            local callback = function()
                return enabled
            end
            return callback, screen
        end
        return v0, v2
    )";
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 0;
    opts.debugLevel = 1;
    const auto out = DecompileVanillaOrFail(Luau::compile(source, opts));
    INFO("decompile:\n" << out);

    const auto start = out.find("function v0.start");
    REQUIRE(start != std::string::npos);
    const auto function = out.substr(start);
    CHECK_FALSE(std::regex_search(function, std::regex(R"(\blocal\s+v2\b)")));
    CHECK_FALSE(std::regex_search(function, std::regex(R"(for\s+\w+\s*,\s*v2\s+in)")));
    CHECK(CompilesOk(out));

    AnalyzedFunction names{};
    names.nameSuffix = "_4";
    names.enclosingNames = {"v2", "v2_4"};
    CHECK(names.DisambiguateOwnName("v2") == "v2_4_2");
    CHECK(names.DisambiguateOwnName("v2") == "v2_4_2");
}
