local function test_string_upper(s)
    return string.upper(s)
end

local function test_string_sub(s, i, j)
    return string.sub(s, i, j)
end

local function test_string_format(fmt, a, b)
    return string.format(fmt, a, b)
end

local function test_string_chain(s)
    return string.lower(string.reverse(string.upper(s)))
end

local function test_string_len(s)
    return #s
end

local function test_string_byte_char(c)
    local b = string.byte(c)
    return string.char(b + 1)
end
