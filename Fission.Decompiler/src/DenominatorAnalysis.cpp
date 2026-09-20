#include "DenominatorAnalysis.hpp"

std::map<int32_t, DominatorInfo> AnalyzeDenominators(const AnalyzedFunction &func) {
    const auto &blocks = func.basicBlocks;
    if (blocks.empty())
        return {};

    std::unordered_map<int32_t, int> idToIdx;
    for (int i = 0; i < (int)blocks.size(); i++)
        idToIdx[blocks[i].dwBlockId] = i;

    int32_t entryId = blocks[0].dwBlockId;

    const int Nmax = blocks.size();

    std::vector<int> dfsParent(Nmax, -1);
    std::vector<int> vertex(Nmax, -1);
    std::vector<int> parent(Nmax, -1);
    std::vector<int> semi(Nmax);
    std::vector<int> idom(Nmax, -1);
    std::vector<int> ancestor(Nmax, -1);
    std::vector<int> best(Nmax, -1);
    std::vector<std::vector<int>> bucket(Nmax);

    std::vector<int> dfsNumber(Nmax, -1);
    int dfsTime = 0;

    // recursion overflows the stack on CFGs with hundreds of blocks; walk with an explicit stack.
    {
        struct Frame {
            int idx;
            size_t succIdx;
        };
        std::vector<Frame> stack;
        auto enter = [&](int idx, int parentIdx) {
            dfsNumber[idx] = dfsTime;
            vertex[dfsTime] = idx;
            semi[dfsTime] = dfsTime;
            best[dfsTime] = dfsTime;
            ancestor[dfsTime] = -1;
            dfsTime++;
            if (parentIdx != -1)
                parent[idx] = parentIdx;
            stack.push_back({idx, 0});
        };
        enter(idToIdx[entryId], -1);
        while (!stack.empty()) {
            auto &top = stack.back();
            const auto &succs = blocks[top.idx].successors;
            bool descended = false;
            while (top.succIdx < succs.size()) {
                int32_t succ = succs[top.succIdx++];
                auto it = idToIdx.find(succ);
                if (it == idToIdx.end())
                    continue;
                int w = it->second;
                if (dfsNumber[w] == -1) {
                    int parentIdx = top.idx;
                    enter(w, parentIdx);
                    descended = true;
                    break;
                }
            }
            if (!descended)
                stack.pop_back();
        }
    }

    const int N = dfsTime;

    auto compress = [&](auto &self, int v) -> void {
        int a = ancestor[v];
        if (ancestor[a] != -1) {
            self(self, a);
            if (semi[best[a]] < semi[best[v]])
                best[v] = best[a];
            ancestor[v] = ancestor[a];
        }
    };

    auto eval = [&](int v) {
        if (ancestor[v] == -1)
            return v;
        compress(compress, v);
        return best[v];
    };

    auto link = [&](int v, int w) { ancestor[w] = v; };

    // every index below is a DFS number or block index derived from the (possibly malformed) CFG;
    // a wild jump target or an inconsistent pred/succ edge can leave a node unreachable (dfsNumber == -1)
    // or a parent unset, so each array access is range-checked rather than trusting the graph shape.
    const auto inRange = [&](int x) { return x >= 0 && x < Nmax; };

    for (int i = N - 1; i >= 1; i--) {
        int w = vertex[i];
        if (!inRange(w))
            continue;
        int wIdx = w;

        for (int32_t predId : blocks[wIdx].predecessors) {
            auto it = idToIdx.find(predId);
            if (it == idToIdx.end())
                continue;

            int v = it->second;
            if (!inRange(v) || dfsNumber[v] == -1)
                continue; // unreachable / out of range

            int u = eval(dfsNumber[v]);
            if (!inRange(u))
                continue;
            semi[i] = std::min(semi[i], semi[u]);
        }

        if (inRange(semi[i]))
            bucket[semi[i]].push_back(i);
        int p = parent[wIdx];
        if (!inRange(p) || dfsNumber[p] < 0)
            continue; // no valid DFS-tree parent: nothing to link or flush

        const int dp = dfsNumber[p];
        link(dp, i);

        for (int v : bucket[dp]) {
            int u = eval(v);
            if (!inRange(u) || !inRange(v))
                continue;
            if (semi[u] < semi[v])
                idom[v] = u;
            else
                idom[v] = dp;
        }
        bucket[dp].clear();
    }

    idom[0] = -1;

    for (int i = 1; i < N; i++) {
        if (idom[i] != semi[i] && idom[i] >= 0 && idom[i] < N)
            idom[i] = idom[idom[i]];
    }

    std::map<int32_t, DominatorInfo> info;

    for (int i = 0; i < N; i++) {
        int blockIdx = vertex[i];
        int32_t blockId = blocks[blockIdx].dwBlockId;

        int idomIdx = idom[i];
        if (idomIdx == -1) {
            info[blockId].idom = -1;
        } else {
            int domBlockIdx = vertex[idomIdx];
            info[blockId].idom = blocks[domBlockIdx].dwBlockId;
        }
    }

    for (auto &[blk, di] : info) {
        if (di.idom != -1)
            info[di.idom].children.push_back(blk);
    }

    for (int i = 0; i < N; i++) {
        int blkIdx = vertex[i];
        int32_t blkId = blocks[blkIdx].dwBlockId;

        const auto &preds = blocks[blkIdx].predecessors;
        if (preds.size() < 2)
            continue;

        for (int32_t pId : preds) {
            if (!info.contains(pId))
                continue;

            int32_t runner = pId;
            while (runner != -1 && runner != info[blkId].idom) {
                info[runner].dominanceFrontier.insert(blkId);
                runner = info[runner].idom;
            }
        }
    }

    return info;
}
