local t = {
    ["a\nb"] = function()
        return nil
    end,
    function()
        return pairs
    end,
    nil,
    { x = 1 },
}
return #t
