-- Exercises Fission's renamers that name call results.
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local Workspace = game:GetService("Workspace")

local part = Instance.new("Part")
local humanoid = Workspace:FindFirstChild("Humanoid")
local module = script:WaitForChild("Module")

local health = humanoid:GetAttribute("Health")
local vector = Vector3.new(1, 2, 3)
local color = Color3.fromRGB(255, 128, 0)
local firstPlayer = Players:GetPlayers()[1]

local ok, result = pcall(function()
    return health + vector.X
end)

local thread = coroutine.create(function(value)
    return value + 1
end)

local started, yielded = coroutine.resume(thread, firstPlayer)

return part, module, color, ok, result, started, yielded, RunService
