//
// Created by Pixeluted on 01/12/2025.
//
#include "Decompiler.hpp"

#include "AbstractSyntaxTree/Nodes/CommentNode.hpp"
#include "Analysis/RobloxTypeInferer.hpp"
#include "Rewriters/DeadLocalEliminator.hpp"
#include "Rewriters/IfChainSimplifier.hpp"
#include "Rewriters/ShortCircuitFolder.hpp"
#include "SafetyGuard.hpp"

#include <libassert/assert.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <unordered_map>

std::string GetIndentation(int indentationLevel) {
    std::string indent(indentationLevel, ' ');
    return indent;
}

std::string FormatIntList(const std::vector<std::uint32_t> &list) {
    if (list.empty())
        return "[]";
    std::stringstream ss;
    ss << "[";
    for (size_t i = 0; i < list.size(); ++i) {
        ss << list[i];
        if (i < list.size() - 1)
            ss << ", ";
    }
    ss << "]";
    return ss.str();
}

void PrintFunctionOntoStream(std::stringstream &stream, int indentationLevel, const AnalyzedFunction &analyzedFunc) {
    LiftedFunction *rawFunc = analyzedFunc.lpLiftedFunction;

    stream << GetIndentation(indentationLevel) << "/* Function Name: '" << rawFunc->name << "' */\n";
    stream << GetIndentation(indentationLevel) << "/* Basic Blocks: " << analyzedFunc.basicBlocks.size() << " */\n";

    for (const auto &block : analyzedFunc.basicBlocks) {
        stream << "\n";
        stream << GetIndentation(indentationLevel + 2) << "BLOCK_" << block.dwBlockId << ":\n";
        stream << GetIndentation(indentationLevel + 4) << "Type: " << BlockTypeToString(block.bType) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Terminator: " << BlockTerminatorToString(block.bTerminator) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Predecessors: " << FormatIntList(block.predecessors) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Successors:   " << FormatIntList(block.successors) << "\n";
        stream << GetIndentation(indentationLevel + 4) << "Code:\n";

        if (block.lpHead == nullptr || block.lpTail == nullptr) {
            stream << GetIndentation(indentationLevel + 6) << "<Empty/Error Block>\n";
            continue;
        }

        LiftedInstruction *currentInst = block.lpHead;
        while (true) {
            stream << GetIndentation(indentationLevel + 6) << "_" << currentInst->instructionIndex << ": " << OperationToString(currentInst->operation) << " ";

            for (std::size_t i = 0; i < currentInst->operands.size(); i++) {
                const auto &operand = currentInst->operands[i];
                switch (operand.type) {
                case LiftedOperandType::Register:
                    stream << "R" << std::to_string(operand.value.reg);
                    break;
                case LiftedOperandType::ImmediateNil:
                    stream << "nil";
                    break;
                case LiftedOperandType::ImmediateInteger:
                    stream << std::format("0x{:X}", static_cast<uint32_t>(operand.value.imm.n));
                    break;
                case LiftedOperandType::ImmediateBool:
                    stream << std::format("{}", operand.value.imm.b ? "true" : "false");
                    break;
                case LiftedOperandType::ImmediateConstant:
                    stream << "K" << std::to_string(operand.value.imm.k);
                    break;
                case LiftedOperandType::ImmediateAux:
                    stream << "AUXV_" << std::to_string(operand.value.imm.u);
                    break;
                default:
                    ASSERT(false, "unhandled mapping of operand to text");
                }
                if (i + 1 != currentInst->operands.size())
                    stream << ", ";
            }

            if (currentInst->instructionRemarks)
                stream << " /* " << *currentInst->instructionRemarks << " */";

            stream << "\n";

            if (currentInst == block.lpTail)
                break;

            currentInst++;
        }
    }

    if (analyzedFunc.innerFunctions.empty())
        stream << "\n" << GetIndentation(indentationLevel) << "/* There are no nested functions inside of this function. */" << "\n";
    else
        stream << "\n" << GetIndentation(indentationLevel) << "/* Functions inside of Function (size: " << analyzedFunc.innerFunctions.size() << ") */" << "\n";

    for (const auto &subfunction : analyzedFunc.innerFunctions)
        PrintFunctionOntoStream(stream, indentationLevel + 4, subfunction);
}

std::string FormatAnalyzedIR(const AnalyzedFunction &func) {
    std::stringstream sstream;

    sstream << "/* Fission IR Viewer */\n";
    sstream << "/* Created by Fission Contributors */\n";
    sstream << GetIndentation(4) << "/* Program Root (Main Procedure/Prototype/Function) */\n";
    PrintFunctionOntoStream(sstream, 4, func);

    return sstream.str();
}

void writefile(const std::filesystem::path &path, const std::string &content) {
    std::ofstream file(path);

    if (!file.is_open())
        return;

    file << content;
    file.close();
}

std::optional<std::string> readfile(const std::filesystem::path &path, const bool isBinary = false) {
    std::ifstream file(path, (isBinary ? std::ios::binary : 0) | std::ios::ate);

    if (!file.is_open())
        return std::nullopt;

    std::streamsize size = file.tellg();

    file.seekg(0, std::ios::beg);

    std::string buffer(size, '\0');

    if (file.read(&buffer[0], size))
        return buffer;

    return buffer;
}

static std::string FormatDecompilerOptions(DecompilerFlags flags) {
    std::vector<std::string> enabled;
    if ((flags & DecompilerFlags::PrintIR) == DecompilerFlags::PrintIR)
        enabled.emplace_back("PrintIR");
    if ((flags & DecompilerFlags::WriteIRToFile) == DecompilerFlags::WriteIRToFile)
        enabled.emplace_back("WriteIRToFile");
    if ((flags & DecompilerFlags::GenerateIRGraph) == DecompilerFlags::GenerateIRGraph)
        enabled.emplace_back("GenerateIRGraph");
    if ((flags & DecompilerFlags::GenerateSSAIRGraph) == DecompilerFlags::GenerateSSAIRGraph)
        enabled.emplace_back("GenerateSSAIRGraph");
    if ((flags & DecompilerFlags::PrintTimingBreakdown) == DecompilerFlags::PrintTimingBreakdown)
        enabled.emplace_back("PrintTimingBreakdown");
    if ((flags & DecompilerFlags::InferTypes) == DecompilerFlags::InferTypes)
        enabled.emplace_back("InferTypes");
    if ((flags & DecompilerFlags::OptimizeIR) == DecompilerFlags::OptimizeIR)
        enabled.emplace_back("OptimizeIR");
    if ((flags & DecompilerFlags::InferRobloxTypes) == DecompilerFlags::InferRobloxTypes)
        enabled.emplace_back("InferRobloxTypes");
    if ((flags & DecompilerFlags::AutoNameVariables) == DecompilerFlags::AutoNameVariables)
        enabled.emplace_back("AutoNameVariables");
    if ((flags & DecompilerFlags::OmitFissionComments) == DecompilerFlags::OmitFissionComments)
        enabled.emplace_back("OmitFissionComments");

    if (enabled.empty())
        return "None";

    std::string result = enabled.front();
    for (size_t i = 1; i < enabled.size(); ++i)
        result += ", " + enabled[i];
    return result;
}

static void AddDecompilerOptionsToHeader(ASTFunction &ast, DecompilerFlags flags) {
    if (ast.statements.empty())
        return;
    auto header = std::dynamic_pointer_cast<CommentNode>(ast.statements.front());
    if (!header)
        return;
    header->comment += "\n    Decompile Options: " + FormatDecompilerOptions(flags);
}

static std::shared_ptr<Expression> MakeTypeAnnotation(const std::string &typeName) {
    return std::make_shared<IdentifierExpressionNode>(std::make_shared<Identifier>(typeName));
}

static std::optional<std::string> GetSimpleIdentifierName(const std::shared_ptr<Expression> &expr) {
    if (auto id = std::dynamic_pointer_cast<IdentifierExpressionNode>(expr); id && id->identifier)
        return id->identifier->name;
    if (auto id = std::dynamic_pointer_cast<Identifier>(expr))
        return id->name;
    return std::nullopt;
}

static std::optional<std::string> InferExpressionType(const std::shared_ptr<Expression> &expr) {
    if (!expr)
        return std::nullopt;
    if (std::dynamic_pointer_cast<NilLiteralNode>(expr))
        return "nil";
    if (std::dynamic_pointer_cast<BooleanLiteralNode>(expr))
        return "boolean";
    if (std::dynamic_pointer_cast<NumberLiteralNode>(expr) || std::dynamic_pointer_cast<IntegerLiteralNode>(expr))
        return "number";
    if (std::dynamic_pointer_cast<StringLiteralNode>(expr))
        return "string";
    if (std::dynamic_pointer_cast<TableLiteralNode>(expr))
        return "table";
    if (std::dynamic_pointer_cast<FunctionDeclarationNode>(expr))
        return "function";
    if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
        if (unary->op == "not ")
            return "boolean";
        if (unary->op == "-" || unary->op == "#")
            return "number";
    }
    if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
        if (binary->op == "<" || binary->op == ">" || binary->op == "<=" || binary->op == ">=" || binary->op == "==" || binary->op == "~=")
            return "boolean";
        if (binary->op == "..")
            return "string";
        if (binary->op == "+" || binary->op == "-" || binary->op == "*" || binary->op == "/" || binary->op == "//" || binary->op == "%" ||
            binary->op == "^")
            return "number";
    }
    return std::nullopt;
}

struct TypeFact {
    std::vector<std::string> concrete;
    bool sawNil = false;

    void Add(const std::optional<std::string> &type) {
        if (!type)
            return;
        if (*type == "nil") {
            sawNil = true;
            return;
        }
        if (std::ranges::find(concrete, *type) == concrete.end())
            concrete.push_back(*type);
    }

    std::optional<std::string> Resolve() const {
        if (concrete.empty())
            return std::nullopt;
        std::string type = concrete.front();
        for (size_t i = 1; i < concrete.size(); ++i)
            type += " | " + concrete[i];
        if (sawNil)
            type += " | nil";
        return type;
    }
};

using FunctionMap = std::unordered_map<std::string, std::vector<std::shared_ptr<FunctionDeclarationNode>>>;

static void CollectFunctionsFromExpression(const std::shared_ptr<Expression> &expr, FunctionMap &functions);
static void CollectFunctionsFromStatements(const std::vector<std::shared_ptr<Statement>> &stmts, FunctionMap &functions);

static void CollectFunctionsFromExpression(const std::shared_ptr<Expression> &expr, FunctionMap &functions) {
    if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr)) {
        if (!fn->functionName.empty())
            functions[fn->functionName].push_back(fn);
        if (fn->lpFunctionBody)
            CollectFunctionsFromStatements(fn->lpFunctionBody->body, functions);
    } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
        CollectFunctionsFromExpression(call->callee, functions);
        for (const auto &arg : call->arguments)
            CollectFunctionsFromExpression(arg, functions);
    } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
        CollectFunctionsFromExpression(nameCall->calledOn, functions);
        CollectFunctionsFromExpression(nameCall->callWhat, functions);
        for (const auto &arg : nameCall->arguments)
            CollectFunctionsFromExpression(arg, functions);
    } else if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
        CollectFunctionsFromExpression(binary->left, functions);
        CollectFunctionsFromExpression(binary->right, functions);
    } else if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
        CollectFunctionsFromExpression(unary->operand, functions);
    } else if (auto table = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
        for (const auto &entry : table->expressions)
            CollectFunctionsFromExpression(entry, functions);
    }
}

static void CollectFunctionsFromStatements(const std::vector<std::shared_ptr<Statement>> &stmts, FunctionMap &functions) {
    for (const auto &stmt : stmts) {
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (!fn->functionName.empty())
                functions[fn->functionName].push_back(fn);
            if (fn->lpFunctionBody)
                CollectFunctionsFromStatements(fn->lpFunctionBody->body, functions);
        } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            CollectFunctionsFromExpression(decl->value, functions);
        } else if (auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            CollectFunctionsFromExpression(assign->left, functions);
            CollectFunctionsFromExpression(assign->right, functions);
        } else if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            CollectFunctionsFromExpression(exprStmt->expression, functions);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &value : ret->returnValues)
                CollectFunctionsFromExpression(value, functions);
        } else if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CollectFunctionsFromExpression(ifStmt->condition, functions);
            if (ifStmt->thenBranch)
                CollectFunctionsFromStatements(ifStmt->thenBranch->body, functions);
            if (ifStmt->elseBranch)
                CollectFunctionsFromStatements(ifStmt->elseBranch->body, functions);
        } else if (auto whileStmt = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            CollectFunctionsFromExpression(whileStmt->condition, functions);
            if (whileStmt->body)
                CollectFunctionsFromStatements(whileStmt->body->body, functions);
        } else if (auto repeatStmt = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            CollectFunctionsFromExpression(repeatStmt->condition, functions);
            if (repeatStmt->body)
                CollectFunctionsFromStatements(repeatStmt->body->body, functions);
        } else if (auto forNum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            CollectFunctionsFromExpression(forNum->startVariable, functions);
            CollectFunctionsFromExpression(forNum->increaseBy, functions);
            CollectFunctionsFromExpression(forNum->maxIncreased, functions);
            if (forNum->lpLoopBody)
                CollectFunctionsFromStatements(forNum->lpLoopBody->body, functions);
        } else if (auto forGen = std::dynamic_pointer_cast<ForGeneralNode>(stmt)) {
            CollectFunctionsFromExpression(forGen->generator, functions);
            CollectFunctionsFromExpression(forGen->state, functions);
            CollectFunctionsFromExpression(forGen->index, functions);
            if (forGen->body)
                CollectFunctionsFromStatements(forGen->body->body, functions);
        }
    }
}

using InferenceFacts = std::unordered_map<FunctionDeclarationNode *, std::vector<TypeFact>>;

static void AddIdentifierUseFact(const std::shared_ptr<Expression> &expr, const std::string &argName, const std::string &type, TypeFact &fact) {
    auto ident = GetSimpleIdentifierName(expr);
    if (ident && *ident == argName)
        fact.Add(type);
}

static void CollectBodyUseFactsFromExpression(const std::shared_ptr<Expression> &expr, const std::string &argName, TypeFact &fact) {
    if (!expr)
        return;

    if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
        if (binary->op == "+" || binary->op == "-" || binary->op == "*" || binary->op == "/" || binary->op == "//" || binary->op == "%" ||
            binary->op == "^") {
            AddIdentifierUseFact(binary->left, argName, "number", fact);
            AddIdentifierUseFact(binary->right, argName, "number", fact);
        } else if (binary->op == "..") {
            AddIdentifierUseFact(binary->left, argName, "string", fact);
            AddIdentifierUseFact(binary->right, argName, "string", fact);
        } else if (binary->op == "<" || binary->op == ">" || binary->op == "<=" || binary->op == ">=" || binary->op == "==" || binary->op == "~=") {
            AddIdentifierUseFact(binary->left, argName, InferExpressionType(binary->right).value_or("unknown"), fact);
            AddIdentifierUseFact(binary->right, argName, InferExpressionType(binary->left).value_or("unknown"), fact);
        }
        CollectBodyUseFactsFromExpression(binary->left, argName, fact);
        CollectBodyUseFactsFromExpression(binary->right, argName, fact);
    } else if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
        if (unary->op == "not ")
            AddIdentifierUseFact(unary->operand, argName, "boolean", fact);
        else if (unary->op == "-" || unary->op == "#")
            AddIdentifierUseFact(unary->operand, argName, "number", fact);
        CollectBodyUseFactsFromExpression(unary->operand, argName, fact);
    } else if (auto index = std::dynamic_pointer_cast<IndexExpressionNode>(expr)) {
        AddIdentifierUseFact(index->left, argName, "table", fact);
        CollectBodyUseFactsFromExpression(index->left, argName, fact);
        CollectBodyUseFactsFromExpression(index->right, argName, fact);
    } else if (auto member = std::dynamic_pointer_cast<MemberExpressionNode>(expr)) {
        AddIdentifierUseFact(member->table, argName, "table", fact);
        CollectBodyUseFactsFromExpression(member->table, argName, fact);
        CollectBodyUseFactsFromExpression(member->key, argName, fact);
    } else if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
        CollectBodyUseFactsFromExpression(call->callee, argName, fact);
        for (const auto &arg : call->arguments)
            CollectBodyUseFactsFromExpression(arg, argName, fact);
    } else if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
        AddIdentifierUseFact(nameCall->calledOn, argName, "table", fact);
        CollectBodyUseFactsFromExpression(nameCall->calledOn, argName, fact);
        CollectBodyUseFactsFromExpression(nameCall->callWhat, argName, fact);
        for (const auto &arg : nameCall->arguments)
            CollectBodyUseFactsFromExpression(arg, argName, fact);
    } else if (auto table = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
        for (const auto &entry : table->expressions)
            CollectBodyUseFactsFromExpression(entry, argName, fact);
    }
}

static void CollectBodyUseFactsFromStatements(const std::vector<std::shared_ptr<Statement>> &stmts, const std::string &argName, TypeFact &fact) {
    for (const auto &stmt : stmts) {
        if (std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt))
            continue;
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            CollectBodyUseFactsFromExpression(decl->value, argName, fact);
        } else if (auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            CollectBodyUseFactsFromExpression(assign->left, argName, fact);
            CollectBodyUseFactsFromExpression(assign->right, argName, fact);
        } else if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            CollectBodyUseFactsFromExpression(exprStmt->expression, argName, fact);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &value : ret->returnValues)
                CollectBodyUseFactsFromExpression(value, argName, fact);
        } else if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            AddIdentifierUseFact(ifStmt->condition, argName, "boolean", fact);
            CollectBodyUseFactsFromExpression(ifStmt->condition, argName, fact);
            if (ifStmt->thenBranch)
                CollectBodyUseFactsFromStatements(ifStmt->thenBranch->body, argName, fact);
            if (ifStmt->elseBranch)
                CollectBodyUseFactsFromStatements(ifStmt->elseBranch->body, argName, fact);
        } else if (auto whileStmt = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            AddIdentifierUseFact(whileStmt->condition, argName, "boolean", fact);
            CollectBodyUseFactsFromExpression(whileStmt->condition, argName, fact);
            if (whileStmt->body)
                CollectBodyUseFactsFromStatements(whileStmt->body->body, argName, fact);
        } else if (auto repeatStmt = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            AddIdentifierUseFact(repeatStmt->condition, argName, "boolean", fact);
            CollectBodyUseFactsFromExpression(repeatStmt->condition, argName, fact);
            if (repeatStmt->body)
                CollectBodyUseFactsFromStatements(repeatStmt->body->body, argName, fact);
        } else if (auto forNum = std::dynamic_pointer_cast<ForNumericNode>(stmt); forNum && forNum->lpLoopBody) {
            CollectBodyUseFactsFromStatements(forNum->lpLoopBody->body, argName, fact);
        } else if (auto forGen = std::dynamic_pointer_cast<ForGeneralNode>(stmt); forGen && forGen->body) {
            CollectBodyUseFactsFromStatements(forGen->body->body, argName, fact);
        }
    }
}

static std::optional<std::string> InferArgumentTypeFromBody(FunctionDeclarationNode *fn, size_t argIndex) {
    if (!fn->argumentsNames.contains(static_cast<int32_t>(argIndex)) || !fn->lpFunctionBody)
        return std::nullopt;
    auto argName = GetSimpleIdentifierName(fn->argumentsNames.at(static_cast<int32_t>(argIndex))->argumentName);
    if (!argName)
        return std::nullopt;
    TypeFact fact;
    CollectBodyUseFactsFromStatements(fn->lpFunctionBody->body, *argName, fact);
    return fact.Resolve();
}

static void CollectCallFactsFromExpression(const std::shared_ptr<Expression> &expr, const FunctionMap &functions, InferenceFacts &facts);
static void CollectCallFactsFromStatements(const std::vector<std::shared_ptr<Statement>> &stmts, const FunctionMap &functions, InferenceFacts &facts);

static void CollectCallFactsFromExpression(const std::shared_ptr<Expression> &expr, const FunctionMap &functions, InferenceFacts &facts) {
    if (!expr)
        return;

    if (auto call = std::dynamic_pointer_cast<CallExpressionNode>(expr)) {
        auto calleeName = GetSimpleIdentifierName(call->callee);
        if (calleeName && functions.contains(*calleeName) && functions.at(*calleeName).size() == 1) {
            auto *fn = functions.at(*calleeName).front().get();
            auto &fnFacts = facts[fn];
            if (fnFacts.size() < static_cast<size_t>(fn->argumentCount))
                fnFacts.resize(fn->argumentCount);
            for (size_t i = 0; i < call->arguments.size() && i < fnFacts.size(); ++i)
                fnFacts[i].Add(InferExpressionType(call->arguments[i]));
        }
        CollectCallFactsFromExpression(call->callee, functions, facts);
        for (const auto &arg : call->arguments)
            CollectCallFactsFromExpression(arg, functions, facts);
        return;
    }

    if (auto nameCall = std::dynamic_pointer_cast<NameCallExpressionNode>(expr)) {
        CollectCallFactsFromExpression(nameCall->calledOn, functions, facts);
        CollectCallFactsFromExpression(nameCall->callWhat, functions, facts);
        for (const auto &arg : nameCall->arguments)
            CollectCallFactsFromExpression(arg, functions, facts);
    } else if (auto binary = std::dynamic_pointer_cast<BinaryExpressionNode>(expr)) {
        CollectCallFactsFromExpression(binary->left, functions, facts);
        CollectCallFactsFromExpression(binary->right, functions, facts);
    } else if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr)) {
        CollectCallFactsFromExpression(unary->operand, functions, facts);
    } else if (auto table = std::dynamic_pointer_cast<TableLiteralNode>(expr)) {
        for (const auto &entry : table->expressions)
            CollectCallFactsFromExpression(entry, functions, facts);
    } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(expr); fn && fn->lpFunctionBody) {
        CollectCallFactsFromStatements(fn->lpFunctionBody->body, functions, facts);
    }
}

static void CollectCallFactsFromStatements(const std::vector<std::shared_ptr<Statement>> &stmts, const FunctionMap &functions, InferenceFacts &facts) {
    for (const auto &stmt : stmts) {
        if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt)) {
            if (fn->lpFunctionBody)
                CollectCallFactsFromStatements(fn->lpFunctionBody->body, functions, facts);
        } else if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            CollectCallFactsFromExpression(decl->value, functions, facts);
        } else if (auto assign = std::dynamic_pointer_cast<AssignmentStatementNode>(stmt)) {
            CollectCallFactsFromExpression(assign->left, functions, facts);
            CollectCallFactsFromExpression(assign->right, functions, facts);
        } else if (auto exprStmt = std::dynamic_pointer_cast<ExpressionStatementNode>(stmt)) {
            CollectCallFactsFromExpression(exprStmt->expression, functions, facts);
        } else if (auto ret = std::dynamic_pointer_cast<ReturnStatementNode>(stmt)) {
            for (const auto &value : ret->returnValues)
                CollectCallFactsFromExpression(value, functions, facts);
        } else if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            CollectCallFactsFromExpression(ifStmt->condition, functions, facts);
            if (ifStmt->thenBranch)
                CollectCallFactsFromStatements(ifStmt->thenBranch->body, functions, facts);
            if (ifStmt->elseBranch)
                CollectCallFactsFromStatements(ifStmt->elseBranch->body, functions, facts);
        } else if (auto whileStmt = std::dynamic_pointer_cast<WhileStatementNode>(stmt)) {
            CollectCallFactsFromExpression(whileStmt->condition, functions, facts);
            if (whileStmt->body)
                CollectCallFactsFromStatements(whileStmt->body->body, functions, facts);
        } else if (auto repeatStmt = std::dynamic_pointer_cast<RepeatStatementNode>(stmt)) {
            CollectCallFactsFromExpression(repeatStmt->condition, functions, facts);
            if (repeatStmt->body)
                CollectCallFactsFromStatements(repeatStmt->body->body, functions, facts);
        } else if (auto forNum = std::dynamic_pointer_cast<ForNumericNode>(stmt)) {
            if (forNum->lpLoopBody)
                CollectCallFactsFromStatements(forNum->lpLoopBody->body, functions, facts);
        } else if (auto forGen = std::dynamic_pointer_cast<ForGeneralNode>(stmt); forGen && forGen->body) {
            CollectCallFactsFromStatements(forGen->body->body, functions, facts);
        }
    }
}

static void AnnotateLocalDeclarations(std::vector<std::shared_ptr<Statement>> &stmts) {
    for (auto &stmt : stmts) {
        if (auto decl = std::dynamic_pointer_cast<VariableDeclarationNode>(stmt)) {
            auto type = InferExpressionType(decl->value);
            if (type && *type != "nil")
                decl->type = MakeTypeAnnotation(*type);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmt); fn && fn->lpFunctionBody) {
            AnnotateLocalDeclarations(fn->lpFunctionBody->body);
        } else if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmt)) {
            if (ifStmt->thenBranch)
                AnnotateLocalDeclarations(ifStmt->thenBranch->body);
            if (ifStmt->elseBranch)
                AnnotateLocalDeclarations(ifStmt->elseBranch->body);
        } else if (auto whileStmt = std::dynamic_pointer_cast<WhileStatementNode>(stmt); whileStmt && whileStmt->body) {
            AnnotateLocalDeclarations(whileStmt->body->body);
        } else if (auto repeatStmt = std::dynamic_pointer_cast<RepeatStatementNode>(stmt); repeatStmt && repeatStmt->body) {
            AnnotateLocalDeclarations(repeatStmt->body->body);
        } else if (auto forNum = std::dynamic_pointer_cast<ForNumericNode>(stmt); forNum && forNum->lpLoopBody) {
            AnnotateLocalDeclarations(forNum->lpLoopBody->body);
        } else if (auto forGen = std::dynamic_pointer_cast<ForGeneralNode>(stmt); forGen && forGen->body) {
            AnnotateLocalDeclarations(forGen->body->body);
        }
    }
}

static std::optional<bool> EvaluateBooleanConstant(const std::shared_ptr<Expression> &expr) {
    if (auto boolean = std::dynamic_pointer_cast<BooleanLiteralNode>(expr))
        return boolean->value;
    if (auto nil = std::dynamic_pointer_cast<NilLiteralNode>(expr)) {
        (void)nil;
        return false;
    }
    if (auto unary = std::dynamic_pointer_cast<UnaryExpressionNode>(expr); unary && unary->op == "not ") {
        auto value = EvaluateBooleanConstant(unary->operand);
        if (value)
            return !*value;
    }
    return std::nullopt;
}

static void OptimizeStatements(std::vector<std::shared_ptr<Statement>> &stmts) {
    for (size_t i = 0; i < stmts.size();) {
        if (auto ifStmt = std::dynamic_pointer_cast<IfStatementNode>(stmts[i])) {
            if (ifStmt->thenBranch)
                OptimizeStatements(ifStmt->thenBranch->body);
            if (ifStmt->elseBranch)
                OptimizeStatements(ifStmt->elseBranch->body);

            auto cond = EvaluateBooleanConstant(ifStmt->condition);
            if (cond) {
                std::vector<std::shared_ptr<Statement>> replacement;
                if (*cond && ifStmt->thenBranch)
                    replacement = ifStmt->thenBranch->body;
                else if (!*cond && ifStmt->elseBranch)
                    replacement = ifStmt->elseBranch->body;
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
                stmts.insert(stmts.begin() + static_cast<std::ptrdiff_t>(i), replacement.begin(), replacement.end());
                i += replacement.size();
                continue;
            }
        } else if (auto whileStmt = std::dynamic_pointer_cast<WhileStatementNode>(stmts[i])) {
            if (whileStmt->body)
                OptimizeStatements(whileStmt->body->body);
            auto cond = EvaluateBooleanConstant(whileStmt->condition);
            if (cond && !*cond) {
                stmts.erase(stmts.begin() + static_cast<std::ptrdiff_t>(i));
                continue;
            }
        } else if (auto repeatStmt = std::dynamic_pointer_cast<RepeatStatementNode>(stmts[i])) {
            if (repeatStmt->body)
                OptimizeStatements(repeatStmt->body->body);
        } else if (auto fn = std::dynamic_pointer_cast<FunctionDeclarationNode>(stmts[i])) {
            if (fn->lpFunctionBody)
                OptimizeStatements(fn->lpFunctionBody->body);
        } else if (auto forNum = std::dynamic_pointer_cast<ForNumericNode>(stmts[i])) {
            if (forNum->lpLoopBody)
                OptimizeStatements(forNum->lpLoopBody->body);
        } else if (auto forGen = std::dynamic_pointer_cast<ForGeneralNode>(stmts[i])) {
            if (forGen->body)
                OptimizeStatements(forGen->body->body);
        }
        ++i;
    }
}

static void ApplyFunctionArgumentFacts(const FunctionMap &functions, const InferenceFacts &facts) {
    for (const auto &[_, overloads] : functions) {
        for (const auto &fnPtr : overloads) {
            auto *fn = fnPtr.get();
            for (int32_t i = 0; i < fn->argumentCount; ++i) {
                if (!fn->argumentsNames.contains(i) || fn->argumentsNames.at(i)->type)
                    continue;
                std::optional<std::string> type;
                if (facts.contains(fn) && facts.at(fn).size() > static_cast<size_t>(i))
                    type = facts.at(fn)[i].Resolve();
                if (!type)
                    type = InferArgumentTypeFromBody(fn, i);
                if (!type)
                    type = "unknown";
                fn->argumentsNames.at(i)->type = MakeTypeAnnotation(*type);
            }
        }
    }
}

static void InferTypes(ASTFunction &ast) {
    FunctionMap functions;
    CollectFunctionsFromStatements(ast.statements, functions);

    InferenceFacts facts;
    CollectCallFactsFromStatements(ast.statements, functions, facts);
    ApplyFunctionArgumentFacts(functions, facts);
    AnnotateLocalDeclarations(ast.statements);
}

static void OptimizeIR(ASTFunction &ast) { OptimizeStatements(ast.statements); }


DecompilationResult Decompiler::CommonDecompilerEntry(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags) {
    Fission::ScopedThrowingAssertHandler assertGuard;

    if (decoder == nullptr)
        return {"", "", "", DecompileResult::FailedToDecompile};

    try {
        return CommonDecompilerEntryImpl(bytecode, decoder, flags);
    } catch (const std::exception &error) {
        DecompilationResult failure{};
        failure.resultCode = DecompileResult::FailedToDecompile;
#ifndef PRODUCTION_BUILD
        failure.decompilationOutput = std::string("--[[ Fission failed to decompile this chunk: ") + error.what() + " ]]";
#endif
        return failure;
    } catch (...) {
        return {"", "", "", DecompileResult::FailedToDecompile};
    }
}

DecompilationResult Decompiler::CommonDecompilerEntryImpl(const std::string &bytecode, Fission::InstructionDecoder *decoder, DecompilerFlags flags) {
    DEBUG_ASSERT(decoder != nullptr);
    DecompilationResult res{};
    const auto deserializeStart = std::chrono::steady_clock::now();
    const auto deserializedBytecode = deserializer.Deserialize(bytecode);
    const auto deserializeEnd = std::chrono::steady_clock::now();
    if (!deserializedBytecode || deserializedBytecode->functions.empty())
        return {"", "", "", DecompileResult::FailedToDeserialize};

    auto bytecodeLifter = BytecodeLifter{decoder};
    const auto bytecodeLiftStart = std::chrono::steady_clock::now();
    auto liftedBytecode = bytecodeLifter.LiftDeserializedBytecode(*deserializedBytecode);
    const auto bytecodeLiftEnd = std::chrono::steady_clock::now();

    const auto controlFlowAnalyzeStart = std::chrono::steady_clock::now();
    const auto basicBlockIdentificationStart = std::chrono::steady_clock::now();
    auto controlFlowAnalyzedFunction = controlFlowAnalyzer.DetermineBasicBlocks(&liftedBytecode);
    const auto basicBlockIdentificationEnd = std::chrono::steady_clock::now();

    const auto optimizeGraphStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.OptimizeGraph(controlFlowAnalyzedFunction);
    const auto optimizeGraphEnd = std::chrono::steady_clock::now();

    const auto identifyLoopStructuresStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.IdentifyStructures(controlFlowAnalyzedFunction);
    const auto identifyLoopStructuresEnd = std::chrono::steady_clock::now();

    const auto unreachablePruningStart = std::chrono::steady_clock::now();
    controlFlowAnalyzer.PruneUnreachable(controlFlowAnalyzedFunction);
    const auto unreachablePruningEnd = std::chrono::steady_clock::now();
    const auto controlFlowAnalyzeEnd = std::chrono::steady_clock::now();

    const auto ssaStart = std::chrono::steady_clock::now();
    SSABuilder.Build(controlFlowAnalyzedFunction);
    const auto ssaEnd = std::chrono::steady_clock::now();

    const auto astStart = std::chrono::steady_clock::now();
    auto liftedAST = ASTLifter.Lift(controlFlowAnalyzedFunction);
    AddDecompilerOptionsToHeader(liftedAST, flags);

    // ---- AST Rewriting ----
    const auto astRewriteStart = std::chrono::steady_clock::now();
    const auto shortCircuitStart = std::chrono::steady_clock::now();
    ShortCircuitFolder{}.Run(liftedAST.statements);
    const auto shortCircuitEnd = std::chrono::steady_clock::now();
    const auto ifChainStart = std::chrono::steady_clock::now();
    IfChainSimplifier{}.Run(liftedAST.statements);
    const auto ifChainEnd = std::chrono::steady_clock::now();
    DeadLocalEliminator{}.Run(liftedAST.statements);
    const auto astRewriteEnd = std::chrono::steady_clock::now();

    const auto irOptimizationStart = std::chrono::steady_clock::now();
    if ((flags & DecompilerFlags::OptimizeIR) == DecompilerFlags::OptimizeIR)
        OptimizeIR(liftedAST);
    const auto irOptimizationEnd = std::chrono::steady_clock::now();

    const auto typeInferenceStart = std::chrono::steady_clock::now();
    if ((flags & DecompilerFlags::InferTypes) == DecompilerFlags::InferTypes)
        InferTypes(liftedAST);
    const auto typeInferenceEnd = std::chrono::steady_clock::now();

    const auto robloxPropagationStart = std::chrono::steady_clock::now();
    const auto inferRobloxTypes = (flags & DecompilerFlags::InferRobloxTypes) == DecompilerFlags::InferRobloxTypes;
    const auto autoNameVariables = (flags & DecompilerFlags::AutoNameVariables) == DecompilerFlags::AutoNameVariables;
    if (inferRobloxTypes || autoNameVariables)
        RobloxTypeInferer{}.Infer(liftedAST, inferRobloxTypes, autoNameVariables);
    const auto robloxPropagationEnd = std::chrono::steady_clock::now();
    const auto astEnd = std::chrono::steady_clock::now();

    RootNode root{liftedAST.statements};

    SourceGenerator.bOmitInformationalComments = (flags & DecompilerFlags::OmitFissionComments) == DecompilerFlags::OmitFissionComments;
    const auto sgenStart = std::chrono::steady_clock::now();
    const auto generator = SourceGenerator.GenerateSource(&root);
    const auto sgenEnd = std::chrono::steady_clock::now();

    std::println(std::cout, "generated source code:\n{}", generator);

    const auto printIR = (flags & DecompilerFlags::PrintIR) == DecompilerFlags::PrintIR;
    const auto writeIR = (flags & DecompilerFlags::WriteIRToFile) == DecompilerFlags::WriteIRToFile;
    const auto formattedIR = FormatAnalyzedIR(controlFlowAnalyzedFunction);
    if (printIR || writeIR) {
        if (writeIR)
            writefile(std::filesystem::path{"ir_out.txt"}, formattedIR);
    }

    const auto generateIRGraph = (flags & DecompilerFlags::GenerateIRGraph) == DecompilerFlags::GenerateIRGraph;
    const auto generateSSAGraph = (flags & DecompilerFlags::GenerateSSAIRGraph) == DecompilerFlags::GenerateSSAIRGraph;
    if (generateIRGraph || generateSSAGraph) {
        const auto dotGraph = visualizer.GenerateDotGraph(
            controlFlowAnalyzedFunction, generateIRGraph && generateSSAGraph ? GraphContent::Both
                                         : generateIRGraph == true           ? GraphContent::IROnly
                                                                             : GraphContent::SSAOnly
        );

        writefile("cfg.dot", dotGraph);
    }

    if ((flags & DecompilerFlags::PrintTimingBreakdown) == DecompilerFlags::PrintTimingBreakdown) {
        res.timingStatistics = std::format(
            "Decompilation Breakdown:\n\t"
            "Deserializing Bytecode: {}\n\t"
            "Lifting into IR: {}\n\t"
            "Control Flow Analysis: {}\n\t"
            "\tDetermining Basic Blocks: {}\n\t"
            "\tGraph Optimization: {}\n\t"
            "\tBlock Pruning: {}\n\t"
            "\tStructure Identification: {}\n\t"
            "IR -> SSA Form: {}\n\t"
            "IR (SSA) -> AST: {}\n\t"
            "AST Rewriting: {}\n\t"
            "\tShort-Circuit Folder: {}\n\t"
            "\tIf-Chain Simplifier: {}\n\t"
            "AST -> Source Code: {}\n\t"
            "IR Optimization: {}\n\t"
            "Type Propagation: {}\n\t"
            "Roblox TypeName/Name Propagation: {}",
            std::chrono::duration<float>(deserializeEnd - deserializeStart), std::chrono::duration<float>(bytecodeLiftEnd - bytecodeLiftStart),
            std::chrono::duration<float>(controlFlowAnalyzeEnd - controlFlowAnalyzeStart),
            std::chrono::duration<float>(basicBlockIdentificationEnd - basicBlockIdentificationStart),
            std::chrono::duration<float>(optimizeGraphEnd - optimizeGraphStart), std::chrono::duration<float>(unreachablePruningEnd - unreachablePruningStart),
            std::chrono::duration<float>(identifyLoopStructuresEnd - identifyLoopStructuresStart), std::chrono::duration<float>(ssaEnd - ssaStart),
            std::chrono::duration<float>(astEnd - astStart), std::chrono::duration<float>(astRewriteEnd - astRewriteStart),
            std::chrono::duration<float>(shortCircuitEnd - shortCircuitStart), std::chrono::duration<float>(ifChainEnd - ifChainStart),
            std::chrono::duration<float>(sgenEnd - sgenStart), std::chrono::duration<float>(irOptimizationEnd - irOptimizationStart),
            std::chrono::duration<float>(typeInferenceEnd - typeInferenceStart), std::chrono::duration<float>(robloxPropagationEnd - robloxPropagationStart)
        );
    }

    res.irOutput = formattedIR;
    res.resultCode = DecompileResult::Success;
    res.decompilationOutput = std::move(generator);
    return res;
}

DecompilationResult Decompiler::DecompileTestCode(const std::string &testCode, const DecompilerFlags flags, const Luau::CompileOptions &compileOpts) {
    try {
        const auto compiledBytecode = Luau::compile(testCode, compileOpts);
        auto normalDecoder = Fission::InstructionDecoder{};
        return CommonDecompilerEntry(compiledBytecode, &normalDecoder, flags);
    } catch (...) {
        return {"", "", "", DecompileResult::FailedToDecompile};
    }
}

DecompilationResult Decompiler::DecompileTestCodeFromFile(const std::string &fileName, const DecompilerFlags flags, const Luau::CompileOptions &compileOpts) {
    const auto readFile = readfile(std::filesystem::path(fileName));
    if (!readFile)
        return {"", "", "", DecompileResult::FailedToReadFile};

    return DecompileTestCode(*readFile, flags, compileOpts);
}

DecompilationResult Decompiler::DecompileRobloxBytecode(const std::string &bytecode, DecompilerFlags flags) {
    auto robloxDecoder = Fission::RobloxClientDecoder{};
    return CommonDecompilerEntry(bytecode, &robloxDecoder, flags);
}

DecompilationResult Decompiler::DecompileRobloxBytecodeFromFile(const std::string &fileName, DecompilerFlags flags) {
    const auto readFile = readfile(std::filesystem::path(fileName), true);
    if (!readFile)
        return {"", "", "", DecompileResult::FailedToReadFile};

    return DecompileRobloxBytecode(*readFile, flags);
}
