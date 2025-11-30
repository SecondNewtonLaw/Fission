//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include "BytecodeLifter.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "DenominatorAnalysis.hpp"

#include <map>
#include <set>
#include <stack>

enum class AccessType { NoAccess, Read, Write, ReadWrite };

class SSABuilder {
    /*
     *  Stack of active SSA versions.
     */
    std::map<int, std::stack<int>> versionStack;
    /*
     *  RegID to next available SSA version.
     */
    std::map<int, int> versionCounter;

    /*
     *  Used for tracking Phi insertions (Register ID -> Set<Block Ids>)
     */
    std::map<int, std::set<int>> blocksDefining;

    std::map<int, std::set<int>> globals;

    static AccessType GetRegisterAccess(const LiftedInstruction &op, size_t operandIndex);

    int32_t NewVersion(int32_t reg);

    int32_t CurrentVersion(int32_t reg);

    void CreatePhiNodes(AnalyzedFunction *lpOriginalFunction, const std::map<int32_t, DominatorInfo> &domInfo);

    void Rename(int blockId, AnalyzedFunction &func, const std::map<int32_t, DominatorInfo> &domInfo);

  public:
    void Build(AnalyzedFunction &func);
};