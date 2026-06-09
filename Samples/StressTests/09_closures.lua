local function test_closure_upvalues(x)
    local a = x + 1
    local function inner()
        return a + 1
    end
    return inner()
end

local function test_closure_in_loop()
    local funcs = {}
    for i = 1, 10 do
        funcs[i] = function()
            return i
        end
    end
    return funcs[1]()
end

local function test_closure_multiple_upvalues()
    local a = 1
    local b = 2
    local c = 3
    local function inner()
        return a + b + c
    end
    return inner()
end

local function test_nested_closures(x)
    local function outer(y)
        local function inner(z)
            return x + y + z
        end
        return inner
    end
    return outer(2)(3)
end
