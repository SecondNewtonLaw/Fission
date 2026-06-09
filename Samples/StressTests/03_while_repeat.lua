local function test_while_basic()
    local x = 0
    while x < 10 do
        x = x + 1
    end
    return x
end

local function test_while_complex_cond()
    local x = 0
    local y = 10
    while x < y and y > 0 do
        x = x + 1
        y = y - 1
    end
    return x, y
end

local function test_repeat_basic()
    local x = 0
    repeat
        x = x + 1
    until x >= 10
    return x
end

local function test_repeat_with_cond()
    local x = 10
    local acc = 0
    repeat
        acc = acc + x
        x = x - 1
    until x <= 0
    return acc
end

local function test_while_infinite_guard()
    local x = 0
    while true do
        x = x + 1
        if x >= 100 then
            break
        end
    end
    return x
end
