//
// Created by Dottik on 21/9/2026.
//

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "../../Fission.Fuzzing/include/FuzzOracle.hpp"
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
    const auto result = decompiler.DecompileTestCode(source);
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
})
)LUA";
    const std::string prelude = R"LUA(
Color3 = { fromRGB = function(r, g, b) return r + g + b end }
ColorSequenceKeypoint = { new = function(t, colorValue) return t * 100 + colorValue end }
ColorSequence = { new = function(values) return #values, values[1], values[5] end }
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto verdict = fuzz::CompareSemantics(Luau::compile(source), Luau::compile(result.decompilationOutput), {Luau::compile(prelude)});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 5\t6\t442\n");
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
static bool AssignsToTableLiteralField(const std::string &out) {
    for (size_t p = out.find("})."); p != std::string::npos; p = out.find("}).", p + 3)) {
        const size_t eol = out.find('\n', p);
        const std::string rest = out.substr(p, (eol == std::string::npos ? out.size() : eol) - p);
        const size_t eq = rest.find(" = ");
        if (eq != std::string::npos) // an assignment (` = `), not a comparison, whose LHS is `}).field`
            return true;
    }
    return false;
}

// Regression: a value produced on BOTH arms of an if-expression and then returned, where the return has
// a later multret-call value (`return (if c then a else b), f()`), must keep its merge phi so the
// returned value is the merged local -- not an undefined register that recompiles to nil, and not a
// store against a discarded table literal. Root cause + fix: SSABuilder ComputeLiveness variadic-RETURN
// branch now spans the tail via VariadicTailCount instead of marking zero reads. Found by the AST fuzzer
// (semantic oracle: original returned the table, decompiled returned nil).
TEST_CASE("Regress fuzz: if-expr table arm returned before a multret tail keeps its merge binding", "[Decompiler][Roundtrip][FuzzRegress][SSA]") {
    // else-arm table needs a post-construction store (`field = obj`, obj is a non-constant register).
    {
        const std::string source = "return (if ... then nil else { field = obj }), tonumber(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the corruption: `({  }).field = obj` on a throwaway table.
        CHECK_FALSE(AssignsToTableLiteralField(out));
        // the table must be built as a bound value and the field assigned to that binding.
        CHECK(out.find("field = obj") != std::string::npos);
    }
    // both arms tables -> both need the merge; same invariant.
    {
        const std::string source = "return (if ... then { a = obj } else { field = obj }), tonumber(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        CHECK_FALSE(AssignsToTableLiteralField(out));
    }
}

// Count call applications: an identifier char immediately followed by `(`. `t(1)` -> 1; the buggy
// double-lift produced the call twice (a body `local vN = vM(vK)` plus the re-lifted `until (vM(vK))`).
static size_t CountCallApplications(const std::string &s) {
    size_t n = 0;
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] == '(' && (std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_'))
            ++n;
    return n;
}

// Regression: a `repeat ... until f()` whose condition is an effectful call must run that call ONCE per
// iteration. The lifter force-emits the header (so mutating defs used by the trailing cond survive), but
// a def whose SOLE consumer is the until-condition was force-emitted as a body statement AND re-lifted
// into the condition -> the call ran twice (semantic oracle: original 1 call, decompiled 2). Fix: defer
// such a sole-condition-use, non-loop-carried def so only the condition materializes it. The loop-carried
// counter case (`x = x - 1 until x <= 0`) must NOT be deferred -- its store has to stay in the body.
TEST_CASE("Regress fuzz: repeat-until condition call is not duplicated into the body", "[Decompiler][Roundtrip][FuzzRegress][Loop]") {
    {
        const std::string source = "repeat until t(1)\n";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the sole call `t(1)` (rendered `vN(vM)`) must appear exactly once, not once in the body and
        // once in the `until`.
        CHECK(CountCallApplications(out) == 1);
    }
    // loop-carried decrement must survive in the body (guard against over-eager deferral).
    {
        const std::string source = R"LUA(
local function f(acc, x)
    repeat
        acc = acc + x
        x = x - 1
    until x <= 0
    return acc
end
return f
)LUA";
        Decompiler decompiler{};
        auto result = decompiler.DecompileTestCode(source);
        INFO("source:\n" << source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        const std::string &out = result.decompilationOutput;
        INFO("decompiled output:\n" << out);
        std::string err;
        CHECK(Recompiles(out, &err));
        INFO("recompile error: " << err);
        // the decrement store `arg -= 1` must be a body statement, not folded away into the condition.
        CHECK(out.find("-= 1") != std::string::npos);
    }
}

TEST_CASE("Regress fuzz: materialized calls are not evaluated again", "[Decompiler][FuzzRegress][Semantic]") {
    EnableLuauFFlagsOnce();
    const auto check = [](const std::string &source, size_t fixture) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto prelude = Luau::compile(fuzz::kSemPreludes[fixture]);
        const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
        INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };

    check("repeat obj(129.25) until math.field[t(true)]\n", 1);
    check(R"LUA(local v0 = ipairs({ not nil, print(116i, 166i), "\n", 18i .. self })
v0["value"] -= (not function()
    return 145.125, select
end)
local v1 = function(p0)
    return p0(true, "a-b")
end
return v1(90) or string, nil
)LUA", 0);
}

TEST_CASE("Regress fuzz: expression evaluation preserves first error", "[Decompiler][FuzzRegress][EvaluationOrder]") {
    EnableLuauFFlagsOnce();
    const auto check = [](const std::string &source, const std::string &expected) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto prelude = Luau::compile("");
        const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
        const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
        INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
        CHECK(original.trace == expected);
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    };

    check("return tostring(next.x, -(147i))\n", "error: attempt to index function with 'x'\n");
    check("return ({ true, - -false })[if string then (nil <= math).y else 0]\n", "error: attempt to perform arithmetic (unm) on boolean\n");
    check(R"LUA(
local v0 = if function()
    return (false)["end"][string.value]
end then tostring(next.x, -(147i)) else true
v0[(if false then "" else true)] = math
local v1 = {
    ["end"] = (if nil then 57i else "") and nil * nil,
    ["field"] = (-"hello").x,
    ["\n"] = v0,
    -"x",
}
v0(v0() <= "x")
local v2 = {}
local v3 = pairs(243)
local v4 = { (nil <= "x\n]")[v2(false, "")], ["x"] = v0(), ["y"] = 33.57142857142857 }
return (if obj then nil else nil) > - -105
)LUA", "error: attempt to index function with 'x'\n");
}

TEST_CASE("Regress fuzz: table constructor preserves array-before-field error", "[Decompiler][FuzzRegress][EvaluationOrder]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local v0 = {
    ["x"] = [[a
b]],
    self[-64i],
    (pairs > "key").y,
}
v0()
v0()
local v1 = [[a
b]]
local v2 = nil
local v3 = v0.x
return ("x\n]")[(nil).x]
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), Luau::compile(""));
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), Luau::compile(""));
    INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
    CHECK(original.trace == "error: attempt to index nil with integer\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Regress fuzz: repeat body conditional is not treated as loop exit", "[Decompiler][FuzzRegress][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
repeat
    local v0, v1 = print, function(p0)
        p0(true, p0)
        p0(nil, 130.75)
        return math
    end
    local v2, v3 = (if function(p2)
        v0(nil, "x")
        obj(false, "x")
    end then math else v1.field), v1(obj, tostring)
    v1 ..= tostring.field.field
until (function(p0, ...)
    p0(p0, false)
    return select, true
end)((-nil), (if tonumber:set() then t(nil, tonumber) else function()
    return true
end))
string *= 182.5
while ... do
end
local v0, v1 = function(p0, p1, p2, ...)
    p1(false, "a-b")
    local v3 = p2(57.5, obj)
    return true
end, (-((not ipairs) <= table[tostring]))
v0 = function(p2)
    v1(pairs)
    v0("x")
    return select, nil
end
tonumber()
v1, v1 = v1.field, ((if v0(71.25) then ... else (if math then pairs else t)) or (select)[false])
return "x"
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    const auto prelude = Luau::compile(fuzz::kSemPreludes[0]);
    const auto original = fuzz::RunLuauTrace(Luau::compile(source), prelude);
    const auto decompiled = fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput), prelude);
    INFO("original: " << original.trace << " decompiled: " << decompiled.trace);
    CHECK(original.trace == "error: attempt to call a table value\n");
    CHECK(decompiled.status == original.status);
    CHECK(decompiled.trace == original.trace);
}

TEST_CASE("Regress fuzz: repeat function condition leaves following return outside", "[Decompiler][FuzzRegress][Loop]") {
    const std::string source = R"LUA(
next /= "a-b";
if next:method(..., (if next.field then function(p0, p1, p2, ...)
    p2(p2, p0);
    return nil, p2;
end else (if tonumber then tostring else obj))) then
    string[tostring.field]((-...));
    local v0 = tostring.field;
    ((-"key"))(...);
else
    for g0_0, g0_1 in select.field() do
        while g0_0 do
            g0_0, g0_0 = g0_0, 571;
            g0_0();
            t = true;
            if (if nil then next else obj) then
                continue
            end
        end
        g0_0, g0_0 = (-(-"value")), { ["hello"] = table };
        repeat
        until (g0_1.field == (-(-tostring)));
    end
    local function f0(...)
        return obj;
    end
    local v1, v2 = (f0.field), (true)();
end
repeat
    for i0 = (if true then select else true), ..., (if nil then "key" else next) do
        string(nil, false);
    end
    if tostring.field[...] then
        break
    end
until function(p0, p1, p2)
    local v3, v4, v5 = obj, p1, p2();
    p0, p2 = print, 493;
    for g6_0, g6_1 in next() do
        g6_0();
        tonumber(99.5);
    end
    return "a-b", p1;
end;
return function(p0, p1, ...)
    p0("a-b", true);
    next();
    t();
    return string, print;
end, true;
)LUA";
    EnableLuauFFlagsOnce();
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    INFO("source:\n" << source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK(result.decompilationOutput.find("while true do") == std::string::npos);
    const auto untilFunction = result.decompilationOutput.find("until (function");
    const auto followingReturnClosure = result.decompilationOutput.find("local function anon_");
    const auto followingReturn = followingReturnClosure == std::string::npos ? std::string::npos : result.decompilationOutput.find("return anon_", followingReturnClosure);
    CHECK(untilFunction != std::string::npos);
    CHECK(followingReturnClosure != std::string::npos);
    CHECK(followingReturn != std::string::npos);
    CHECK(untilFunction < followingReturnClosure);
    const bool hasStrandedConditionClosure = result.decompilationOutput.find("local function anon_") != std::string::npos &&
                                              result.decompilationOutput.find("_2(") != std::string::npos;
    CHECK_FALSE(hasStrandedConditionClosure);
}

TEST_CASE("Repeat conditional latch preserves closure condition semantics", "[Decompiler][FuzzRegress][Loop][Semantic]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local iterations = 0
repeat
    iterations += 1
until function()
    return iterations == 1
end
return iterations
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    const auto originalBc = Luau::compile(source, Luau::CompileOptions{1, 2});
    const auto decompiledBc = Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2});
    const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
    REQUIRE_FALSE(originalBc.empty());
    REQUIRE_FALSE(decompiledBc.empty());
    REQUIRE_FALSE(preludeBc.empty());

    const auto verdict = fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 1\n");
    CHECK(verdict.decompiled.trace == verdict.original.trace);
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress fuzz: loop update between continue and break conditions survives", "[Decompiler][FuzzRegress][Loop]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
local total = 0
for i = 1, 11 do
    if i % 2 == 0 then continue end
    total += i
    if total > 13 then break end
end
return total
)LUA";
    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);

    const auto originalBc = Luau::compile(source, Luau::CompileOptions{1, 2});
    const auto decompiledBc = Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2});
    const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
    REQUIRE_FALSE(originalBc.empty());
    REQUIRE_FALSE(decompiledBc.empty());
    REQUIRE_FALSE(preludeBc.empty());
    const auto verdict = fuzz::CompareSemantics(originalBc, decompiledBc, {preludeBc});
    INFO("original: " << verdict.original.trace << " decompiled: " << verdict.decompiled.trace);
    CHECK(verdict.original.trace == "return: 16\n");
    CHECK(verdict.kind == fuzz::SemVerdict::Kind::Match);
}

TEST_CASE("Regress fuzz: call callee is evaluated before effectful arguments", "[Decompiler][FuzzRegress][CallOrder]") {
    EnableLuauFFlagsOnce();
    const auto decompileAndRun = [](const std::string &source) {
        Decompiler decompiler{};
        const auto result = decompiler.DecompileTestCode(source);
        REQUIRE(result.resultCode == DecompileResult::Success);
        INFO("decompiled output:\n" << result.decompilationOutput);
        const auto preludeBc = Luau::compile("", Luau::CompileOptions{1, 2});
        return std::pair{
            fuzz::RunLuauTrace(Luau::compile(source, Luau::CompileOptions{1, 2}), preludeBc),
            fuzz::RunLuauTrace(Luau::compile(result.decompilationOutput, Luau::CompileOptions{1, 2}), preludeBc)
        };
    };

    for (const std::string source :
         {"print.field.field(print(\"key\"), { field = print, math })\n", "if select then print.field.field(print(\"key\"), { field = print, math }) end\n"}) {
        const auto [original, decompiled] = decompileAndRun(source);
        CHECK(original.status == fuzz::SemTrace::Status::Error);
        CHECK(decompiled.status == original.status);
        CHECK(original.prints.empty());
        CHECK(decompiled.trace == original.trace);
    }
    {
        const std::string source = R"LUA(
local log = {}
local callable = setmetatable({}, { __call = function() log[#log + 1] = "call" end })
local root = setmetatable({}, { __index = function()
    log[#log + 1] = "callee"
    return { field = callable }
end })
local function argument()
    log[#log + 1] = "argument"
    return 1
end
root.field.field(argument(), { field = print, math })
return table.concat(log, ",")
)LUA";
        const auto [original, decompiled] = decompileAndRun(source);
        CHECK(original.status == fuzz::SemTrace::Status::Ok);
        CHECK(original.trace == "return: \"callee,argument,call\"\n");
        CHECK(decompiled.status == original.status);
        CHECK(decompiled.trace == original.trace);
    }
}

TEST_CASE("Regress fuzz: repeat body does not shadow values used after loop", "[Decompiler][FuzzRegress][ForwardRef]") {
    EnableLuauFFlagsOnce();
    const std::string source = R"LUA(
repeat
    if ... then
        repeat
            ipairs();
            if (-(-tonumber)) then
                break
            end
        until (-(-{ [pairs] = "x", data = next, string, field = pairs }));
    end
until ({ k = ..., [tostring] = table(), y = string(obj, string) } and (if {  } then ("").field else nil));
string = (not true);
local function f0()
    f0();
    f0(nil);
    return f0;
end
f0 += (if false then nil else 27);
return (f0:set(100.75, tostring)), f0[""][f0];
)LUA";

    Decompiler decompiler{};
    const auto result = decompiler.DecompileTestCode(source);
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompiled output:\n" << result.decompilationOutput);
    std::string error;
    REQUIRE(Recompiles(result.decompilationOutput, &error));
    INFO("recompile error: " << error);
    CHECK_FALSE(fuzz::UsesGeneratedLocalBeforeDeclared(result.decompilationOutput, &source));
    CHECK(CountOccurrences(result.decompilationOutput, "local function f0") == 1);
    CHECK(CountOccurrences(result.decompilationOutput, "repeat") == 1);
}
