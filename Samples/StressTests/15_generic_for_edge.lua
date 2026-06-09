local function test_generic_for_multi_ret()
    local function multi_iter()
        return 1, 2, 3
    end
    local function wrap()
        return multi_iter, {}, 0
    end
    for a, b, c in wrap() do
        return a + b + c
    end
end

local function test_generic_for_with_break()
    local t = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}
    local acc = 0
    for i, v in ipairs(t) do
        if v > 5 then
            break
        end
        acc = acc + v
    end
    return acc
end

local function test_pairs_table_literal()
    local acc = 0
    for k, v in pairs({a = 1, b = 2, c = 3}) do
        acc = acc + v
    end
    return acc
end

local function test_generic_for_with_early_return(tbl, target)
    for i, v in pairs(tbl) do
        if v == target then
            return i
        end
    end
    return nil
end
