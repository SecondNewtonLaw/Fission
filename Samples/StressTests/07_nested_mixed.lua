local function test_complex_nesting()
    local matrix = {}
    for i = 1, 5 do
        local row = {}
        for j = 1, 5 do
            if i == j then
                row[j] = 1
            elseif i < j then
                row[j] = i + j
            else
                row[j] = i * j
            end
        end
        matrix[i] = row
    end
    return matrix
end

local function test_loop_with_early_return(tbl, target)
    for i, v in ipairs(tbl) do
        if v == target then
            return i
        end
    end
    return -1
end

local function test_while_true_with_complex_inner()
    local x = 0
    local y = 0
    while true do
        x = x + 1
        if x > 100 then
            break
        end
        if x % 2 == 0 then
            y = y + x
        end
        if y > 500 then
            break
        end
    end
    return x, y
end

local function test_repeat_until_table(t)
    local i = 1
    local acc = 0
    repeat
        acc = acc + t[i]
        i = i + 1
    until t[i] == nil
    return acc
end
