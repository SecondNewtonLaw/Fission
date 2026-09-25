local function make()
    return 7
end
export local value = make()
export function get()
    return value
end
value = get() + 1
