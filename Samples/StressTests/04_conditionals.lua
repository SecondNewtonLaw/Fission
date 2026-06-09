local function test_if_elseif_chain(x)
    if x < 10 then
        return "small"
    elseif x < 50 then
        return "medium"
    elseif x < 100 then
        return "large"
    else
        return "huge"
    end
end

local function test_nested_ifs(a, b, c)
    if a then
        if b then
            if c then
                return "all true"
            else
                return "a and b true, c false"
            end
        else
            return "a true, b false"
        end
    else
        return "a false"
    end
end

local function test_if_return_early(x)
    if x < 0 then
        return -1
    end
    if x == 0 then
        return 0
    end
    return 1
end
