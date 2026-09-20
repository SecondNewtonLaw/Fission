// Classify one fuzz sample and attribute caught assertions to their pipeline stage.
#pragma once

// Generator.hpp depends on these transitively.
#include "libassert/assert.hpp"
#include <algorithm>

#include "ASTLifter.hpp"
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "InstructionDecoder.hpp"
#include "Rewriters/DeadLocalEliminator.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/ShortCircuitFolder.hpp"
#include "SSABuilder.hpp"
#include "SafetyGuard.hpp"
#include "SourceGenerator/Generator.hpp"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#pragma clang diagnostic pop

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace fuzz {
    constexpr int kOpt = 1, kDebug = 2;

    inline void EnableLuauFlags() {
        static bool done = false;
        if (done)
            return;
        done = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    inline bool LuauCompiles(const std::string &source, std::string *bcOut = nullptr) {
        Luau::CompileOptions opts{};
        opts.optimizationLevel = kOpt;
        opts.debugLevel = kDebug;
        const std::string bc = Luau::compile(source, opts);
        if (bcOut)
            *bcOut = bc;
        return !bc.empty() && bc[0] != '\0';
    }

    // flatten every function's opcode stream depth-first; nullopt if it does not compile/lift.
    inline std::optional<std::vector<LiftedOperation>> LiftOpcodes(const std::string &source) {
        std::string bc;
        if (!LuauCompiles(source, &bc))
            return std::nullopt;
        try {
            Deserializer des{};
            auto d = des.Deserialize(bc);
            if (!d || d->functions.empty())
                return std::nullopt;
            Fission::InstructionDecoder decoder{};
            BytecodeLifter lifter{&decoder};
            const LiftedFunction lifted = lifter.LiftDeserializedBytecode(*d);
            std::vector<LiftedOperation> ops;
            std::function<void(const LiftedFunction &)> walk = [&](const LiftedFunction &f) {
                for (const auto &inst : f.instructions)
                    ops.push_back(inst.operation);
                for (const auto &sub : f.subfunctions)
                    walk(sub);
            };
            walk(lifted);
            return ops;
        } catch (...) {
            return std::nullopt;
        }
    }

    // Run the pipeline stage-by-stage, returning the stage name that threw, or nullptr if all
    // stages completed. `stage` tracks progress so the catch attributes the failure precisely.
    // Caller must have a Fission::ScopedThrowingAssertHandler live so asserts throw here.
    inline const char *RunStagesAttributed(const std::string &bytecode) {
        const char *stage = "Deserializer";
        try {
            Fission::InstructionDecoder decoder{};
            Deserializer des{};
            auto d = des.Deserialize(bytecode);
            if (!d || d->functions.empty())
                return nullptr; // undecodable input is not a crash

            stage = "BytecodeLifter";
            BytecodeLifter lifter{&decoder};
            auto lifted = lifter.LiftDeserializedBytecode(*d);

            stage = "DetermineBasicBlocks";
            ControlFlowAnalyzer cfa{};
            auto cf = cfa.DetermineBasicBlocks(&lifted);
            stage = "OptimizeGraph";
            cfa.OptimizeGraph(cf);
            stage = "IdentifyStructures";
            cfa.IdentifyStructures(cf);
            stage = "PruneUnreachable";
            cfa.PruneUnreachable(cf);

            stage = "SSABuilder";
            SSABuilder ssa{};
            ssa.Build(cf);

            stage = "ASTLifter";
            ASTLifter astl{};
            auto ast = astl.Lift(cf);

            stage = "ShortCircuitFolder";
            ShortCircuitFolder{}.Run(ast.statements);
            stage = "IfChainSimplifier";
            IfChainSimplifier{}.Run(ast.statements);
            stage = "DeadLocalEliminator";
            DeadLocalEliminator{}.Run(ast.statements);

            stage = "SourceGenerator";
            RootNode root{ast.statements};
            SourceGenerator sg{};
            (void)sg.GenerateSource(&root);
            return nullptr;
        } catch (...) {
            return stage;
        }
    }

    struct DecompiledOut {
        DecompileResult code;
        std::string output;
    };

    inline DecompiledOut FullDecompile(const std::string &source) {
        Decompiler decompiler{};
        DecompiledOut r{};
        try {
            auto result = decompiler.DecompileTestCode(source, static_cast<DecompilerFlags>(0), Luau::CompileOptions{kOpt, kDebug});
            r.code = result.resultCode;
            r.output = result.decompilationOutput;
        } catch (...) {
            r.code = DecompileResult::FailedToDecompile;
            return r;
        }
        return r;
    }

    // Detect malformed source involving generated register names.
    inline std::vector<std::string> Lines(const std::string &source) {
        std::vector<std::string> lines;
        std::istringstream in(source);
        std::string line;
        while (std::getline(in, line))
            lines.push_back(line);
        return lines;
    }

    inline bool UsesGeneratedLocalBeforeDeclared(const std::string &source) {
        const auto lines = Lines(source);
        const std::regex declRe(R"(\blocal\s+(v\d+)\b)");
        std::map<std::string, size_t> firstDecl;
        for (size_t i = 0; i < lines.size(); ++i)
            for (auto it = std::sregex_iterator(lines[i].begin(), lines[i].end(), declRe); it != std::sregex_iterator(); ++it) {
                const std::string name = (*it)[1];
                if (!firstDecl.count(name))
                    firstDecl[name] = i;
            }
        for (const auto &[name, declIdx] : firstDecl) {
            const std::regex useRe("\\b" + name + "\\b");
            for (size_t i = 0; i < declIdx; ++i)
                if (std::regex_search(lines[i], useRe))
                    return true;
        }
        return false;
    }

    // Sub-classify a forward-ref output for per-milestone attribution of the DeclarationHoister work.
    // "NO_DECL"       ; a `vN` is used but never has a `local vN` anywhere (pure bare-assign leak).
    // "USE_BEFORE_DECL":  a `vN` has a `local vN`, but a use precedes it (decl placed too deep/late).
    // "PHANTOM_VARARG"; a `pairs(..., vN)`-style multret-tail arg naming an undeclared vN (separate root).
    // Returns the dominant tag (checked in that priority). Heuristic, mirrors scratchpad classify_fwd2.
    inline std::string ClassifyForwardRef(const std::string &source) {
        const auto lines = Lines(source);
        const std::regex declRe(R"(\blocal\s+(v\d+)\b)");
        const std::regex varargArgRe(R"(\.\.\.\s*,\s*(v\d+))");
        std::map<std::string, size_t> firstDecl, firstUse;
        for (size_t i = 0; i < lines.size(); ++i) {
            for (auto it = std::sregex_iterator(lines[i].begin(), lines[i].end(), declRe); it != std::sregex_iterator(); ++it)
                if (!firstDecl.count((*it)[1]))
                    firstDecl[(*it)[1]] = i;
        }
        // phantom-vararg: an undeclared vN appearing as a `..., vN` trailing call arg
        for (const auto &ln : lines)
            for (auto it = std::sregex_iterator(ln.begin(), ln.end(), varargArgRe); it != std::sregex_iterator(); ++it)
                if (!firstDecl.count((*it)[1]))
                    return "PHANTOM_VARARG";
        // collect first use of every vN (any token), then compare against its decl
        const std::regex vnRe(R"(\b(v\d+)\b)");
        for (size_t i = 0; i < lines.size(); ++i)
            for (auto it = std::sregex_iterator(lines[i].begin(), lines[i].end(), vnRe); it != std::sregex_iterator(); ++it) {
                const std::string n = (*it)[1];
                if (!firstUse.count(n))
                    firstUse[n] = i;
            }
        bool anyUseBeforeDecl = false, anyNoDecl = false;
        for (const auto &[n, useIdx] : firstUse) {
            const auto d = firstDecl.find(n);
            if (d == firstDecl.end())
                anyNoDecl = true;
            else if (useIdx < d->second)
                anyUseBeforeDecl = true;
        }
        if (anyNoDecl)
            return "NO_DECL";
        if (anyUseBeforeDecl)
            return "USE_BEFORE_DECL";
        return "OTHER";
    }

    // Mutate valid bytecode and feed it to the deserializer + vanilla pipeline. Hostile/garbage
    // input must fail gracefully (FailedToDeserialize/FailedToDecompile), never hard-crash.
    // Returns a stage name if a raw throw escaped the boundary, else nullptr.
    inline const char *ProbeMutatedBytecode(const std::string &bytecode, uint32_t mutationSeed) {
        if (bytecode.size() < 2)
            return nullptr;
        std::string m = bytecode;
        // deterministic byte flips driven by the seed (no global RNG; resume-safe).
        uint32_t s = mutationSeed * 2654435761u + 1u;
        const int flips = 1 + (s % 6);
        for (int i = 0; i < flips; ++i) {
            s = s * 1103515245u + 12345u;
            m[s % m.size()] = static_cast<char>((s >> 8) & 0xFF);
        }
        try {
            Decompiler decompiler{};
            (void)decompiler.DecompileVanillaBytecode(m, static_cast<DecompilerFlags>(0));
            return nullptr; // graceful (FailedToDeserialize/Decompile handled internally)
        } catch (...) {
            return "DecompileVanillaBytecode(mutated)";
        }
    }
} // namespace fuzz
