local function test_method_call()
    local t = {value = 42}
    function t:getValue()
        return self.value
    end
    return t:getValue()
end

local function test_mixed_table_ctor()
    local t = {a = 1, [2] = "two", "three", 4}
    return t
end

local function test_nested_table()
    local t = {
        inner = {
            value = 10
        },
        other = {
            {x = 1, y = 2},
            {x = 3, y = 4}
        }
    }
    return t.inner.value + t.other[1].x
end

local function test_table_index_assign()
    local t = {}
    for i = 1, 10 do
        t[i] = i * i
    end
    return t
end
