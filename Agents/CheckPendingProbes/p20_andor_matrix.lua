local F = {}
F[#F + 1] = function(a, b, c, d) return (a or b) and c end
F[#F + 1] = function(a, b, c, d) return (a or b) and c or d end
F[#F + 1] = function(a, b, c, d) return (a and b or c) and d end
F[#F + 1] = function(a, b, c, d) return (a or b) and (c and d) end
F[#F + 1] = function(a, b, c, d) return (a and b) or (c and d) end
F[#F + 1] = function(a, b, c, d) return (a or b) or (c and d) end
F[#F + 1] = function(a, b, c, d) return a and (b or c) and d end
F[#F + 1] = function(a, b, c, d) return a or (b and c) or d end
F[#F + 1] = function(a, b, c, d) return (a or (b and c)) and d end
F[#F + 1] = function(a, b, c, d) return ((a or b) and c) or d end
F[#F + 1] = function(a, b, c, d) return not (a or b) and c end
F[#F + 1] = function(a, b, c, d) return (not a or b) and c end
F[#F + 1] = function(a, b, c, d) return (a or not b) and c end
F[#F + 1] = function(a, b, c, d) return (a == b or c) and d end
F[#F + 1] = function(a, b, c, d) return (a ~= nil or b) and c end
F[#F + 1] = function(a, b, c, d) return (a and b == c) or d end
F[#F + 1] = function(a, b, c, d) return (a == b) and (c or d) end
F[#F + 1] = function(a, b, c, d) return a and b and c and d end
F[#F + 1] = function(a, b, c, d) return a or b or c or d end
F[#F + 1] = function(a, b, c, d) return (a or b) and (c or d) or a end
F[#F + 1] = function(a, b, c, d) local v = (a or b) and c v = v or d return v end
F[#F + 1] = function(a, b, c, d) local v = (a and b) or c return v, d end
F[#F + 1] = function(a, b, c, d) if (a or b) and c then return 1 elseif d then return 2 end return 3 end
F[#F + 1] = function(a, b, c, d) local t = { (a or b) and c } return t[1] end
F[#F + 1] = function(a, b, c, d) local t = { k = (a or b) and c } return t.k end
F[#F + 1] = function(a, b, c, d) G = (a or b) and c return G end
F[#F + 1] = function(a, b, c, d) local u local function s() u = (a or b) and c end s() return u end
F[#F + 1] = function(a, b, c, d) local x = 0 while (a or b) and c and x < 1 do x += 1 end return x end
F[#F + 1] = function(a, b, c, d) return tostring((a or b) and c) end
F[#F + 1] = function(a, b, c, d) a = (a or b) and c return a end
F[#F + 1] = function(a, b, c, d) return if (a or b) then c else d end
F[#F + 1] = function(a, b, c, d) return (if a then b else c) and d end
F[#F + 1] = function(a, b, c, d) return (a or b) and c, (a or c) and d end
F[#F + 1] = function(a, b, c, d) return ((a or b) or c) and d end
F[#F + 1] = function(a, b, c, d) return (a or (b or c)) and d end
F[#F + 1] = function(a, b, c, d) return (a and b) and (c or d) end
F[#F + 1] = function(a, b, c, d) return a and (b or c) end
F[#F + 1] = function(a, b, c, d) return (a or 1) and b end
F[#F + 1] = function(a, b, c, d) return (a or b) and 5 end
F[#F + 1] = function(a, b, c, d) return (a or "k") and (b or "m") end
local V = { false, true, nil, 0 }
for n = 1, #F do
    local row = {}
    for p = 1, 4 do for q = 1, 4 do for r = 1, 4 do
        local s = (p + q + r) % 4 + 1
        local v1, v2 = F[n](V[p], V[q], V[r], V[s])
        row[#row + 1] = tostring(v1) .. (v2 ~= nil and ("/" .. tostring(v2)) or "")
    end end end
    print(n, table.concat(row, " "))
end
