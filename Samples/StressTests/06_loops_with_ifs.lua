local function test_for_with_if()
    local t = {}
    for i = 1, 20 do
        if i % 2 == 0 then
            t[#t + 1] = i
        end
    end
    return t
end

local function test_while_with_if_elseif()
    local x = 0
    local cat = {}
    while x < 10 do
        if x < 3 then
            cat[#cat + 1] = "low"
        elseif x < 7 then
            cat[#cat + 1] = "mid"
        else
            cat[#cat + 1] = "high"
        end
        x = x + 1
    end
    return cat
end

local function test_for_break_early()
    local acc = 0
    for i = 1, 100 do
        if i > 50 then
            break
        end
        acc = acc + i
    end
    return acc
end

local function test_repeat_until_with_if()
    local x = 1
    local found = false
    repeat
        if x % 7 == 0 then
            found = true
        end
        x = x + 1
    until found or x > 100
    return x - 1
end
