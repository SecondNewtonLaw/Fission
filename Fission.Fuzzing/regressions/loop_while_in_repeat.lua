local events = {}
local function mark(tag, value)
    table.insert(events, tag)
    return value
end
local i = 0
repeat
    while mark("inner", i < 1) do
        i += 1
    end
    mark("tail", 0)
until mark("until", true)
return table.concat(events, ",")
