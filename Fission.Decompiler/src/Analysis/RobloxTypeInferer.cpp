#include "Analysis/RobloxTypeInferer.hpp"

#include "AbstractSyntaxTree/Nodes/RootNode.hpp"

#include <cctype>
#include <format>

std::shared_ptr<Expression> RobloxTypeInferer::MakeTypeAnnotation(const std::string &typeName) {
    return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(typeName));
}

std::optional<std::string> RobloxTypeInferer::IdentifierName(const std::shared_ptr<Expression> &expr) {
    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
        return id->identifier->name;
    if (auto id = std::dynamic_pointer_cast<Identifier>(expr))
        return id->name;
    return std::nullopt;
}

// An auto-generated register/loop/closure name (v3, uv_0, arg1, a2, i_4, anon_5_0)
// is a runtime value, never a type. Using it as a type annotation produces invalid
// Luau (`local x: v2 = ...`), so callers must reject these as type names.
static bool IsGeneratedName(const std::string &name) {
    auto digitsFrom = [&](size_t i) { return i < name.size() && std::isdigit(static_cast<unsigned char>(name[i])); };
    if (name.rfind("uv_", 0) == 0)
        return digitsFrom(3);
    if (name.rfind("arg", 0) == 0)
        return digitsFrom(3);
    if (name.rfind("anon_", 0) == 0)
        return digitsFrom(5);
    if (name.rfind("i_", 0) == 0)
        return digitsFrom(2);
    if (name.size() >= 2 && (name[0] == 'v' || name[0] == 'a')) {
        for (size_t i = 1; i < name.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    }
    return false;
}

std::optional<std::string> RobloxTypeInferer::StringLiteralValue(const std::shared_ptr<Expression> &expr) {
    if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(expr))
        return str->value;
    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::MemberKeyName(const std::shared_ptr<Expression> &expr) {
    if (auto str = std::dynamic_pointer_cast<StringLiteralNode>(expr))
        return str->value;
    return IdentifierName(expr);
}

std::optional<std::string> RobloxTypeInferer::ClassArgument(const std::vector<std::shared_ptr<Expression>> &args, size_t index) {
    if (args.size() <= index)
        return std::nullopt;
    return StringLiteralValue(args[index]);
}

std::optional<std::string>
RobloxTypeInferer::CallReturnType(const std::string &methodName, const std::vector<std::shared_ptr<Expression>> &args, size_t classArgIndex) {
    if (methodName == "FindFirstChild" || methodName == "WaitForChild" || methodName == "FindFirstAncestor")
        return "Instance";
    if (methodName == "FindFirstChildOfClass" || methodName == "FindFirstChildWhichIsA" || methodName == "GetService" ||
        methodName == "FindFirstAncestorOfClass" || methodName == "FindFirstAncestorWhichIsA")
        return ClassArgument(args, classArgIndex).value_or("Instance");

    if (methodName == "lower" || methodName == "upper" || methodName == "rep" || methodName == "reverse")
        return "string";
    if (methodName == "format" || methodName == "char" || methodName == "pack")
        return "string";
    if (methodName == "split")
        return "{string}";
    if (methodName == "byte" || methodName == "len" || methodName == "packsize")
        return "number";
    if (methodName == "find")
        return "number";
    if (methodName == "match" || methodName == "sub")
        return "string";

    if (methodName == "abs" || methodName == "ceil" || methodName == "floor" || methodName == "round" || methodName == "sqrt")
        return "number";
    if (methodName == "sin" || methodName == "cos" || methodName == "tan" || methodName == "asin" || methodName == "acos")
        return "number";
    if (methodName == "atan" || methodName == "atan2" || methodName == "log" || methodName == "log10" || methodName == "exp")
        return "number";
    if (methodName == "min" || methodName == "max" || methodName == "pow" || methodName == "sign" || methodName == "clamp")
        return "number";
    if (methodName == "rad" || methodName == "deg" || methodName == "noise" || methodName == "ldexp")
        return "number";
    if (methodName == "random")
        return "number";

    if (methodName == "insert" || methodName == "remove" || methodName == "sort" || methodName == "clear")
        return std::nullopt;
    if (methodName == "create" || methodName == "freeze" || methodName == "clone" || methodName == "pack")
        return "table";
    if (methodName == "find")
        return "number";
    if (methodName == "keys" || methodName == "values")
        return "table";
    if (methodName == "concat")
        return "string";
    if (methodName == "maxn" || methodName == "getn")
        return "number";

    if (methodName == "clock" || methodName == "time" || methodName == "difftime")
        return "number";
    if (methodName == "date")
        return "string";

    if (methodName == "create" || methodName == "wrap")
        return "thread";
    if (methodName == "resume")
        return "boolean";
    if (methodName == "status" || methodName == "running")
        return "string";
    if (methodName == "isyieldable")
        return "boolean";

    if (methodName == "band" || methodName == "bor" || methodName == "bxor" || methodName == "bnot")
        return "number";
    if (methodName == "lshift" || methodName == "rshift" || methodName == "arshift")
        return "number";
    if (methodName == "lrotate" || methodName == "rrotate" || methodName == "extract" || methodName == "replace")
        return "number";
    if (methodName == "countlz" || methodName == "countrz")
        return "number";
    if (methodName == "btest")
        return "boolean";

    if (methodName == "len" || methodName == "create")
        return "number";
    if (methodName == "tostring")
        return "string";
    if (methodName == "fromstring")
        return "buffer";
    if (methodName == "read")
        return "number";

    return std::nullopt;
}

std::optional<std::string>
RobloxTypeInferer::CallAutoName(const std::string &methodName, const std::vector<std::shared_ptr<Expression>> &args, size_t classArgIndex) {
    if (methodName == "FindFirstChild" || methodName == "WaitForChild" || methodName == "FindFirstAncestor")
        return ClassArgument(args, classArgIndex);
    if (methodName == "FindFirstChildOfClass" || methodName == "FindFirstChildWhichIsA" || methodName == "GetService" ||
        methodName == "FindFirstAncestorOfClass" || methodName == "FindFirstAncestorWhichIsA")
        return ClassArgument(args, classArgIndex);
    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::GlobalFunctionType(const std::string &name, const std::vector<std::shared_ptr<Expression>> &args) {
    if (name == "assert")
        return "boolean";
    if (name == "type" || name == "typeof")
        return "string";
    if (name == "tostring")
        return "string";
    if (name == "tonumber")
        return "number";
    if (name == "rawequal" || name == "rawget")
        return "boolean";
    if (name == "rawlen")
        return "number";
    if (name == "setmetatable") {
        // `setmetatable(x, MyClass)` is conventionally typed as the class, but only
        // when the metatable is a real named identifier — an auto-generated local
        // (v2, uv_0, ...) is a value, not a type, and must fall back to `table`.
        if (args.size() >= 2)
            if (auto secondType = IdentifierName(args[1]); secondType && !IsGeneratedName(*secondType))
                return *secondType;
        return "table";
    }
    if (name == "require" || name == "newproxy")
        return "table";
    if (name == "next" || name == "pairs" || name == "ipairs")
        return "function";
    if (name == "loadstring")
        return "function";
    if (name == "collectgarbage" || name == "gcinfo")
        return "number";
    if (name == "getfenv")
        return "table";
    (void)args;
    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::GlobalFunctionAutoName(const std::string &name, const std::vector<std::shared_ptr<Expression>> &args) {
    (void)name;
    (void)args;
    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::LibraryReceiverMethodType(const std::string &receiverType, const std::string &methodName) {
    if (receiverType == "string") {
        if (methodName == "lower" || methodName == "upper" || methodName == "rep" || methodName == "reverse")
            return "string";
        if (methodName == "format" || methodName == "sub")
            return "string";
        if (methodName == "len" || methodName == "byte")
            return "number";
        if (methodName == "find" || methodName == "match")
            return "string";
        if (methodName == "gsub" || methodName == "gmatch")
            return "function";
        if (methodName == "split")
            return "{string}";
    }
    if (receiverType == "number") {
        (void)methodName;
    }
    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::LiteralType(const std::shared_ptr<Expression> &expr) {
    if (std::dynamic_pointer_cast<StringLiteralNode>(expr))
        return "string";
    if (std::dynamic_pointer_cast<NumberLiteralNode>(expr))
        return "number";
    if (std::dynamic_pointer_cast<IntegerLiteralNode>(expr))
        return "integer";
    if (std::dynamic_pointer_cast<BooleanLiteralNode>(expr))
        return "boolean";
    if (std::dynamic_pointer_cast<VectorNode>(expr))
        return "Vector3";
    if (std::dynamic_pointer_cast<NilLiteralNode>(expr))
        return "nil";
    return std::nullopt;
}

std::string RobloxTypeInferer::SanitizeIdentifier(std::string name) {
    for (auto &ch : name)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
            ch = '_';
    if (name.empty())
        return {};
    if (std::isdigit(static_cast<unsigned char>(name.front())))
        name.insert(name.begin(), '_');
    return name;
}

void RobloxTypeInferer::AnnotateCallReturn(CallExpressionNode *call, const std::optional<std::string> &type, TypeEnv &env) {
    if (!type || call->rets.empty())
        return;
    if (call->retTypes.size() < call->rets.size())
        call->retTypes.resize(call->rets.size());
    call->retTypes[0] = MakeTypeAnnotation(*type);
    if (auto retName = IdentifierName(call->rets[0]))
        env[*retName] = *type;
}

void RobloxTypeInferer::AnnotateCallReturn(NameCallExpressionNode *call, const std::optional<std::string> &type, TypeEnv &env) {
    if (!type || call->rets.empty())
        return;
    if (call->retTypes.size() < call->rets.size())
        call->retTypes.resize(call->rets.size());
    call->retTypes[0] = MakeTypeAnnotation(*type);
    if (auto retName = IdentifierName(call->rets[0]))
        env[*retName] = *type;
}

std::optional<std::string> RobloxTypeInferer::ExpressionType(const std::shared_ptr<Expression> &expr, const TypeEnv &env) {
    if (!expr)
        return std::nullopt;

    if (auto literalType = LiteralType(expr))
        return literalType;

    if (auto name = IdentifierName(expr)) {
        if (*name == "game")
            return "DataModel";
        if (*name == "workspace")
            return "Workspace";
        if (*name == "script")
            return "LuaSourceContainer";
        if (env.contains(*name))
            return env.at(*name);
    }

    if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
        auto tableName = IdentifierName(member->table);
        auto keyName = MemberKeyName(member->key);
        if (tableName && keyName) {
            if (*tableName == "script" && *keyName == "Parent")
                return "Instance";
            if (*tableName == "game" && *keyName == "Workspace")
                return "Workspace";
        }
    }

    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
        if (auto methodName = IdentifierName(nameCall->callWhat)) {
            if (auto result = CallReturnType(*methodName, nameCall->arguments, 0))
                return result;
            if (auto receiverType = ExpressionType(nameCall->calledOn, env))
                if (auto result = LibraryReceiverMethodType(*receiverType, *methodName))
                    return result;
        }
    }

    if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
        if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(call->callee)) {
            if (auto methodName = MemberKeyName(member->key))
                if (auto result = CallReturnType(*methodName, call->arguments, 1))
                    return result;
        }
        if (auto globalId = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callee)) {
            if (auto gname = IdentifierName(globalId))
                if (auto result = GlobalFunctionType(*gname, call->arguments))
                    return result;
        }
    }

    return std::nullopt;
}

std::optional<std::string> RobloxTypeInferer::ExpressionAutoName(const std::shared_ptr<Expression> &expr) {
    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
        if (auto methodName = IdentifierName(nameCall->callWhat))
            if (auto result = CallAutoName(*methodName, nameCall->arguments, 0))
                return result;
    }

    if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
        if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(call->callee)) {
            if (auto methodName = MemberKeyName(member->key))
                if (auto result = CallAutoName(*methodName, call->arguments, 1))
                    return result;
        }
        if (auto globalId = std::dynamic_pointer_cast<IdentifierExpressionNode>(call->callee)) {
            if (auto gname = IdentifierName(globalId))
                if (auto result = GlobalFunctionAutoName(*gname, call->arguments))
                    return result;
        }
    }

    return std::nullopt;
}

std::string RobloxTypeInferer::ResolveAutoName(const std::string &currentName, const std::string &wantedName) {
    auto clean = SanitizeIdentifier(wantedName);
    if (clean.empty())
        return currentName;
    if (currentName == clean && !m_autoNames.contains(clean)) {
        m_autoNames.insert(clean);
        return currentName;
    }
    if (!m_names.contains(clean)) {
        m_names.erase(currentName);
        m_names.insert(clean);
        m_autoNames.insert(clean);
        return clean;
    }

    std::string prefixed;
    do {
        prefixed = std::format("v{}_{}", ++m_autoNameCounter, clean);
    } while (m_names.contains(prefixed));
    m_names.erase(currentName);
    m_names.insert(prefixed);
    m_autoNames.insert(prefixed);
    return prefixed;
}

void RobloxTypeInferer::RenameIdentifier(const std::shared_ptr<Expression> &expr, const std::string &name) {
    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
        id->identifier->name = name;
    else if (auto identifier = std::dynamic_pointer_cast<Identifier>(expr))
        identifier->name = name;
}

void RobloxTypeInferer::RegisterExistingNames(const std::vector<std::shared_ptr<Statement>> &stmts) {
    for (const auto &stmt : stmts) {
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            if (auto name = IdentifierName(decl->identifier))
                m_names.insert(*name);
        } else if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(exprStmt->expression)) {
                for (const auto &ret : call->rets)
                    if (auto name = IdentifierName(ret))
                        m_names.insert(*name);
            } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(exprStmt->expression)) {
                for (const auto &ret : nameCall->rets)
                    if (auto name = IdentifierName(ret))
                        m_names.insert(*name);
            }
        }
    }
}

void RobloxTypeInferer::VisitNode(const std::shared_ptr<ASTNode> &node) {
    if (node)
        node->Accept(this);
}

void RobloxTypeInferer::VisitStatementList(const std::vector<std::shared_ptr<Statement>> &stmts, const TypeEnv &env) {
    auto saved = m_env;
    auto savedRenames = m_renames;
    m_env = env;
    if (m_autoNameVariables)
        RegisterExistingNames(stmts);
    for (const auto &stmt : stmts)
        VisitNode(stmt);
    m_env = saved;
    m_renames = savedRenames;
}

void RobloxTypeInferer::Infer(ASTFunction &ast, bool inferTypes, bool autoNameVariables) {
    m_inferTypes = inferTypes;
    m_autoNameVariables = autoNameVariables;
    VisitStatementList(ast.statements, {});
}

void RobloxTypeInferer::Visit(IntegerLiteralNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(Identifier *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(CommentNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(BreakStatementNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(ContinueStatementNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(StringLiteralNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(NumberLiteralNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(BooleanLiteralNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(IdentifierExpressionNode *lpNode) {
    if (lpNode->identifier && m_renames.contains(lpNode->identifier->name))
        lpNode->identifier->name = m_renames.at(lpNode->identifier->name);
}
void RobloxTypeInferer::Visit(NilLiteralNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(NoExpressionNode *lpNode) { (void)lpNode; }
void RobloxTypeInferer::Visit(VarArgExpression *lpNode) { (void)lpNode; }

void RobloxTypeInferer::Visit(RootNode *lpNode) {
    for (const auto &stmt : lpNode->programBody)
        VisitNode(stmt);
}

void RobloxTypeInferer::Visit(FunctionArgumentExpression *lpNode) { VisitNode(lpNode->argumentName); }
void RobloxTypeInferer::Visit(VectorNode *lpNode) { (void)lpNode; }

void RobloxTypeInferer::Visit(FunctionDeclarationNode *lpNode) {
    if (lpNode->lpFunctionBody)
        VisitStatementList(lpNode->lpFunctionBody->body, m_env);
}

void RobloxTypeInferer::Visit(CallExpressionNode *lpNode) {
    VisitNode(lpNode->callee);
    for (const auto &arg : lpNode->arguments)
        VisitNode(arg);
}

void RobloxTypeInferer::Visit(NameCallExpressionNode *lpNode) {
    VisitNode(lpNode->calledOn);
    VisitNode(lpNode->callWhat);
    for (const auto &arg : lpNode->arguments)
        VisitNode(arg);
}

void RobloxTypeInferer::Visit(UnaryExpressionNode *lpNode) { VisitNode(lpNode->operand); }

void RobloxTypeInferer::Visit(IndexExpressionNode *lpNode) {
    VisitNode(lpNode->left);
    VisitNode(lpNode->right);
}

void RobloxTypeInferer::Visit(MemberExpressionNode *lpNode) {
    VisitNode(lpNode->table);
    VisitNode(lpNode->key);
}

void RobloxTypeInferer::Visit(ReturnStatementNode *lpNode) {
    for (const auto &value : lpNode->returnValues)
        VisitNode(value);
}

void RobloxTypeInferer::Visit(ExpressionStatementNode *lpNode) {
    if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(lpNode->expression)) {
        if (m_inferTypes)
            AnnotateCallReturn(call.get(), ExpressionType(call, m_env), m_env);
        if (m_autoNameVariables && !call->rets.empty())
            if (auto current = IdentifierName(call->rets[0]))
                if (auto wanted = ExpressionAutoName(call)) {
                    auto resolved = ResolveAutoName(*current, *wanted);
                    RenameIdentifier(call->rets[0], resolved);
                    if (resolved != *current && (*current != *wanted || SanitizeIdentifier(*wanted) != *wanted)) {
                        m_renames[*current] = resolved;
                        if (m_env.contains(*current))
                            m_env[resolved] = m_env.at(*current);
                    }
                }
    } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(lpNode->expression)) {
        if (m_inferTypes)
            AnnotateCallReturn(nameCall.get(), ExpressionType(nameCall, m_env), m_env);
        if (m_autoNameVariables && !nameCall->rets.empty())
            if (auto current = IdentifierName(nameCall->rets[0]))
                if (auto wanted = ExpressionAutoName(nameCall)) {
                    auto resolved = ResolveAutoName(*current, *wanted);
                    RenameIdentifier(nameCall->rets[0], resolved);
                    if (resolved != *current && (*current != *wanted || SanitizeIdentifier(*wanted) != *wanted)) {
                        m_renames[*current] = resolved;
                        if (m_env.contains(*current))
                            m_env[resolved] = m_env.at(*current);
                    }
                }
    }
    VisitNode(lpNode->expression);
}

void RobloxTypeInferer::Visit(BlockStatementNode *lpNode) { VisitStatementList(lpNode->body, m_env); }

void RobloxTypeInferer::Visit(WhileStatementNode *lpNode) {
    VisitNode(lpNode->condition);
    if (lpNode->body)
        VisitStatementList(lpNode->body->body, m_env);
}

void RobloxTypeInferer::Visit(IfStatementNode *lpNode) {
    VisitNode(lpNode->condition);
    if (lpNode->thenBranch)
        VisitStatementList(lpNode->thenBranch->body, m_env);
    if (lpNode->elseBranch)
        VisitStatementList(lpNode->elseBranch->body, m_env);
}

void RobloxTypeInferer::Visit(AssignmentStatementNode *lpNode) {
    if (m_inferTypes)
        if (auto name = IdentifierName(lpNode->left))
            if (auto type = ExpressionType(lpNode->right, m_env))
                m_env[*name] = *type;
    VisitNode(lpNode->left);
    VisitNode(lpNode->right);
}

void RobloxTypeInferer::Visit(TableBinaryExpressionNode *lpNode) { Visit(static_cast<BinaryExpressionNode *>(lpNode)); }

void RobloxTypeInferer::Visit(BinaryExpressionNode *lpNode) {
    VisitNode(lpNode->left);
    VisitNode(lpNode->right);
}

void RobloxTypeInferer::Visit(CompoundBinaryExpressionNode *lpNode) {
    VisitNode(lpNode->left);
    VisitNode(lpNode->right);
}

void RobloxTypeInferer::Visit(VariableDeclarationNode *lpNode) {
    auto type = ExpressionType(lpNode->value, m_env);
    if (m_inferTypes && type) {
        if (!lpNode->type)
            lpNode->type = MakeTypeAnnotation(*type);
        if (auto name = IdentifierName(lpNode->identifier))
            m_env[*name] = *type;
    }
    if (m_autoNameVariables)
        if (auto current = IdentifierName(lpNode->identifier))
            if (auto wanted = ExpressionAutoName(lpNode->value)) {
                auto resolved = ResolveAutoName(*current, *wanted);
                RenameIdentifier(lpNode->identifier, resolved);
                if (resolved != *current && (*current != *wanted || SanitizeIdentifier(*wanted) != *wanted)) {
                    m_renames[*current] = resolved;
                    if (m_env.contains(*current))
                        m_env[resolved] = m_env.at(*current);
                }
            }
    VisitNode(lpNode->value);
}

void RobloxTypeInferer::Visit(TableLiteralNode *lpNode) {
    for (const auto &entry : lpNode->expressions)
        VisitNode(entry);
}

void RobloxTypeInferer::Visit(ForNumericNode *lpNode) {
    VisitNode(lpNode->loopVariable);
    VisitNode(lpNode->startVariable);
    VisitNode(lpNode->increaseBy);
    VisitNode(lpNode->maxIncreased);
    if (lpNode->lpLoopBody)
        VisitStatementList(lpNode->lpLoopBody->body, m_env);
}

void RobloxTypeInferer::Visit(ForGeneralNode *lpNode) {
    for (const auto &var : lpNode->loopVariables)
        VisitNode(var);
    VisitNode(lpNode->generator);
    VisitNode(lpNode->state);
    VisitNode(lpNode->index);
    if (lpNode->body)
        VisitStatementList(lpNode->body->body, m_env);
}

void RobloxTypeInferer::Visit(RepeatStatementNode *lpNode) {
    if (lpNode->body)
        VisitStatementList(lpNode->body->body, m_env);
    VisitNode(lpNode->condition);
}
