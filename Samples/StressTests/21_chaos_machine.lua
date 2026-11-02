-- Control-flow-flattened state machine.
--
-- The original sample expressed its bitwise fallback with the binary `~`
-- operator, which is not Luau syntax (Luau only exposes bit32). The fallback
-- is written with bit32.bxor here so the sample compiles under Luau 0.738
-- while keeping the flattened state machine, vararg selection, nested
-- closures and metatable dispatch that the lifter has to invert.
--
-- Large numeric arguments push the accumulator past the threshold on the
-- first pass, so this driver terminates instead of spinning for ~1M loops.
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

return chaosMachine(0x100000, "abc")
