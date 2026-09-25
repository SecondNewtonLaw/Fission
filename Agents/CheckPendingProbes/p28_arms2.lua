local out
local function L(s) out[#out + 1] = tostring(s) end
local F = {}
-- first arm has nested terminator + stmt; second arm varies; third arm varies
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") else L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") continue else L("c") end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") continue else L("c") break end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end L("a") elseif b then L("b") return "r" else L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") return "x" end L("a") elseif b then L("b") continue else L("c") end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") continue end L("a") elseif b then L("b") break else L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then L("a") elseif b then if c then L("x") break end L("b") else L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then L("a") continue elseif b then if c then L("x") break end L("b") end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then L("a") elseif b then L("b") else if c then L("x") break end L("c") end if i == 2 then continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break end elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then break end L("a") elseif b then continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if c then L("x") break else L("y") end elseif b then L("b") continue elseif c then L("c") break end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then while c do L("w") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then for j = 1, 2 do if c then break end L("j" .. j) end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) local r = 0 for i = 1, 3 do if a then if c then r = -1 break end r += 1 elseif b then r += 10 continue end r += 100 end return r end
F[#F + 1] = function(a, b, c) for i = 1, 3 do if a then if b then if c then L("abc") break end L("ab") end L("a") elseif c then L("c") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b, c) if a then if c then L("x") return "x" end L("a") elseif b then L("b") return "b" end L("t") return "e" end
F[#F + 1] = function(a, b, c) local i = 0 while true do i += 1 if i > 3 then break end if a then if c then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
local V = { false, true, nil }
for n = 1, #F do
    out = {}
    for p = 1, 3 do for q = 1, 3 do for r = 1, 3 do
        L("=" .. tostring(F[n](V[p], V[q], V[r])))
    end end end
    print(n, table.concat(out, " "))
end
