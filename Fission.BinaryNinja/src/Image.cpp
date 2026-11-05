//
// Parsed Luau bytecode image shared by the Binary Ninja view and architecture.
//

#include "Image.hpp"

#include "SafetyGuard.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <format>
#include <mutex>
#include <set>

namespace fission::bn {

    namespace {

        std::string EscapeForDisplay(std::string_view value, size_t limit = 96) {
            std::string out;
            out.reserve(std::min(value.size(), limit) + 2);
            for (size_t i = 0; i < value.size() && i < limit; ++i) {
                const unsigned char c = static_cast<unsigned char>(value[i]);
                switch (c) {
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                default:
                    if (c < 0x20 || c == 0x7F)
                        out += std::format("\\x{:02x}", c);
                    else
                        out += static_cast<char>(c);
                    break;
                }
            }
            if (value.size() > limit)
                out += "...";
            return out;
        }

        std::string ProtoName(const DeserializedFunction &proto) {
            std::string name;
            if (proto.debugName && !proto.debugName->empty())
                name = *proto.debugName;
            else if (proto.bIsMain)
                name = "main";
            else
                name = std::format("proto_{}", proto.bytecodeId);

            // Symbol names are displayed verbatim by Binary Ninja; control bytes
            // from a hostile debug table should not escape into the UI.
            for (char &character : name) {
                const auto byte = static_cast<unsigned char>(character);
                if (byte < 0x20 || byte == 0x7F || character == '\0')
                    character = '_';
            }
            return name;
        }

        const LiftedFunction *FindLifted(const LiftedFunction &root, const DeserializedFunction *proto) {
            std::vector<const LiftedFunction *> stack;
            stack.push_back(&root);
            while (!stack.empty()) {
                const LiftedFunction *current = stack.back();
                stack.pop_back();
                if (current->lpDeserialized == proto)
                    return current;
                for (const auto &child : current->subfunctions)
                    stack.push_back(&child);
            }
            return nullptr;
        }

        std::string JoinImportPath(const std::vector<std::string> &path) {
            std::string joined;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i)
                    joined += ".";
                joined += path[i];
            }
            return joined;
        }

        bool WritesRegister(const LiftedInstruction &instruction, uint8_t reg) {
            const auto &ops = instruction.operands;
            const auto writesBase = [&]() { return !ops.empty() && ops[0].type == LiftedOperandType::Register && ops[0].value.reg == reg; };

            switch (instruction.operation) {
            case LiftedOperation::LOAD:
            case LiftedOperation::LOADNJUMP:
            case LiftedOperation::MOVE:
            case LiftedOperation::GETGLOBAL:
            case LiftedOperation::GETUPVAL:
            case LiftedOperation::GETIMPORT:
            case LiftedOperation::GETTABLE:
            case LiftedOperation::GETTABLEKS:
            case LiftedOperation::GETTABLEN:
            case LiftedOperation::NEWCLOSURE:
            case LiftedOperation::DUPCLOSURE:
            case LiftedOperation::NEWTABLE:
            case LiftedOperation::DUPTABLE:
            case LiftedOperation::ADD:
            case LiftedOperation::SUB:
            case LiftedOperation::MUL:
            case LiftedOperation::DIV:
            case LiftedOperation::MOD:
            case LiftedOperation::POW:
            case LiftedOperation::ADDK:
            case LiftedOperation::SUBK:
            case LiftedOperation::MULK:
            case LiftedOperation::DIVK:
            case LiftedOperation::MODK:
            case LiftedOperation::POWK:
            case LiftedOperation::SUBRK:
            case LiftedOperation::DIVRK:
            case LiftedOperation::AND:
            case LiftedOperation::OR:
            case LiftedOperation::ANDK:
            case LiftedOperation::ORK:
            case LiftedOperation::CONCAT:
            case LiftedOperation::NOT:
            case LiftedOperation::MINUS:
            case LiftedOperation::LENGTH:
            case LiftedOperation::IDIV:
            case LiftedOperation::IDIVK:
            case LiftedOperation::NEWCLASS:
                return writesBase();
            case LiftedOperation::NAMECALL:
                return writesBase() || (!ops.empty() && ops[0].value.reg + 1 == reg);
            case LiftedOperation::FORNLOOP:
            case LiftedOperation::FORGLOOP:
                return !ops.empty() && ops[0].value.reg + 2 == reg;
            case LiftedOperation::GETVARARGS:
            case LiftedOperation::CALL:
            case LiftedOperation::CALLFB: {
                if (ops.empty() || ops[0].type != LiftedOperandType::Register)
                    return false;
                const uint8_t base = ops[0].value.reg;
                if (reg == base)
                    return true;
                if (ops.size() < 3)
                    return false;
                const int count = ops[2].value.imm.n;
                return reg > base && (count == 0 || reg < base + (count - 1));
            }
            default:
                return false;
            }
        }

        bool IsControlFlowBoundary(LiftedOperation operation) {
            switch (operation) {
            case LiftedOperation::JUMP:
            case LiftedOperation::LOADNJUMP:
            case LiftedOperation::JUMPIF:
            case LiftedOperation::JUMPIFNOT:
            case LiftedOperation::JUMPIFEQ:
            case LiftedOperation::JUMPIFLE:
            case LiftedOperation::JUMPIFLT:
            case LiftedOperation::JUMPIFNOTEQ:
            case LiftedOperation::JUMPIFNOTLE:
            case LiftedOperation::JUMPIFNOTLT:
            case LiftedOperation::JUMPXEQK:
            case LiftedOperation::FORNPREP:
            case LiftedOperation::FORNLOOP:
            case LiftedOperation::FORGLOOP:
            case LiftedOperation::FORGPREP:
            case LiftedOperation::FORGPREP_INEXT:
            case LiftedOperation::FORGPREP_NEXT:
            case LiftedOperation::RETURN:
            case LiftedOperation::CMPPROTO:
            case LiftedOperation::CONDVALUE:
            case LiftedOperation::PHI:
                return true;
            default:
                return false;
            }
        }

        std::optional<uint32_t> ClosureProtoFromConstant(const DeserializedFunction &proto, int32_t index) {
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return std::nullopt;
            const auto &constant = proto.constants[static_cast<size_t>(index)];
            if (constant.kType != LUA_TFUNCTION)
                return std::nullopt;
            const auto *target = std::get_if<DeserializedFunction *>(&constant.constantData);
            if (!target || !*target)
                return std::nullopt;
            return (*target)->bytecodeId;
        }

        std::optional<uint32_t>
        ResolveClosureProto(const DeserializedFunction &proto, const LiftedFunction &lifted, size_t callIndex, uint8_t reg, int depth = 0) {
            if (depth > 4 || callIndex == 0)
                return std::nullopt;

            const size_t limit = callIndex > 64 ? callIndex - 64 : 0;
            for (size_t i = callIndex; i-- > limit;) {
                if (i >= lifted.instructions.size())
                    continue;
                const auto &instruction = lifted.instructions[i];
                if (IsControlFlowBoundary(instruction.operation))
                    return std::nullopt;
                if (!WritesRegister(instruction, reg))
                    continue;

                switch (instruction.operation) {
                case LiftedOperation::NEWCLOSURE: {
                    if (instruction.operands.size() < 2)
                        return std::nullopt;
                    const int32_t index = instruction.operands[1].value.imm.k;
                    if (index < 0 || static_cast<size_t>(index) >= proto.subfunctions.size())
                        return std::nullopt;
                    const DeserializedFunction *target = proto.subfunctions[static_cast<size_t>(index)];
                    return target ? std::optional<uint32_t>(target->bytecodeId) : std::nullopt;
                }
                case LiftedOperation::DUPCLOSURE:
                case LiftedOperation::LOAD:
                    if (instruction.operands.size() < 2)
                        return std::nullopt;
                    return ClosureProtoFromConstant(proto, instruction.operands[1].value.imm.k);
                case LiftedOperation::MOVE:
                    if (instruction.operands.size() < 2)
                        return std::nullopt;
                    return ResolveClosureProto(proto, lifted, i, instruction.operands[1].value.reg, depth + 1);
                default:
                    return std::nullopt;
                }
            }
            return std::nullopt;
        }

        Image::UpvalueKind MergeUpvalueKind(Image::UpvalueKind left, Image::UpvalueKind right) {
            if (left == Image::UpvalueKind::Unknown)
                return right;
            if (right == Image::UpvalueKind::Unknown)
                return left;
            return left == right ? left : Image::UpvalueKind::Other;
        }

        Image::UpvalueKind ConstantKind(const DeserializedFunction &proto, const LiftedOperand &operand) {
            if (operand.type != LiftedOperandType::ImmediateConstant)
                return Image::UpvalueKind::Unknown;
            const int32_t index = operand.value.imm.k;
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return Image::UpvalueKind::Unknown;
            switch (proto.constants[static_cast<size_t>(index)].kType) {
            case LUA_TNUMBER:
            case LUA_TINTEGER:
                return Image::UpvalueKind::Number;
            case LUA_TSTRING:
                return Image::UpvalueKind::Other;
            default:
                return Image::UpvalueKind::Unknown;
            }
        }

        // Conservatively infers which upvalue slots hold numbers by following
        // GETUPVAL/SETUPVAL through local register uses. Numeric uses mark a
        // slot as Number; table/call/closure uses mark it as Other. A slot is
        // only rendered as `double` when it has numeric uses and no conflicting
        // non-numeric use.
        void InferUpvalueKinds(const std::vector<ProtoView> &protos, std::vector<std::vector<Image::UpvalueKind>> &kinds) {
            for (const auto &view : protos) {
                if (!view.lifted || !view.proto)
                    continue;
                const uint32_t protoId = view.proto->bytecodeId;
                if (protoId >= kinds.size() || kinds[protoId].empty())
                    continue;

                for (int pass = 0; pass < 8; ++pass) {
                    std::array<Image::UpvalueKind, 256> regKind{};
                    std::array<int16_t, 256> regUpvalue;
                    regUpvalue.fill(-1);
                    bool changed = false;

                    auto markUpvalue = [&](uint8_t index, Image::UpvalueKind kind) {
                        if (kind == Image::UpvalueKind::Unknown || index >= kinds[protoId].size())
                            return;
                        const auto merged = MergeUpvalueKind(kinds[protoId][index], kind);
                        if (merged != kinds[protoId][index]) {
                            kinds[protoId][index] = merged;
                            changed = true;
                        }
                    };
                    auto noteUse = [&](uint8_t reg, Image::UpvalueKind kind) {
                        const int16_t origin = regUpvalue[reg];
                        if (origin >= 0)
                            markUpvalue(static_cast<uint8_t>(origin), kind);
                    };
                    auto numeric = [&](const LiftedOperand &operand) {
                        if (operand.type == LiftedOperandType::Register)
                            noteUse(operand.value.reg, Image::UpvalueKind::Number);
                    };
                    auto other = [&](const LiftedOperand &operand) {
                        if (operand.type == LiftedOperandType::Register)
                            noteUse(operand.value.reg, Image::UpvalueKind::Other);
                    };
                    auto setDest = [&](const LiftedOperand &operand, Image::UpvalueKind kind, int16_t origin = -1) {
                        if (operand.type != LiftedOperandType::Register)
                            return;
                        regKind[operand.value.reg] = kind;
                        regUpvalue[operand.value.reg] = origin;
                    };

                    for (const auto &instruction : view.lifted->instructions) {
                        const auto &ops = instruction.operands;
                        switch (instruction.operation) {
                        case LiftedOperation::GETUPVAL:
                            if (ops.size() >= 2 && ops[1].type == LiftedOperandType::ImmediateInteger) {
                                const int32_t index = ops[1].value.imm.n;
                                if (index >= 0 && static_cast<size_t>(index) < kinds[protoId].size())
                                    setDest(ops[0], kinds[protoId][static_cast<size_t>(index)], static_cast<int16_t>(index));
                            }
                            break;

                        case LiftedOperation::SETUPVAL:
                            if (ops.size() >= 2 && ops[0].type == LiftedOperandType::Register && ops[1].type == LiftedOperandType::ImmediateInteger) {
                                const int32_t index = ops[1].value.imm.n;
                                if (index < 0 || static_cast<size_t>(index) >= kinds[protoId].size())
                                    break;
                                Image::UpvalueKind kind = regKind[ops[0].value.reg];
                                if (kind == Image::UpvalueKind::Unknown && regUpvalue[ops[0].value.reg] >= 0)
                                    kind = kinds[protoId][static_cast<size_t>(regUpvalue[ops[0].value.reg])];
                                markUpvalue(static_cast<uint8_t>(index), kind);
                            }
                            break;

                        case LiftedOperation::MOVE:
                            if (ops.size() >= 2 && ops[0].type == LiftedOperandType::Register && ops[1].type == LiftedOperandType::Register)
                                setDest(ops[0], regKind[ops[1].value.reg], regUpvalue[ops[1].value.reg]);
                            break;

                        case LiftedOperation::LOAD:
                            if (ops.size() >= 2)
                                setDest(ops[0], ConstantKind(*view.proto, ops[1]));
                            break;

                        case LiftedOperation::LOADNJUMP:
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Number);
                            break;

                        case LiftedOperation::ADD:
                        case LiftedOperation::SUB:
                        case LiftedOperation::MUL:
                        case LiftedOperation::DIV:
                        case LiftedOperation::MOD:
                        case LiftedOperation::POW:
                        case LiftedOperation::IDIV:
                            if (ops.size() >= 3) {
                                numeric(ops[1]);
                                numeric(ops[2]);
                                setDest(ops[0], Image::UpvalueKind::Number);
                            }
                            break;

                        case LiftedOperation::ADDK:
                        case LiftedOperation::SUBK:
                        case LiftedOperation::MULK:
                        case LiftedOperation::DIVK:
                        case LiftedOperation::MODK:
                        case LiftedOperation::POWK:
                        case LiftedOperation::IDIVK:
                            if (ops.size() >= 3) {
                                numeric(ops[1]);
                                setDest(ops[0], Image::UpvalueKind::Number);
                            }
                            break;

                        case LiftedOperation::SUBRK:
                        case LiftedOperation::DIVRK:
                            if (ops.size() >= 3) {
                                numeric(ops[2]);
                                setDest(ops[0], Image::UpvalueKind::Number);
                            }
                            break;

                        case LiftedOperation::MINUS:
                            if (ops.size() >= 2) {
                                numeric(ops[1]);
                                setDest(ops[0], Image::UpvalueKind::Number);
                            }
                            break;

                        case LiftedOperation::LENGTH:
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Number);
                            break;

                        case LiftedOperation::NOT:
                        case LiftedOperation::CONCAT:
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Other);
                            break;

                        case LiftedOperation::NEWTABLE:
                        case LiftedOperation::DUPTABLE:
                        case LiftedOperation::NEWCLOSURE:
                        case LiftedOperation::DUPCLOSURE:
                        case LiftedOperation::NEWCLASS:
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Other);
                            break;

                        case LiftedOperation::GETGLOBAL:
                        case LiftedOperation::GETIMPORT:
                        case LiftedOperation::GETVARARGS:
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Unknown);
                            break;

                        case LiftedOperation::GETTABLE:
                        case LiftedOperation::GETTABLEKS:
                        case LiftedOperation::GETTABLEN:
                        case LiftedOperation::GETUDATAKS:
                            if (ops.size() >= 2)
                                other(ops[1]);
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Unknown);
                            break;

                        case LiftedOperation::SETTABLE:
                        case LiftedOperation::SETTABLEKS:
                        case LiftedOperation::SETTABLEN:
                        case LiftedOperation::SETUDATAKS:
                            if (ops.size() >= 2)
                                other(ops[1]);
                            break;

                        case LiftedOperation::SETLIST:
                            if (!ops.empty())
                                other(ops[0]);
                            break;

                        case LiftedOperation::NAMECALL:
                        case LiftedOperation::NAMECALLUDATA:
                            if (ops.size() >= 2)
                                other(ops[1]);
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Unknown);
                            break;

                        case LiftedOperation::CALL:
                        case LiftedOperation::CALLFB:
                            if (!ops.empty())
                                other(ops[0]);
                            if (!ops.empty())
                                setDest(ops[0], Image::UpvalueKind::Unknown);
                            break;

                        case LiftedOperation::FORNPREP:
                        case LiftedOperation::FORNLOOP:
                            if (!ops.empty() && ops[0].type == LiftedOperandType::Register) {
                                const uint8_t base = ops[0].value.reg;
                                for (unsigned offset = 0; offset < 3; ++offset) {
                                    const unsigned reg = static_cast<unsigned>(base) + offset;
                                    if (reg < 256)
                                        noteUse(static_cast<uint8_t>(reg), Image::UpvalueKind::Number);
                                }
                                setDest(ops[0], Image::UpvalueKind::Number);
                            }
                            break;

                        case LiftedOperation::FORGPREP:
                        case LiftedOperation::FORGPREP_INEXT:
                        case LiftedOperation::FORGPREP_NEXT:
                        case LiftedOperation::FORGLOOP:
                            if (!ops.empty())
                                other(ops[0]);
                            break;

                        case LiftedOperation::JUMPXEQK:
                            if (ops.size() >= 3) {
                                const auto kind = ConstantKind(*view.proto, ops[2]);
                                if (kind == Image::UpvalueKind::Number)
                                    numeric(ops[0]);
                                else if (kind == Image::UpvalueKind::Other)
                                    other(ops[0]);
                            }
                            break;

                        case LiftedOperation::JUMPIFLE:
                        case LiftedOperation::JUMPIFLT:
                        case LiftedOperation::JUMPIFNOTLE:
                        case LiftedOperation::JUMPIFNOTLT:
                            if (ops.size() >= 3) {
                                numeric(ops[0]);
                                numeric(ops[2]);
                            }
                            break;

                        case LiftedOperation::JUMPIFEQ:
                        case LiftedOperation::JUMPIFNOTEQ:
                            if (ops.size() >= 3) {
                                const auto kind = ConstantKind(*view.proto, ops[2]);
                                if (kind == Image::UpvalueKind::Number)
                                    numeric(ops[0]);
                                else if (kind == Image::UpvalueKind::Other)
                                    other(ops[0]);
                            }
                            break;

                        default:
                            break;
                        }
                    }

                    if (!changed)
                        break;
                }
            }
        }

        Image::FieldKind MergeFieldKind(Image::FieldKind left, Image::FieldKind right) {
            if (left == Image::FieldKind::Unknown)
                return right;
            if (right == Image::FieldKind::Unknown)
                return left;
            return left == right ? left : Image::FieldKind::Unknown;
        }

        Image::FieldKind ConstantFieldKind(const DeserializedFunction &proto, const LiftedOperand &operand) {
            if (operand.type != LiftedOperandType::ImmediateConstant)
                return Image::FieldKind::Unknown;
            const int32_t index = operand.value.imm.k;
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return Image::FieldKind::Unknown;
            switch (proto.constants[static_cast<size_t>(index)].kType) {
            case LUA_TNUMBER:
                return Image::FieldKind::Number;
            case LUA_TINTEGER:
                return Image::FieldKind::Integer;
            case LUA_TBOOLEAN:
                return Image::FieldKind::Boolean;
            case LUA_TSTRING:
                return Image::FieldKind::String;
            case LUA_TTABLE:
                return Image::FieldKind::Table;
            case LUA_TFUNCTION:
                return Image::FieldKind::Function;
            default:
                return Image::FieldKind::Unknown;
            }
        }

        // Infers table-field and method-result types from how each loaded value
        // is used. Field names/offsets themselves are collected from bytecode,
        // so the generated LuauTable type stays data-driven.
        void InferFieldKinds(
            const std::vector<ProtoView> &protos, const std::unordered_map<std::string, int32_t> &offsets, const std::unordered_set<std::string> &methodFields,
            const std::vector<std::vector<Image::UpvalueKind>> &upvalueKinds, const std::unordered_map<uint64_t, std::pair<uint32_t, uint8_t>> &upvalueSlots,
            std::unordered_map<std::string, Image::FieldKind> &valueKinds, std::unordered_map<std::string, Image::FieldKind> &resultKinds
        ) {
            std::vector<std::string> fields;
            fields.reserve(offsets.size());
            for (const auto &[name, offset] : offsets) {
                (void)offset;
                fields.push_back(name);
            }
            std::sort(fields.begin(), fields.end(), [&](const std::string &left, const std::string &right) { return offsets.at(left) < offsets.at(right); });

            std::unordered_map<std::string, int> indexByName;
            std::vector<bool> isMethod(fields.size(), false);
            for (size_t i = 0; i < fields.size(); ++i) {
                indexByName.emplace(fields[i], static_cast<int>(i));
                isMethod[i] = methodFields.contains(fields[i]);
                valueKinds[fields[i]] = isMethod[i] ? Image::FieldKind::Function : Image::FieldKind::Unknown;
                resultKinds[fields[i]] = Image::FieldKind::Unknown;
            }

            for (const auto &view : protos) {
                if (!view.lifted || !view.proto)
                    continue;

                for (int pass = 0; pass < 8; ++pass) {
                    std::array<int16_t, 256> regOrigin;
                    std::array<Image::FieldKind, 256> regKind{};
                    std::array<int16_t, 256> methodAtReg;
                    regOrigin.fill(-1);
                    methodAtReg.fill(-1);
                    bool changed = false;

                    const auto fieldIndex = [&](const LiftedInstruction &instruction) -> int {
                        const auto &ops = instruction.operands;
                        if (ops.size() < 3 || ops[2].type != LiftedOperandType::ImmediateConstant)
                            return -1;
                        const int32_t index = ops[2].value.imm.k;
                        if (index < 0 || static_cast<size_t>(index) >= view.proto->constants.size())
                            return -1;
                        const auto &constant = view.proto->constants[static_cast<size_t>(index)];
                        if (constant.kType != LUA_TSTRING)
                            return -1;
                        const auto it = indexByName.find(std::get<std::string>(constant.constantData));
                        return it == indexByName.end() ? -1 : it->second;
                    };

                    const auto markValueKind = [&](int index, Image::FieldKind kind) {
                        if (index < 0 || kind == Image::FieldKind::Unknown)
                            return;
                        const std::string &name = fields[static_cast<size_t>(index)];
                        const auto merged = MergeFieldKind(valueKinds[name], kind);
                        if (merged != valueKinds[name]) {
                            valueKinds[name] = merged;
                            changed = true;
                        }
                    };
                    const auto markKind = [&](int index, Image::FieldKind kind) {
                        if (index < 0 || kind == Image::FieldKind::Unknown)
                            return;
                        auto &map = isMethod[static_cast<size_t>(index)] ? resultKinds : valueKinds;
                        const std::string &name = fields[static_cast<size_t>(index)];
                        const auto merged = MergeFieldKind(map[name], kind);
                        if (merged != map[name]) {
                            map[name] = merged;
                            changed = true;
                        }
                    };
                    const auto noteUse = [&](uint8_t reg, Image::FieldKind kind) {
                        const int16_t origin = regOrigin[reg];
                        if (origin >= 0)
                            markKind(origin, kind);
                    };
                    const auto numeric = [&](const LiftedOperand &operand) {
                        if (operand.type == LiftedOperandType::Register)
                            noteUse(operand.value.reg, Image::FieldKind::Number);
                    };
                    const auto tableUse = [&](const LiftedOperand &operand) {
                        if (operand.type == LiftedOperandType::Register)
                            noteUse(operand.value.reg, Image::FieldKind::Table);
                    };
                    const auto stringUse = [&](const LiftedOperand &operand) {
                        if (operand.type == LiftedOperandType::Register)
                            noteUse(operand.value.reg, Image::FieldKind::String);
                    };
                    const auto setDest = [&](const LiftedOperand &operand, int origin, Image::FieldKind kind) {
                        if (operand.type != LiftedOperandType::Register)
                            return;
                        regOrigin[operand.value.reg] = static_cast<int16_t>(origin);
                        regKind[operand.value.reg] = kind;
                    };

                    for (const auto &instruction : view.lifted->instructions) {
                        const auto &ops = instruction.operands;
                        switch (instruction.operation) {
                        case LiftedOperation::GETTABLEKS:
                        case LiftedOperation::GETUDATAKS:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            if (!ops.empty())
                                setDest(ops[0], fieldIndex(instruction), Image::FieldKind::Unknown);
                            break;

                        case LiftedOperation::SETTABLEKS:
                        case LiftedOperation::SETUDATAKS:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            break;

                        case LiftedOperation::GETTABLE:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Unknown);
                            break;

                        case LiftedOperation::SETTABLE:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            break;

                        case LiftedOperation::SETLIST:
                            if (!ops.empty())
                                tableUse(ops[0]);
                            break;

                        case LiftedOperation::GETTABLEN:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Unknown);
                            break;

                        case LiftedOperation::SETTABLEN:
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            break;

                        case LiftedOperation::NAMECALL:
                        case LiftedOperation::NAMECALLUDATA: {
                            if (ops.size() >= 2)
                                tableUse(ops[1]);
                            const int index = fieldIndex(instruction);
                            if (!ops.empty()) {
                                setDest(ops[0], index, Image::FieldKind::Function);
                                if (ops[0].type == LiftedOperandType::Register)
                                    methodAtReg[ops[0].value.reg] = static_cast<int16_t>(index);
                            }
                            break;
                        }

                        case LiftedOperation::CALL:
                        case LiftedOperation::CALLFB: {
                            if (ops.empty() || ops[0].type != LiftedOperandType::Register)
                                break;
                            const uint8_t base = ops[0].value.reg;
                            const int16_t calleeOrigin = regOrigin[base];
                            if (calleeOrigin >= 0 && !isMethod[static_cast<size_t>(calleeOrigin)])
                                markValueKind(calleeOrigin, Image::FieldKind::Function);
                            const int method = methodAtReg[base];
                            if (method >= 0) {
                                int resultCount = 1;
                                if (ops.size() >= 3) {
                                    const int retc = ops[2].value.imm.n;
                                    resultCount = retc <= 1 ? 1 : std::min(retc - 1, 4);
                                }
                                for (int i = 0; i < resultCount; ++i) {
                                    const unsigned reg = static_cast<unsigned>(base) + static_cast<unsigned>(i);
                                    if (reg < 256) {
                                        regOrigin[reg] = static_cast<int16_t>(method);
                                        regKind[reg] = Image::FieldKind::Unknown;
                                    }
                                }
                                methodAtReg[base] = -1;
                            }
                            break;
                        }

                        case LiftedOperation::SETUPVAL:
                            if (ops.size() >= 2 && ops[0].type == LiftedOperandType::Register && ops[1].type == LiftedOperandType::ImmediateInteger) {
                                const uint32_t protoId = view.proto->bytecodeId;
                                const int32_t index = ops[1].value.imm.n;
                                if (protoId < upvalueKinds.size() && index >= 0 && static_cast<size_t>(index) < upvalueKinds[protoId].size() &&
                                    upvalueKinds[protoId][static_cast<size_t>(index)] == Image::UpvalueKind::Number)
                                    noteUse(ops[0].value.reg, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::CAPTURE:
                            if (ops.size() >= 2 && ops[1].type == LiftedOperandType::Register && instruction.instructionIndex >= 0) {
                                const size_t captureIndex = static_cast<size_t>(instruction.instructionIndex);
                                if (captureIndex < view.captureSlots.size() && view.captureSlots[captureIndex] != 0) {
                                    const auto it = upvalueSlots.find(view.captureSlots[captureIndex]);
                                    if (it != upvalueSlots.end()) {
                                        const auto &[capturedProto, capturedIndex] = it->second;
                                        if (capturedProto < upvalueKinds.size() && capturedIndex < upvalueKinds[capturedProto].size() &&
                                            upvalueKinds[capturedProto][capturedIndex] == Image::UpvalueKind::Number)
                                            noteUse(ops[1].value.reg, Image::FieldKind::Number);
                                    }
                                }
                            }
                            break;

                        case LiftedOperation::MOVE:
                            if (ops.size() >= 2 && ops[0].type == LiftedOperandType::Register && ops[1].type == LiftedOperandType::Register) {
                                regOrigin[ops[0].value.reg] = regOrigin[ops[1].value.reg];
                                regKind[ops[0].value.reg] = regKind[ops[1].value.reg];
                            }
                            break;

                        case LiftedOperation::LOAD:
                            if (ops.size() >= 2)
                                setDest(ops[0], -1, ConstantFieldKind(*view.proto, ops[1]));
                            break;

                        case LiftedOperation::LOADNJUMP:
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            break;

                        case LiftedOperation::GETGLOBAL:
                        case LiftedOperation::GETIMPORT:
                        case LiftedOperation::GETVARARGS:
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Unknown);
                            break;

                        case LiftedOperation::NEWTABLE:
                        case LiftedOperation::DUPTABLE:
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Table);
                            break;

                        case LiftedOperation::NEWCLOSURE:
                        case LiftedOperation::DUPCLOSURE:
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Function);
                            break;

                        case LiftedOperation::ADD:
                        case LiftedOperation::SUB:
                        case LiftedOperation::MUL:
                        case LiftedOperation::DIV:
                        case LiftedOperation::MOD:
                        case LiftedOperation::POW:
                        case LiftedOperation::IDIV:
                            if (ops.size() >= 3) {
                                numeric(ops[1]);
                                numeric(ops[2]);
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::ADDK:
                        case LiftedOperation::SUBK:
                        case LiftedOperation::MULK:
                        case LiftedOperation::DIVK:
                        case LiftedOperation::MODK:
                        case LiftedOperation::POWK:
                        case LiftedOperation::IDIVK:
                            if (ops.size() >= 3) {
                                numeric(ops[1]);
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::SUBRK:
                        case LiftedOperation::DIVRK:
                            if (ops.size() >= 3) {
                                numeric(ops[2]);
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::MINUS:
                            if (ops.size() >= 2) {
                                numeric(ops[1]);
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::LENGTH:
                            if (ops.size() >= 2) {
                                tableUse(ops[1]);
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::CONCAT:
                            if (ops.size() >= 3) {
                                stringUse(ops[1]);
                                stringUse(ops[2]);
                                setDest(ops[0], -1, Image::FieldKind::String);
                            }
                            break;

                        case LiftedOperation::NOT:
                            if (!ops.empty())
                                setDest(ops[0], -1, Image::FieldKind::Boolean);
                            break;

                        case LiftedOperation::JUMPXEQK:
                            if (ops.size() >= 3) {
                                const auto kind = ConstantFieldKind(*view.proto, ops[2]);
                                if (kind == Image::FieldKind::Number || kind == Image::FieldKind::Integer)
                                    numeric(ops[0]);
                                else if (kind == Image::FieldKind::String)
                                    stringUse(ops[0]);
                            }
                            break;

                        case LiftedOperation::JUMPIFLE:
                        case LiftedOperation::JUMPIFLT:
                        case LiftedOperation::JUMPIFNOTLE:
                        case LiftedOperation::JUMPIFNOTLT:
                            if (ops.size() >= 3) {
                                numeric(ops[0]);
                                numeric(ops[2]);
                            }
                            break;

                        case LiftedOperation::JUMPIFEQ:
                        case LiftedOperation::JUMPIFNOTEQ:
                            if (ops.size() >= 3) {
                                const auto kind = ConstantFieldKind(*view.proto, ops[2]);
                                if (kind == Image::FieldKind::Number || kind == Image::FieldKind::Integer)
                                    numeric(ops[0]);
                                else if (kind == Image::FieldKind::String)
                                    stringUse(ops[0]);
                            }
                            break;

                        case LiftedOperation::FORNPREP:
                        case LiftedOperation::FORNLOOP:
                            if (!ops.empty() && ops[0].type == LiftedOperandType::Register) {
                                const uint8_t base = ops[0].value.reg;
                                for (unsigned offset = 0; offset < 3; ++offset) {
                                    const unsigned reg = static_cast<unsigned>(base) + offset;
                                    if (reg < 256)
                                        noteUse(static_cast<uint8_t>(reg), Image::FieldKind::Number);
                                }
                                setDest(ops[0], -1, Image::FieldKind::Number);
                            }
                            break;

                        case LiftedOperation::FORGPREP:
                        case LiftedOperation::FORGPREP_INEXT:
                        case LiftedOperation::FORGPREP_NEXT:
                            if (!ops.empty())
                                tableUse(ops[0]);
                            break;

                        case LiftedOperation::FORGLOOP:
                            if (!ops.empty() && ops[0].type == LiftedOperandType::Register) {
                                const int16_t origin = regOrigin[ops[0].value.reg];
                                if (origin >= 0 && !isMethod[static_cast<size_t>(origin)])
                                    markValueKind(origin, Image::FieldKind::Function);
                            }
                            break;

                        default:
                            break;
                        }
                    }

                    if (!changed)
                        break;
                }
            }
        }

    } // namespace

    bool InstructionWritesRegister(const LiftedInstruction &instruction, uint8_t reg) { return WritesRegister(instruction, reg); }

    namespace {

        // The synthetic address range is only known after a file has been parsed,
        // so AllocateBase -> Add must not interleave with another parse or two
        // images could claim overlapping ranges and ImageRegistry::Find would
        // resolve addresses to the wrong image.
        std::mutex &ParseMutex() {
            static std::mutex mutex;
            return mutex;
        }

    } // namespace

    uint64_t ImageRegistry::s_nextBase = ImageRegistry::kInitialBase;

    std::shared_ptr<Image> Image::Parse(std::string bytes) {
        if (bytes.size() < 3)
            return nullptr;

        std::lock_guard parseLock(ParseMutex());
        auto image = std::shared_ptr<Image>(new Image());
        image->m_bytes = std::move(bytes);
        image->m_baseAddress = ImageRegistry::AllocateBase(image->m_bytes.size());

        try {
            // Hostile bytecode must never take down the host: route libassert through
            // the throwing handler and bound the lift with the same budget the CLI uses.
            Fission::ScopedThrowingAssertHandler assertHandler;
            Fission::ScopedDecompileBudget budget(std::chrono::seconds(60));

            Deserializer deserializer;
            auto parsed = deserializer.Deserialize(image->m_bytes);
            if (!parsed)
                return nullptr;

            // Moving the vector transfers its backing storage, so the proto pointers the
            // deserializer captured in `subfunctions` / `lpMainFunction` stay valid.
            image->m_bytecode = std::move(*parsed);

            if (!image->m_bytecode.lpMainFunction)
                return nullptr;

            image->m_stringAddresses.reserve(image->m_bytecode.stringTable.size());
            for (size_t i = 0; i < image->m_bytecode.stringTable.size(); ++i) {
                if (i >= image->m_bytecode.stringTableOffsets.size() || image->m_bytecode.stringTableOffsets[i] >= image->m_bytes.size())
                    continue;
                image->m_stringAddresses.try_emplace(image->m_bytecode.stringTable[i], image->m_baseAddress + image->m_bytecode.stringTableOffsets[i]);
            }

            BytecodeLifter lifter(&image->m_decoder);
            image->m_lifted = std::make_unique<LiftedFunction>(lifter.LiftDeserializedBytecode(image->m_bytecode));
        } catch (const Fission::DecompilerError &) {
            return nullptr;
        } catch (const std::exception &) {
            return nullptr;
        } catch (...) {
            return nullptr;
        }

        if (!image->m_lifted)
            return nullptr;

        const auto alignUp = [](uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); };

        const ProtoView *mainProto = nullptr;
        image->m_protoById.assign(image->m_bytecode.functions.size(), nullptr);

        for (auto &proto : image->m_bytecode.functions) {
            ProtoView view{};
            view.proto = &proto;
            view.lifted = FindLifted(*image->m_lifted, &proto);
            view.name = ProtoName(proto);
            view.fileOffset = proto.instructionOffset;
            view.instructionCount = proto.instructions.size();

            if (view.fileOffset + view.instructionCount * sizeof(uint32_t) > image->m_bytes.size())
                return nullptr;

            image->m_protos.push_back(std::move(view));
        }

        // Luau instruction arrays live at their file offsets. The architecture
        // declares alignment 1 so Binary Ninja accepts the (frequently unaligned)
        // proto starts without shifting them.
        for (auto &view : image->m_protos)
            view.address = image->m_baseAddress + view.fileOffset;
        image->m_codeBase = image->m_baseAddress;
        image->m_codeEnd = image->m_baseAddress + image->m_bytes.size();

        std::sort(image->m_protos.begin(), image->m_protos.end(), [](const ProtoView &a, const ProtoView &b) { return a.address < b.address; });

        // Disambiguate debug names that collide after shadowing/scoping.
        std::unordered_map<std::string, int> nameCounts;
        for (auto &view : image->m_protos) {
            if (++nameCounts[view.name] > 1)
                view.name = std::format("{}_{}", view.name, view.proto->bytecodeId);
        }

        for (auto &view : image->m_protos) {
            if (view.proto->bytecodeId < image->m_protoById.size())
                image->m_protoById[view.proto->bytecodeId] = &view;
            if (view.proto == image->m_bytecode.lpMainFunction)
                mainProto = &view;
        }

        if (!mainProto)
            return nullptr;
        image->m_mainProto = mainProto;

        image->m_upvalueKinds.resize(image->m_bytecode.functions.size());
        for (size_t i = 0; i < image->m_bytecode.functions.size(); ++i)
            image->m_upvalueKinds[i].assign(image->m_bytecode.functions[i].nups, Image::UpvalueKind::Unknown);
        InferUpvalueKinds(image->m_protos, image->m_upvalueKinds);

        for (auto &view : image->m_protos) {
            view.tableAddresses.assign(view.instructionCount, 0);
            view.captureSlots.assign(view.instructionCount, 0);
            view.directCallTargets.assign(view.instructionCount, 0);
        }

        // Collect every name that needs a synthetic slot. Keeping this pass
        // separate lets the address layout stay deterministic (sorted names).
        std::set<std::string> globalNames;
        std::set<std::string> fieldNames;
        size_t tableSiteCount = 0;
        for (const auto &view : image->m_protos) {
            if (!view.lifted)
                continue;
            const auto &proto = *view.proto;
            for (const auto &instruction : view.lifted->instructions) {
                const auto &ops = instruction.operands;
                switch (instruction.operation) {
                case LiftedOperation::GETGLOBAL:
                case LiftedOperation::SETGLOBAL:
                    if (ops.size() >= 2) {
                        const int32_t index = ops[1].value.imm.k;
                        if (index >= 0 && static_cast<size_t>(index) < proto.constants.size()) {
                            const auto &constant = proto.constants[static_cast<size_t>(index)];
                            if (constant.kType == LUA_TSTRING)
                                globalNames.insert(std::get<std::string>(constant.constantData));
                        }
                    }
                    break;
                case LiftedOperation::GETIMPORT:
                    if (ops.size() >= 3) {
                        const auto path = image->ImportPath(proto, ops[2].value.imm.u);
                        if (!path.empty())
                            globalNames.insert(JoinImportPath(path));
                    }
                    break;
                case LiftedOperation::GETTABLEKS:
                case LiftedOperation::SETTABLEKS:
                case LiftedOperation::NEWCLASSMEMBER:
                    if (ops.size() >= 3) {
                        const int32_t index = ops[2].value.imm.k;
                        if (index >= 0 && static_cast<size_t>(index) < proto.constants.size()) {
                            const auto &constant = proto.constants[static_cast<size_t>(index)];
                            if (constant.kType == LUA_TSTRING)
                                fieldNames.insert(std::get<std::string>(constant.constantData));
                        }
                    }
                    break;
                case LiftedOperation::NAMECALL:
                    if (ops.size() >= 3) {
                        const int32_t index = ops[2].value.imm.k;
                        if (index >= 0 && static_cast<size_t>(index) < proto.constants.size()) {
                            const auto &constant = proto.constants[static_cast<size_t>(index)];
                            if (constant.kType == LUA_TSTRING) {
                                const auto &name = std::get<std::string>(constant.constantData);
                                fieldNames.insert(name);
                                image->m_methodFields.insert(name);
                            }
                        }
                    }
                    break;
                case LiftedOperation::NEWTABLE:
                    ++tableSiteCount;
                    break;
                case LiftedOperation::DUPTABLE:
                    ++tableSiteCount;
                    if (ops.size() >= 2) {
                        const int32_t index = ops[1].value.imm.k;
                        if (index >= 0 && static_cast<size_t>(index) < proto.constants.size()) {
                            const auto &constant = proto.constants[static_cast<size_t>(index)];
                            if (constant.kType == LUA_TTABLE) {
                                const auto &table = std::get<LuauTable>(constant.constantData);
                                for (const auto &key : table.keys)
                                    fieldNames.insert(key);
                            }
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }

        image->m_syntheticBase = alignUp(image->m_baseAddress + image->m_bytes.size() + 0x1000, 0x1000);
        image->m_functionBase = image->m_syntheticBase;
        image->m_globalBase = alignUp(image->m_functionBase + std::max<size_t>(image->m_bytecode.functions.size(), 1) * 8 + 0x1000, 0x1000);
        size_t globalIndex = 0;
        for (const auto &name : globalNames) {
            const uint64_t address = image->m_globalBase + globalIndex * 8;
            image->m_globalSlots.push_back({name, address});
            image->m_globalSlotByName.emplace(name, address);
            ++globalIndex;
        }

        image->m_upvalueBase = alignUp(image->m_globalBase + std::max<size_t>(globalNames.size(), 1) * 8 + 0x1000, 0x1000);

        // Pack upvalue slots per proto: each prototype only reserves space for the
        // upvalues it actually declares (nups), padded to slot alignment.
        image->m_upvalueOffsets.assign(image->m_bytecode.functions.size(), 0);
        uint64_t upvalueCursor = 0;
        for (size_t i = 0; i < image->m_bytecode.functions.size(); ++i) {
            upvalueCursor = alignUp(upvalueCursor, kUpvalueSlotSize);
            image->m_upvalueOffsets[i] = upvalueCursor;
            upvalueCursor += static_cast<uint64_t>(image->m_bytecode.functions[i].nups) * kUpvalueSlotSize;
        }
        image->m_upvalueSize = upvalueCursor;

        image->m_tableBase = alignUp(image->m_upvalueBase + std::max<uint64_t>(image->m_upvalueSize, kUpvalueSlotSize) + 0x1000, 0x1000);
        image->m_syntheticEnd = alignUp(image->m_tableBase + std::max<size_t>(tableSiteCount, 1) * kTableStride + 0x1000, 0x1000);
        image->m_addressSpaceSize = image->m_syntheticEnd - image->m_baseAddress;

        uint32_t fieldIndex = 0;
        for (const auto &name : fieldNames) {
            const uint32_t offset = kFieldBase + fieldIndex * 8;
            if (offset + 8 > kTableStride)
                break;
            image->m_fieldOffsets.emplace(name, static_cast<int32_t>(offset));
            ++fieldIndex;
        }
        std::unordered_map<uint64_t, std::pair<uint32_t, uint8_t>> upvalueSlots;
        for (uint32_t protoId = 0; protoId < image->m_bytecode.functions.size(); ++protoId) {
            const auto &function = image->m_bytecode.functions[protoId];
            for (uint8_t index = 0; index < function.nups; ++index)
                upvalueSlots.emplace(image->UpvalueSlot(protoId, index), std::pair<uint32_t, uint8_t>{protoId, index});
        }

        uint32_t tableIndex = 0;
        for (auto &view : image->m_protos) {
            if (!view.lifted)
                continue;
            const auto &proto = *view.proto;
            int32_t closureProtoId = -1;
            uint8_t captureIndex = 0;

            for (size_t i = 0; i < view.lifted->instructions.size(); ++i) {
                const auto &instruction = view.lifted->instructions[i];
                const auto &ops = instruction.operands;
                switch (instruction.operation) {
                case LiftedOperation::NEWCLOSURE: {
                    closureProtoId = -1;
                    captureIndex = 0;
                    if (ops.size() >= 2) {
                        const int32_t index = ops[1].value.imm.k;
                        if (index >= 0 && static_cast<size_t>(index) < proto.subfunctions.size()) {
                            const DeserializedFunction *target = proto.subfunctions[static_cast<size_t>(index)];
                            if (target)
                                closureProtoId = static_cast<int32_t>(target->bytecodeId);
                        }
                    }
                    break;
                }
                case LiftedOperation::DUPCLOSURE:
                    closureProtoId = -1;
                    captureIndex = 0;
                    if (ops.size() >= 2) {
                        if (const auto target = ClosureProtoFromConstant(proto, ops[1].value.imm.k))
                            closureProtoId = static_cast<int32_t>(*target);
                    }
                    break;
                case LiftedOperation::CAPTURE:
                    if (closureProtoId >= 0) {
                        view.captureSlots[i] = image->UpvalueSlot(static_cast<uint32_t>(closureProtoId), captureIndex);
                        ++captureIndex;
                    }
                    break;
                case LiftedOperation::NEWTABLE:
                case LiftedOperation::DUPTABLE: {
                    const uint64_t address = image->m_tableBase + tableIndex * kTableStride;
                    view.tableAddresses[i] = address;
                    image->m_tableSites.push_back({address, proto.bytecodeId, static_cast<uint32_t>(i)});
                    ++tableIndex;
                    break;
                }
                case LiftedOperation::CALL:
                case LiftedOperation::CALLFB:
                    if (!ops.empty()) {
                        if (const auto target = ResolveClosureProto(proto, *view.lifted, i, ops[0].value.reg)) {
                            if (const ProtoView *targetView = image->ProtoByIndex(*target))
                                view.directCallTargets[i] = targetView->address;
                        }
                    }
                    break;
                default:
                    break;
                }
            }
        }

        InferFieldKinds(
            image->m_protos, image->m_fieldOffsets, image->m_methodFields, image->m_upvalueKinds, upvalueSlots, image->m_fieldValueKinds,
            image->m_fieldResultKinds
        );

        ImageRegistry::Add(image);
        return image;
    }

    const ProtoView *Image::ProtoAtAddress(uint64_t address) const {
        if (address < m_baseAddress)
            return nullptr;
        auto it =
            std::upper_bound(m_protos.begin(), m_protos.end(), address, [](uint64_t candidate, const ProtoView &view) { return candidate < view.address; });
        if (it == m_protos.begin())
            return nullptr;
        --it;
        return it->Contains(address) ? &*it : nullptr;
    }

    const ProtoView *Image::ProtoByIndex(uint32_t protoId) const {
        if (protoId >= m_protoById.size())
            return nullptr;
        return m_protoById[protoId];
    }

    uint64_t Image::GlobalSlot(std::string_view name) const {
        const auto it = m_globalSlotByName.find(std::string(name));
        return it == m_globalSlotByName.end() ? 0 : it->second;
    }

    uint64_t Image::UpvalueSlot(uint32_t protoId, uint8_t index) const {
        if (protoId >= m_bytecode.functions.size() || index >= m_bytecode.functions[protoId].nups)
            return 0;
        return m_upvalueBase + m_upvalueOffsets[protoId] + static_cast<uint64_t>(index) * kUpvalueSlotSize;
    }

    bool Image::IsNumericUpvalue(uint32_t protoId, uint8_t index) const {
        if (protoId >= m_upvalueKinds.size() || index >= m_upvalueKinds[protoId].size())
            return false;
        return m_upvalueKinds[protoId][index] == UpvalueKind::Number;
    }

    bool Image::IsMethodField(std::string_view name) const { return m_methodFields.contains(std::string(name)); }

    Image::FieldKind Image::FieldValueKind(std::string_view name) const {
        const auto it = m_fieldValueKinds.find(std::string(name));
        return it == m_fieldValueKinds.end() ? FieldKind::Unknown : it->second;
    }

    Image::FieldKind Image::FieldResultKind(std::string_view name) const {
        const auto it = m_fieldResultKinds.find(std::string(name));
        return it == m_fieldResultKinds.end() ? FieldKind::Unknown : it->second;
    }

    int32_t Image::FieldOffset(std::string_view name) const {
        const auto it = m_fieldOffsets.find(std::string(name));
        return it == m_fieldOffsets.end() ? -1 : it->second;
    }

    std::optional<uint64_t> Image::StringAddress(std::string_view value) const {
        if (const auto it = m_stringAddresses.find(std::string(value)); it != m_stringAddresses.end())
            return it->second;
        return std::nullopt;
    }

    std::optional<std::string> Image::ConstantString(const DeserializedFunction &proto, int32_t constantIndex) const {
        if (constantIndex < 0 || static_cast<size_t>(constantIndex) >= proto.constants.size())
            return std::nullopt;
        const auto &constant = proto.constants[static_cast<size_t>(constantIndex)];
        if (constant.kType != LUA_TSTRING)
            return std::nullopt;
        const auto *value = std::get_if<std::string>(&constant.constantData);
        if (!value)
            return std::nullopt;
        return *value;
    }

    std::optional<uint64_t> Image::ConstantStringAddress(const DeserializedFunction &proto, int32_t constantIndex) const {
        const auto value = ConstantString(proto, constantIndex);
        return value ? StringAddress(*value) : std::nullopt;
    }

    std::vector<std::string> Image::ImportPath(const DeserializedFunction &proto, uint32_t aux) const {
        const int id0 = static_cast<int>((aux >> 20) & 1023);
        const int id1 = static_cast<int>((aux >> 10) & 1023);
        const int id2 = static_cast<int>(aux & 1023);
        const int count = static_cast<int>(aux >> 30);
        if (count <= 0 || count > 3)
            return {};

        std::vector<std::string> path;
        path.reserve(static_cast<size_t>(count));

        const auto append = [&](int index) {
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return false;
            const auto &constant = proto.constants[static_cast<size_t>(index)];
            if (constant.kType != LUA_TSTRING)
                return false;
            const auto *value = std::get_if<std::string>(&constant.constantData);
            if (!value)
                return false;
            path.push_back(*value);
            return true;
        };

        if (!append(id0))
            return {};
        if (count > 1 && !append(id1))
            return {};
        if (count > 2 && !append(id2))
            return {};
        return path;
    }

    std::string Image::DescribeConstant(const DeserializedFunction &proto, int32_t constantIndex) const {
        if (constantIndex < 0 || static_cast<size_t>(constantIndex) >= proto.constants.size())
            return "<constant out of range>";

        const auto &constant = proto.constants[static_cast<size_t>(constantIndex)];
        if (constant.IsClassShape()) {
            const auto &shape = std::get<LuauClassShape>(constant.constantData);
            return std::format("class {}", shape.className);
        }

        switch (constant.kType) {
        case LUA_TNIL:
            return "nil";
        case LUA_TBOOLEAN:
            return std::get<bool>(constant.constantData) ? "true" : "false";
        case LUA_TNUMBER: {
            const double value = std::get<double>(constant.constantData);
            return std::format("{:.14g}", value);
        }
        case LUA_TINTEGER:
            return std::format("{}i", std::get<int64_t>(constant.constantData));
        case LUA_TSTRING: {
            const auto &value = std::get<std::string>(constant.constantData);
            return std::format("\"{}\"", EscapeForDisplay(value));
        }
        case LUA_TVECTOR: {
            const auto &vector = std::get<LuauVectorConstant>(constant.constantData);
            return std::visit(
                [](const auto &components) {
                    return std::format("vector({:.6g}, {:.6g}, {:.6g}, {:.6g})", components[0], components[1], components[2], components[3]);
                },
                vector.components
            );
        }
        case LUA_TTABLE: {
            const auto &table = std::get<LuauTable>(constant.constantData);
            std::string out = "table{";
            for (size_t i = 0; i < table.keys.size(); ++i) {
                if (i)
                    out += ", ";
                out += EscapeForDisplay(table.keys[i], 24);
            }
            out += "}";
            return out;
        }
        case LUA_TFUNCTION: {
            const auto *protoPtr = std::get<DeserializedFunction *>(constant.constantData);
            if (protoPtr) {
                if (protoPtr->debugName && !protoPtr->debugName->empty())
                    return std::format("closure {}", *protoPtr->debugName);
                return std::format("closure proto_{}", protoPtr->bytecodeId);
            }
            return "closure";
        }
        default:
            return "constant";
        }
    }

    namespace {

        // Function-local statics avoid static initialization order dependencies between
        // the plugin's translation units.
        std::mutex &RegistryMutex() {
            static std::mutex mutex;
            return mutex;
        }

        std::vector<std::shared_ptr<Image>> &Registry() {
            static std::vector<std::shared_ptr<Image>> images;
            return images;
        }

    } // namespace

    void ImageRegistry::Add(const std::shared_ptr<Image> &image) {
        if (!image)
            return;
        std::lock_guard lock(RegistryMutex());
        Registry().push_back(image);
        // The image owns synthetic storage past the file-backed range; keep the
        // next allocation clear of it even though it was not known at AllocateBase.
        s_nextBase = std::max(s_nextBase, image->BaseAddress() + image->AddressSpaceSize() + 0x10000);
    }

    std::shared_ptr<Image> ImageRegistry::Find(uint64_t address) {
        std::lock_guard lock(RegistryMutex());
        for (const auto &image : Registry()) {
            if (address >= image->BaseAddress() && address < image->BaseAddress() + image->AddressSpaceSize())
                return image;
        }
        return nullptr;
    }

    std::shared_ptr<Image> ImageRegistry::FindExactBase(uint64_t base) {
        std::lock_guard lock(RegistryMutex());
        for (const auto &image : Registry()) {
            if (image->BaseAddress() == base)
                return image;
        }
        return nullptr;
    }

    uint64_t ImageRegistry::AllocateBase(uint64_t size) {
        constexpr uint64_t kAlignment = 0x10000;
        constexpr uint64_t kGap = 0x10000;
        std::lock_guard lock(RegistryMutex());
        const uint64_t base = s_nextBase;
        const uint64_t span = (size + kAlignment - 1) & ~(kAlignment - 1);
        s_nextBase = base + span + kGap;
        return base;
    }

} // namespace fission::bn
