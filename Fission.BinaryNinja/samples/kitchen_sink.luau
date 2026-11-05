-- Kitchen-sink Luau sample for the Fission Binary Ninja plugin.
-- Every value printed here is deterministic so the same file can be executed
-- from source and from compiled bytecode as an execution-parity oracle.

local lines = {}

local function emit(...)
	local parts = table.create(select("#", ...))
	for i = 1, select("#", ...) do
		local value = select(i, ...)
		parts[i] = tostring(value)
	end
	lines[#lines + 1] = table.concat(parts, "\t")
end

------------------------------------------------------------------------
-- Scalars, arithmetic, strings
------------------------------------------------------------------------
local a, b = 17, 5
emit("arithmetic", a + b, a - b, a * b, a / b, a % b, a ^ 2, a // b)

local negative = -a
emit("unary", negative, -negative, not true, not nil, not 0)

local text = "Hello, " .. "Luau" .. "!"
emit("string", text, #text, string.upper(text), string.sub(text, 1, 5), string.rep("ab", 3))
emit("format", string.format("%04d/%s/%.2f", 42, "ok", 3.14159))
emit("bytes", string.byte("ABC", 1, 3))
emit("interp", `value={a} flag={b > 3}`)

local escaped = "quote=\" slash=\\ tab=\t newline=\n"
emit("escaped", #escaped)

------------------------------------------------------------------------
-- Tables and iteration
------------------------------------------------------------------------
local array = { 3, 1, 4, 1, 5, 9, 2, 6 }
table.insert(array, 5)
table.insert(array, 3, 8)
table.remove(array, 2)
table.sort(array)
emit("array", #array, table.concat(array, ","))

local dictionary = {
	alpha = 1,
	beta = 2,
	gamma = 3,
	[10] = "ten",
}
dictionary.delta = a + b
emit("dict", dictionary.alpha + dictionary.beta + dictionary.gamma, dictionary.delta, dictionary[10])

local keyCount, valueSum = 0, 0
for key, value in pairs(dictionary) do
	keyCount += 1
	if type(value) == "number" then
		valueSum += value
	end
end
emit("pairs", keyCount, valueSum)

local ipairsSum = 0
for index, value in ipairs(array) do
	ipairsSum += index * value
end
emit("ipairs", ipairsSum)

local generalized = 0
for _, value in array do
	generalized += value
end
emit("generalized", generalized)

------------------------------------------------------------------------
-- Control flow
------------------------------------------------------------------------
local function classify(n)
	if n < 0 then
		return "negative"
	elseif n == 0 then
		return "zero"
	elseif n < 10 then
		return "small"
	else
		return "large"
	end
end
emit("classify", classify(-4), classify(0), classify(7), classify(40))

local label = if a > b then "a>b" else "a<=b"
emit("ifexpr", label)

local whileSum, whileIndex = 0, 1
while whileIndex <= 10 do
	whileSum += whileIndex
	whileIndex += 1
end
emit("while", whileSum, whileIndex)

local repeatIndex = 0
repeat
	repeatIndex += 2
until repeatIndex >= 9
emit("repeat", repeatIndex)

local steps = {}
for i = 10, 1, -3 do
	steps[#steps + 1] = i
end
local continueSum = 0
for i = 1, 10 do
	if i % 3 == 0 then
		continue
	end
	continueSum += i
	if i == 8 then
		break
	end
end
emit("loops", table.concat(steps, ","), continueSum)

------------------------------------------------------------------------
-- Functions, varargs, multiple returns, closures
------------------------------------------------------------------------
local function fib(n)
	if n < 2 then
		return n
	end
	return fib(n - 1) + fib(n - 2)
end
emit("fib", fib(12))

local function swap(x, y)
	return y, x
end
local swappedA, swappedB = swap(a, b)
emit("swap", swappedA, swappedB)

local function collect(...)
	return select("#", ...), ...
end
local count, first, second = collect("x", "y", "z")
emit("varargs", count, first, second)

local function makeCounter(start)
	local value = start
	return function(step)
		value += step or 1
		return value
	end
end
local counter = makeCounter(10)
counter(2)
counter(3)
emit("closure", counter())

local function makeAccumulators()
	local totals = { count = 0 }
	local function add(n)
		totals.count += n
		return totals.count
	end
	local function reset()
		totals.count = 0
	end
	return add, reset
end
local add, reset = makeAccumulators()
add(4)
add(6)
local afterReset = add(0)
reset()
emit("upvalues", afterReset, add(1))

------------------------------------------------------------------------
-- Methods and metatables
------------------------------------------------------------------------
local Vector = {}
Vector.__index = Vector

function Vector.new(x, y)
	return setmetatable({ x = x, y = y }, Vector)
end

function Vector:length()
	return math.sqrt(self.x * self.x + self.y * self.y)
end

function Vector:add(other)
	return Vector.new(self.x + other.x, self.y + other.y)
end

Vector.__add = Vector.add
Vector.__tostring = function(self)
	return string.format("(%g, %g)", self.x, self.y)
end
Vector.__eq = function(lhs, rhs)
	return lhs.x == rhs.x and lhs.y == rhs.y
end

local v1, v2 = Vector.new(3, 4), Vector.new(1, 2)
local v3 = v1 + v2
emit("vector", tostring(v1), v1:length(), tostring(v3), tostring(v1 + v1), v1 == v3, v1 == Vector.new(3, 4))

local raw = { nested = { value = 99 } }
emit("rawget", rawget(raw, "nested").value, rawlen({ 1, 2, 3 }), rawequal(raw, raw))

------------------------------------------------------------------------
-- Errors, protected calls, coroutines
------------------------------------------------------------------------
local ok, message = pcall(function()
	error("intentional failure")
end)
emit("pcall", ok, type(message) == "string" and string.find(message, "intentional failure") ~= nil)

local ok2, result = pcall(function(left, right)
	return left * right
end, 6, 7)
emit("pcall2", ok2, result)

local handlerSeen = false
local ok3 = xpcall(function()
	error("covered")
end, function(err)
	handlerSeen = string.find(err, "covered") ~= nil
	return "handled"
end)
emit("xpcall", ok3, handlerSeen)

local co = coroutine.create(function(prefix)
	local value = coroutine.yield(prefix .. "-one")
	coroutine.yield(value .. "-two")
	return "done"
end)
local started, yielded1 = coroutine.resume(co, "start")
local resumed, yielded2 = coroutine.resume(co, "middle")
local finished, finalValue = coroutine.resume(co)
emit("coroutine", started, yielded1, resumed, yielded2, finished, finalValue, coroutine.status(co))

------------------------------------------------------------------------
-- Builtin libraries and fast paths
------------------------------------------------------------------------
emit("math", math.floor(3.9), math.ceil(3.1), math.abs(-7), math.max(1, 9, 4), math.min(1, 9, 4))
emit("bit32", bit32.band(0xF0F0, 0x0FF0), bit32.bor(0x00F0, 0x0F00), bit32.bxor(0xAAAA, 0xFFFF), bit32.lshift(1, 8), bit32.rshift(256, 4))
emit("types", type(1), type("x"), type(true), type({}), type(print), typeof(nil), tostring(nil), tonumber("42"), tonumber("no"))

local buf = buffer.create(16)
buffer.writeu32(buf, 0, 0xDEADBEEF)
buffer.writef32(buf, 4, 1.5)
emit("buffer", buffer.readu32(buf, 0), buffer.readf32(buf, 4), buffer.len(buf))

local vec = vector.create(1, 2, 3)
emit("vectorlib", vector.magnitude(vec), vector.dot(vec, vec), vector.abs(vector.create(-1, 2, -3)))

local utf8Text = "héllo"
emit("utf8", utf8.len(utf8Text), #utf8Text, utf8.char(0x41, 0x42))

local frozen = table.freeze({ x = 1 })
emit("freeze", frozen.x, table.isfrozen(frozen))

------------------------------------------------------------------------
-- Larger numeric loop for a real CFG shape
------------------------------------------------------------------------
local total = 0
for i = 1, 200 do
	if i % 7 == 0 then
		total += i * 2
	elseif i % 5 == 0 then
		total -= i
	else
		total += i % 3
	end
end
emit("numeric", total)

print(table.concat(lines, "\n"))
print("digest", #lines, #table.concat(lines, "|"))

