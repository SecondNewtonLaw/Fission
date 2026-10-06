//
// Created by Dottik on 2/6/2026.
//

// Type- and name-inference tests (run with InferTypes | InferRobloxTypes).

#include "IntegrationTestSupport.hpp"
#include <regex>
#include <string>

namespace {

    std::string DecompileTyped(const std::string &source, const std::string &environment = "") {
        fuzz::EnableLuauFlags();
        Decompiler decompiler{};
        const auto flags = DecompilerFlags::InferTypes | DecompilerFlags::InferRobloxTypes | DecompilerFlags::AutoNameVariables;
        std::string output;
        for (int optimization : {0, 1, 2}) {
            for (int debug : {0, 1, 2}) {
                INFO("O" << optimization << " D" << debug << "\n" << source);
                const Luau::CompileOptions options{optimization, debug};
                const auto bytecode = Luau::compile(source, options);
                REQUIRE_FALSE(bytecode.empty());
                REQUIRE(bytecode.front() != '\0');
                const auto result = decompiler.DecompileVanillaBytecode(bytecode, flags);
                REQUIRE(result.resultCode == DecompileResult::Success);
                integration_test::CheckOutput(bytecode, result.decompilationOutput, options, environment);
                if (optimization == 2 && debug == 1)
                    output = result.decompilationOutput;
            }
        }
        return output;
    }

    bool ContainsRegex(const std::string &hay, const std::regex &re) { return std::regex_search(hay, re); }

} // namespace

// Generated register names cannot be type annotations. Auto-named metatables fall back to table.
TEST_CASE("Types: setmetatable with an auto-named metatable falls back to table", "[Decompiler][Types][Integration]") {
    const auto out = DecompileTyped(R"(
        local function make(mt)
            local self = setmetatable({}, mt)
            return self
        end
        local value = make({__index = {answer = 42}})
        print(value.answer)
        value.answer = 7
        print(value.answer)
    )");
    INFO("decompile:\n" << out);
    // No annotation may be a generated identifier.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*arg\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*uv_\d+\b)")));
}

// Whole-program sanity: a class-style module must not emit any generated-name
// type annotation anywhere.
TEST_CASE("Types: class module emits no register-named annotations", "[Decompiler][Types][Integration]") {
    const auto out = DecompileTyped(R"(
        local Klass = {}
        Klass.__index = Klass
        function Klass.new()
            local self = setmetatable({}, Klass)
            self.values = {1, 2, 3}
            self.name = "k"
            return self
        end
        function Klass.get(self)
            return self.values
        end
        local first, second = Klass.new(), Klass.new()
        first.values[1] = 9
        print(Klass.get(first)[1], Klass.get(second)[1], first.name)
    )");
    INFO("decompile:\n" << out);
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*uv_\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*arg\d+\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*i_\d+\b)")));
}

// A number-typed local that survives optimisation gets `: number`.
// (Simple literals are inlined at -O2, so keep one alive via accumulation.)
TEST_CASE("Types: surviving numeric local is annotated number", "[Decompiler][Types][Integration]") {
    const auto out = DecompileTyped(R"(
        local function f(n)
            local total = 0
            for i = 1, n do
                total = total + i
            end
            return total
        end
        print(f(0), f(1), f(5))
    )");
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(:\s*number\b)")));
    // Still no register-named annotations.
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(:\s*v\d+\b)")));
}

// Name inference

// A multi-use `:WaitForChild("X")` / `:GetService("X")` result is named after
// the child/service instead of `v1`.
TEST_CASE("Names: WaitForChild/GetService results are named after the child", "[Decompiler][Names][Integration]") {
    const auto out = DecompileTyped(
        R"(
        local function f(parent)
            local thing = parent:WaitForChild("MyThing")
            thing.A = 1
            thing.B = 2
            local svc = game:GetService("RunService")
            svc.X = 1
            svc.Y = 2
            return thing, svc
        end
        local child, service = f(parent)
        print(child.A, child.B, service.X, service.Y)
    )",
        R"(
        parent = {WaitForChild = function(self, name) print("child", name) return {} end}
        game = {GetService = function(self, name) print("service", name) return {} end}
    )"
    );
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bMyThing\b\s*[:=])")));
    CHECK(ContainsRegex(out, std::regex(R"(\bRunService\b\s*[:=])")));
}

// `require(path:WaitForChild("Module"))` is named after the module. The child
// string constant is inlined into the call (single-version pure constants inline
// even as call arguments), so the require-naming heuristic can see it.
TEST_CASE("Names: require of a WaitForChild is named after the module", "[Decompiler][Names][Integration]") {
    const auto out = DecompileTyped(
        R"(
        local function f(parent)
            local mod = require(parent:WaitForChild("Helper"))
            mod.A = 1
            mod.B = 2
            return mod
        end
        local module = f(parent)
        print(module.A, module.B)
    )",
        R"(
        parent = {WaitForChild = function(self, name) print("child", name) return name end}
        require = function(name) print("require", name) return {} end
    )"
    );
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(\bHelper\b\s*[:=])")));
}
