//
// Created by Dottik on 1/12/2025.
//

#pragma once
#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "AbstractSyntaxTree/Nodes/FunctionDeclarationNode.hpp"
#include "AbstractSyntaxTree/Nodes/RootNode.hpp"
#include "ControlFlowAnalyzer.hpp"
#include "Deserializer.hpp"

#include <cstdint>
#include <format>
#include <stack>
#include <string>
#include <unordered_set>

class IRLifter {
  private:
    std::unordered_set<uint32_t> visited;

    RootNode m_Root;
    const DeserializedFunction *m_lpDeserialized = nullptr;
    const AnalyzedFunction *m_lpAnalyzed = nullptr;
    const LiftedFunction *m_lpLifted = nullptr;

    std::vector<std::shared_ptr<Identifier>> m_registers;
    std::stack<std::shared_ptr<Expression>> m_expressionStack;

    std::shared_ptr<Identifier> Reg(uint8_t r) {
        if (r >= m_registers.size())
            m_registers.resize(r + 16);
        if (!m_registers[r]) {
            m_registers[r] = std::make_shared<Identifier>(r == 0 ? "self" : std::format("v{}", r));
        }
        return m_registers[r];
    }

    void Push(std::shared_ptr<Expression> e) { m_expressionStack.push(std::move(e)); }

    std::shared_ptr<Expression> Pop() {
        auto t = std::move(m_expressionStack.top());
        m_expressionStack.pop();
        return t;
    }

    std::shared_ptr<Expression> Const(uint32_t kidx) {
        const auto &k = m_lpDeserialized->constants[kidx];
        switch (k.kType) {
        case LUA_TNIL:
            return std::make_shared<NilLiteralNode>();
        case LUA_TBOOLEAN:
            return std::make_shared<BooleanLiteralNode>(std::get<bool>(k.constantData));
        case LUA_TNUMBER:
            return std::make_shared<NumberLiteralNode>(std::get<double>(k.constantData));
        case LUA_TSTRING:
            return std::make_shared<StringLiteralNode>(std::get<std::string>(k.constantData));
        case LUA_TVECTOR: {
            // auto &v = std::get<LuauVector>(k.constantData);
            // return std::make_shared<CallExpressionNode>(
            //     std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>("Vector3")),
            //     std::vector<std::shared_ptr<Expression>>{
            //         std::make_shared<NumberLiteralNode>(v.x), std::make_shared<NumberLiteralNode>(v.y), std::make_shared<NumberLiteralNode>(v.z)
            //     }
            // );
        }
        default:
            return std::make_shared<CommentNode>("<!-- complex constant -->");
        }
    }

    void LiftBlock(const BasicBlock *bb) {
        if (!bb || bb->bType == BlockType::Dead)
            return;
    }

    std::string GetFunctionName(const AnalyzedFunction *lpAnalyzed) {
        if (lpAnalyzed->lpLiftedFunction->lpDeserialized->debugName)
            return *lpAnalyzed->lpLiftedFunction->lpDeserialized->debugName;
        if (lpAnalyzed->lpLiftedFunction->lpDeserialized->bytecodeId == 0)
            return "_start";
        return std::format("f{}", lpAnalyzed->lpLiftedFunction->lpDeserialized->bytecodeId);
    }

    std::shared_ptr<FunctionDeclarationNode> LiftFunction(const AnalyzedFunction *lpAnalyzed) {
        auto func = std::make_shared<FunctionDeclarationNode>();
        func->name = std::make_shared<Identifier>(this->GetFunctionName(lpAnalyzed));
        func->body = std::make_shared<BlockStatementNode>();

        // TODO: lift block by block then compose the high level structure back.
        return func;
    }

  public:
    IRLifter(const AnalyzedFunction *analyzed) {
        m_lpAnalyzed = analyzed;
        m_lpLifted = analyzed->lpLiftedFunction;
        m_lpDeserialized = m_lpLifted->lpDeserialized;
        m_registers.resize(m_lpDeserialized->maxstacksize + 32);
    }

    RootNode *Lift() {
        // func->name = std::make_shared<Identifier>(m_lpLifted->name);
        // func->body = std::make_shared<BlockStatementNode>();
        auto func = LiftFunction(this->m_lpAnalyzed);
        return &m_Root;
    }
};