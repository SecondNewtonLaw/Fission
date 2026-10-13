-- Fuzz finding (seed 7, SEM_DIVERGE_t3_1, minimized): a loop-carried store to a
-- captured register whose declaration was never emitted (the if-expression's
-- then-value is multret-empty) declared a shadowing local per scope instead of
-- assigning the capture's binding. Later reads observed nil instead of true.
local v0 = (if (if math.field then (-(-"x")) else (obj)) then 1 else tostring.field.field);
repeat
    v0 = true
until {};
local function f2(...)
    local w = v0;
    return "key";
end
return v0
