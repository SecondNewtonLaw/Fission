local F = {}
F[#F + 1] = function(a, b, c, d) return (if a then b else c) or d end
F[#F + 1] = function(a, b, c, d) return (a or b) and (if c then d else a) end
F[#F + 1] = function(a, b, c, d) return if a or b then c else d end
F[#F + 1] = function(a, b, c, d) return if (a or b) and c then d else a end
F[#F + 1] = function(a, b, c, d) return if a then (b or c) and d else b end
F[#F + 1] = function(a, b, c, d) return a == b or c == d end
F[#F + 1] = function(a, b, c, d) return a == b and c == d end
F[#F + 1] = function(a, b, c, d) return (a == b or c) == d end
F[#F + 1] = function(a, b, c, d) return a ~= b and c or d end
F[#F + 1] = function(a, b, c, d) return (a == nil) or (b == false) and c end
F[#F + 1] = function(a, b, c, d) return not a == not b end
F[#F + 1] = function(a, b, c, d) return (not a) and (not b) or c end
F[#F + 1] = function(a, b, c, d) return a and not b or not c and d end
F[#F + 1] = function(a, b, c, d) return if a then b elseif c then d else nil end
F[#F + 1] = function(a, b, c, d) return if not a then b elseif not c then d else a end
F[#F + 1] = function(a, b, c, d) local v = if a then b else c return v or d end
F[#F + 1] = function(a, b, c, d) local v = a == b return v, (c == d) or a end
F[#F + 1] = function(a, b, c, d) return (a == true or b == "s") and c end
F[#F + 1] = function(a, b, c, d) return (a == 1 or b == 2) and (c == 3 or d) end
F[#F + 1] = function(a, b, c, d) return ((a or b) and c or d) and a end
F[#F + 1] = function(a, b, c, d) return (a and b or c) or d end
F[#F + 1] = function(a, b, c, d) return a and b or c and d or a end
F[#F + 1] = function(a, b, c, d) return (a or b) and c and d end
F[#F + 1] = function(a, b, c, d) return a and (b or c) or d end
local V = { false, true, nil, 0 }
for n = 1, #F do
    local row = {}
    for p = 1, 4 do for q = 1, 4 do for r = 1, 4 do
        local s = (p * r + q) % 4 + 1
        local v1, v2 = F[n](V[p], V[q], V[r], V[s])
        row[#row + 1] = tostring(v1) .. (v2 ~= nil and ("/" .. tostring(v2)) or "")
    end end end
    print(n, table.concat(row, " "))
end
