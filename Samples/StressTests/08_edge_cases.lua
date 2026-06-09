local function test_return_in_nested_if(x, y)
    if x then
        if y then
            return "both"
        end
        return "only x"
    end
    return "neither"
end

local function test_or_fallback(a, b, c)
    local v = a or b or c
    if v then
        return v
    end
    return "fallback"
end

local function test_and_or_in_loop()
    local found = false
    local i = 0
    while not found do
        i = i + 1
        found = i > 10 or (i % 3 == 0)
    end
    return i
end

local function test_conditional_chain_in_for()
    local results = {}
    for i = 1, 30 do
        local label = ""
        if i % 15 == 0 then
            label = "fizzbuzz"
        elseif i % 3 == 0 then
            label = "fizz"
        elseif i % 5 == 0 then
            label = "buzz"
        else
            label = tostring(i)
        end
        results[i] = label
    end
    return results
end
