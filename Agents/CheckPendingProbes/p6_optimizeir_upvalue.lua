-- run with Fission.CLI --decompile-test <this> --opt 1 --optimize-ir, then execute the emitted source
local a = 0
local function set() a = 5 end
set()
print(a)
