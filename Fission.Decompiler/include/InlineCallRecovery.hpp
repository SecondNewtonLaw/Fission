//
// Created by Dottik on 1/10/2026.
//

#pragma once
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

// Source line spans of every function in a chunk; an instruction whose line falls in another function's span is that function's body inlined
// by the compiler at O2. Needs line info (debug level 1+).
struct InlineSourceMap {
    struct Source {
        const DeserializedFunction *function;
        int32_t parent;
        int32_t firstLine;
        int32_t lastLine;
    };
    std::vector<Source> sources;
    // per source line: the innermost inlinable function whose body spans it, or -1
    std::vector<int32_t> innermostByLine;
    bool hasLineInfo = false;
    std::unordered_map<const DeserializedFunction *, const LiftedFunction *> lifted;
};

std::shared_ptr<const InlineSourceMap> BuildInlineSourceMap(const DeserializedBytecode &bytecode, Fission::InstructionDecoder *decoder, const LiftedFunction &root);

// Fills LiftedFunction::inlineOrigin for the whole tree.
void AssignInlineOrigins(LiftedFunction &root, const InlineSourceMap &map);

// Rewrites each inlined body that aligns with its callee into a call of the callee's closure. True when an instruction stream changed, after
// which the tree's CFA and SSA must be rebuilt.
bool RecoverInlinedCalls(AnalyzedFunction &root, const InlineSourceMap &map);
