#pragma once

#include "ASTLifter.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

class RobloxTypeInferer : public Visitor {
  public:
    void Infer(ASTFunction &ast, bool inferTypes = true, bool autoNameVariables = false);

    void Visit(IntegerLiteralNode *lpNode) override;
    void Visit(RootNode *lpNode) override;
    void Visit(Identifier *lpNode) override;
    void Visit(FunctionDeclarationNode *lpNode) override;
    void Visit(CommentNode *lpNode) override;
    void Visit(CallExpressionNode *lpNode) override;
    void Visit(UnaryExpressionNode *lpNode) override;
    void Visit(IndexExpressionNode *lpNode) override;
    void Visit(MemberExpressionNode *lpNode) override;
    void Visit(ReturnStatementNode *lpNode) override;
    void Visit(ExpressionStatementNode *lpNode) override;
    void Visit(BreakStatementNode *lpNode) override;
    void Visit(ContinueStatementNode *lpNode) override;
    void Visit(BlockStatementNode *lpNode) override;
    void Visit(WhileStatementNode *lpNode) override;
    void Visit(IfStatementNode *lpNode) override;
    void Visit(AssignmentStatementNode *lpNode) override;
    void Visit(TableBinaryExpressionNode *lpNode) override;
    void Visit(BinaryExpressionNode *lpNode) override;
    void Visit(StringLiteralNode *lpNode) override;
    void Visit(NumberLiteralNode *lpNode) override;
    void Visit(BooleanLiteralNode *lpNode) override;
    void Visit(IdentifierExpressionNode *lpNode) override;
    void Visit(VariableDeclarationNode *lpNode) override;
    void Visit(NilLiteralNode *lpNode) override;
    void Visit(TableLiteralNode *lpNode) override;
    void Visit(NoExpressionNode *lpNode) override;
    void Visit(NameCallExpressionNode *lpNode) override;
    void Visit(ForNumericNode *lpNode) override;
    void Visit(ForGeneralNode *lpNode) override;
    void Visit(CompoundBinaryExpressionNode *lpNode) override;
    void Visit(RepeatStatementNode *lpNode) override;
    void Visit(VarArgExpression *lpNode) override;
    void Visit(FunctionArgumentExpression *lpNode) override;
    void Visit(VectorNode *lpNode) override;

  private:
    using TypeEnv = std::unordered_map<std::string, std::string>;

    TypeEnv m_env;
    std::unordered_map<std::string, std::string> m_renames;
    std::set<std::string> m_names;
    std::set<std::string> m_autoNames;
    int32_t m_autoNameCounter = 0;
    bool m_inferTypes = true;
    bool m_autoNameVariables = false;

    static std::shared_ptr<Expression> MakeTypeAnnotation(const std::string &typeName);
    static std::optional<std::string> IdentifierName(const std::shared_ptr<Expression> &expr);
    static std::optional<std::string> StringLiteralValue(const std::shared_ptr<Expression> &expr);
    static std::optional<std::string> MemberKeyName(const std::shared_ptr<Expression> &expr);
    static std::optional<std::string> ClassArgument(const std::vector<std::shared_ptr<Expression>> &args, size_t index);
    static std::optional<std::string> CallReturnType(const std::string &methodName, const std::vector<std::shared_ptr<Expression>> &args, size_t classArgIndex);
    static std::optional<std::string> CallAutoName(const std::string &methodName, const std::vector<std::shared_ptr<Expression>> &args, size_t classArgIndex);
    static std::string SanitizeIdentifier(std::string name);
    static void AnnotateCallReturn(CallExpressionNode *call, const std::optional<std::string> &type, TypeEnv &env);
    static void AnnotateCallReturn(NameCallExpressionNode *call, const std::optional<std::string> &type, TypeEnv &env);

    static std::optional<std::string> GlobalFunctionType(const std::string &name, const std::vector<std::shared_ptr<Expression>> &args);
    static std::optional<std::string> GlobalFunctionAutoName(const std::string &name, const std::vector<std::shared_ptr<Expression>> &args);
    static std::optional<std::string> LibraryReceiverMethodType(const std::string &receiverType, const std::string &methodName);
    static std::optional<std::string> LiteralType(const std::shared_ptr<Expression> &expr);

    std::optional<std::string> ExpressionType(const std::shared_ptr<Expression> &expr, const TypeEnv &env);
    std::optional<std::string> ExpressionAutoName(const std::shared_ptr<Expression> &expr);
    std::string ResolveAutoName(const std::string &currentName, const std::string &wantedName);
    void RenameIdentifier(const std::shared_ptr<Expression> &expr, const std::string &name);
    void RegisterExistingNames(const std::vector<std::shared_ptr<Statement>> &stmts);
    void VisitNode(const std::shared_ptr<ASTNode> &node);
    void VisitStatementList(const std::vector<std::shared_ptr<Statement>> &stmts, const TypeEnv &env);
};
