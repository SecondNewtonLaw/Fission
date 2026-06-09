local function test_deep_and_chain(a, b, c, d, e)
    return a and b and c and d and e
end

local function test_deep_or_chain(a, b, c, d, e)
    return a or b or c or d or e
end

local function test_mixed_and_or_complex(a, b, c, d)
    return (a or b) and (c or d)
end

local function test_x_or_y_and_z(x, y, z)
    return x or y and z
end

local function test_short_circuit_in_while(x, y, z)
    while x > 0 and y < 10 and z ~= 0 do
        x = x - 1
        y = y + 1
        z = z - 1
    end
    return x, y, z
end

local function test_short_circuit_in_if_chain(a, b, c, d)
    if a and b then
        return "a and b"
    elseif c or d then
        return "c or d"
    elseif a or (b and c) then
        return "a or b and c"
    end
    return "none"
end
