local function test_varargs_sum(...)
    local args = {...}
    local sum = 0
    for i = 1, #args do
        sum = sum + args[i]
    end
    return sum
end

local function test_varargs_with_select(...)
    local first = select(1, ...)
    local second = select(2, ...)
    return first, second
end

local function test_varargs_passthrough(...)
    return test_varargs_sum(...)
end

local function test_varargs_after_fixed(a, b, ...)
    local rest = {...}
    return a, b, #rest
end
