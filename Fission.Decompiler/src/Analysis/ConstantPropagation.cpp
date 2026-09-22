#include "Analysis/ConstantPropagation.hpp"

#include "Deserializer.hpp"
#include "SSABuilder.hpp"
#include "SafetyGuard.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Fission::ConstantPropagationDetail {

    struct StringValue {
        std::string value;
        bool operator==(const StringValue &) const = default;
    };

    struct SymbolValue {
        std::string path;
        bool operator==(const SymbolValue &) const = default;
    };

    struct TableValue {
        const LiftedInstruction *allocation = nullptr;
        bool operator==(const TableValue &) const = default;
    };

    struct ClosureValue {
        const LiftedInstruction *allocation = nullptr;
        LiftedFunction *function = nullptr;
        bool operator==(const ClosureValue &) const = default;
    };

    using Constant = std::variant<std::monostate, bool, double, int64_t, StringValue, SymbolValue, TableValue, ClosureValue>;

    enum class LatticeState { Unknown, Constant, Overdefined };

    struct Value {
        LatticeState state = LatticeState::Unknown;
        Constant constant{};

        static Value Known(Constant value) { return {LatticeState::Constant, std::move(value)}; }
        static Value Overdefined() { return {LatticeState::Overdefined, {}}; }
    };

    static bool SameConstant(const Constant &left, const Constant &right) {
        if (left.index() != right.index())
            return false;
        if (const auto *number = std::get_if<double>(&left))
            return std::bit_cast<uint64_t>(*number) == std::bit_cast<uint64_t>(std::get<double>(right));
        return left == right;
    }

    static Value Join(Value left, const Value &right) {
        if (left.state == LatticeState::Unknown)
            return right;
        if (right.state == LatticeState::Unknown)
            return left;
        if (left.state == LatticeState::Overdefined || right.state == LatticeState::Overdefined)
            return Value::Overdefined();
        if (!SameConstant(left.constant, right.constant))
            return Value::Overdefined();
        return left;
    }

    static bool MergeInto(Value &destination, const Value &incoming) {
        const Value merged = Join(destination, incoming);
        if (merged.state == destination.state && (merged.state != LatticeState::Constant || SameConstant(merged.constant, destination.constant)))
            return false;
        destination = merged;
        return true;
    }

    struct FieldState {
        std::unordered_map<const LiftedInstruction *, Value> writes;
    };

    struct TableState {
        LiftedFunction *owner = nullptr;
        int32_t allocationIndex = -1;
        int32_t firstCallIndex = std::numeric_limits<int32_t>::max();
        bool dynamicWrite = false;
        bool escaped = false;
        std::unordered_map<std::string, FieldState> fields;
    };

    struct FunctionState {
        bool reachable = false;
        bool external = false;
        std::vector<Value> parameters;
        std::vector<Value> upvalues;
        std::vector<Value> returns;
    };

    struct ProgramContext {
        std::unordered_map<LiftedFunction *, AnalyzedFunction *> analyzedFunctions;
        std::unordered_map<int32_t, LiftedFunction *> functionsByBytecodeId;
        std::unordered_map<LiftedFunction *, FunctionState> functions;
        std::vector<LiftedFunction *> functionOrder;
        std::unordered_map<const LiftedInstruction *, TableState> tables;
        std::unordered_map<const LiftedInstruction *, std::vector<Value>> closureCaptures;
        std::unordered_map<const LiftedInstruction *, LiftedFunction *> instructionOwners;
        std::unordered_map<const LiftedInstruction *, uint32_t> instructionBlocks;
        LiftedFunction *root = nullptr;
        bool changed = false;
        bool finalized = false;
    };

    static bool SameValue(const Value &left, const Value &right) {
        return left.state == right.state && (left.state != LatticeState::Constant || SameConstant(left.constant, right.constant));
    }

    static void RegisterFunctions(AnalyzedFunction &root, ProgramContext &context) {
        std::vector<AnalyzedFunction *> pending{&root};
        while (!pending.empty()) {
            AnalyzedFunction &function = *pending.back();
            pending.pop_back();
            context.analyzedFunctions[function.lpLiftedFunction] = &function;
            context.functionOrder.push_back(function.lpLiftedFunction);
            if (function.lpLiftedFunction && function.lpLiftedFunction->lpDeserialized)
                context.functionsByBytecodeId[function.lpLiftedFunction->lpDeserialized->bytecodeId] = function.lpLiftedFunction;
            auto &state = context.functions[function.lpLiftedFunction];
            state.parameters.resize(function.lpLiftedFunction ? function.lpLiftedFunction->numparams : 0);
            state.upvalues.resize(function.lpLiftedFunction && function.lpLiftedFunction->lpDeserialized ? function.lpLiftedFunction->lpDeserialized->nups : 0);
            if (function.lpLiftedFunction)
                for (auto &instruction : function.lpLiftedFunction->instructions)
                    context.instructionOwners[&instruction] = function.lpLiftedFunction;
            for (auto inner = function.innerFunctions.rbegin(); inner != function.innerFunctions.rend(); ++inner)
                pending.push_back(&*inner);
        }
    }

    static std::optional<std::string> FieldKey(const Constant &constant) {
        if (const auto *string = std::get_if<StringValue>(&constant))
            return "s:" + string->value;
        if (const auto *integer = std::get_if<int64_t>(&constant))
            return "n:" + std::to_string(*integer);
        if (const auto *number = std::get_if<double>(&constant); number && std::isfinite(*number) && std::trunc(*number) == *number)
            return "n:" + std::to_string(static_cast<int64_t>(*number));
        if (const auto *boolean = std::get_if<bool>(&constant))
            return *boolean ? "b:1" : "b:0";
        return std::nullopt;
    }

    static std::optional<Constant> DecodeConstant(const AnalyzedFunction &function, int32_t index) {
        if (!function.lpLiftedFunction || !function.lpLiftedFunction->lpDeserialized || index < 0)
            return std::nullopt;
        const auto &constants = function.lpLiftedFunction->lpDeserialized->constants;
        if (static_cast<size_t>(index) >= constants.size())
            return std::nullopt;
        const auto &value = constants[index];
        switch (value.kType) {
        case LUA_TNIL:
            return Constant{std::monostate{}};
        case LUA_TBOOLEAN:
            return Constant{std::get<bool>(value.constantData)};
        case LUA_TNUMBER:
            return Constant{std::get<double>(value.constantData)};
        case LUA_TINTEGER:
            return Constant{std::get<int64_t>(value.constantData)};
        case LUA_TSTRING:
            return Constant{StringValue{std::get<std::string>(value.constantData)}};
        default:
            return std::nullopt;
        }
    }

    static Value ReadOperand(
        const AnalyzedFunction &function, const LiftedOperand &operand, const std::unordered_map<SSARef, Value> &values, const ProgramContext *context = nullptr
    ) {
        switch (operand.type) {
        case LiftedOperandType::ImmediateNil:
            return Value::Known(std::monostate{});
        case LiftedOperandType::ImmediateBool:
            return Value::Known(operand.value.imm.b);
        case LiftedOperandType::ImmediateInteger:
            return Value::Known(static_cast<double>(operand.value.imm.n));
        case LiftedOperandType::ImmediateConstant:
            if (auto constant = DecodeConstant(function, operand.value.imm.k))
                return Value::Known(std::move(*constant));
            return Value::Overdefined();
        case LiftedOperandType::Register: {
            const auto it = values.find({operand.value.reg, operand.ssaVersion});
            if (it != values.end())
                return it->second;
            if (context && function.lpLiftedFunction && operand.value.reg < function.lpLiftedFunction->numparams) {
                const auto state = context->functions.find(function.lpLiftedFunction);
                if (state != context->functions.end() && operand.value.reg < state->second.parameters.size()) {
                    const Value value = state->second.parameters[operand.value.reg];
                    return context->finalized && value.state == LatticeState::Unknown ? Value::Overdefined() : value;
                }
            }
            return Value::Overdefined();
        }
        default:
            return Value::Overdefined();
        }
    }

    static std::optional<bool> Truthiness(const Value &value) {
        if (value.state != LatticeState::Constant)
            return std::nullopt;
        if (std::holds_alternative<std::monostate>(value.constant))
            return false;
        if (const auto *boolean = std::get_if<bool>(&value.constant))
            return *boolean;
        if (std::holds_alternative<SymbolValue>(value.constant) || std::holds_alternative<TableValue>(value.constant) ||
            std::holds_alternative<ClosureValue>(value.constant))
            return std::nullopt;
        return true;
    }

    static Value EvaluateArithmetic(LiftedOperation operation, const Value &left, const Value &right) {
        if (left.state == LatticeState::Overdefined || right.state == LatticeState::Overdefined)
            return Value::Overdefined();
        if (left.state != LatticeState::Constant || right.state != LatticeState::Constant)
            return {};
        const auto *a = std::get_if<double>(&left.constant);
        const auto *b = std::get_if<double>(&right.constant);
        if (!a || !b || !std::isfinite(*a) || !std::isfinite(*b))
            return Value::Overdefined();

        double result = 0;
        switch (operation) {
        case LiftedOperation::ADD:
        case LiftedOperation::ADDK:
            result = *a + *b;
            break;
        case LiftedOperation::SUB:
        case LiftedOperation::SUBK:
        case LiftedOperation::SUBRK:
            result = *a - *b;
            break;
        case LiftedOperation::MUL:
        case LiftedOperation::MULK:
            result = *a * *b;
            break;
        case LiftedOperation::DIV:
        case LiftedOperation::DIVK:
        case LiftedOperation::DIVRK:
            if (*b == 0)
                return Value::Overdefined();
            result = *a / *b;
            break;
        case LiftedOperation::IDIV:
        case LiftedOperation::IDIVK:
            if (*b == 0)
                return Value::Overdefined();
            result = std::floor(*a / *b);
            break;
        case LiftedOperation::MOD:
        case LiftedOperation::MODK:
            if (*b == 0)
                return Value::Overdefined();
            result = *a - std::floor(*a / *b) * *b;
            break;
        case LiftedOperation::POW:
        case LiftedOperation::POWK:
            result = std::pow(*a, *b);
            break;
        default:
            return Value::Overdefined();
        }
        if (!std::isfinite(result))
            return Value::Overdefined();
        return Value::Known(result);
    }

    static std::optional<uint32_t> ToUint32(const Value &value) {
        if (value.state != LatticeState::Constant)
            return std::nullopt;
        double number = 0;
        if (const auto *floating = std::get_if<double>(&value.constant))
            number = *floating;
        else if (const auto *integer = std::get_if<int64_t>(&value.constant))
            number = static_cast<double>(*integer);
        else
            return std::nullopt;
        if (!std::isfinite(number) || std::trunc(number) != number || std::abs(number) > 9007199254740991.0)
            return std::nullopt;
        double reduced = std::fmod(number, 4294967296.0);
        if (reduced < 0)
            reduced += 4294967296.0;
        return static_cast<uint32_t>(reduced);
    }

    static Value EvaluateBuiltin(const SymbolValue &symbol, const std::vector<Value> &arguments) {
        if (symbol.path == "bit32.bnot" && arguments.size() == 1) {
            if (auto value = ToUint32(arguments[0]))
                return Value::Known(static_cast<double>(~*value));
            return arguments[0].state == LatticeState::Unknown ? Value{} : Value::Overdefined();
        }
        if ((symbol.path == "bit32.bxor" || symbol.path == "bit32.band" || symbol.path == "bit32.bor") && !arguments.empty()) {
            uint32_t result = symbol.path == "bit32.band" ? std::numeric_limits<uint32_t>::max() : 0;
            for (const auto &argument : arguments) {
                const auto value = ToUint32(argument);
                if (!value)
                    return argument.state == LatticeState::Unknown ? Value{} : Value::Overdefined();
                if (symbol.path == "bit32.bxor")
                    result ^= *value;
                else if (symbol.path == "bit32.band")
                    result &= *value;
                else
                    result |= *value;
            }
            return Value::Known(static_cast<double>(result));
        }
        if ((symbol.path == "bit32.lshift" || symbol.path == "bit32.rshift" || symbol.path == "bit32.arshift") && arguments.size() == 2) {
            const auto value = ToUint32(arguments[0]);
            const auto shift = ToUint32(arguments[1]);
            if (!value || !shift || *shift >= 32)
                return Value::Overdefined();
            if (symbol.path == "bit32.lshift")
                return Value::Known(static_cast<double>(*value << *shift));
            if (symbol.path == "bit32.rshift")
                return Value::Known(static_cast<double>(*value >> *shift));
            return Value::Known(static_cast<double>(static_cast<uint32_t>(static_cast<int32_t>(*value) >> *shift)));
        }
        return Value::Overdefined();
    }

    static std::optional<SymbolValue> ImportSymbol(const AnalyzedFunction &function, const LiftedInstruction &instruction) {
        if (!function.lpLiftedFunction || !function.lpLiftedFunction->lpDeserialized)
            return std::nullopt;
        const auto &constants = function.lpLiftedFunction->lpDeserialized->constants;
        std::string path;
        if (instruction.operation == LiftedOperation::GETGLOBAL && instruction.operands.size() > 1) {
            const auto constant = DecodeConstant(function, instruction.operands[1].value.imm.k);
            if (!constant || !std::holds_alternative<StringValue>(*constant))
                return std::nullopt;
            path = std::get<StringValue>(*constant).value;
        } else if (instruction.operation == LiftedOperation::GETIMPORT && instruction.operands.size() > 2) {
            const uint32_t data = instruction.operands[2].value.imm.u;
            const int count = static_cast<int>(data >> 30);
            for (int i = 0; i < count; ++i) {
                const int shift = 20 - i * 10;
                const int index = static_cast<int>(data >> shift) & 1023;
                if (index < 0 || static_cast<size_t>(index) >= constants.size() || constants[index].kType != LUA_TSTRING)
                    return std::nullopt;
                if (!path.empty())
                    path += '.';
                path += std::get<std::string>(constants[index].constantData);
            }
        }
        if (path == "bit32" || path.starts_with("bit32.") || path == "setmetatable") {
            const std::string root = path.starts_with("bit32") ? "bit32" : path;
            for (const auto &candidate : function.lpLiftedFunction->instructions) {
                if (candidate.operation != LiftedOperation::SETGLOBAL || candidate.operands.size() < 2)
                    continue;
                const auto name = DecodeConstant(function, candidate.operands[1].value.imm.k);
                if (name && std::holds_alternative<StringValue>(*name) && std::get<StringValue>(*name).value == root)
                    return std::nullopt;
            }
            return SymbolValue{std::move(path)};
        }
        return std::nullopt;
    }

    static Value ReadTableKey(
        const AnalyzedFunction &function, const LiftedInstruction &instruction, const std::unordered_map<SSARef, Value> &values, const ProgramContext *context
    ) {
        if (instruction.operands.size() < 3)
            return Value::Overdefined();
        if (instruction.operation == LiftedOperation::GETTABLEN || instruction.operation == LiftedOperation::SETTABLEN)
            return Value::Known(static_cast<double>(instruction.operands[2].value.imm.n + 1));
        return ReadOperand(function, instruction.operands[2], values, context);
    }

    static void MarkEscaped(ProgramContext &context, const Value &root) {
        std::vector<Value> pending{root};
        std::unordered_set<const LiftedInstruction *> visited;
        while (!pending.empty()) {
            const Value value = std::move(pending.back());
            pending.pop_back();
            if (value.state != LatticeState::Constant)
                continue;
            if (const auto *closure = std::get_if<ClosureValue>(&value.constant)) {
                if (!visited.insert(closure->allocation).second)
                    continue;
                auto target = context.functions.find(closure->function);
                if (target != context.functions.end() && !target->second.external) {
                    target->second.external = true;
                    target->second.reachable = true;
                    for (auto &parameter : target->second.parameters)
                        parameter = Value::Overdefined();
                    for (auto &upvalue : target->second.upvalues)
                        upvalue = Value::Overdefined();
                    context.changed = true;
                }
                const auto captures = context.closureCaptures.find(closure->allocation);
                if (captures != context.closureCaptures.end())
                    pending.insert(pending.end(), captures->second.begin(), captures->second.end());
            } else if (const auto *table = std::get_if<TableValue>(&value.constant)) {
                if (!visited.insert(table->allocation).second)
                    continue;
                auto found = context.tables.find(table->allocation);
                if (found == context.tables.end())
                    continue;
                if (!found->second.escaped) {
                    found->second.escaped = true;
                    context.changed = true;
                }
                for (const auto &[key, field] : found->second.fields)
                    for (const auto &[write, stored] : field.writes)
                        pending.push_back(stored);
            }
        }
    }

    static Value ResolveField(
        const AnalyzedFunction &function, const LiftedInstruction &instruction, const Value &tableValue, const Value &keyValue, const ProgramContext *context
    ) {
        if (tableValue.state == LatticeState::Unknown || keyValue.state == LatticeState::Unknown)
            return {};
        if (tableValue.state != LatticeState::Constant || keyValue.state != LatticeState::Constant)
            return Value::Overdefined();
        if (const auto *base = std::get_if<SymbolValue>(&tableValue.constant)) {
            const auto *field = std::get_if<StringValue>(&keyValue.constant);
            if (field && base->path == "bit32")
                return Value::Known(SymbolValue{base->path + "." + field->value});
            return Value::Overdefined();
        }
        if (!context)
            return Value::Overdefined();
        const auto *table = std::get_if<TableValue>(&tableValue.constant);
        const auto key = FieldKey(keyValue.constant);
        if (!table || !key)
            return Value::Overdefined();
        const auto tableIt = context->tables.find(table->allocation);
        if (tableIt == context->tables.end() || tableIt->second.dynamicWrite || tableIt->second.escaped)
            return Value::Overdefined();
        const auto fieldIt = tableIt->second.fields.find(*key);
        if (fieldIt == tableIt->second.fields.end() || fieldIt->second.writes.empty())
            return Value::Overdefined();
        const auto allocationBlock = context->instructionBlocks.find(table->allocation);
        const auto readBlock = context->instructionBlocks.find(&instruction);
        if (allocationBlock == context->instructionBlocks.end() || readBlock == context->instructionBlocks.end())
            return Value::Overdefined();

        Value result{};
        for (const auto &[write, value] : fieldIt->second.writes) {
            const auto owner = context->instructionOwners.find(write);
            const auto writeBlock = context->instructionBlocks.find(write);
            if (owner == context->instructionOwners.end() || owner->second != tableIt->second.owner || writeBlock == context->instructionBlocks.end() ||
                write->instructionIndex <= tableIt->second.allocationIndex)
                return Value::Overdefined();
            if (function.lpLiftedFunction == tableIt->second.owner) {
                if (allocationBlock->second != readBlock->second || writeBlock->second != readBlock->second ||
                    write->instructionIndex >= instruction.instructionIndex)
                    return Value::Overdefined();
            } else {
                const auto analyzedOwner = context->analyzedFunctions.find(tableIt->second.owner);
                if (analyzedOwner == context->analyzedFunctions.end() || analyzedOwner->second->basicBlocks.empty() ||
                    allocationBlock->second != analyzedOwner->second->basicBlocks.front().dwBlockId ||
                    writeBlock->second != analyzedOwner->second->basicBlocks.front().dwBlockId || write->instructionIndex >= tableIt->second.firstCallIndex)
                    return Value::Overdefined();
            }
            if (value.state == LatticeState::Unknown)
                return context->finalized ? Value::Overdefined() : Value{};
            if (value.state == LatticeState::Overdefined)
                return Value::Overdefined();
            result = Join(result, value);
        }
        if (result.state == LatticeState::Constant && !std::holds_alternative<SymbolValue>(result.constant) &&
            !std::holds_alternative<TableValue>(result.constant) && !std::holds_alternative<ClosureValue>(result.constant))
            return Value::Overdefined();
        return result;
    }

    static void RecordTableWrite(
        const AnalyzedFunction &function, LiftedInstruction &instruction, const std::unordered_map<SSARef, Value> &values, ProgramContext &context
    ) {
        if (instruction.operands.size() < 3)
            return;
        const Value stored = ReadOperand(function, instruction.operands[0], values, &context);
        const Value target = ReadOperand(function, instruction.operands[1], values, &context);
        if (target.state != LatticeState::Constant || !std::holds_alternative<TableValue>(target.constant)) {
            MarkEscaped(context, stored);
            return;
        }
        const auto tableValue = std::get<TableValue>(target.constant);
        auto found = context.tables.find(tableValue.allocation);
        if (found == context.tables.end())
            return;
        const Value keyValue = ReadTableKey(function, instruction, values, &context);
        if (keyValue.state != LatticeState::Constant) {
            if (!found->second.dynamicWrite) {
                found->second.dynamicWrite = true;
                context.changed = true;
            }
            return;
        }
        const auto key = FieldKey(keyValue.constant);
        if (!key) {
            if (!found->second.dynamicWrite) {
                found->second.dynamicWrite = true;
                context.changed = true;
            }
            return;
        }
        auto &slot = found->second.fields[*key].writes[&instruction];
        if (!SameValue(slot, stored)) {
            slot = stored;
            context.changed = true;
        }
        if (found->second.escaped)
            MarkEscaped(context, stored);
    }

    static std::vector<Value> CallArguments(
        const AnalyzedFunction &function, const LiftedInstruction &instruction, const std::unordered_map<SSARef, Value> &values, const ProgramContext *context
    ) {
        std::vector<Value> arguments;
        const auto uses = function.implicitUses.find(&instruction);
        if (uses == function.implicitUses.end() || instruction.operands.empty())
            return arguments;
        arguments.reserve(uses->second.size());
        for (size_t i = 0; i < uses->second.size(); ++i) {
            LiftedOperand argument{};
            argument.type = LiftedOperandType::Register;
            argument.value.reg = static_cast<uint8_t>(instruction.operands[0].value.reg + 1 + i);
            argument.ssaVersion = uses->second[i];
            arguments.push_back(ReadOperand(function, argument, values, context));
        }
        return arguments;
    }

    static Value AliasOnly(const Value &value) {
        if (value.state != LatticeState::Constant)
            return value;
        if (std::holds_alternative<TableValue>(value.constant) || std::holds_alternative<ClosureValue>(value.constant) ||
            std::holds_alternative<SymbolValue>(value.constant))
            return value;
        return Value::Overdefined();
    }

    static void NoteTableUse(ProgramContext &context, LiftedFunction *owner, int32_t instructionIndex, const Value &value) {
        if (value.state != LatticeState::Constant)
            return;
        if (const auto *table = std::get_if<TableValue>(&value.constant)) {
            auto found = context.tables.find(table->allocation);
            if (found != context.tables.end() && found->second.owner == owner) {
                const int32_t firstCall = std::min(found->second.firstCallIndex, instructionIndex);
                if (firstCall != found->second.firstCallIndex) {
                    found->second.firstCallIndex = firstCall;
                    context.changed = true;
                }
            }
        }
    }

    static void MergeCallInputs(ProgramContext &context, const ClosureValue &closure, const std::vector<Value> &arguments) {
        auto target = context.functions.find(closure.function);
        if (target == context.functions.end())
            return;
        if (!target->second.reachable) {
            target->second.reachable = true;
            context.changed = true;
        }
        for (size_t i = 0; i < target->second.parameters.size(); ++i) {
            const Value incoming = i < arguments.size() ? AliasOnly(arguments[i]) : Value::Overdefined();
            context.changed |= MergeInto(target->second.parameters[i], incoming);
        }
        const auto captures = context.closureCaptures.find(closure.allocation);
        if (captures != context.closureCaptures.end())
            for (size_t i = 0; i < target->second.upvalues.size(); ++i) {
                const Value incoming = i < captures->second.size() ? AliasOnly(captures->second[i]) : Value::Overdefined();
                context.changed |= MergeInto(target->second.upvalues[i], incoming);
            }
    }

    using Definitions = std::unordered_map<LiftedInstruction *, std::vector<SSARef>>;

    static Value EvaluateDefinition(
        const AnalyzedFunction &function, LiftedInstruction &instruction, const SSARef &destination, const std::unordered_map<SSARef, Value> &values,
        ProgramContext *context, bool singleDefinition
    ) {
        auto operand = [&](size_t index) {
            return index < instruction.operands.size() ? ReadOperand(function, instruction.operands[index], values, context) : Value::Overdefined();
        };
        switch (instruction.operation) {
        case LiftedOperation::LOAD:
        case LiftedOperation::LOADNJUMP:
        case LiftedOperation::MOVE:
            return operand(1);
        case LiftedOperation::GETGLOBAL:
        case LiftedOperation::GETIMPORT:
            if (auto symbol = ImportSymbol(function, instruction))
                return Value::Known(std::move(*symbol));
            return Value::Overdefined();
        case LiftedOperation::GETUPVAL:
            if (context && instruction.operands.size() > 1 && function.lpLiftedFunction) {
                const auto state = context->functions.find(function.lpLiftedFunction);
                const int32_t index = instruction.operands[1].value.imm.n;
                if (state != context->functions.end() && index >= 0 && static_cast<size_t>(index) < state->second.upvalues.size()) {
                    const Value value = state->second.upvalues[index];
                    return context->finalized && value.state == LatticeState::Unknown ? Value::Overdefined() : value;
                }
            }
            return Value::Overdefined();
        case LiftedOperation::NEWTABLE:
        case LiftedOperation::DUPTABLE:
            if (context && function.lpLiftedFunction) {
                auto [found, inserted] = context->tables.try_emplace(&instruction);
                if (inserted) {
                    found->second.owner = function.lpLiftedFunction;
                    found->second.allocationIndex = instruction.instructionIndex;
                }
                context->changed |= inserted;
                return Value::Known(TableValue{&instruction});
            }
            return Value::Overdefined();
        case LiftedOperation::NEWCLOSURE:
        case LiftedOperation::DUPCLOSURE:
            if (context && function.lpLiftedFunction && instruction.operands.size() > 1) {
                LiftedFunction *target = nullptr;
                if (instruction.operation == LiftedOperation::NEWCLOSURE) {
                    const int32_t index = instruction.operands[1].value.imm.k;
                    if (index >= 0 && static_cast<size_t>(index) < function.innerFunctions.size())
                        target = function.innerFunctions[index].lpLiftedFunction;
                } else if (instruction.operands.size() > 2) {
                    const auto found = context->functionsByBytecodeId.find(instruction.operands[2].value.imm.n);
                    if (found != context->functionsByBytecodeId.end())
                        target = found->second;
                }
                if (!target)
                    return Value::Overdefined();

                std::vector<Value> captures;
                const auto &instructions = function.lpLiftedFunction->instructions;
                const size_t position = static_cast<size_t>(&instruction - instructions.data());
                for (size_t i = position + 1; i < instructions.size() && instructions[i].operation == LiftedOperation::CAPTURE; ++i) {
                    const auto &capture = instructions[i];
                    if (capture.operands.size() < 2) {
                        captures.push_back(Value::Overdefined());
                        continue;
                    }
                    const int32_t mode = capture.operands[0].value.imm.n;
                    if (mode == 0) {
                        captures.push_back(ReadOperand(function, capture.operands[1], values, context));
                    } else if (mode == 2) {
                        const int32_t index = capture.operands[1].value.reg;
                        const bool written = std::ranges::any_of(function.lpLiftedFunction->instructions, [index](const LiftedInstruction &candidate) {
                            return candidate.operation == LiftedOperation::SETUPVAL && candidate.operands.size() > 1 &&
                                   candidate.operands[1].value.imm.n == index;
                        });
                        const auto state = context->functions.find(function.lpLiftedFunction);
                        captures.push_back(
                            !written && state != context->functions.end() && index >= 0 && static_cast<size_t>(index) < state->second.upvalues.size()
                                ? state->second.upvalues[index]
                                : Value::Overdefined()
                        );
                    } else {
                        captures.push_back(Value::Overdefined());
                    }
                }
                auto &stored = context->closureCaptures[&instruction];
                if (!std::ranges::equal(stored, captures, SameValue)) {
                    stored = std::move(captures);
                    context->changed = true;
                }
                for (const auto &capture : stored)
                    NoteTableUse(*context, function.lpLiftedFunction, instruction.instructionIndex, capture);
                return Value::Known(ClosureValue{&instruction, target});
            }
            return Value::Overdefined();
        case LiftedOperation::GETTABLE:
        case LiftedOperation::GETTABLEKS:
        case LiftedOperation::GETTABLEN: {
            const Value table = operand(1);
            const Value key = ReadTableKey(function, instruction, values, context);
            return ResolveField(function, instruction, table, key, context);
        }
        case LiftedOperation::NAMECALL: {
            const Value table = operand(1);
            if (destination.regIndex == instruction.operands[0].value.reg + 1)
                return table;
            return ResolveField(function, instruction, table, operand(2), context);
        }
        case LiftedOperation::ADD:
        case LiftedOperation::SUB:
        case LiftedOperation::MUL:
        case LiftedOperation::DIV:
        case LiftedOperation::IDIV:
        case LiftedOperation::MOD:
        case LiftedOperation::POW:
        case LiftedOperation::ADDK:
        case LiftedOperation::SUBK:
        case LiftedOperation::MULK:
        case LiftedOperation::DIVK:
        case LiftedOperation::IDIVK:
        case LiftedOperation::MODK:
        case LiftedOperation::POWK:
            return EvaluateArithmetic(instruction.operation, operand(1), operand(2));
        case LiftedOperation::SUBRK:
        case LiftedOperation::DIVRK: {
            LiftedOperand constant = instruction.operands[1];
            constant.type = LiftedOperandType::ImmediateConstant;
            return EvaluateArithmetic(instruction.operation, ReadOperand(function, constant, values, context), operand(2));
        }
        case LiftedOperation::AND:
        case LiftedOperation::ANDK: {
            const Value left = operand(1);
            if (auto truthy = Truthiness(left))
                return *truthy ? operand(2) : left;
            return left.state == LatticeState::Overdefined ? Value::Overdefined() : Value{};
        }
        case LiftedOperation::OR:
        case LiftedOperation::ORK: {
            const Value left = operand(1);
            if (auto truthy = Truthiness(left))
                return *truthy ? left : operand(2);
            return left.state == LatticeState::Overdefined ? Value::Overdefined() : Value{};
        }
        case LiftedOperation::NOT: {
            const Value value = operand(1);
            if (auto truthy = Truthiness(value))
                return Value::Known(!*truthy);
            return value.state == LatticeState::Overdefined ? Value::Overdefined() : Value{};
        }
        case LiftedOperation::MINUS: {
            const Value value = operand(1);
            if (value.state == LatticeState::Unknown)
                return {};
            if (value.state == LatticeState::Constant)
                if (const auto *number = std::get_if<double>(&value.constant); number && std::isfinite(*number))
                    return Value::Known(-*number);
            return Value::Overdefined();
        }
        case LiftedOperation::CALL:
        case LiftedOperation::CALLFB: {
            const Value callee = operand(0);
            if (callee.state == LatticeState::Unknown)
                return {};
            const auto arguments = CallArguments(function, instruction, values, context);
            const auto *symbol = callee.state == LatticeState::Constant ? std::get_if<SymbolValue>(&callee.constant) : nullptr;
            if (context && (!symbol || symbol->path != "setmetatable"))
                for (const auto &argument : arguments)
                    NoteTableUse(*context, function.lpLiftedFunction, instruction.instructionIndex, argument);

            if (symbol) {
                if (!singleDefinition)
                    return Value::Overdefined();
                if (symbol->path == "setmetatable" && !arguments.empty())
                    return arguments.front();
                return EvaluateBuiltin(*symbol, arguments);
            }
            if (const auto *closure = callee.state == LatticeState::Constant ? std::get_if<ClosureValue>(&callee.constant) : nullptr) {
                if (context) {
                    MergeCallInputs(*context, *closure, arguments);
                    const auto target = context->functions.find(closure->function);
                    if (!singleDefinition)
                        return Value::Overdefined();
                    const size_t resultIndex = destination.regIndex - instruction.operands[0].value.reg;
                    if (target != context->functions.end() && resultIndex < target->second.returns.size()) {
                        Value result = AliasOnly(target->second.returns[resultIndex]);
                        return context->finalized && result.state == LatticeState::Unknown ? Value::Overdefined() : result;
                    }
                }
                return {};
            }
            if (context)
                for (const auto &argument : arguments)
                    MarkEscaped(*context, argument);
            return Value::Overdefined();
        }
        default:
            return Value::Overdefined();
        }
    }

    static std::optional<bool> EqualConstants(const Constant &left, const Constant &right) {
        if (std::holds_alternative<SymbolValue>(left) || std::holds_alternative<SymbolValue>(right) || std::holds_alternative<TableValue>(left) ||
            std::holds_alternative<TableValue>(right) || std::holds_alternative<ClosureValue>(left) || std::holds_alternative<ClosureValue>(right))
            return std::nullopt;
        if (const auto *a = std::get_if<double>(&left)) {
            const auto *b = std::get_if<double>(&right);
            return b ? std::optional<bool>(*a == *b) : std::optional<bool>(false);
        }
        if (std::holds_alternative<int64_t>(left) != std::holds_alternative<int64_t>(right) &&
            (std::holds_alternative<int64_t>(left) || std::holds_alternative<int64_t>(right)))
            return std::nullopt;
        if (left.index() != right.index())
            return false;
        return left == right;
    }

    static std::optional<bool> CompareConstants(const Constant &left, const Constant &right, LiftedOperation operation) {
        if (operation == LiftedOperation::JUMPIFEQ || operation == LiftedOperation::JUMPIFNOTEQ) {
            const auto equal = EqualConstants(left, right);
            if (!equal)
                return std::nullopt;
            return operation == LiftedOperation::JUMPIFEQ ? *equal : !*equal;
        }
        if (const auto *a = std::get_if<double>(&left)) {
            const auto *b = std::get_if<double>(&right);
            if (!b)
                return std::nullopt;
            const bool less = *a < *b;
            const bool lessEqual = *a <= *b;
            if (operation == LiftedOperation::JUMPIFLT)
                return less;
            if (operation == LiftedOperation::JUMPIFLE)
                return lessEqual;
            if (operation == LiftedOperation::JUMPIFNOTLT)
                return !less;
            if (operation == LiftedOperation::JUMPIFNOTLE)
                return !lessEqual;
        }
        return std::nullopt;
    }

    static std::optional<bool> EvaluateBranch(
        const AnalyzedFunction &function, const LiftedInstruction &instruction, const std::unordered_map<SSARef, Value> &values,
        const ProgramContext *context = nullptr
    ) {
        auto operand = [&](size_t index) {
            return index < instruction.operands.size() ? ReadOperand(function, instruction.operands[index], values, context) : Value::Overdefined();
        };
        if (instruction.operation == LiftedOperation::JUMPIF || instruction.operation == LiftedOperation::JUMPIFNOT) {
            const auto truthy = Truthiness(operand(0));
            if (!truthy)
                return std::nullopt;
            return instruction.operation == LiftedOperation::JUMPIF ? *truthy : !*truthy;
        }
        if (instruction.operation == LiftedOperation::JUMPXEQK && instruction.operands.size() >= 4) {
            const Value left = operand(0);
            const Value right = operand(2);
            if (left.state != LatticeState::Constant || right.state != LatticeState::Constant)
                return std::nullopt;
            const auto equal = EqualConstants(left.constant, right.constant);
            if (!equal)
                return std::nullopt;
            return instruction.operands[3].value.imm.b ? !*equal : *equal;
        }
        if (instruction.operands.size() < 3)
            return std::nullopt;
        const Value left = operand(0);
        const Value right = operand(2);
        if (left.state != LatticeState::Constant || right.state != LatticeState::Constant)
            return std::nullopt;
        return CompareConstants(left.constant, right.constant, instruction.operation);
    }

    static bool CanMaterialize(const Constant &constant) {
        return !std::holds_alternative<SymbolValue>(constant) && !std::holds_alternative<TableValue>(constant) &&
               !std::holds_alternative<ClosureValue>(constant);
    }

    static void Materialize(AnalyzedFunction &function, LiftedInstruction &instruction, const SSARef &destination, const Constant &constant) {
        LiftedOperand dest{};
        dest.type = LiftedOperandType::Register;
        dest.value.reg = destination.regIndex;
        dest.ssaVersion = destination.version;
        LiftedOperand value{};
        if (std::holds_alternative<std::monostate>(constant)) {
            value.type = LiftedOperandType::ImmediateNil;
        } else if (const auto *boolean = std::get_if<bool>(&constant)) {
            value.type = LiftedOperandType::ImmediateBool;
            value.value.imm.b = *boolean;
        } else if (
            const auto *number = std::get_if<double>(&constant); number && std::isfinite(*number) && !(*number == 0 && std::signbit(*number)) &&
                                                                 std::trunc(*number) == *number && *number >= std::numeric_limits<int32_t>::min() &&
                                                                 *number <= std::numeric_limits<int32_t>::max()
        ) {
            value.type = LiftedOperandType::ImmediateInteger;
            value.value.imm.n = static_cast<int32_t>(*number);
        } else {
            auto *deserialized = function.lpLiftedFunction->lpDeserialized;
            LuauConstant stored{};
            if (number) {
                stored.kType = LUA_TNUMBER;
                stored.constantData = *number;
            } else if (const auto *integer = std::get_if<int64_t>(&constant)) {
                stored.kType = LUA_TINTEGER;
                stored.constantData = *integer;
            } else {
                stored.kType = LUA_TSTRING;
                stored.constantData = std::get<StringValue>(constant).value;
            }
            deserialized->constants.push_back(std::move(stored));
            value.type = LiftedOperandType::ImmediateConstant;
            value.value.imm.k = static_cast<int32_t>(deserialized->constants.size() - 1);
        }
        instruction.operation = LiftedOperation::LOAD;
        instruction.operands = {dest, value};
        instruction.instructionRemarks.reset();
    }

    static std::optional<uint32_t> StringConstantIndex(AnalyzedFunction &function, const std::string &value) {
        auto &constants = function.lpLiftedFunction->lpDeserialized->constants;
        for (size_t i = 0; i < std::min<size_t>(constants.size(), 1024); ++i)
            if (constants[i].kType == LUA_TSTRING && std::get<std::string>(constants[i].constantData) == value)
                return static_cast<uint32_t>(i);
        if (constants.size() >= 1024)
            return std::nullopt;
        LuauConstant constant{};
        constant.kType = LUA_TSTRING;
        constant.constantData = value;
        constants.push_back(std::move(constant));
        return static_cast<uint32_t>(constants.size() - 1);
    }

    static LiftedInstruction *FindSingleUseTableRead(AnalyzedFunction &function, LiftedOperand operand) {
        while (operand.type == LiftedOperandType::Register && function.IsSingleUse(operand)) {
            LiftedInstruction *definition = function.GetDefinition(operand);
            if (!definition)
                return nullptr;
            if (definition->operation == LiftedOperation::MOVE && definition->operands.size() > 1) {
                operand = definition->operands[1];
                continue;
            }
            if (definition->operation == LiftedOperation::GETTABLE || definition->operation == LiftedOperation::GETTABLEKS ||
                definition->operation == LiftedOperation::GETTABLEN)
                return definition;
            return nullptr;
        }
        return nullptr;
    }

    static bool CanonicalizeFoldableBuiltin(
        AnalyzedFunction &function, LiftedInstruction &call, const Value &result, const std::unordered_map<SSARef, Value> &values, ProgramContext &context
    ) {
        if (call.operands.empty() || result.state != LatticeState::Constant || !CanMaterialize(result.constant))
            return false;
        const Value callee = ReadOperand(function, call.operands[0], values, &context);
        const auto *symbol = callee.state == LatticeState::Constant ? std::get_if<SymbolValue>(&callee.constant) : nullptr;
        if (!symbol || !symbol->path.starts_with("bit32."))
            return false;
        LiftedInstruction *source = FindSingleUseTableRead(function, call.operands[0]);
        if (!source)
            return false;
        const size_t separator = symbol->path.find('.');
        if (separator == std::string::npos || symbol->path.find('.', separator + 1) != std::string::npos)
            return false;
        const auto root = StringConstantIndex(function, symbol->path.substr(0, separator));
        const auto member = StringConstantIndex(function, symbol->path.substr(separator + 1));
        if (!root || !member)
            return false;

        LiftedOperand destination = source->operands[0];
        LiftedOperand cached{};
        cached.type = LiftedOperandType::ImmediateConstant;
        cached.value.imm.k = static_cast<int32_t>(*root);
        LiftedOperand path{};
        path.type = LiftedOperandType::ImmediateAux;
        path.value.imm.u = (2u << 30) | (*root << 20) | (*member << 10);
        source->operation = LiftedOperation::GETIMPORT;
        source->operands = {destination, cached, path};
        source->instructionRemarks.reset();
        return true;
    }

    static uint64_t EdgeKey(uint32_t from, uint32_t to) { return (static_cast<uint64_t>(from) << 32) | to; }

    static bool CollectStableDependencies(const AnalyzedFunction &function, const SSARef &reference, std::vector<SSARef> &dependencies) {
        const auto definition = function.definitionMap.find(reference);
        if (definition == function.definitionMap.end())
            return false;
        const LiftedInstruction &instruction = *definition->second;
        for (const auto &operand : instruction.operands)
            if (operand.type == LiftedOperandType::Register) {
                const SSARef dependency{operand.value.reg, operand.ssaVersion};
                if (dependency != reference)
                    dependencies.push_back(dependency);
            }
        if (instruction.operation != LiftedOperation::CALL && instruction.operation != LiftedOperation::CALLFB)
            return true;
        const auto uses = function.implicitUses.find(&instruction);
        if (uses == function.implicitUses.end() || instruction.operands.empty())
            return false;
        for (size_t i = 0; i < uses->second.size(); ++i)
            dependencies.push_back(SSARef{static_cast<uint8_t>(instruction.operands[0].value.reg + 1 + i), uses->second[i]});
        return true;
    }

    static bool IsStableReference(const AnalyzedFunction &function, const SSARef &reference, std::unordered_map<SSARef, bool> &memo) {
        if (const auto found = memo.find(reference); found != memo.end())
            return found->second;
        struct Frame {
            SSARef reference;
            std::vector<SSARef> dependencies;
            size_t next = 0;
            bool stable = false;
        };
        std::vector<Frame> stack;
        std::unordered_set<SSARef> active;
        const auto push = [&](const SSARef &current) {
            Frame frame{};
            frame.reference = current;
            frame.stable = CollectStableDependencies(function, current, frame.dependencies);
            active.insert(current);
            stack.push_back(std::move(frame));
        };
        push(reference);
        while (!stack.empty()) {
            Frame &frame = stack.back();
            if (!frame.stable || frame.next == frame.dependencies.size()) {
                const bool stable = frame.stable;
                memo[frame.reference] = stable;
                active.erase(frame.reference);
                stack.pop_back();
                if (!stack.empty() && !stable)
                    stack.back().stable = false;
                continue;
            }
            const SSARef dependency = frame.dependencies[frame.next++];
            if (const auto found = memo.find(dependency); found != memo.end()) {
                if (!found->second)
                    frame.stable = false;
            } else if (active.contains(dependency)) {
                frame.stable = false;
            } else {
                push(dependency);
            }
        }
        return memo[reference];
    }

    static ConstantPropagationStats RunOne(AnalyzedFunction &function, ProgramContext *context, bool rewrite) {
        Definitions definitions;
        std::unordered_map<const LiftedInstruction *, uint32_t> instructionBlocks;
        for (const auto &block : function.basicBlocks) {
            for (const auto &phi : block.phiNodes)
                instructionBlocks[&phi] = block.dwBlockId;
            if (block.lpHead)
                for (const LiftedInstruction *instruction = block.lpHead; instruction <= block.lpTail; ++instruction)
                    instructionBlocks[instruction] = block.dwBlockId;
        }
        if (context)
            context->instructionBlocks.insert(instructionBlocks.begin(), instructionBlocks.end());
        std::unordered_map<SSARef, Value> values;
        for (const auto &[reference, instruction] : function.definitionMap) {
            definitions[instruction].push_back(reference);
            values.try_emplace(reference);
        }

        std::unordered_set<uint32_t> executableBlocks{0};
        std::unordered_set<uint64_t> executableEdges;
        bool changed = true;
        while (changed) {
            CheckDecompileDeadline();
            changed = false;
            for (auto &block : function.basicBlocks) {
                if (!executableBlocks.contains(block.dwBlockId))
                    continue;
                for (auto &phi : block.phiNodes) {
                    Value incoming{};
                    for (size_t i = 0; i < block.predecessors.size() && i + 1 < phi.operands.size(); ++i)
                        if (executableEdges.contains(EdgeKey(block.predecessors[i], block.dwBlockId)))
                            incoming = Join(incoming, ReadOperand(function, phi.operands[i + 1], values, context));
                    if (block.dwBlockId == 0 && phi.operands.size() > block.predecessors.size() + 1)
                        incoming = Join(incoming, ReadOperand(function, phi.operands.back(), values, context));
                    const auto defs = definitions.find(&phi);
                    if (defs != definitions.end())
                        for (const auto &reference : defs->second)
                            changed |= MergeInto(values[reference], incoming);
                }
                if (block.lpHead)
                    for (LiftedInstruction *instruction = block.lpHead; instruction <= block.lpTail; ++instruction) {
                        const auto defs = definitions.find(instruction);
                        if (defs != definitions.end())
                            for (const auto &reference : defs->second)
                                changed |= MergeInto(
                                    values[reference], EvaluateDefinition(function, *instruction, reference, values, context, defs->second.size() == 1)
                                );
                        else if (context && (instruction->operation == LiftedOperation::CALL || instruction->operation == LiftedOperation::CALLFB))
                            EvaluateDefinition(
                                function, *instruction, SSARef{instruction->operands.empty() ? 0 : instruction->operands[0].value.reg, 0}, values, context,
                                false
                            );

                        if (context && (instruction->operation == LiftedOperation::SETTABLE || instruction->operation == LiftedOperation::SETTABLEKS ||
                                        instruction->operation == LiftedOperation::SETTABLEN))
                            RecordTableWrite(function, *instruction, values, *context);
                        if (context && instruction->operation == LiftedOperation::SETLIST && !instruction->operands.empty()) {
                            const Value target = ReadOperand(function, instruction->operands[0], values, context);
                            if (target.state == LatticeState::Constant)
                                if (const auto *table = std::get_if<TableValue>(&target.constant)) {
                                    auto found = context->tables.find(table->allocation);
                                    if (found != context->tables.end() && !found->second.dynamicWrite) {
                                        found->second.dynamicWrite = true;
                                        context->changed = true;
                                    }
                                }
                        }
                        if (context && instruction->operation == LiftedOperation::SETGLOBAL && !instruction->operands.empty())
                            MarkEscaped(*context, ReadOperand(function, instruction->operands[0], values, context));
                        if (context && instruction->operation == LiftedOperation::SETUPVAL && instruction->operands.size() > 1 && function.lpLiftedFunction) {
                            auto state = context->functions.find(function.lpLiftedFunction);
                            const int32_t index = instruction->operands[1].value.imm.n;
                            if (state != context->functions.end() && index >= 0 && static_cast<size_t>(index) < state->second.upvalues.size())
                                context->changed |= MergeInto(state->second.upvalues[index], Value::Overdefined());
                        }
                    }

                auto markEdge = [&](uint32_t successor) {
                    changed |= executableEdges.insert(EdgeKey(block.dwBlockId, successor)).second;
                    changed |= executableBlocks.insert(successor).second;
                };
                if (block.bTerminator == BlockTerminator::Conditional && block.lpTail && block.ifStatementTrue && block.ifStatementFalse &&
                    block.lpTail->operation != LiftedOperation::FORNLOOP && block.lpTail->operation != LiftedOperation::FORGLOOP) {
                    const auto condition = EvaluateBranch(function, *block.lpTail, values, context);
                    if (condition)
                        markEdge(*condition ? *block.ifStatementTrue : *block.ifStatementFalse);
                    else {
                        markEdge(*block.ifStatementTrue);
                        markEdge(*block.ifStatementFalse);
                    }
                } else {
                    for (uint32_t successor : block.successors)
                        markEdge(successor);
                }
            }
        }

        if (context && function.lpLiftedFunction) {
            auto state = context->functions.find(function.lpLiftedFunction);
            if (state != context->functions.end())
                for (const auto &block : function.basicBlocks) {
                    if (!executableBlocks.contains(block.dwBlockId) || !block.lpHead)
                        continue;
                    for (const LiftedInstruction *instruction = block.lpHead; instruction <= block.lpTail; ++instruction) {
                        if (instruction->operation != LiftedOperation::RETURN)
                            continue;
                        const auto uses = function.implicitUses.find(instruction);
                        if (uses == function.implicitUses.end() || instruction->operands.empty())
                            continue;
                        if (state->second.returns.size() < uses->second.size())
                            state->second.returns.resize(uses->second.size());
                        for (size_t i = 0; i < uses->second.size(); ++i) {
                            LiftedOperand returned{};
                            returned.type = LiftedOperandType::Register;
                            returned.value.reg = static_cast<uint8_t>(instruction->operands[0].value.reg + i);
                            returned.ssaVersion = uses->second[i];
                            const Value value = ReadOperand(function, returned, values, context);
                            context->changed |= MergeInto(state->second.returns[i], value);
                            if (function.lpLiftedFunction == context->root)
                                MarkEscaped(*context, value);
                        }
                    }
                }
        }

        ConstantPropagationStats stats{};
        if (!rewrite)
            return stats;
        if (context) {
            for (auto &[instruction, refs] : definitions) {
                const auto block = instructionBlocks.find(instruction);
                if (refs.size() != 1 || block == instructionBlocks.end() || !executableBlocks.contains(block->second) ||
                    (instruction->operation != LiftedOperation::CALL && instruction->operation != LiftedOperation::CALLFB))
                    continue;
                const auto value = values.find(refs.front());
                if (value != values.end() && CanonicalizeFoldableBuiltin(function, *instruction, value->second, values, *context))
                    ++stats.foldedInstructions;
            }
            return stats;
        }
        std::unordered_map<SSARef, bool> memo;
        for (auto &[instruction, refs] : definitions) {
            const auto block = instructionBlocks.find(instruction);
            if (refs.size() != 1 || block == instructionBlocks.end() || !executableBlocks.contains(block->second))
                continue;
            const auto value = values.find(refs.front());
            if (value == values.end() || value->second.state != LatticeState::Constant || !CanMaterialize(value->second.constant))
                continue;
            if (std::holds_alternative<std::monostate>(value->second.constant) || std::holds_alternative<bool>(value->second.constant))
                continue;
            if (!IsStableReference(function, refs.front(), memo))
                continue;
            switch (instruction->operation) {
            case LiftedOperation::MOVE:
            case LiftedOperation::ADD:
            case LiftedOperation::SUB:
            case LiftedOperation::MUL:
            case LiftedOperation::DIV:
            case LiftedOperation::IDIV:
            case LiftedOperation::MOD:
            case LiftedOperation::POW:
            case LiftedOperation::ADDK:
            case LiftedOperation::SUBK:
            case LiftedOperation::MULK:
            case LiftedOperation::DIVK:
            case LiftedOperation::IDIVK:
            case LiftedOperation::MODK:
            case LiftedOperation::POWK:
            case LiftedOperation::SUBRK:
            case LiftedOperation::DIVRK:
            case LiftedOperation::AND:
            case LiftedOperation::OR:
            case LiftedOperation::ANDK:
            case LiftedOperation::ORK:
            case LiftedOperation::NOT:
            case LiftedOperation::MINUS:
            case LiftedOperation::CALL:
            case LiftedOperation::CALLFB:
                Materialize(function, *instruction, refs.front(), value->second.constant);
                ++stats.foldedInstructions;
                break;
            default:
                break;
            }
        }

        return stats;
    }

} // namespace Fission::ConstantPropagationDetail

namespace Fission {

    ConstantPropagationStats ConstantPropagation::Run(AnalyzedFunction &function) {
        ConstantPropagationDetail::ProgramContext context{};
        context.root = function.lpLiftedFunction;
        ConstantPropagationDetail::RegisterFunctions(function, context);
        context.functions[context.root].reachable = true;

        bool converged = false;
        for (size_t round = 0; round < 32; ++round) {
            context.changed = false;
            for (LiftedFunction *lifted : context.functionOrder) {
                auto state = context.functions.find(lifted);
                auto analyzed = context.analyzedFunctions.find(lifted);
                if (state != context.functions.end() && state->second.reachable && analyzed != context.analyzedFunctions.end())
                    ConstantPropagationDetail::RunOne(*analyzed->second, &context, false);
            }
            if (!context.changed) {
                converged = true;
                break;
            }
        }
        context.finalized = converged;

        ConstantPropagationStats stats{};
        for (LiftedFunction *lifted : context.functionOrder) {
            auto analyzed = context.analyzedFunctions.find(lifted);
            if (analyzed == context.analyzedFunctions.end())
                continue;
            const auto state = context.functions.find(lifted);
            if (converged && state != context.functions.end() && state->second.reachable) {
                const auto aliases = ConstantPropagationDetail::RunOne(*analyzed->second, &context, true);
                stats.foldedInstructions += aliases.foldedInstructions;
            }
            const auto local = ConstantPropagationDetail::RunOne(*analyzed->second, nullptr, true);
            stats.foldedInstructions += local.foldedInstructions;
            stats.prunedBranches += local.prunedBranches;
        }
        return stats;
    }

} // namespace Fission
