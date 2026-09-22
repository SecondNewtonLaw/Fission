//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include "BytecodeLifter.hpp"
#include "DenominatorAnalysis.hpp"
#include "FissionDebugNotes.hpp"

#include <map>
#include <set>
#include <stack>

enum class AccessType { NoAccess, Read, Write, ReadWrite, Deferred /* calculated during second pass */ };

class SSABuilder {
    FissionDebugNotes *m_debugNotes = nullptr;
    std::string m_debugFunction;

    template <typename... Args> void Explain(BasicBlock &block, std::format_string<Args...> format, Args &&...args) const {
        if (!m_debugNotes || !m_debugNotes->Enabled())
            return;
        m_debugNotes->AddBlock(FissionDebugStage::SSA, m_debugFunction, block.dwBlockId, block.analysisNotes,
                               std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args> void ExplainDetail(BasicBlock &block, std::format_string<Args...> format, Args &&...args) const {
        if (m_debugNotes && m_debugNotes->Enabled())
            m_debugNotes->AddBlock(FissionDebugStage::SSA, m_debugFunction, block.dwBlockId, block.analysisNotes,
                                   std::format(format, std::forward<Args>(args)...), false);
    }

    template <typename... Args> void Explain(std::format_string<Args...> format, Args &&...args) const {
        if (m_debugNotes)
            m_debugNotes->Add(FissionDebugStage::SSA, format, std::forward<Args>(args)...);
    }

    /*
     *  Stack of active SSA versions.
     */
    std::vector<std::vector<int>> versionStack;
    /*
     *  RegID to next available SSA version.
     */
    std::vector<int> versionCounter;
    /*
     *  Block x register: header phis that exist only for the FOR*PREP read, which runs on loop entry.
     */
    std::vector<std::vector<bool>> entryOnlyPhis;

    int32_t NewVersion(int32_t reg);

    int32_t CurrentVersion(int32_t reg);

    void CreatePhiNodes(AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo);

    std::vector<int> RenameBlock(int blockId, AnalyzedFunction &func);
    void Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo);

  public:
    static AccessType GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex);
    static std::vector<int> GetImplicitDefinitions(const LiftedInstruction &inst);

    void Build(AnalyzedFunction &func);
    void SetDebugNotes(FissionDebugNotes *debugNotes) { m_debugNotes = debugNotes; }
};
