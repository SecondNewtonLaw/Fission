local function test_concat_chain(a, b, c, d)
    return a .. b .. c .. d
end

local function test_complex_arithmetic(x, y, z)
    return (x + y) * z - (x / y) + (x % y)
end

local function test_mixed_comparisons(a, b, c)
    if a < b and b < c or a > c then
        return "complex"
    elseif a == b or b == c or a == c then
        return "equal"
    end
    return "default"
end

local function test_unary_not_in_expr(x, y)
    if not (x and y) then
        return "not both"
    end
    if not x or not y then
        return "not one"
    end
    return "both"
end

local function test_nested_ternary_style(x, y, z)
    local result = (x and y) or (z and 42) or "fallback"
    return result
end
