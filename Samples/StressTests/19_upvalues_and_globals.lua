local global_var = 0

local function test_upvalue_write()
    local counter = 0
    local function increment()
        counter = counter + 1
        return counter
    end
    return increment, increment, increment
end

local function test_shared_upvalue()
    local shared = 0
    local function get()
        return shared
    end
    local function set(val)
        shared = val
    end
    set(42)
    return get()
end

local function test_upvalue_in_closure_returning_closure(x)
    local function make_adder(n)
        return function(m)
            return n + m
        end
    end
    local add5 = make_adder(5)
    local add10 = make_adder(10)
    return add5(x) + add10(x)
end

local function test_global_read_write()
    GLOBAL_TEST = 42
    return GLOBAL_TEST
end
