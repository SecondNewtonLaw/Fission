local function test_or_chain(a, b, c)
    return a or b or c or "default"
end

local function test_and_or_combined(a, b, c, d)
    return a and b or c and d
end

local function test_complex_short_circuit(x, y)
    local t = {}
    t.value = x or y or 42
    t.guard = x and y and true or false
    return t
end

local function test_or_in_if(x, y, z)
    if x or y or z then
        return "at least one truthy"
    else
        return "all falsy"
    end
end

local function test_and_in_if(x, y, z)
    if x and y and z then
        return "all truthy"
    else
        return "at least one falsy"
    end
end
