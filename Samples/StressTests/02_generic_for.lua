local function test_generic_for_pairs()
    local t = {a = 1, b = 2, c = 3}
    local acc = 0
    for k, v in pairs(t) do
        acc = acc + v
    end
    return acc
end

local function test_generic_for_ipairs()
    local t = {10, 20, 30}
    local acc = 0
    for i, v in ipairs(t) do
        acc = acc + v
    end
    return acc
end

local function test_custom_iterator()
    local function custom_iter(tbl, idx)
        idx = idx + 1
        if tbl[idx] ~= nil then
            return idx, tbl[idx]
        end
    end
    local function wrap(tbl)
        return custom_iter, tbl, 0
    end
    local t = {1, 2, 3}
    local acc = 0
    for i, v in wrap(t) do
        acc = acc + v
    end
    return acc
end
