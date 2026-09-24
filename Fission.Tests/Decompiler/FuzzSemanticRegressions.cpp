//
// Created by Dottik on 21/9/2026.
//

#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cctype>
#include <cstring>
#include <string>

static void EnableLuauFFlagsOnce() {
    static bool enabled = false;
    if (enabled)
        return;
    enabled = true;
    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
}

// Luau::compile encodes a compile error as a buffer whose first byte is 0 followed by the message.
static bool Recompiles(const std::string &source, std::string *errorOut) {
    Luau::CompileOptions opts{};
    opts.optimizationLevel = 1;
    opts.debugLevel = 2;
    const std::string bc = Luau::compile(source, opts);
    const bool ok = !bc.empty() && bc[0] != '\0';
    if (!ok && errorOut)
        *errorOut = bc.size() > 1 ? bc.substr(1) : "(empty)";
    return ok;
}

// True if the output contains an `anon_<n>_<n> = ...` bare assignment -- the closure-leak shape where a
// phi-consumed branch closure is written to an undeclared global instead of the merge-target local.
static bool HasAnonBareAssign(const std::string &s) {
    for (size_t p = s.find("anon_"); p != std::string::npos; p = s.find("anon_", p + 1)) {
        size_t q = p + 5;
        while (q < s.size() && (std::isdigit(static_cast<unsigned char>(s[q])) || s[q] == '_'))
            ++q;
        while (q < s.size() && s[q] == ' ')
            ++q;
        if (q < s.size() && s[q] == '=' && (q + 1 >= s.size() || s[q + 1] != '=')) // assignment, not `==`
            return true;
    }
    return false;
}

TEST_CASE("Regress fuzz: materialized numeric loop tables evaluate once", "[Decompiler][FuzzRegress][LoopTable]") {
    EnableLuauFFlagsOnce();
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    std::string header;
    SECTION("t3_1 table step after conditional limit") { header = "..., (if function() end then false else nil), {y = data.y, data[1]}"; }
    SECTION("table start before conditional limit") { header = "{y = data.y, data[1]}, (if function() end then 2 else 3), ..."; }
    SECTION("table limit after conditional start") { header = "(if function() end then 1 else 2), {y = data.y, data[1]}, ..."; }
    SECTION("table step is the first invalid bound") { header = "..., (if function() end then 2 else 3), {y = data.y, data[1]}"; }
    const std::string source = R"LUA(
local data = setmetatable({}, {__index = function(_, key)
    print("read", key)
    return 1
end})
local function run(...)
    for i = )LUA" + header + R"LUA( do
    end
end
local ok = pcall(run, 1)
return ok
)LUA";
    const Luau::CompileOptions options{optimization, debug};
    const auto bytecode = Luau::compile(source, options);
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode.front() != '\0');
    Decompiler decompiler;
    const auto result = decompiler.DecompileVanillaBytecode(bytecode);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    const auto compiled = Luau::compile(result.decompilationOutput, options);
    REQUIRE(!compiled.empty());
    REQUIRE(compiled.front() != '\0');
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(compiled, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == "\"read\"\t\"y\"\n\"read\"\t1\nreturn: false\n");
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Regress fuzz: adjacent loop boundaries and bindings", "[Decompiler][FuzzRegress][AdjacentLoops]") {
    EnableLuauFFlagsOnce();
    const Luau::CompileOptions options{GENERATE(0, 1, 2), GENERATE(0, 2)};
    std::string source;
    std::string expectedTrace;
    SECTION("while exit is another loop header") {
        expectedTrace = "error: attempt to index function with 'field'\n";
        source = R"LUA(
while {print} do
    local f = tonumber.field
    f(f)
end
while (if -math then 344 else ipairs(nil)) do
    print()
end
)LUA";
    }
    SECTION("generic loop follows captured loop variable") {
        expectedTrace = "1\n1\nreturn: 7\n";
        source = R"LUA(
local function iter() return next, {3}, nil end
for a, b in iter() do
    local function f() return a end
    print(f())
end
for c in iter() do
    print(c * c)
end
return 7
)LUA";
    }
    const auto bytecode = Luau::compile(source, options);
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode.front() != '\0');
    Decompiler decompiler;
    const auto result = decompiler.DecompileVanillaBytecode(bytecode);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    const auto compiled = Luau::compile(result.decompilationOutput, options);
    REQUIRE(!compiled.empty());
    REQUIRE(compiled.front() != '\0');
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(compiled, prelude);
    INFO("original: " << original.trace);
    INFO("reconstructed: " << reconstructed.trace);
    REQUIRE(original.trace == expectedTrace);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Regress fuzz: adjacent while loops keep return outside repeat", "[Decompiler][FuzzRegress][AdjacentLoops]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local v0 = (t.field).field
local v1, v2, v3 = nil, v0:set(true, "a-b"), (if pairs then tonumber else string)
local v4 = (v2[v3][{}] * math.field)
while v2.field:run(v4.field, 399) do
    local v5 = tostring[65.75][v1(false)]
    v4((if {k = string, x = 470, nil, true} then {v2, nil, k = ipairs} else obj(true, t)), function(p6, p7) return true, p7 end)
    ipairs -= print:get(true, obj)
    if ({data = v4} and true) then break end
end
(201.5)(t:run(117), v4.field)
next()
while v2.field[{data = "hello", data = string}][(...)][{(if t then t else pairs), [("hello")] = tonumber.field, ..., [{[nil] = 38.25}] = function(p5, p6)
    string()
    tonumber(next, t)
    v0(string)
    return
end}] do end
return #68.75
)LUA";
    Decompiler decompiler;
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO(result.decompilationOutput);
    std::string error;
    const bool recompiles = Recompiles(result.decompilationOutput, &error);
    INFO(error);
    CHECK(recompiles);
}

static size_t CountOccurrences(const std::string &haystack, const std::string &needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos; pos = haystack.find(needle, pos + needle.size()))
        ++n;
    return n;
}

// Count `local <name>` declarations of an EXACT register name (so `v1` does not match `v10`). A single
// register declared twice (once at an outer scope, once nested) is the phi shadowing bug: the nested
// `local` shadows the outer, so branch writes never reach the outer read.
static size_t CountLocalDeclsOf(const std::string &s, const std::string &name) {
    const std::string needle = "local " + name;
    size_t n = 0;
    for (size_t p = s.find(needle); p != std::string::npos; p = s.find(needle, p + needle.size())) {
        const size_t after = p + needle.size();
        if (after < s.size() && std::isdigit(static_cast<unsigned char>(s[after])))
            continue; // `local v1` must not match `local v10`
        ++n;
    }
    return n;
}

// A table constructor must inline each method call exactly once without spilling it to a local.
TEST_CASE("SETLIST method-call elements inline once, not double-emitted", "[Decompiler][Roundtrip][SetList]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local settings = {}
settings.include = {
    game:GetService("Workspace"),
    game:GetService("Players"),
    game:GetService("Lighting"),
    game:GetService("ReplicatedStorage"),
}
return settings
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // Each element call must appear exactly once.
    CHECK(CountOccurrences(result.decompilationOutput, "game:GetService(\"Workspace\")") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "game:GetService(\"Players\")") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "game:GetService(\"Lighting\")") == 1);
}

// A phi-consumed branch closure must bind to the merge target, never an anonymous global.
TEST_CASE("Closure as an if-expression branch binds to the local, not a bare anon global", "[Decompiler][Roundtrip][ClosurePhi]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local v0 = (if (string.field ~= tonumber) then function(p0, p1)
return p0
end else ipairs)
v0 = v0("hello")
return v0
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The branch closure must bind to the merged local, never leak as `anon_N = function`.
    CHECK_FALSE(HasAnonBareAssign(result.decompilationOutput));
}

// A nested short-circuit diamond sharing its outer merge must declare the phi register once.
TEST_CASE("and/or chain used after a later statement is not phi-shadowed", "[Decompiler][Roundtrip][PhiShadow]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function classify(n)
    local sign = n > 0 and "positive" or n < 0 and "negative" or "zero"
    local parity = n % 2 == 0 and "even" or "odd"
    return sign .. " " .. parity
end
return classify(-7)
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The `sign` register (v1) must be declared exactly once -- a second, nested `local v1` is the shadow.
    CHECK(CountLocalDeclsOf(result.decompilationOutput, "v1") == 1);
}

// `return (select(n, ...))`: the source parens truncate a multiret call to ONE value. The bytecode
// records the fixed return count, but the decompiler dropped the parens, emitting `return select(n,
// ...)` -- a bare tail call that spreads ALL of select's results, changing the returned arity (here
// "b" vs "b", "c"). The fix marks a truncated multiret call inlined as the last return value to render
// parenthesized. Durable invariant: the parens survive and the output recompiles.
TEST_CASE("truncated multiret call in return position keeps its adjust-to-one parens", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function nthOrLast(n, ...)
    local count = select("#", ...)
    if n > count then n = count end
    return (select(n, ...))
end
return nthOrLast(2, "a", "b", "c")
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The truncating parens must be preserved: `(select(` inside a return, not a bare `return select(`.
    CHECK(CountOccurrences(result.decompilationOutput, "(select(") >= 1);
    CHECK(CountOccurrences(result.decompilationOutput, "return select(") == 0);
}

TEST_CASE("truncated multiret call in last argument keeps its adjust-to-one parens", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    // `two()` spreads in the first join (last arg, multret) but is truncated to one value in the second
    // (`(two())`). A bare `join("-", two())` on the second would re-spread and pass "a","b" -> the
    // original returns "a-b","a", a naive decompile "a-b","a-b". The parens must survive on the second.
    const std::string source = R"LUA(
local function two() return "a","b" end
local function join(sep, ...) return table.concat({...}, sep) end
return join("-", two()), join("-", (two()))
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // Exactly the truncated call is parenthesized; the spread one stays bare.
    CHECK(CountOccurrences(result.decompilationOutput, "(two())") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "join(\"-\", two())") == 1);
}

TEST_CASE("fixed vararg passed to a rebound fast builtin stays one argument", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
_G.typeof = function(...) return select("#", ...) end
local function probe(...) return typeof((...)) end
print(probe(1, 2, 3))
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("fixed nested fast builtin result stays one final argument", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
math.floor = function() end
math.max = function(...) return select("#", ...) end
print(math.max(0, (math.floor(1))))
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("real FASTCALL1 typeof keeps a fixed-one vararg", "[Decompiler][Roundtrip][AdjustToOne][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function probe(...)
    return select("#", typeof((...)))
end
return probe(1, 2, 3)
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 1\n");
    CHECK(verdict.decompiled.trace == "return: 1\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("real FASTCALL2 math chain keeps its result arity", "[Decompiler][Roundtrip][AdjustToOne][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function probe(...)
    return math.max(2, math.floor((...))), 2
end
return probe(1, 0, 3)
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 2\t2\n");
    CHECK(verdict.decompiled.trace == "return: 2\t2\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("variadic SETLIST C=0 keeps a middle conditional phi", "[Decompiler][Roundtrip][SetList][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function f(a, b, c, flag, g)
    return { a, if flag then b else c, g() }
end
return f(1, 2, 3, true, function() return 4 end), f(1, 2, 3, false, function() return 5 end)
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: {1=1,2=2,3=4}\t{1=1,2=3,3=5}\n");
    CHECK(verdict.decompiled.trace == "return: {1=1,2=2,3=4}\t{1=1,2=3,3=5}\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("variadic SETLIST C=0 keeps nested call elements", "[Decompiler][Roundtrip][SetList][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
return ColorSequence.new({
    ColorSequenceKeypoint.new(0, Color3.fromRGB(1, 2, 3)),
    ColorSequenceKeypoint.new(1, Color3.fromRGB(4, 5, 6)),
    ColorSequenceKeypoint.new(2, Color3.fromRGB(7, 8, 9)),
    ColorSequenceKeypoint.new(3, Color3.fromRGB(10, 11, 12)),
    ColorSequenceKeypoint.new(4, Color3.fromRGB(13, 14, 15)),
    ColorSequenceKeypoint.new(5, Color3.fromRGB(16, 17, 18)),
    ColorSequenceKeypoint.new(6, Color3.fromRGB(19, 20, 21)),
    ColorSequenceKeypoint.new(7, Color3.fromRGB(22, 23, 24)),
    ColorSequenceKeypoint.new(8, Color3.fromRGB(25, 26, 27)),
    ColorSequenceKeypoint.new(9, Color3.fromRGB(28, 29, 30)),
})
)LUA";
    const std::string prelude = R"LUA(
Color3 = { fromRGB = function(r, g, b) return r + g + b end }
ColorSequenceKeypoint = { new = function(t, colorValue) return t * 100 + colorValue end }
ColorSequence = { new = function(values) return #values, values[1], values[10] end }
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile(prelude)});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 10\t6\t987\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("multi-result GETVARARGS preserves reversed locals", "[Decompiler][Roundtrip][Varargs][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function swap(...)
    local a, b = ...
    return b, a
end
return swap(11, 22, 33)
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 22\t11\n");
    CHECK(verdict.decompiled.trace == "return: 22\t11\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("truncated multiret call in last table element keeps its adjust-to-one parens", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    // `{pair(), pair()}` spreads the tail -> length 3; `{pair(), (pair())}` truncates it -> length 2.
    // A bare last element would re-spread, so the second table needs `(pair())`.
    const std::string source = R"LUA(
local function pair() return 1,2 end
local t={pair(),pair()}
local u={pair(),(pair())}
return #t,#u
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // Exactly the truncated table's tail is parenthesized; the spread table stays bare.
    CHECK(CountOccurrences(result.decompilationOutput, "(pair())") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "{ pair(), pair() }") == 1);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile("")});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 3\t2\n");
    CHECK(verdict.decompiled.trace == "return: 3\t2\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("single-return fast builtins in spread positions get no redundant parens", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    // buffer.readu8 / math.floor have a fixed bytecode retcount like a source-parenthesized call, but
    // they never tail-spread, so the discriminator must NOT wrap them: `foo(1, buffer.readu8(b, 0))`
    // and `{ 1, math.floor(2.5) }` stay bare (this is what added 1978 churn lines when unguarded).
    const std::string source = R"LUA(
local b = buffer.create(8)
local function foo(a, x) return a end
return foo(1, buffer.readu8(b, 0)), { 1, math.floor(2.5) }
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    CHECK(CountOccurrences(result.decompilationOutput, "(buffer.readu8(") == 0);
    CHECK(CountOccurrences(result.decompilationOutput, "(math.floor(") == 0);
}

TEST_CASE("truncated multiret builtin (table.unpack) in last argument keeps its parens", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    // table.unpack is a fast builtin but genuinely multi-return (Luau getBuiltinInfo.results < 0): a
    // truncated `count((table.unpack(t)))` must keep its parens while `count(table.unpack(t))` stays bare.
    const std::string source = R"LUA(
local t = {10,20,30}
local function count(...) return select("#", ...) end
return count((table.unpack(t))), count(table.unpack(t))
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    // The truncated call has the adjust-to-one paren on top of count's own `(` -> `count((table.unpack`;
    // the spread call has only count's `(` -> `count(table.unpack`. (Var may be auto-renamed, so match
    // the paren shape, not the table name.)
    CHECK(CountOccurrences(result.decompilationOutput, "((table.unpack(") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "count(table.unpack(") == 1);
}

TEST_CASE("fixed builtin calls preserve one result when fallback can be rebound", "[Decompiler][Roundtrip][AdjustToOne]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local function id(x) return x end
return id((math.modf(3.5))), id((math.floor(3.5)))
)LUA";
    Decompiler decompiler{};
    auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string err;
    CHECK(Recompiles(result.decompilationOutput, &err));
    INFO("recompile error: " << err);
    CHECK(CountOccurrences(result.decompilationOutput, "((math.modf(") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "((math.floor(") == 1);
}

// True if the output contains an assignment onto a field of a freshly-built anonymous table literal,
// i.e. `({ ... }).field = expr`. That statement stores into a table that is never bound to anything and
// is discarded on the next line -- it is ALWAYS a dropped binding, never intended code. It was the
// visible symptom of the SSABuilder variadic-RETURN liveness bug: a returned register defined on both
// arms of an if-expression lost its merge phi (the variadic RETURN marked no reads, so the base
// register was not live-in at the merge and its phi got pruned), so the table's post-construction
// SETTABLEKS was emitted against an inline throwaway literal and the RETURN read an undefined version.
