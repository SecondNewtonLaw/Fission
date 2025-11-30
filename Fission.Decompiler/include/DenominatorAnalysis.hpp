//
// Created by Dottik on 30/11/2025.
//

#pragma once
#include "ControlFlowAnalyzer.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <vector>

struct DominatorInfo {
    int32_t idom = -1; // immediate denominator (parent)
    std::vector<int32_t> children;
    std::set<int32_t> dominanceFrontier;
};

std::map<int32_t, DominatorInfo> AnalyzeDenominators(const AnalyzedFunction &func);