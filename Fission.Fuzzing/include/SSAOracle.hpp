//
// Created by Dottik on 22/9/2026.
//
// Differential SSA check: each register read in Fission's SSA, flattened through phis to its defining
// instructions, must equal the definitions that reach that read in the VM (independent reaching-defs
// dataflow over the raw Luau words). Multret values are represented by the producer's base register,
// matching how Fission versions them; FASTCALL shadows of a CALL are not compared. FORGLOOP's per-iteration
// reads of the hidden state/control slots are not source-visible and are not compared.
#pragma once

#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"
#include "SSABuilder.hpp"

#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fuzz {
    struct SSAMismatch {
        std::string kind;
        std::string function;
        int32_t pc = -1;
        int reg = -1;
        std::string operation;
        std::string detail;
    };

    struct SSAOracleReport {
        std::vector<SSAMismatch> mismatches;
        std::string dumps;
        size_t functions = 0;
        size_t readsCompared = 0;
        size_t functionsSkipped = 0;
    };

    namespace ssa_oracle {
        constexpr int32_t kEntry = -1;
        constexpr int32_t kUnresolved = -2;

        struct Access {
            std::vector<int> reads;
            std::vector<int> writes;
            bool tailUnknown = false;
        };

        inline void AddRange(std::vector<int> &into, int from, int count) {
            for (int i = 0; i < count; ++i)
                into.push_back(from + i);
        }

        inline int MultretTop(const std::vector<uint32_t> &code, const std::vector<int32_t> &opStarts, size_t opIndex) {
            for (size_t k = opIndex; k-- > 0;) {
                const uint32_t insn = code[opStarts[k]];
                const auto op = static_cast<LuauOpcode>(LUAU_INSN_OP(insn));
                if (op == LOP_NOP || Luau::isFastCall(op))
                    continue;
                if ((op == LOP_CALL || op == LOP_CALLFB) && LUAU_INSN_C(insn) == 0)
                    return LUAU_INSN_A(insn);
                if (op == LOP_GETVARARGS && LUAU_INSN_B(insn) == 0)
                    return LUAU_INSN_A(insn);
                return -1;
            }
            return -1;
        }

        inline Access Describe(const std::vector<uint32_t> &code, const std::vector<int32_t> &opStarts, size_t opIndex) {
            const int32_t pc = opStarts[opIndex];
            const uint32_t insn = code[pc];
            const auto op = static_cast<LuauOpcode>(LUAU_INSN_OP(insn));
            const uint32_t aux = static_cast<size_t>(pc + 1) < code.size() ? code[pc + 1] : 0;
            const int a = LUAU_INSN_A(insn), b = LUAU_INSN_B(insn), c = LUAU_INSN_C(insn);
            Access access;
            auto tail = [&](int from) {
                const int top = MultretTop(code, opStarts, opIndex);
                if (top < from) {
                    access.tailUnknown = true;
                    return;
                }
                AddRange(access.reads, from, top - from + 1);
            };
            switch (op) {
            case LOP_LOADNIL:
            case LOP_LOADB:
            case LOP_LOADN:
            case LOP_LOADK:
            case LOP_LOADKX:
            case LOP_GETGLOBAL:
            case LOP_GETUPVAL:
            case LOP_GETIMPORT:
            case LOP_NEWCLOSURE:
            case LOP_DUPCLOSURE:
            case LOP_NEWTABLE:
            case LOP_DUPTABLE:
                access.writes.push_back(a);
                break;
            case LOP_MOVE:
            case LOP_NOT:
            case LOP_MINUS:
            case LOP_LENGTH:
            case LOP_GETTABLEKS:
            case LOP_GETTABLEN:
            case LOP_GETUDATAKS:
            case LOP_ADDK:
            case LOP_SUBK:
            case LOP_MULK:
            case LOP_DIVK:
            case LOP_MODK:
            case LOP_POWK:
            case LOP_ANDK:
            case LOP_ORK:
            case LOP_IDIVK:
                access.reads.push_back(b);
                access.writes.push_back(a);
                break;
            case LOP_GETTABLE:
            case LOP_ADD:
            case LOP_SUB:
            case LOP_MUL:
            case LOP_DIV:
            case LOP_MOD:
            case LOP_POW:
            case LOP_AND:
            case LOP_OR:
            case LOP_IDIV:
                access.reads.push_back(b);
                access.reads.push_back(c);
                access.writes.push_back(a);
                break;
            case LOP_SUBRK:
            case LOP_DIVRK:
                access.reads.push_back(c);
                access.writes.push_back(a);
                break;
            case LOP_CONCAT:
                AddRange(access.reads, b, c - b + 1);
                access.writes.push_back(a);
                break;
            case LOP_SETGLOBAL:
            case LOP_SETUPVAL:
            case LOP_JUMPIF:
            case LOP_JUMPIFNOT:
            case LOP_JUMPXEQKNIL:
            case LOP_JUMPXEQKB:
            case LOP_JUMPXEQKN:
            case LOP_JUMPXEQKS:
            case LOP_CMPPROTO:
                access.reads.push_back(a);
                break;
            case LOP_SETTABLE:
                access.reads.push_back(a);
                access.reads.push_back(b);
                access.reads.push_back(c);
                break;
            case LOP_SETTABLEKS:
            case LOP_SETTABLEN:
            case LOP_SETUDATAKS:
                access.reads.push_back(a);
                access.reads.push_back(b);
                break;
            case LOP_JUMPIFEQ:
            case LOP_JUMPIFLE:
            case LOP_JUMPIFLT:
            case LOP_JUMPIFNOTEQ:
            case LOP_JUMPIFNOTLE:
            case LOP_JUMPIFNOTLT:
                access.reads.push_back(a);
                access.reads.push_back(static_cast<int>(aux & 0xFF));
                break;
            case LOP_NAMECALL:
            case LOP_NAMECALLUDATA:
                access.reads.push_back(b);
                access.writes.push_back(a);
                access.writes.push_back(a + 1);
                break;
            case LOP_CALL:
            case LOP_CALLFB:
                access.reads.push_back(a);
                if (b > 0)
                    AddRange(access.reads, a + 1, b - 1);
                else
                    tail(a + 1);
                if (c == 0)
                    access.writes.push_back(a);
                else
                    AddRange(access.writes, a, c - 1);
                break;
            case LOP_RETURN:
                if (b > 0)
                    AddRange(access.reads, a, b - 1);
                else
                    tail(a);
                break;
            case LOP_SETLIST:
                access.reads.push_back(a);
                if (c > 0)
                    AddRange(access.reads, b, c - 1);
                else
                    tail(b);
                break;
            case LOP_GETVARARGS:
                if (b == 0)
                    access.writes.push_back(a);
                else
                    AddRange(access.writes, a, b - 1);
                break;
            case LOP_FORNPREP:
            case LOP_FORGPREP:
            case LOP_FORGPREP_INEXT:
            case LOP_FORGPREP_NEXT:
                AddRange(access.reads, a, 3);
                break;
            case LOP_FORNLOOP:
                AddRange(access.reads, a, 3);
                access.writes.push_back(a + 2);
                break;
            case LOP_FORGLOOP:
                access.reads.push_back(a);
                AddRange(access.writes, a + 2, 1 + static_cast<int>(aux & 0xFF));
                break;
            case LOP_CAPTURE:
                if (a == LCT_VAL || a == LCT_REF)
                    access.reads.push_back(b);
                break;
            case LOP_NEWCLASS:
                if (b != 0xFF)
                    access.reads.push_back(b);
                access.writes.push_back(a);
                break;
            case LOP_NEWCLASSMEMBER:
                access.reads.push_back(a);
                access.reads.push_back(c);
                break;
            default:
                break;
            }
            return access;
        }

        inline std::vector<int32_t> Successors(const std::vector<uint32_t> &code, int32_t pc) {
            const uint32_t insn = code[pc];
            const auto op = static_cast<LuauOpcode>(LUAU_INSN_OP(insn));
            const int32_t next = pc + Luau::getOpLength(op);
            const int32_t target = Luau::isFastCall(op) ? -1 : Luau::getJumpTarget(insn, static_cast<uint32_t>(pc));
            switch (op) {
            case LOP_RETURN:
                return {};
            case LOP_JUMP:
            case LOP_JUMPBACK:
            case LOP_JUMPX:
            case LOP_FORGPREP:
            case LOP_FORGPREP_INEXT:
            case LOP_FORGPREP_NEXT:
                return {target};
            case LOP_LOADB:
                return {target >= 0 ? target : next};
            default:
                if (target >= 0)
                    return {next, target};
                return {next};
            }
        }

        inline void Union(std::vector<int32_t> &into, const std::vector<int32_t> &from, bool &changed) {
            if (from.empty())
                return;
            std::vector<int32_t> merged;
            merged.reserve(into.size() + from.size());
            std::ranges::set_union(into, from, std::back_inserter(merged));
            if (merged.size() != into.size()) {
                into = std::move(merged);
                changed = true;
            }
        }

        inline std::string Format(const std::vector<int32_t> &defs) {
            std::string text = "{";
            for (size_t i = 0; i < defs.size(); ++i) {
                if (i)
                    text += ",";
                text += defs[i] == kEntry ? std::string("entry") : defs[i] == kUnresolved ? std::string("unresolved") : std::format("pc{}", defs[i]);
            }
            return text + "}";
        }

        inline void Flatten(const AnalyzedFunction &fn, int reg, int32_t version, std::set<std::pair<int, int32_t>> &visited, std::set<int32_t> &out) {
            if (version < 0 || !visited.insert({reg, version}).second)
                return;
            const auto it = fn.definitionMap.find(SSARef{reg, version});
            if (it == fn.definitionMap.end() || it->second == nullptr) {
                out.insert(version == 0 ? kEntry : kUnresolved);
                return;
            }
            const LiftedInstruction *def = it->second;
            if (def->operation != LiftedOperation::PHI) {
                out.insert(def->instructionIndex);
                return;
            }
            for (size_t i = 1; i < def->operands.size(); ++i)
                Flatten(fn, reg, def->operands[i].ssaVersion, visited, out);
        }

        inline int ImplicitBase(const LiftedInstruction &inst) {
            if (inst.operands.empty())
                return -1;
            switch (inst.operation) {
            case LiftedOperation::CALL:
            case LiftedOperation::CALLFB:
                return inst.operands[0].value.reg + 1;
            case LiftedOperation::RETURN:
            case LiftedOperation::FORNPREP:
            case LiftedOperation::FORNLOOP:
            case LiftedOperation::FORGPREP:
            case LiftedOperation::FORGPREP_INEXT:
            case LiftedOperation::FORGPREP_NEXT:
                return inst.operands[0].value.reg;
            case LiftedOperation::SETLIST:
            case LiftedOperation::CONCAT:
                return inst.operands.size() > 1 ? inst.operands[1].value.reg : -1;
            default:
                return -1;
            }
        }

        inline std::string Dump(const AnalyzedFunction &fn, const std::string &name) {
            std::string text = "-- SSA " + name + "\n";
            for (const auto &block : fn.basicBlocks) {
                std::string preds, succs;
                for (const uint32_t p : block.predecessors)
                    preds += std::format(" B{}", p);
                for (const uint32_t s : block.successors)
                    succs += std::format(" B{}", s);
                text += std::format("-- B{} preds:{} succs:{}{}\n", block.dwBlockId, preds, succs,
                                    block.loopLatch ? std::format(" latch=B{}", *block.loopLatch) : std::string());
                for (const auto &phi : block.phiNodes) {
                    std::string inputs;
                    for (size_t i = 1; i < phi.operands.size(); ++i)
                        inputs += std::format(" v{}", phi.operands[i].ssaVersion);
                    text += std::format("--     PHI R{}:v{} <-{}\n", phi.operands[0].value.reg, phi.operands[0].ssaVersion, inputs);
                }
                if (!block.lpHead)
                    continue;
                for (const LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst) {
                    std::string operands;
                    for (size_t i = 0; i < inst->operands.size(); ++i) {
                        const auto &operand = inst->operands[i];
                        operands += operand.type == LiftedOperandType::Register ? std::format(" R{}:v{}", operand.value.reg, operand.ssaVersion)
                                                                                : std::format(" #{}", operand.value.imm.n);
                    }
                    text += std::format("--     {:>4} {}{}\n", inst->instructionIndex, OperationToString(inst->operation), operands);
                }
            }
            return text;
        }

        inline void CheckFunction(const AnalyzedFunction &fn, SSAOracleReport &report) {
            const LiftedFunction *lifted = fn.lpLiftedFunction;
            const DeserializedFunction *raw = lifted ? lifted->lpDeserialized : nullptr;
            if (!raw || raw->instructions.empty() || raw->instructions.size() != lifted->instructions.size()) {
                ++report.functionsSkipped;
                return;
            }
            ++report.functions;
            const std::string name = std::format("F{}({})", static_cast<int>(raw->bytecodeId), lifted->name);
            auto add = [&](std::string kind, int32_t pc, int reg, std::string detail) {
                const std::string op = pc >= 0 ? std::string(OperationToString(lifted->instructions[pc].operation)) : "";
                report.mismatches.push_back({std::move(kind), name, pc, reg, op, std::move(detail)});
            };

            std::vector<uint32_t> code;
            code.reserve(raw->instructions.size());
            for (const auto &instruction : raw->instructions)
                code.push_back(instruction.instruction);

            std::vector<int32_t> opStarts;
            std::vector<int32_t> opIndexAt(code.size(), -1);
            for (int32_t pc = 0; pc < static_cast<int32_t>(code.size());) {
                opIndexAt[pc] = static_cast<int32_t>(opStarts.size());
                opStarts.push_back(pc);
                pc += Luau::getOpLength(static_cast<LuauOpcode>(LUAU_INSN_OP(code[pc])));
            }

            std::vector<std::vector<int32_t>> successors(opStarts.size());
            std::vector<bool> leader(opStarts.size(), false);
            leader[0] = true;
            for (size_t k = 0; k < opStarts.size(); ++k) {
                for (const int32_t target : Successors(code, opStarts[k])) {
                    if (target < 0 || target >= static_cast<int32_t>(code.size()) || opIndexAt[target] < 0) {
                        ++report.functionsSkipped;
                        return;
                    }
                    successors[k].push_back(opIndexAt[target]);
                }
                const bool fallsThroughOnly = successors[k].size() == 1 && successors[k][0] == static_cast<int32_t>(k + 1);
                if (!fallsThroughOnly) {
                    for (const int32_t s : successors[k])
                        leader[s] = true;
                    if (k + 1 < opStarts.size())
                        leader[k + 1] = true;
                }
            }

            std::vector<int32_t> blockOf(opStarts.size());
            std::vector<std::pair<size_t, size_t>> blocks;
            for (size_t k = 0; k < opStarts.size(); ++k) {
                if (leader[k])
                    blocks.push_back({k, k});
                blocks.back().second = k;
                blockOf[k] = static_cast<int32_t>(blocks.size() - 1);
            }

            std::vector<Access> accesses(opStarts.size());
            for (size_t k = 0; k < opStarts.size(); ++k)
                accesses[k] = Describe(code, opStarts, k);

            const int regCount = raw->maxstacksize + 1;
            using State = std::vector<std::vector<int32_t>>;
            std::vector<State> in(blocks.size(), State(regCount));
            std::vector<bool> reached(blocks.size(), false);
            for (auto &defs : in[0])
                defs = {kEntry};
            reached[0] = true;

            auto transfer = [&](size_t block, State state) {
                for (size_t k = blocks[block].first; k <= blocks[block].second; ++k)
                    for (const int reg : accesses[k].writes)
                        if (reg >= 0 && reg < regCount)
                            state[reg] = {opStarts[k]};
                return state;
            };

            std::vector<size_t> work{0};
            std::vector<bool> queued(blocks.size(), false);
            queued[0] = true;
            while (!work.empty()) {
                const size_t block = work.back();
                work.pop_back();
                queued[block] = false;
                const State out = transfer(block, in[block]);
                for (const int32_t s : successors[blocks[block].second]) {
                    const size_t succ = static_cast<size_t>(blockOf[s]);
                    bool changed = !reached[succ];
                    reached[succ] = true;
                    for (int r = 0; r < regCount; ++r)
                        Union(in[succ][r], out[r], changed);
                    if (changed && !queued[succ]) {
                        queued[succ] = true;
                        work.push_back(succ);
                    }
                }
            }

            std::vector<const LiftedInstruction *> fissionAt(code.size(), nullptr);
            std::vector<const BasicBlock *> fissionBlockAt(code.size(), nullptr);
            for (const auto &block : fn.basicBlocks)
                if (block.lpHead)
                    for (const LiftedInstruction *inst = block.lpHead; inst <= block.lpTail; ++inst)
                        if (inst->instructionIndex >= 0 && inst->instructionIndex < static_cast<int32_t>(code.size())) {
                            fissionAt[inst->instructionIndex] = inst;
                            fissionBlockAt[inst->instructionIndex] = &block;
                        }

            // Fission's CFG sends the latch back into the FOR*PREP block; the prep's read of that block's
            // phi denotes the entry value (the loop variable binding shares the version).
            auto flattenRead = [&](const LiftedInstruction &inst, const BasicBlock &block, int reg, int32_t version, std::set<int32_t> &leaves) {
                std::set<std::pair<int, int32_t>> visited;
                const auto def = fn.definitionMap.find(SSARef{reg, version});
                const bool loopPrep = inst.operation == LiftedOperation::FORNPREP || inst.operation == LiftedOperation::FORGPREP ||
                                      inst.operation == LiftedOperation::FORGPREP_INEXT || inst.operation == LiftedOperation::FORGPREP_NEXT;
                if (loopPrep && block.loopLatch && def != fn.definitionMap.end() &&
                    std::ranges::any_of(block.phiNodes, [&](const LiftedInstruction &phi) { return &phi == def->second; })) {
                    visited.insert({reg, version});
                    for (size_t p = 0; p < block.predecessors.size() && p + 1 < def->second->operands.size(); ++p)
                        if (block.predecessors[p] != *block.loopLatch)
                            Flatten(fn, reg, def->second->operands[p + 1].ssaVersion, visited, leaves);
                    return;
                }
                Flatten(fn, reg, version, visited, leaves);
            };

            for (size_t block = 0; block < blocks.size(); ++block) {
                if (!reached[block])
                    continue;
                State state = in[block];
                for (size_t k = blocks[block].first; k <= blocks[block].second; ++k) {
                    const int32_t pc = opStarts[k];
                    const auto &access = accesses[k];
                    const LiftedInstruction *inst = fissionAt[pc];
                    if (inst && inst->operation != LiftedOperation::NOP) {
                        std::map<int, std::set<int32_t>> versions;
                        for (size_t i = 0; i < inst->operands.size(); ++i) {
                            const auto &operand = inst->operands[i];
                            if (operand.type != LiftedOperandType::Register)
                                continue;
                            const AccessType mode = SSABuilder::GetRegisterAccess(*inst, i);
                            if (mode == AccessType::Read || mode == AccessType::ReadWrite)
                                versions[operand.value.reg].insert(operand.ssaVersion);
                        }
                        if (const auto implicit = fn.implicitUses.find(inst); implicit != fn.implicitUses.end()) {
                            const int base = ImplicitBase(*inst);
                            if (base < 0)
                                add("UNMAPPED_IMPLICIT", pc, -1, "");
                            else
                                for (size_t i = 0; i < implicit->second.size(); ++i)
                                    versions[base + static_cast<int>(i)].insert(implicit->second[i]);
                        }

                        const std::set<int> oracleReads(access.reads.begin(), access.reads.end());
                        for (const int reg : oracleReads) {
                            if (reg < 0 || reg >= regCount)
                                continue;
                            const auto found = versions.find(reg);
                            if (found == versions.end()) {
                                add("MISSING_READ", pc, reg, "oracle=" + Format(state[reg]));
                                continue;
                            }
                            if (found->second.size() != 1) {
                                add("SPLIT_READ", pc, reg, std::format("{} versions", found->second.size()));
                                continue;
                            }
                            std::set<int32_t> leaves;
                            flattenRead(*inst, *fissionBlockAt[pc], reg, *found->second.begin(), leaves);
                            const std::vector<int32_t> fission(leaves.begin(), leaves.end());
                            ++report.readsCompared;
                            if (fission != state[reg]) {
                                std::vector<int32_t> missing, extra;
                                std::ranges::set_difference(state[reg], fission, std::back_inserter(missing));
                                std::ranges::set_difference(fission, state[reg], std::back_inserter(extra));
                                const bool skippedLoadB = extra.empty() && std::ranges::all_of(missing, [&](int32_t def) {
                                    return def >= 0 && LUAU_INSN_OP(code[def]) == LOP_LOADB && LUAU_INSN_C(code[def]) != 0;
                                });
                                // A superset is imprecise but sound; any missing reaching definition is not.
                                add(leaves.contains(kUnresolved) ? "UNRESOLVED"
                                    : skippedLoadB               ? "LOADB_SKIP_LOST"
                                    : missing.empty()            ? "EXTRA_REACHING"
                                    : extra.empty()              ? "MISSING_REACHING"
                                                                 : "WRONG_REACHING",
                                    pc, reg, std::format("v{} fission={} oracle={}", *found->second.begin(), Format(fission), Format(state[reg])));
                            }
                        }
                        if (!access.tailUnknown && !Luau::isFastCall(static_cast<LuauOpcode>(LUAU_INSN_OP(code[pc]))))
                            for (const auto &[reg, _] : versions)
                                if (!oracleReads.contains(reg))
                                    add("EXTRA_READ", pc, reg, "");
                    }
                    for (const int reg : access.writes)
                        if (reg >= 0 && reg < regCount)
                            state[reg] = {pc};
                }
            }
        }

        inline void CheckRecursive(const AnalyzedFunction &fn, SSAOracleReport &report) {
            const size_t before = report.mismatches.size();
            CheckFunction(fn, report);
            if (report.mismatches.size() != before)
                report.dumps += Dump(fn, report.mismatches.back().function);
            for (const auto &inner : fn.innerFunctions)
                CheckRecursive(inner, report);
        }
    } // namespace ssa_oracle

    inline SSAOracleReport CheckSSAAgainstVM(const AnalyzedFunction &root) {
        SSAOracleReport report;
        ssa_oracle::CheckRecursive(root, report);
        return report;
    }
} // namespace fuzz
