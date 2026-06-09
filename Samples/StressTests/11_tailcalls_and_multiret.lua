local function helper(x)
    return x * 2
end

local function test_tail_call(x)
    return helper(x)
end

local function test_multi_return()
    return 1, 2, 3
end

local function test_multi_assign()
    local a, b, c = test_multi_return()
    return a + b + c
end

local function test_multi_return_call()
    local function get_coords()
        return 10, 20, 30
    end
    local x, y, z = get_coords()
    return x + y + z
end

local function test_conditional_tail_call(x)
    if x > 0 then
        return helper(x)
    end
    return 0
end
