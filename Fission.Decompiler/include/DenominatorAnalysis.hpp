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

class DominatorAnalyzer {
  public:
    std::map<int32_t, DominatorInfo> Analyze(const AnalyzedFunction &func) {
        std::map<int32_t, DominatorInfo> info;
        const auto &blocks = func.basicBlocks;

        std::map<int32_t, size_t> idToIndex;
        for (size_t i = 0; i < blocks.size(); ++i)
            idToIndex[blocks[i].dwBlockId] = i;

        for (const auto &b : blocks)
            info[b.dwBlockId] = {};

        // compute denominators
        // Dom(n) = {n} U (Intersect(Dom(p) for all p in preds))

        std::map<int32_t, std::set<int32_t>> domSets;
        std::vector<int32_t> allBlockIds;
        for (const auto &b : blocks)
            allBlockIds.push_back(b.dwBlockId);

        domSets[0] = {0};
        for (const auto &b : blocks) {
            if (b.dwBlockId != 0)
                domSets[b.dwBlockId] = {allBlockIds.begin(), allBlockIds.end()};
        }

        bool changed = true;
        while (changed) {
            changed = false;
            for (const auto &block : blocks) {
                if (block.dwBlockId == 0)
                    continue; // skip entry block, we don't want to modify it.

                std::set<int32_t> newDom;
                bool firstPred = true;

                for (int32_t predId : block.predecessors) {
                    // unreachable predecessor (dead code)
                    if (!domSets.contains(predId))
                        continue;

                    if (firstPred) {
                        newDom = domSets[predId];
                        firstPred = false;
                    } else {
                        // set intersection
                        std::set<int32_t> intersection;
                        std::ranges::set_intersection(newDom, domSets[predId], std::inserter(intersection, intersection.begin()));
                        newDom = intersection;
                    }
                }

                // self
                newDom.insert(block.dwBlockId);

                // change
                if (newDom != domSets[block.dwBlockId]) {
                    domSets[block.dwBlockId] = newDom;
                    changed = true;
                }
            }
        }

        // compute IDOM, which is the denominator closest to this node.
        for (const auto &block : blocks) {
            if (block.dwBlockId == 0)
                continue;

            int32_t currentId = block.dwBlockId;
            const auto &dominators = domSets[currentId];

            int32_t bestIdom = -1;
            size_t maxDomSize = 0;

            for (int32_t domId : dominators) {
                if (domId == currentId)
                    continue; // cannot be self.

                if (domSets[domId].size() > maxDomSize) {
                    maxDomSize = domSets[domId].size();
                    bestIdom = domId;
                }
            }

            info[currentId].idom = bestIdom;
            if (bestIdom != -1) {
                info[bestIdom].children.push_back(currentId);
            }
        }

        for (const auto &block : blocks) {
            if (block.predecessors.size() >= 2) {
                for (const int32_t predId : block.predecessors) {
                    int32_t runner = predId;

                    while (runner != info[block.dwBlockId].idom && runner != -1) {
                        info[runner].dominanceFrontier.insert(block.dwBlockId);
                        runner = info[runner].idom;
                    }
                }
            }
        }

        return info;
    }
};