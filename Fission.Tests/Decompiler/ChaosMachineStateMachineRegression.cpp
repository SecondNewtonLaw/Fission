#include "IntegrationTestSupport.hpp"

// Chaos Machine state machine (2026-11-01): a control-flow-flattened loop whose
// states are table keys, with obfuscated identifiers, a string decoder built on
// `bit32.bxor`, recursive metatable proxies and nested closures.
//
// The original sample spelled its unreachable `bit32` fallback with Lua 5.3's
// binary `~` operator, which is not Luau syntax; the fallback uses bit32.bxor
// here so the fixture compiles under Luau 0.738 while keeping the shape the
// decompiler has to invert.
namespace {
    const char *kChaosStateMachine = R"LUA(local function lIlIlI(lIIlIl, ...)
    local IIllII = {...}
    local lIllIl = function(IIllIl, lIlIlI)
        local IlIlIl = {}
        for lIIlll = 1, #IIllIl do
            local llIllI = string.byte(IIllIl, lIIlll)
            local lIlIIl = bit32 and bit32.bxor(llIllI, lIlIlI + (lIIlll % 5)) or bit32.bxor(llIllI, lIlIlI + (lIIlll % 5))
            IlIlIl[lIIlll] = string.char(lIlIIl)
        end
        return table.concat(IlIlIl)
    end

    local IIlIlI = lIllIl("\x33\x26\x31\x24\x37\x7A\x21\x32\x33\x7A\x20\x3D\x26\x32\x35", 0x42)

    local function llIlII(IlIlII, IlllII)
        if IlllII <= 0 then return IlIlII end
        local lIIlII = {}
        return setmetatable({}, {
            __index = function(IIlIll, lIlIll)
                if lIlIll == "unwrap" then return IlIlII end
                local lIIIlI = (type(IlIlII) == "table" and IlIlII[lIlIll]) or IIlIlI
                return llIlII(lIIIlI, IlllII - 1)
            end,
            __newindex = function(IIlIll, lIlIll, IllIIl)
                lIIlII[lIlIll] = llIlII(IllIIl, IlllII - 1)
            end,
            __call = function(IIlIll, ...)
                if type(IlIlII) == "function" then
                    return IlIlII(...)
                else
                    return llIlII(IlIlII, IlllII - 1)
                end
            end,
            __concat = function(llIlll, Illlll)
                return tostring(llIlll) .. "_" .. tostring(Illlll)
            end,
            __metatable = "LOCKED_" .. IlllII
        })
    end

    local llllII = 0xDEADBEEF
    local IllIll = {}
    local lIIIII = ""

    IllIll[0xDEADBEEF] = function()
        llllII = (#IIllII > 0 and 0x1337CAFE) or 0x0BADBEEF
        return llIlII(IIllII, 3)
    end

    IllIll[0x1337CAFE] = function()
        local IllIlI = llIlII(function(llIIll) return llIIll .. IIlIlI end, 2)
        llllII = 0xCAFEBABE
        return IllIlI("RUN")
    end

    IllIll[0x0BADBEEF] = function()
        llllII = 0xCAFEBABE
        return lIllIl("\x10\x11\x12", 0xFF)
    end

    IllIll[0xCAFEBABE] = function()
        llllII = 0x0
        return "END"
    end

    while llllII ~= 0 do
        local lIlIII = IllIll[llllII]
        if lIlIII then
            local lIllll, lIIlIl = pcall(lIlIII)
            if lIllll then
                lIIIII = tostring(lIIlIl) .. tostring(lIIIII)
            else
                llllII = 0xCAFEBABE
            end
        else
            break
        end
    end

    return lIIIII
end

return lIlIlI
)LUA";

    const char *kDriver = R"LUA(
local machine = __integration_subject()
print(type(machine))
print(pcall(machine))
)LUA";
} // namespace

TEST_CASE("Chaos machine obfuscated state machine preserves execution", "[Decompiler][ChaosMachine][Integration]") {
    fuzz::EnableLuauFlags();
    const std::string source = kChaosStateMachine;
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
                integration_test::CheckOutput(originalBytecode, result.decompilationOutput, options, "", kDriver);
            }
        }
    }
}