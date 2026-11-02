#include "IntegrationTestSupport.hpp"

// Control-flow-flattened state machine with nested closures, varargs, bit32 and
// a metatable-driven dispatch. It is the kind of hostile shape where the loop
// and conditional lifters are most likely to mis-associate a branch target.
//
// The original sample used the Luau-invalid binary bitwise operators (`~`, `|`);
// they are expressed through bit32 here so the fixture compiles under Luau 0.738
// while preserving the shape the decompiler has to invert.
namespace {
    const char *kChaosMachine = R"(
local function chaosMachine(...)
    local _env = (getfenv or function() return _G end)()
    local _args = {...}
    local _state = 0xDEAD
    local _accumulator = 0

    while true do
        if _state == 0xDEAD then
            _state = (#_args > 0 and 0xBEEF) or 0xCAFE

        elseif _state == 0xBEEF then
            local _subArgs = { select(1, ...) }
            for _i = 1, #_subArgs do
                local _val = _subArgs[_i]
                if type(_val) == "number" then
                    _accumulator = (bit32 and bit32.bxor(_accumulator, _val)) or bit32.bxor(_accumulator, _val)
                else
                    _accumulator = _accumulator + string.len(tostring(_val))
                end
            end
            _state = 0xFACE

        elseif _state == 0xCAFE then
            local function innerClosure()
                local innerState = 1337
                return function()
                    innerState = innerState * 2
                    return innerState
                end
            end
            _accumulator = innerClosure()()
            _state = 0xFACE

        elseif _state == 0xFACE then
            local fakeTable = setmetatable({}, {
                __index = function(t, k)
                    return function(...)
                        return k .. tostring(...)
                    end
                end
            })

            if _accumulator > 999999 then
                _state = 0x0BAD
            else
                _state = 0x0001
            end

        elseif _state == 0x0001 then
            _accumulator = _accumulator + 1
            if _accumulator % 2 == 0 then
                _state = 0xBEEF
            else
                _state = 0xDEAD
            end

        elseif _state == 0x0BAD then
            return "DECOMPILER_BREAK", _accumulator
        else
            break
        end
    end
end

return chaosMachine
)";

    // Large numeric arguments push the accumulator past the threshold on the
    // first pass, so the state machine terminates instead of looping for ~1M
    // iterations and turning the comparison into a timeout match.
    const char *kDriver = R"(
local f = __integration_subject()
print(f(1048576, "abc"))
print(f(-1))
print(f(1048576, "xyz", 7))
)";
} // namespace

TEST_CASE("Control-flow-flattened chaos machine preserves execution", "[Decompiler][ChaosMachine][Integration]") {
    fuzz::EnableLuauFlags();
    const std::string source = kChaosMachine;
    for (const int optimization : {0, 1, 2}) {
        for (const int debug : {0, 1, 2}) {
            for (const DecompilerFlags irFlags : {static_cast<DecompilerFlags>(0), DecompilerFlags::OptimizeIR}) {
                INFO("O" << optimization << " D" << debug << " IR=" << (irFlags != static_cast<DecompilerFlags>(0)));
                const Luau::CompileOptions options{optimization, debug};
                const auto originalBytecode = Luau::compile(source, options);
                REQUIRE_FALSE(originalBytecode.empty());
                REQUIRE(originalBytecode.front() != '\0');
                Decompiler decompiler;
                const auto result = decompiler.DecompileVanillaBytecode(originalBytecode, irFlags | DecompilerFlags::AutoNameVariables);
                REQUIRE(result.resultCode == DecompileResult::Success);
                integration_test::CheckOutput(originalBytecode, result.decompilationOutput, options, "", kDriver);
            }
        }
    }
}
