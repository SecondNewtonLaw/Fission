local function mark(value)
    print(value)
    return value
end

local count = 0
repeat
    repeat
        count += 1
    until mark(count >= 2)
    count += 1
until if mark(false) then false else mark(count >= 5)

print(count)
return count
