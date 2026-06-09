local function test_numeric_for_with_break()
    local acc = 0
    for i = 1, 100 do
        if i > 50 then
            break
        end
        acc = acc + i
    end
    return acc
end

local function test_numeric_for_with_if_else_break()
    local odds = 0
    local evens = 0
    for i = 1, 100 do
        if i % 2 == 0 then
            evens = evens + 1
            if evens > 25 then
                break
            end
        else
            odds = odds + 1
        end
    end
    return odds, evens
end

local function test_numeric_for_with_continue_pattern()
    local sum = 0
    for i = 1, 20 do
        if i % 3 == 0 then
            -- skip multiples of 3
        else
            sum = sum + i
        end
    end
    return sum
end

local function test_for_with_inline_function_call()
    local function double(x) return x * 2 end
    local acc = 0
    for i = 1, double(10) do
        acc = acc + i
    end
    return acc
end
