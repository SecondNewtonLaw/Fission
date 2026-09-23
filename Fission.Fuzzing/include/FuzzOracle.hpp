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
#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Compiler.h"
#include "Luau/Parser.h"
#pragma clang diagnostic pop

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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
            stage = "PruneUnreachable";
            cfa.PruneUnreachable(cf);
            stage = "IdentifyStructures";
            cfa.IdentifyStructures(cf);

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

    struct GeneratedNames {
        struct Site {
            Luau::Position position;
            size_t scope = static_cast<size_t>(-1);
        };

        std::set<std::string> locals;
        std::set<std::string> globals;
        std::map<std::string, std::vector<Site>> localSites;
        std::map<std::string, std::vector<Site>> globalSites;
        std::vector<Luau::Location> scopes;
    };

    inline bool IsGeneratedName(std::string_view name) {
        if (name.size() < 2 || name[0] != 'v')
            return false;
        size_t i = 1;
        while (i < name.size() && name[i] >= '0' && name[i] <= '9')
            ++i;
        if (i == 1 || i == name.size())
            return i == name.size();
        if (name[i++] != '_' || i == name.size())
            return false;
        return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(i), name.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    inline std::optional<GeneratedNames> FindGeneratedNames(const std::string &source) {
        Luau::Allocator allocator;
        Luau::AstNameTable names{allocator};
        auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
        if (!parsed.errors.empty() || !parsed.root)
            return std::nullopt;

        struct Visitor final : Luau::AstVisitor {
            GeneratedNames result;

            void AddLocal(const Luau::AstLocal *local) {
                if (local && IsGeneratedName(local->name.value)) {
                    result.locals.emplace(local->name.value);
                    result.localSites[local->name.value].push_back({local->location.begin});
                }
            }

            bool visit(Luau::AstStatBlock *node) override {
                result.scopes.push_back(node->location);
                return true;
            }

            bool visit(Luau::AstStatIf *node) override {
                AddLocal(node->conditionLocal);
                return true;
            }

            bool visit(Luau::AstStatLocal *node) override {
                for (const auto *local : node->vars)
                    AddLocal(local);
                return true;
            }

            bool visit(Luau::AstStatLocalFunction *node) override {
                AddLocal(node->name);
                return true;
            }

            bool visit(Luau::AstStatFor *node) override {
                AddLocal(node->var);
                return true;
            }

            bool visit(Luau::AstStatForIn *node) override {
                for (const auto *local : node->vars)
                    AddLocal(local);
                return true;
            }

            bool visit(Luau::AstExprGlobal *node) override {
                if (IsGeneratedName(node->name.value)) {
                    result.globals.emplace(node->name.value);
                    result.globalSites[node->name.value].push_back({node->location.begin});
                }
                return true;
            }
        } visitor;

        parsed.root->visit(&visitor);

        const auto scopeFor = [&](const Luau::Position position) {
            size_t result = static_cast<size_t>(-1);
            for (size_t i = 0; i < visitor.result.scopes.size(); ++i) {
                const auto &scope = visitor.result.scopes[i];
                if (position < scope.begin || scope.end < position)
                    continue;
                if (result == static_cast<size_t>(-1) ||
                    (scope.begin >= visitor.result.scopes[result].begin && scope.end <= visitor.result.scopes[result].end))
                    result = i;
            }
            return result;
        };
        for (auto &entry : visitor.result.localSites)
            for (auto &site : entry.second)
                site.scope = scopeFor(site.position);
        for (auto &entry : visitor.result.globalSites)
            for (auto &site : entry.second)
                site.scope = scopeFor(site.position);
        return visitor.result;
    }

    inline bool HasSameScopeGlobalBeforeLocal(const GeneratedNames &names, const std::string &name) {
        const auto globals = names.globalSites.find(name);
        const auto locals = names.localSites.find(name);
        if (globals == names.globalSites.end() || locals == names.localSites.end())
            return false;
        for (const auto &global : globals->second)
            for (const auto &local : locals->second)
                if (global.scope == local.scope && global.position < local.position)
                    return true;
        return false;
    }

    inline bool UsesGeneratedLocalBeforeDeclared(const std::string &source, const std::string *original = nullptr) {
        const auto names = FindGeneratedNames(source);
        if (!names)
            return false;
        const auto originalNames = original ? FindGeneratedNames(*original) : std::nullopt;
        for (const auto &name : names->globals) {
            if (!originalNames) {
                if (HasSameScopeGlobalBeforeLocal(*names, name))
                    return true;
            } else {
                const bool outputForward = HasSameScopeGlobalBeforeLocal(*names, name);
                const bool originalForward = HasSameScopeGlobalBeforeLocal(*originalNames, name);
                if (!originalNames->globals.contains(name) || (outputForward && !originalForward))
                    return true;
            }
        }
        return false;
    }

    inline std::string ClassifyForwardRef(const std::string &source, const std::string *original = nullptr, std::string *culprit = nullptr) {
        const auto names = FindGeneratedNames(source);
        if (!names)
            return "OTHER";
        const auto originalNames = original ? FindGeneratedNames(*original) : std::nullopt;
        for (const auto &name : names->globals) {
            const bool outputForward = HasSameScopeGlobalBeforeLocal(*names, name);
            const bool originalForward = originalNames && HasSameScopeGlobalBeforeLocal(*originalNames, name);
            if (outputForward && !originalForward) {
                if (culprit)
                    *culprit = name;
                return "USE_BEFORE_DECL";
            }
            if (!names->locals.contains(name) || (originalNames && !originalNames->globals.contains(name))) {
                if (culprit)
                    *culprit = name;
                return "NO_DECL";
            }
        }
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
