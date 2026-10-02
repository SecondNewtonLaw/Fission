//
// Created by Dottik on 1/10/2026.
//

#include "ASTLifter.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Traversal.hpp"

void ASTLifter::StampInlineSources(std::vector<std::shared_ptr<Statement>> &statements) {
    const auto &lifted = *m_currentFunction->lpLiftedFunction;
    const auto at = [](const std::vector<int32_t> &table, int32_t pc, int32_t fallback) {
        return pc >= 0 && static_cast<size_t>(pc) < table.size() ? table[pc] : fallback;
    };
    std::vector<std::shared_ptr<Statement>> rebuilt;
    rebuilt.reserve(statements.size());
    int32_t lastCall = -1;
    for (auto &statement : statements) {
        if (!statement) {
            rebuilt.push_back(std::move(statement));
            continue;
        }
        if (statement->originPc >= 0)
            statement->inlineSource = at(lifted.inlineOrigin, statement->originPc, -1);
        ForEachChildBlock(statement, [&](std::vector<std::shared_ptr<Statement>> &body) { StampInlineSources(body); });
        const int32_t call = at(lifted.recoveredCallAt, statement->originPc, -1);
        if (call >= 0 && call != lastCall && m_inlineSources) {
            const auto &source = m_inlineSources->sources[lifted.recoveredCalls[call]];
            rebuilt.push_back(std::make_shared<CommentNode>(
                std::format("Fission: INFO: call to {} recovered from its inlined body",
                            source.function->debugName ? std::format("'{}'", *source.function->debugName) : std::string("an anonymous function")),
                true, true
            ));
        }
        lastCall = call;
        rebuilt.push_back(std::move(statement));
    }
    statements = std::move(rebuilt);
}
