local function test_numeric_for()
    local sum = 0
    for i = 1, 10 do
        sum = sum + i
    end
    return sum
end

local function test_numeric_for_step()
    local sum = 0
    for i = 0, 100, 5 do
        sum = sum + i
    end
    return sum
end

local function test_nested_numeric_for()
    local prod = 0
    for i = 1, 5 do
        for j = 1, 5 do
            prod = prod + (i * j)
        end
    end
    return prod
end

local function test_numeric_for_down()
    local acc = 0
    for i = 10, 1, -1 do
        acc = acc + i
    end
    return acc
end
