local out = {}
local function L(s) out[#out + 1] = s end
local function f(b) local x = 1 if b then x = x + 10 end while x > 5 do x -= 4 L(x) end return x end
print(f(true), f(false), table.concat(out, ","))
