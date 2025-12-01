local function CFATestStress(A, B, C)
    local acc = 0
    local t = {10, 20, 30, 40, 50}
    local f = function(x) return x * 3, x - 1 end

    for i = 1, #t do
        local v = t[i]

        if v % 3 == 1 then
            local p, q = f(v + A)
            acc = acc + p
            if q < 0 then
                return acc
            end
            -- This code emits a JUMP after the RETURN present here. The JUMP is unreachable under normal LUAU VM conditions, as there is no label, and return cannot be skipped, since the JUMPIFNOTLT emited by the compiler skips directly to the FORNLOOP instruction.
        elseif v % 4 == 2 then
            acc = acc + (v * 2)
        else
            acc = acc - B
        end

   end
end