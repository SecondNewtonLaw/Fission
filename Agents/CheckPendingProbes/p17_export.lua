export local x = 5
export local y = 10
export function add(a, b)
    return a + b + x
end
y = add(y, 1)
