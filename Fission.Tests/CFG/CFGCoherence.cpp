//
// CFG coherence and sense tests.
//
// Each test compiles a Luau snippet, runs the full CFA pipeline
// (DetermineBasicBlocks -> OptimizeGraph -> PruneUnreachable -> IdentifyStructures),
// and asserts invariants the graph should hold regardless of the lifter's
// downstream behavior.
//

#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "Luau/Common.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <string>

namespace {

    // Holds owning storage for every stage of the pipeline so the
    // non-owning pointers inside AnalyzedFunction / BasicBlock stay valid for the
    // lifetime of the test case.
    struct AnalyzedSnippet {
        std::optional<DeserializedBytecode> deserialized;
        std::unique_ptr<LiftedFunction> lifted;
        AnalyzedFunction analyzed;
    };

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    std::unique_ptr<AnalyzedSnippet> CompileAndAnalyze(const std::string &source) {
        EnableLuauFFlagsOnce();

        auto snippet = std::make_unique<AnalyzedSnippet>();

        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const auto compiled = Luau::compile(source, opts);

        Deserializer deserializer{};
        snippet->deserialized = deserializer.Deserialize(compiled);
        REQUIRE(snippet->deserialized.has_value());
        REQUIRE_FALSE(snippet->deserialized->functions.empty());

        Fission::InstructionDecoder decoder{};
        BytecodeLifter lifter{&decoder};
        snippet->lifted = std::make_unique<LiftedFunction>(lifter.LiftDeserializedBytecode(*snippet->deserialized));

        ControlFlowAnalyzer cfa{};
        snippet->analyzed = cfa.DetermineBasicBlocks(snippet->lifted.get());
        cfa.OptimizeGraph(snippet->analyzed);
        cfa.PruneUnreachable(snippet->analyzed);
        cfa.IdentifyStructures(snippet->analyzed);

        return snippet;
    }

    bool IsAlive(const BasicBlock &b) { return b.bType != BlockType::Dead && b.bType != BlockType::Error; }

    // Every alive block must hold valid instruction range.
    void CheckHeadTailValid(const AnalyzedFunction &f) {
        for (const auto &blk : f.basicBlocks) {
            if (!IsAlive(blk))
                continue;
            INFO("Block " << blk.dwBlockId << " type=" << BlockTypeToString(blk.bType) << " should have valid lpHead/lpTail");
            CHECK(blk.lpHead != nullptr);
            CHECK(blk.lpTail != nullptr);
            if (blk.lpHead && blk.lpTail)
                CHECK(blk.lpHead <= blk.lpTail);
        }
    }

    // Edge symmetry: every successor edge has a matching predecessor edge and vice versa.
    void CheckEdgeSymmetry(const AnalyzedFunction &f) {
        for (const auto &blk : f.basicBlocks) {
            if (!IsAlive(blk))
                continue;
            for (auto sid : blk.successors) {
                REQUIRE(sid < f.basicBlocks.size());
                const auto &s = f.basicBlocks[sid];
                INFO("Block " << blk.dwBlockId << " -> succ " << sid << " is missing the reverse edge in succ.predecessors");
                CHECK(std::find(s.predecessors.begin(), s.predecessors.end(), blk.dwBlockId) != s.predecessors.end());
            }
            for (auto pid : blk.predecessors) {
                REQUIRE(pid < f.basicBlocks.size());
                const auto &p = f.basicBlocks[pid];
                INFO("Block " << blk.dwBlockId << " <- pred " << pid << " is missing the reverse edge in pred.successors");
                CHECK(std::find(p.successors.begin(), p.successors.end(), blk.dwBlockId) != p.successors.end());
            }
        }
    }

    // Every alive block must be reachable from the entry block (id 0).
    void CheckReachability(const AnalyzedFunction &f) {
        if (f.basicBlocks.empty())
            return;
        std::set<uint32_t> seen;
        std::queue<uint32_t> q;
        seen.insert(0);
        q.push(0);
        while (!q.empty()) {
            const auto id = q.front();
            q.pop();
            for (auto sid : f.basicBlocks[id].successors) {
                if (seen.insert(sid).second)
                    q.push(sid);
            }
        }
        for (const auto &blk : f.basicBlocks) {
            if (!IsAlive(blk))
                continue;
            INFO("Block " << blk.dwBlockId << " type=" << BlockTypeToString(blk.bType) << " is alive but unreachable from entry");
            CHECK(seen.count(blk.dwBlockId) == 1u);
        }
    }

    // IfHeader blocks must always have both branch targets recorded.
    void CheckIfHeaderHasBranches(const AnalyzedFunction &f) {
        for (const auto &blk : f.basicBlocks) {
            if (blk.bType != BlockType::IfHeader)
                continue;
            INFO("IfHeader block " << blk.dwBlockId << " missing ifStatementTrue/ifStatementFalse");
            CHECK(blk.ifStatementTrue.has_value());
            CHECK(blk.ifStatementFalse.has_value());
            // Both branch targets must reference existing blocks and appear as successors.
            if (blk.ifStatementTrue.has_value()) {
                CHECK(*blk.ifStatementTrue < f.basicBlocks.size());
                CHECK(std::find(blk.successors.begin(), blk.successors.end(), *blk.ifStatementTrue) != blk.successors.end());
            }
            if (blk.ifStatementFalse.has_value()) {
                CHECK(*blk.ifStatementFalse < f.basicBlocks.size());
                CHECK(std::find(blk.successors.begin(), blk.successors.end(), *blk.ifStatementFalse) != blk.successors.end());
            }
        }
    }

    // LoopHeader/LoopLatch pairs must be mutually linked.
    void CheckLoopHeaderLatchPair(const AnalyzedFunction &f) {
        for (const auto &blk : f.basicBlocks) {
            if (blk.bType != BlockType::LoopHeader)
                continue;
            INFO("LoopHeader block " << blk.dwBlockId << " missing loopLatch");
            CHECK(blk.loopLatch.has_value());
            if (blk.loopLatch.has_value()) {
                REQUIRE(*blk.loopLatch < f.basicBlocks.size());
                const auto &latch = f.basicBlocks[*blk.loopLatch];
                INFO("LoopLatch " << *blk.loopLatch << " should point back to header " << blk.dwBlockId);
                CHECK(latch.loopHeader.has_value());
                if (latch.loopHeader.has_value())
                    CHECK(*latch.loopHeader == blk.dwBlockId);
            }
        }
    }

    // Return blocks terminate the function and must have no successors.
    void CheckReturnHasNoSuccessors(const AnalyzedFunction &f) {
        for (const auto &blk : f.basicBlocks) {
            if (blk.bType != BlockType::Return)
                continue;
            INFO("Return block " << blk.dwBlockId << " unexpectedly has successors");
            CHECK(blk.successors.empty());
            CHECK(blk.bTerminator == BlockTerminator::Return);
        }
    }

    // Block IDs must equal their index in the basicBlocks vector (LinkBasicBlocks relies on this).
    void CheckBlockIdsMatchIndices(const AnalyzedFunction &f) {
        for (size_t i = 0; i < f.basicBlocks.size(); ++i) {
            INFO("Block at vector index " << i << " has dwBlockId=" << f.basicBlocks[i].dwBlockId);
            CHECK(f.basicBlocks[i].dwBlockId == i);
        }
    }

    void RunStandardCoherenceChecks(const AnalyzedFunction &f) {
        CheckBlockIdsMatchIndices(f);
        CheckHeadTailValid(f);
        CheckEdgeSymmetry(f);
        CheckReachability(f);
        CheckReturnHasNoSuccessors(f);
        CheckIfHeaderHasBranches(f);
        CheckLoopHeaderLatchPair(f);
    }

    size_t CountBlocksByType(const AnalyzedFunction &f, BlockType t) {
        size_t n = 0;
        for (const auto &b : f.basicBlocks)
            if (b.bType == t)
                ++n;
        return n;
    }

} // namespace

TEST_CASE("CFG: empty function itself has entry as the return", "[CFG]") {
    auto snip = CompileAndAnalyze("return");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    // A bare `return` should collapse to a single block that is both entry and return.
    CHECK(CountBlocksByType(f, BlockType::Return) >= 1);
}

TEST_CASE("CFG: linear straight-line code has a single live block", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local a = 1
        local b = 2
        print(a + b)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    size_t alive = 0;
    for (const auto &b : f.basicBlocks)
        if (IsAlive(b))
            ++alive;
    INFO("Straight-line code should collapse to a single alive block after CFA optimization");
    CHECK(alive == 1u);
}

TEST_CASE("CFG: if/then without else produces a recognizable IfHeader", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local x = ...
        if x then
            print("a")
        end
        print("b")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("Expected at least one IfHeader block for `if x then ... end`");
    CHECK(CountBlocksByType(f, BlockType::IfHeader) >= 1);
}

TEST_CASE("CFG: if/else has IfHeader with two distinct branch targets", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local x = ...
        if x then
            print("a")
        else
            print("b")
        end
        print("c")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    REQUIRE(CountBlocksByType(f, BlockType::IfHeader) >= 1);

    for (const auto &b : f.basicBlocks) {
        if (b.bType != BlockType::IfHeader)
            continue;
        REQUIRE(b.ifStatementTrue.has_value());
        REQUIRE(b.ifStatementFalse.has_value());
        INFO("IfHeader true/false branches should be distinct blocks for an if/else");
        CHECK(*b.ifStatementTrue != *b.ifStatementFalse);
    }
}

TEST_CASE("CFG: while loop emits a LoopHeader/LoopLatch pair", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        print(i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("`while` loop should produce at least one LoopHeader and one LoopLatch");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}

TEST_CASE("CFG: numeric for loop is identified as a ForNumeric structure", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        for i = 1, 10 do
            print(i)
        end
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    bool sawNumericLoop = false;
    for (const auto &b : f.basicBlocks) {
        if ((b.dwBlockFlags & static_cast<uint32_t>(LoopBlockFlags::ForNumericLoop)) != 0u) {
            sawNumericLoop = true;
            break;
        }
    }
    INFO("Expected at least one block flagged with LoopBlockFlags::ForNumericLoop");
    CHECK(sawNumericLoop);
}

TEST_CASE("CFG: generic for (pairs) is identified as a ForGeneral structure", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        for k, v in pairs({1, 2, 3}) do
            print(k, v)
        end
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    constexpr uint32_t anyGenericMask = static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop) | static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Pairs) |
                                        static_cast<uint32_t>(LoopBlockFlags::ForGeneralLoop_Indexed);
    bool sawGeneric = false;
    for (const auto &b : f.basicBlocks) {
        if ((b.dwBlockFlags & anyGenericMask) != 0u) {
            sawGeneric = true;
            break;
        }
    }
    INFO("Expected at least one block flagged as a ForGeneralLoop variant");
    CHECK(sawGeneric);
}

TEST_CASE("CFG: nested if statements both yield IfHeader blocks", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local a = ...
        local b = ...
        if a then
            if b then
                print("ab")
            else
                print("a only")
            end
        end
        print("done")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("Two nested if statements should produce at least two IfHeader blocks");
    CHECK(CountBlocksByType(f, BlockType::IfHeader) >= 2);
}

TEST_CASE("CFG: early return inside `if` produces a reachable Return block", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local function f(x)
            if x then
                return 42
            end
            return 0
        end
        print(f(true))
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    // The lifter recurses into inner functions; check at least one of them
    // (or the main) ends up with two Return blocks (the early one + the tail one).
    auto countReturns = [](const AnalyzedFunction &fn) { return CountBlocksByType(fn, BlockType::Return); };
    bool anyHasTwoReturns = countReturns(f) >= 2;
    for (const auto &inner : f.innerFunctions)
        anyHasTwoReturns = anyHasTwoReturns || countReturns(inner) >= 2;

    INFO("Expected at least one function (main or inner) to expose 2 Return blocks");
    CHECK(anyHasTwoReturns);
}

TEST_CASE("CFG: every alive block has a defined terminator", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local s = 0
        for i = 1, 5 do
            if i % 2 == 0 then
                s = s + i
            else
                s = s - i
            end
        end
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    for (const auto &b : f.basicBlocks) {
        if (!IsAlive(b))
            continue;
        INFO("Block " << b.dwBlockId << " has BlockTerminator::Error (terminator never set)");
        CHECK(b.bTerminator != BlockTerminator::Error);
    }
}

TEST_CASE("CFG: inner function CFAs satisfy the same invariants as main", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local function inner(x)
            if x > 0 then
                return x * 2
            end
            return -1
        end
        print(inner(5))
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    REQUIRE_FALSE(f.innerFunctions.empty());
    for (const auto &inner : f.innerFunctions) {
        RunStandardCoherenceChecks(inner);
    }
}

TEST_CASE("CFG: repeat-until loop produces LoopHeader/LoopLatch", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        repeat
            i = i + 1
        until i >= 10
        print(i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("repeat-until should produce at least one LoopHeader and one LoopLatch");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}

TEST_CASE("CFG: break inside while exits the loop correctly", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 100 do
            if i > 10 then
                break
            end
            i = i + 1
        end
        print(i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    // The Luau compiler may merge `if cond then break end` into a single JUMPIF
    // when break is the last statement before the back-edge. The critical invariant
    // is that the IfHeader's exit branch reaches post-loop code (verified by reachability).
    INFO("`break` inside while should have LoopHeader and reachable Return");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);

    bool foundReturn = false;
    for (const auto &b : f.basicBlocks) {
        if (b.bType == BlockType::Return) {
            foundReturn = true;
            break;
        }
    }
    CHECK(foundReturn);
}

TEST_CASE("CFG: continue inside while jumps back to loop header", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        local s = 0
        while i < 10 do
            i = i + 1
            if i % 2 == 0 then
                continue
            end
            s = s + i
        end
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("`continue` inside while should produce at least one LoopHeader/LoopLatch pair");
    INFO("(continue block may be merged with back-edge by the Luau compiler)");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}

TEST_CASE("CFG: conditional break in a numeric for loop", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        for i = 1, 100 do
            if i * i > 50 then
                break
            end
        end
        print("done")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    // The Luau compiler may merge the break into the if-condition.
    // Verify structural coherence and that post-loop code is reachable.
    bool foundReturn = false;
    for (const auto &b : f.basicBlocks) {
        if (b.bType == BlockType::Return) {
            foundReturn = true;
            break;
        }
    }
    CHECK(foundReturn);
}

TEST_CASE("CFG: nested loops (for inside while)", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local s = 0
        local outer = 0
        while outer < 3 do
            for inner = 1, 5 do
                s = s + inner
            end
            outer = outer + 1
        end
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("nested while/for should produce at least 2 LoopHeader blocks");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 2);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 2);
}

TEST_CASE("CFG: if-elseif-else chain produces multiple IfHeaders", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local x = ...
        if x < 0 then
            print("neg")
        elseif x == 0 then
            print("zero")
        elseif x > 100 then
            print("large")
        else
            print("normal")
        end
        print("done")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("if-elseif-elseif-else should produce at least 3 IfHeader blocks");
    CHECK(CountBlocksByType(f, BlockType::IfHeader) >= 3);
}

TEST_CASE("CFG: loop with if/else inside both branches runnable", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local s = 0
        for i = 1, 10 do
            if i % 2 == 0 then
                s = s + i
            else
                s = s - i
            end
        end
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("for loop with if/else inside should have IfHeader and LoopHeader");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::IfHeader) >= 1);
}

TEST_CASE("CFG: break inside nested if inside while", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        local found = false
        while i < 100 do
            if i > 10 then
                if i % 7 == 0 then
                    found = true
                    break
                end
            end
            i = i + 1
        end
        print(found, i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("nested if inside while should produce multiple IfHeader blocks");
    CHECK(CountBlocksByType(f, BlockType::IfHeader) >= 2);
}

TEST_CASE("CFG: two sequential while loops produce separate headers", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 5 do
            i = i + 1
        end
        local j = 0
        while j < 5 do
            j = j + 1
        end
        print(i, j)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("two sequential while loops should produce at least 2 LoopHeader blocks");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 2);
}

TEST_CASE("CFG: repeat-until with continue", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        local s = 0
        repeat
            i = i + 1
            if i % 2 == 0 then
                continue
            end
            s = s + i
        until i >= 10
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("repeat-until with continue should have LoopHeader, LoopLatch");
    INFO("(continue block may be merged with back-edge by the Luau compiler)");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}

TEST_CASE("CFG: multiple continues in a while loop", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 20 do
            i = i + 1
            if i % 2 == 0 then
                continue
            end
            local x = i * 3
            if x > 30 then
                continue
            end
            print(i)
        end
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("two continues in one loop should produce LoopHeader, LoopLatch");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}

TEST_CASE("CFG: while true do break end", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while true do
            if i > 10 then
                break
            end
            i = i + 1
        end
        print(i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("`break` in while-true loop should still produce coherent CFG");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
    CHECK(CountBlocksByType(f, BlockType::Return) >= 1);
}

TEST_CASE("CFG: break in middle of loop body", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 100 do
            i = i + 1
            if i > 50 then
                break
            end
            print(i)
        end
        print("done")
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("break in the middle should still allow coherence");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
    CHECK(CountBlocksByType(f, BlockType::Return) >= 1);
}

TEST_CASE("CFG: triple-nested loops", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local s = 0
        for a = 1, 5 do
            local b = 0
            while b < a do
                b = b + 1
                repeat
                    s = s + b
                until s > 100
            end
        end
        print(s)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("triple-nested for/while/repeat loops should produce at least 3 LoopHeaders");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 3);
    CHECK(CountBlocksByType(f, BlockType::Return) >= 1);
}

TEST_CASE("CFG: empty while loop body", "[CFG]") {
    auto snip = CompileAndAnalyze(R"(
        local i = 0
        while i < 10 do
            i = i + 1
        end
        print(i)
    )");
    const auto &f = snip->analyzed;

    REQUIRE_FALSE(f.basicBlocks.empty());
    RunStandardCoherenceChecks(f);

    INFO("while loop with minimal body produces a LoopHeader/LoopLatch pair");
    CHECK(CountBlocksByType(f, BlockType::LoopHeader) >= 1);
    CHECK(CountBlocksByType(f, BlockType::LoopLatch) >= 1);
}
