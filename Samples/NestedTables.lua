local v0 = Instance.new("Completely Legitimate Roblox Instance!")
local v1 = UDim2.new(0, 0, 0, 0)
v0.Rat = v1

local v = 2
local nestedTable
nestedTable = { TestGlobalOrImport, {"Hello!", bye = "bye", [2] = 2}, { 1, 2, 3 }, { v0, v1, nestedTable} }

print(nestedTable)
f(nestedTable)
local g = rat()
local t = { nestedTable }
f(g, t)
