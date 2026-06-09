local function test_if_in_numeric_for_return()
    local t = {}
    for i = 1, 10 do
        if i == 5 then
            return t
        end
        t[i] = i
    end
    return t
end

local function test_if_else_in_while()
    local x = 0
    local acc = 0
    while x < 10 do
        x = x + 1
        if x < 5 then
            acc = acc + x
        else
            acc = acc - x
        end
    end
    return acc
end

local function test_while_with_nested_if_break_continue()
    local x = 0
    local y = 0
    while x < 20 do
        x = x + 1
        if x % 2 == 0 then
            x = x + 1
        end
        if x > 15 then
            break
        end
        y = y + x
    end
    return x, y
end

local function test_repeat_until_with_if_else()
    local x = 0
    local acc = 0
    repeat
        x = x + 1
        if x % 2 == 0 then
            acc = acc + x
        else
            acc = acc - x
        end
    until x >= 10
    return acc
end
