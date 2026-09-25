-- run with Fission.CLI --decompile-test <this> (AutoNameVariables), then execute the emitted source
local folder = { FindFirstChild = function(self, c) return { Name = c } end, WaitForChild = function(self, c) return { Name = c } end }
script = { Name = "real-script" }
workspace = { Name = "real-ws" }
local function f()
    local a = folder:FindFirstChild("script")
    local b = folder:WaitForChild("print")
    local c = folder:FindFirstChild("workspace")
    print(a.Name, b.Name, c.Name, script.Name, workspace.Name)
end
f()
local function h()
    local s = folder:FindFirstChild("tostring")
    return s.Name, tostring(1)
end
print(h())
