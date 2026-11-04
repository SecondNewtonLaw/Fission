//
// Regression tests for ConditionFolder and the and-chain terminal fold.
//

#include "ControlFlowRewriterTestSupport.hpp"
#include "Decompiler.hpp"
#include "IntegrationTestSupport.hpp"
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    const std::string kAndChainSource = R"LUA(
local function pick(a, b, c)
    return a and b and c
end
return pick
)LUA";

    const std::string kAndChainDriver = "local f=__integration_subject(); print(f(1,2,3),f(nil,2,3),f(1,nil,3),f(false,true,true),f(true,true,nil))";

    const std::string kWhileGuardSource = R"LUA(
local function drain(a, b)
    local steps = 0
    while a > 0 and b > 0 do
        a = a - 1
        b = b - 1
        steps = steps + 1
    end
    return a, b, steps
end
return drain
)LUA";

    const std::string kWhileGuardDriver = "local f=__integration_subject(); print(f(4,3),f(0,5),f(2,0),f(7,9))";

    const std::string kNegationSource = R"LUA(
local function same(a, b)
    if not (a == b) then
        return "different"
    end
    return "same"
end
local function both(a, b)
    return not (not a or not b)
end
return same, both
)LUA";

    const std::string kNegationDriver =
        "local same,both=__integration_subject(); print(same(1,1),same(1,2),same(nil,nil),both(true,true),both(true,nil),both(nil,true),both(false,false))";
} // namespace

TEST_CASE("Source: and-chain return folds into one expression", "[Decompiler][Condition][Regression][Integration]") {
    const auto output = control_flow_regression::DecompileOrFail(kAndChainSource, 1, 2, kAndChainDriver);
    INFO("rendered source:\n" << output);
    // `return a and b and c` must not survive as a guarded chain of temporaries.
    CHECK(control_flow_regression::Contains(output, "return a and b and c"));
    CHECK_FALSE(control_flow_regression::Contains(output, "if not v"));
    integration_test::Check(kAndChainSource, kAndChainDriver);
}

TEST_CASE("Source: while condition stays a conjunction instead of a leading break guard", "[Decompiler][Condition][Regression][Integration]") {
    const auto output = control_flow_regression::DecompileOrFail(kWhileGuardSource, 1, 2, kWhileGuardDriver);
    INFO("rendered source:\n" << output);
    CHECK(control_flow_regression::Contains(output, "while a > 0 and b > 0 do"));
    CHECK_FALSE(control_flow_regression::Contains(output, "break"));
    integration_test::Check(kWhileGuardSource, kWhileGuardDriver);
}

TEST_CASE("Source: negations fold back to the positive condition", "[Decompiler][Condition][Regression][Integration]") {
    const auto output = control_flow_regression::DecompileOrFail(kNegationSource, 1, 2, kNegationDriver);
    INFO("rendered source:\n" << output);
    // `not (a == b)` is `a ~= b`; the negated temp for `not (not a or not b)` is folded back into the
    // return. That expression must stay boolean-valued, so it is not rewritten to `a and b`.
    CHECK(control_flow_regression::Contains(output, "a ~= b"));
    CHECK_FALSE(control_flow_regression::Contains(output, "not (a == b)"));
    CHECK(control_flow_regression::Contains(output, "return not (not a or not b)"));
    CHECK_FALSE(control_flow_regression::Contains(output, "return not v"));
    integration_test::Check(kNegationSource, kNegationDriver);
}

TEST_CASE("Source: one-shot repeat folds to straight-line code", "[Decompiler][Condition][Regression][Integration]") {
    const std::string source = R"LUA(
local function step(n)
    local acc = 0
    repeat
        acc = acc + n
        n = n - 1
    until true
    return acc
end
return step
)LUA";
    const std::string driver = "local f=__integration_subject(); print(f(3), f(0), f(-2))";
    const auto output = control_flow_regression::DecompileOrFail(source, 0, 2, driver);
    INFO("rendered source:\n" << output);
    // `repeat ... until true` runs its body once; it must not be rendered as a loop.
    CHECK_FALSE(control_flow_regression::Contains(output, "until true"));
    integration_test::Check(source, driver);
}

TEST_CASE("Source: loop guard folding preserves loop-carried effects", "[Decompiler][Condition][Regression][Integration]") {
    const std::string source = R"LUA(
local function scan(state, limit)
    local calls = 0
    local function ready()
        calls = calls + 1
        return state.pending < limit
    end
    while state.running and ready() do
        state.pending = state.pending + 1
        if state.pending % 3 == 0 then
            state.running = false
        end
    end
    return calls, state.pending, state.running
end
return scan
)LUA";
    const std::string driver =
        "local f=__integration_subject(); print(f({running=true,pending=0},5)); print(f({running=true,pending=4},9)); print(f({running=false,pending=0},3))";
    const auto output = control_flow_regression::DecompileOrFail(source, 1, 2, driver);
    INFO("rendered source:\n" << output);
    CHECK(control_flow_regression::Contains(output, "while "));
    integration_test::Check(source, driver);
}
