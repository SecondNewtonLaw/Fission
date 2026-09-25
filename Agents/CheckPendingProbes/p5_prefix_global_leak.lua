v0, v1, v2 = 1, 2, 3
local function g(x) local y = x .. "!" return y end
print(g("a"), v0, v1, v2)
do local m = { 1 } end
print(type(rawget(_G, "_v0")), type(rawget(_G, "_v1")), type(rawget(_G, "_v2")))
