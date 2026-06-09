local function test_nested_repeat_until()
    local x = 0
    local y = 0
    repeat
        x = x + 1
        repeat
            y = y + 1
        until y >= x
    until x >= 5
    return x, y
end

local function test_break_in_nested_loops()
    local acc = 0
    for i = 1, 10 do
        local j = 0
        while j < 10 do
            j = j + 1
            if i * j > 50 then
                break
            end
            if i == j then
                break
            end
            acc = acc + 1
        end
    end
    return acc
end

local function test_multiple_breaks_in_while()
    local x = 0
    local y = 0
    while x < 100 do
        x = x + 1
        if x > 50 then
            break
        end
        y = y + x
        if y > 100 then
            break
        end
    end
    return x, y
end

local function test_numeric_for_complex_bounds()
    local t = {1, 2, 3, 4, 5}
    local acc = 0
    for i = #t, 1, -1 do
        acc = acc + t[i]
    end
    return acc
end
