--!optimize 0
local count = 12
count = count + 1
local label = "rat"
print(label)
local enabled = true
if enabled then
    print(count)
end

--[[
    sample results in 'if not true then return end' on opt 0, further up it gets optimized away.
]]