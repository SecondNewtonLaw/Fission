local out
local function L(s) out[#out + 1] = tostring(s) end
local F = {}
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") return "r" end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") continue end L("a") elseif b then L("b") break end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") return "r" end L("a") elseif b then L("b") break end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") break end L("a") elseif b then L("b") return "q" end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") continue end L("a") elseif b then L("b") return "q" end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") break end L("a") else L("n") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") break end L("a") elseif b then L("b") elseif i == 2 then continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) local i = 0 while i < 3 do i += 1 if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) local i = 0 repeat i += 1 if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. i) until i >= 3 return "e" end
F[#F + 1] = function(a, b) if a then if b then L("x") return "r" end L("a") elseif b then L("b") return "q" end L("t") return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then L("a") if b then L("x") break end elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a then if b then L("x") break end if i == 2 then continue end L("a") elseif b then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for i = 1, 3 do if a and i > 1 then if b then L("x") break end L("a") elseif b or i == 3 then L("b") continue end L("t" .. i) end return "e" end
F[#F + 1] = function(a, b) for _, v in { 1, 2, 3 } do if a then if b then L("x") break end L("a") elseif b then L("b") continue end L("t" .. v) end return "e" end
local V = { false, true, nil, 0 }
for n = 1, #F do
    out = {}
    for p = 1, 4 do for q = 1, 4 do
        L("=" .. tostring(F[n](V[p], V[q])))
    end end
    print(n, table.concat(out, " "))
end
