//
// Binary Ninja architecture that lifts Luau bytecode to BNIL.
//

#include "Architecture.hpp"

#include "BytecodeLifter.hpp"
#include "Image.hpp"
#include "Intrinsics.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>

#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"
#include "lowlevelilinstruction.h"

namespace fission::bn {

    using namespace BinaryNinja;

    namespace {

        constexpr size_t kRegisterSize = 8;
        constexpr uint32_t kRegisterCount = 256;
        // Fixed-register ABI used only to make calls legible to Binary Ninja:
        // arguments travel in a0..a15 and results in v0..v15, while the VM's
        // frame-local registers R0..R255 stay callee-saved.
        constexpr uint32_t kArgumentCount = 16;
        constexpr uint32_t kArgumentBase = kRegisterCount;
        constexpr uint32_t kResultCount = 16;
        constexpr uint32_t kResultBase = kArgumentBase + kArgumentCount;
        // The VM has no architectural stack; reserve an unused register for the
        // stack-pointer query Binary Ninja performs.
        constexpr uint32_t kStackPointerRegister = kResultBase + kResultCount;
        // Semantic result registers used by builtins whose return convention is
        // known (pcall/xpcall/resume and coroutine.create/wrap). Naming these in
        // the architecture makes HLIL print `ok, result = pcall(...)` rather than
        // the anonymous `v0, v1` register names.
        constexpr uint32_t kPcallOkRegister = kStackPointerRegister + 1;
        constexpr uint32_t kPcallResultRegister = kPcallOkRegister + 1;
        constexpr uint32_t kThreadRegister = kPcallResultRegister + 1;
        // Semantic single-result registers (GetService children, constructor
        // results, getter results) are assigned names on demand from this pool.
        constexpr uint32_t kNamedResultBase = kThreadRegister + 1;
        constexpr uint32_t kNamedResultCount = 256;
        constexpr uint32_t kTotalRegisters = kNamedResultBase + kNamedResultCount;
        constexpr size_t kInstructionSize = sizeof(uint32_t);

        constexpr uint32_t ArgumentRegister(uint32_t index) { return kArgumentBase + index; }
        constexpr uint32_t ResultRegister(uint32_t index) { return kResultBase + index; }

        constexpr std::array<const char *, LOP__COUNT> kOpcodeNames = {
            "NOP",
            "BREAK",
            "LOADNIL",
            "LOADB",
            "LOADN",
            "LOADK",
            "MOVE",
            "GETGLOBAL",
            "SETGLOBAL",
            "GETUPVAL",
            "SETUPVAL",
            "CLOSEUPVALS",
            "GETIMPORT",
            "GETTABLE",
            "SETTABLE",
            "GETTABLEKS",
            "SETTABLEKS",
            "GETTABLEN",
            "SETTABLEN",
            "NEWCLOSURE",
            "NAMECALL",
            "CALL",
            "RETURN",
            "JUMP",
            "JUMPBACK",
            "JUMPIF",
            "JUMPIFNOT",
            "JUMPIFEQ",
            "JUMPIFLE",
            "JUMPIFLT",
            "JUMPIFNOTEQ",
            "JUMPIFNOTLE",
            "JUMPIFNOTLT",
            "ADD",
            "SUB",
            "MUL",
            "DIV",
            "MOD",
            "POW",
            "ADDK",
            "SUBK",
            "MULK",
            "DIVK",
            "MODK",
            "POWK",
            "AND",
            "OR",
            "ANDK",
            "ORK",
            "CONCAT",
            "NOT",
            "MINUS",
            "LENGTH",
            "NEWTABLE",
            "DUPTABLE",
            "SETLIST",
            "FORNPREP",
            "FORNLOOP",
            "FORGLOOP",
            "FORGPREP_INEXT",
            "FASTCALL3",
            "FORGPREP_NEXT",
            "NATIVECALL",
            "GETVARARGS",
            "DUPCLOSURE",
            "PREPVARARGS",
            "LOADKX",
            "JUMPX",
            "FASTCALL",
            "COVERAGE",
            "CAPTURE",
            "SUBRK",
            "DIVRK",
            "FASTCALL1",
            "FASTCALL2",
            "FASTCALL2K",
            "FORGPREP",
            "JUMPXEQKNIL",
            "JUMPXEQKB",
            "JUMPXEQKN",
            "JUMPXEQKS",
            "IDIV",
            "IDIVK",
            "GETUDATAKS",
            "SETUDATAKS",
            "NAMECALLUDATA",
            "NEWCLASSMEMBER",
            "CALLFB",
            "CMPPROTO",
            "FASTPCALL",
            "NEWCLASS",
        };

        static_assert(kOpcodeNames.size() == static_cast<size_t>(LOP__COUNT));

        uint32_t ReadRawInstruction(const uint8_t *data) {
            uint32_t word = 0;
            std::memcpy(&word, data, sizeof(word));
            return word;
        }

        std::string FormatInteger(int64_t value) {
            if (value < 0)
                return std::format("-0x{:x}", static_cast<uint64_t>(-value));
            return std::format("0x{:x}", static_cast<uint64_t>(value));
        }

        std::string JoinPath(const std::vector<std::string> &path) {
            std::string joined;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i)
                    joined += ".";
                joined += path[i];
            }
            return joined;
        }

        std::string SanitizeResultName(std::string name) {
            if (name.empty() || std::isdigit(static_cast<unsigned char>(name[0])))
                return {};
            for (const char character : name) {
                if (!std::isalnum(static_cast<unsigned char>(character)) && character != '_')
                    return {};
            }
            name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
            static const std::unordered_set<std::string> reserved = {"and", "break",    "do",     "else", "elseif", "end",   "false",
                                                                     "for", "function", "if",     "in",   "local",  "nil",   "not",
                                                                     "or",  "repeat",   "return", "then", "true",   "until", "while"};
            if (reserved.contains(name))
                return {};
            return name;
        }

        const char *DigitWord(char digit) {
            switch (digit) {
            case '0':
                return "zero";
            case '1':
                return "one";
            case '2':
                return "two";
            case '3':
                return "three";
            case '4':
                return "four";
            case '5':
                return "five";
            case '6':
                return "six";
            case '7':
                return "seven";
            case '8':
                return "eight";
            case '9':
                return "nine";
            default:
                return nullptr;
            }
        }

        std::string CamelCaseResultName(const std::string &value) {
            std::vector<std::string> words;
            std::string current;
            for (const char character : value) {
                if (std::isspace(static_cast<unsigned char>(character))) {
                    if (!current.empty()) {
                        words.push_back(current);
                        current.clear();
                    }
                } else {
                    current.push_back(character);
                }
            }
            if (!current.empty())
                words.push_back(current);
            if (words.empty())
                return {};

            std::string out = words[0];
            out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
            for (size_t i = 1; i < words.size(); ++i) {
                std::string word = words[i];
                for (char &character : word)
                    character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
                word[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(word[0])));
                out += word;
            }
            if (!out.empty()) {
                if (const char *spelled = DigitWord(out[0]))
                    out = std::string(spelled) + out.substr(1);
            }
            return SanitizeResultName(out);
        }

        class Lifter {
          public:
            Lifter(LowLevelILFunction &il, Architecture *arch, const Image &image, const ProtoView &proto, uint64_t address, uint32_t raw)
                : m_il(il), m_arch(arch), m_image(image), m_proto(proto), m_address(address), m_raw(raw),
                  m_index((address - proto.address) / kInstructionSize) {
                m_branchTarget = Luau::getJumpTarget(raw, static_cast<uint32_t>(m_index));
            }

            void Run(const LiftedInstruction &instruction);

          private:
            LowLevelILFunction &m_il;
            Architecture *m_arch;
            const Image &m_image;
            const ProtoView &m_proto;
            uint64_t m_address;
            uint32_t m_raw;
            uint64_t m_index = 0;
            int m_branchTarget = -1;

            [[nodiscard]] bool HasTarget() const { return m_branchTarget >= 0 && static_cast<size_t>(m_branchTarget) < m_proto.instructionCount; }
            [[nodiscard]] uint64_t TargetAddress() const { return m_proto.address + static_cast<uint64_t>(m_branchTarget) * kInstructionSize; }
            [[nodiscard]] uint64_t FallthroughAddress() const {
                return m_address + static_cast<size_t>(Luau::getOpLength(static_cast<LuauOpcode>(m_raw & 0xFF))) * kInstructionSize;
            }

            void Add(ExprId expr) { m_il.AddInstruction(expr); }

            ExprId Register(uint32_t reg) { return m_il.Register(kRegisterSize, reg); }
            void SetRegister(uint32_t reg, ExprId value) { Add(m_il.SetRegister(kRegisterSize, reg, value)); }

            // Stores a value in a semantic-name register and copies it to the
            // bytecode register. Binary Ninja coalesces the copy into the source
            // expression, giving HLIL names like `workspace = game:GetService(...)`.
            bool StoreNamedResult(const std::string &name, ExprId value, uint32_t dest) {
                if (name.empty())
                    return false;
                auto *luau = dynamic_cast<LuauArchitecture *>(m_arch);
                if (!luau)
                    return false;
                const uint32_t named = luau->NamedResultRegister(name);
                SetRegister(named, value);
                SetRegister(dest, Register(named));
                return true;
            }

            ExprId Const(int64_t value) { return m_il.Const(kRegisterSize, static_cast<uint64_t>(value)); }
            ExprId ConstPointer(uint64_t address) { return m_il.ConstPointer(kRegisterSize, address); }
            ExprId Float(double value) { return m_il.FloatConstDouble(value); }

            ExprId LoadMemory(ExprId address) { return m_il.Load(kRegisterSize, address); }
            ExprId FieldLoad(ExprId base, int32_t constantIndex);
            void StoreMemory(ExprId address, ExprId value) { Add(m_il.Store(kRegisterSize, address, value)); }
            ExprId Offset(ExprId base, int64_t offset) { return m_il.Add(kRegisterSize, base, Const(offset)); }
            ExprId Indexed(ExprId base, ExprId index, int64_t scale) { return m_il.Add(kRegisterSize, base, m_il.Mult(kRegisterSize, index, Const(scale))); }
            ExprId SelectByTruthiness(ExprId condition, ExprId whenTrue, ExprId whenFalse) {
                const ExprId mask = m_il.Neg(kRegisterSize, m_il.BoolToInt(kRegisterSize, condition));
                return m_il.Or(kRegisterSize, m_il.And(kRegisterSize, whenTrue, mask), m_il.And(kRegisterSize, whenFalse, m_il.Not(kRegisterSize, mask)));
            }

            std::optional<std::string> ConstantString(const DeserializedFunction &proto, int32_t index) const;
            std::string CalleeGlobal(uint32_t reg) const;
            std::string CalleeMethod(uint32_t reg) const;
            std::optional<std::string> ConstantStringForRegister(uint32_t reg) const;
            std::optional<std::string> FieldNameForRegister(uint32_t reg) const;
            std::string SemanticResultName(const std::string &callee, const std::string &method, uint32_t base) const;
            int32_t FieldOffset(const DeserializedFunction &proto, int32_t constantIndex) const;
            uint64_t GlobalSlot(const DeserializedFunction &proto, int32_t constantIndex) const;
            uint64_t GlobalSlotForImport(const DeserializedFunction &proto, uint32_t aux) const;
            uint64_t UpvalueSlot(uint8_t index) const;
            void EmitPrologue();

            ExprId Truthy(ExprId value) { return m_il.CompareNotEqual(kRegisterSize, value, Const(0)); }
            ExprId Falsey(ExprId value) { return m_il.CompareEqual(kRegisterSize, value, Const(0)); }
            ExprId Negate(ExprId condition) { return m_il.CompareEqual(kRegisterSize, condition, Const(0)); }

            ExprId ConstantValue(const DeserializedFunction &proto, int32_t index);
            ExprId OperandValue(const LiftedOperand &operand);
            std::optional<double> ConstantDouble(const DeserializedFunction &proto, int32_t index) const;
            std::optional<uint64_t> ClosureAddress(const DeserializedFunction &proto, int32_t index) const;
            std::optional<uint64_t> ClosureAddressFromSubfunction(const DeserializedFunction &proto, int32_t index) const;
            ExprId KeyPointer(const DeserializedFunction &proto, int32_t index);

            void EmitIntrinsic(Intrinsic id, const std::vector<uint32_t> &outputs, const std::vector<ExprId> &params) {
                std::vector<RegisterOrFlag> out;
                out.reserve(outputs.size());
                for (const uint32_t reg : outputs)
                    out.push_back(RegisterOrFlag::Register(reg));
                Add(m_il.Intrinsic(out, IntrinsicId(id), params));
            }

            void JumpTo(uint64_t target) { Add(m_il.Jump(ConstPointer(target))); }
            void BranchIf(ExprId condition, uint64_t taken, uint64_t fallthrough);
            void ConditionalBranch(ExprId condition) {
                if (HasTarget())
                    BranchIf(condition, TargetAddress(), FallthroughAddress());
            }
            void UnconditionalBranch() {
                // A jump to the next instruction is a no-op; Fission lifts those as NOP
                // and GetInstructionInfo omits the branch edge to match.
                if (HasTarget() && m_branchTarget != static_cast<int>(m_index + 1))
                    JumpTo(TargetAddress());
            }

            void EmitCall(const LiftedInstruction &instruction);
            void EmitReturn(const LiftedInstruction &instruction);
            void EmitNumericLoopPrep(const LiftedInstruction &instruction);
            void EmitNumericLoopLatch(const LiftedInstruction &instruction);
            void EmitGenericLoopLatch(const LiftedInstruction &instruction);
            void EmitGenericLoopPrep(const LiftedInstruction &instruction);
        };

        std::optional<uint64_t> Lifter::ClosureAddressFromSubfunction(const DeserializedFunction &proto, int32_t index) const {
            if (index < 0 || static_cast<size_t>(index) >= proto.subfunctions.size())
                return std::nullopt;
            const DeserializedFunction *target = proto.subfunctions[static_cast<size_t>(index)];
            if (!target)
                return std::nullopt;
            if (const ProtoView *view = m_image.ProtoByIndex(target->bytecodeId))
                return view->address;
            return std::nullopt;
        }

        std::optional<uint64_t> Lifter::ClosureAddress(const DeserializedFunction &proto, int32_t index) const {
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return std::nullopt;
            const auto &constant = proto.constants[static_cast<size_t>(index)];
            if (constant.kType != LUA_TFUNCTION)
                return std::nullopt;
            const auto *target = std::get_if<DeserializedFunction *>(&constant.constantData);
            if (!target || !*target)
                return std::nullopt;
            if (const ProtoView *view = m_image.ProtoByIndex((*target)->bytecodeId))
                return view->address;
            return std::nullopt;
        }

        std::optional<double> Lifter::ConstantDouble(const DeserializedFunction &proto, int32_t index) const {
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return std::nullopt;
            const auto &constant = proto.constants[static_cast<size_t>(index)];
            if (constant.kType == LUA_TNUMBER)
                return std::get<double>(constant.constantData);
            if (constant.kType == LUA_TINTEGER)
                return static_cast<double>(std::get<int64_t>(constant.constantData));
            return std::nullopt;
        }

        ExprId Lifter::KeyPointer(const DeserializedFunction &proto, int32_t index) {
            if (const auto address = m_image.ConstantStringAddress(proto, index))
                return ConstPointer(*address);
            return Const(0);
        }

        std::optional<std::string> Lifter::ConstantString(const DeserializedFunction &proto, int32_t index) const {
            return m_image.ConstantString(proto, index);
        }

        ExprId Lifter::FieldLoad(ExprId base, int32_t constantIndex) {
            const int32_t offset = FieldOffset(*m_proto.proto, constantIndex);
            if (offset < static_cast<int32_t>(Image::kFieldBase))
                return LoadMemory(Offset(base, 0));
            return LoadMemory(Offset(base, offset));
        }

        std::string Lifter::CalleeGlobal(uint32_t reg) const {
            if (!m_proto.lifted)
                return {};
            uint32_t current = reg;
            for (size_t i = m_index; i-- > 0;) {
                const auto &instruction = m_proto.lifted->instructions[i];
                if (!InstructionWritesRegister(instruction, static_cast<uint8_t>(current)))
                    continue;
                const auto &ops = instruction.operands;
                if (instruction.operation == LiftedOperation::MOVE && ops.size() >= 2 && ops[1].type == LiftedOperandType::Register) {
                    current = ops[1].value.reg;
                    continue;
                }
                if (instruction.operation == LiftedOperation::GETGLOBAL && ops.size() >= 2 && ops[1].type == LiftedOperandType::ImmediateConstant)
                    return ConstantString(*m_proto.proto, ops[1].value.imm.k).value_or("");
                if (instruction.operation == LiftedOperation::GETIMPORT && ops.size() >= 3) {
                    const auto path = m_image.ImportPath(*m_proto.proto, ops[2].value.imm.u);
                    std::string joined;
                    for (size_t part = 0; part < path.size(); ++part) {
                        if (part)
                            joined += ".";
                        joined += path[part];
                    }
                    return joined;
                }
                return {};
            }
            return {};
        }

        std::string Lifter::CalleeMethod(uint32_t reg) const {
            if (!m_proto.lifted)
                return {};
            uint32_t current = reg;
            for (size_t i = m_index; i-- > 0;) {
                const auto &instruction = m_proto.lifted->instructions[i];
                if (!InstructionWritesRegister(instruction, static_cast<uint8_t>(current)))
                    continue;
                const auto &ops = instruction.operands;
                if (instruction.operation == LiftedOperation::MOVE && ops.size() >= 2 && ops[1].type == LiftedOperandType::Register) {
                    current = ops[1].value.reg;
                    continue;
                }
                if (instruction.operation == LiftedOperation::NAMECALL && ops.size() >= 3 && ops[2].type == LiftedOperandType::ImmediateConstant)
                    return ConstantString(*m_proto.proto, ops[2].value.imm.k).value_or("");
                return {};
            }
            return {};
        }

        std::optional<std::string> Lifter::ConstantStringForRegister(uint32_t reg) const {
            if (!m_proto.lifted)
                return std::nullopt;
            uint32_t current = reg;
            for (size_t i = m_index; i-- > 0;) {
                const auto &instruction = m_proto.lifted->instructions[i];
                if (!InstructionWritesRegister(instruction, static_cast<uint8_t>(current)))
                    continue;
                const auto &ops = instruction.operands;
                if (instruction.operation == LiftedOperation::MOVE && ops.size() >= 2 && ops[1].type == LiftedOperandType::Register) {
                    current = ops[1].value.reg;
                    continue;
                }
                if (instruction.operation == LiftedOperation::LOAD && ops.size() >= 2 && ops[1].type == LiftedOperandType::ImmediateConstant)
                    return ConstantString(*m_proto.proto, ops[1].value.imm.k);
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::optional<std::string> Lifter::FieldNameForRegister(uint32_t reg) const {
            if (!m_proto.lifted)
                return std::nullopt;
            uint32_t current = reg;
            for (size_t i = m_index; i-- > 0;) {
                const auto &instruction = m_proto.lifted->instructions[i];
                if (!InstructionWritesRegister(instruction, static_cast<uint8_t>(current)))
                    continue;
                const auto &ops = instruction.operands;
                if (instruction.operation == LiftedOperation::MOVE && ops.size() >= 2 && ops[1].type == LiftedOperandType::Register) {
                    current = ops[1].value.reg;
                    continue;
                }
                if ((instruction.operation == LiftedOperation::GETTABLEKS || instruction.operation == LiftedOperation::GETTABLE ||
                     instruction.operation == LiftedOperation::GETUDATAKS) &&
                    ops.size() >= 3) {
                    if (ops[2].type == LiftedOperandType::ImmediateConstant)
                        return ConstantString(*m_proto.proto, ops[2].value.imm.k);
                    if (ops[2].type == LiftedOperandType::Register)
                        return ConstantStringForRegister(ops[2].value.reg);
                }
                return std::nullopt;
            }
            return std::nullopt;
        }

        std::string Lifter::SemanticResultName(const std::string &callee, const std::string &method, uint32_t base) const {
            if (!method.empty()) {
                if (method == "GetAttribute") {
                    if (const auto attribute = ConstantStringForRegister(base + 2))
                        return CamelCaseResultName(*attribute);
                    return {};
                }
                if (method == "GetService" || method == "FindFirstChild" || method == "WaitForChild") {
                    if (const auto child = ConstantStringForRegister(base + 2))
                        return SanitizeResultName(*child);
                    return {};
                }
                if (method.size() > 3 && (method.starts_with("Get") || method.starts_with("get")) && std::isupper(static_cast<unsigned char>(method[3]))) {
                    std::string leaf = method.substr(3);
                    leaf[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(leaf[0])));
                    return SanitizeResultName(leaf);
                }
                return {};
            }

            if (callee == "Instance.new") {
                if (const auto className = ConstantStringForRegister(base + 1))
                    return SanitizeResultName(*className);
                return {};
            }

            if (callee == "require") {
                if (const auto leaf = FieldNameForRegister(base + 1))
                    return SanitizeResultName(*leaf);
                return {};
            }

            const auto dot = callee.find('.');
            if (dot != std::string::npos) {
                const std::string type = callee.substr(0, dot);
                const std::string member = callee.substr(dot + 1);
                if (!type.empty() && std::isupper(static_cast<unsigned char>(type[0]))) {
                    const bool constructor =
                        member == "new" || (member.size() > 4 && member.starts_with("from") && std::isupper(static_cast<unsigned char>(member[4])));
                    if (constructor)
                        return SanitizeResultName(type);
                }
            }
            return {};
        }

        int32_t Lifter::FieldOffset(const DeserializedFunction &proto, int32_t constantIndex) const {
            const auto name = ConstantString(proto, constantIndex);
            if (!name)
                return -1;
            return m_image.FieldOffset(*name);
        }

        uint64_t Lifter::GlobalSlot(const DeserializedFunction &proto, int32_t constantIndex) const {
            const auto name = ConstantString(proto, constantIndex);
            return name ? m_image.GlobalSlot(*name) : 0;
        }

        uint64_t Lifter::GlobalSlotForImport(const DeserializedFunction &proto, uint32_t aux) const {
            const auto path = m_image.ImportPath(proto, aux);
            if (path.empty())
                return 0;
            return m_image.GlobalSlot(JoinPath(path));
        }

        uint64_t Lifter::UpvalueSlot(uint8_t index) const { return m_image.UpvalueSlot(m_proto.proto->bytecodeId, index); }

        ExprId Lifter::ConstantValue(const DeserializedFunction &proto, int32_t index) {
            if (index < 0 || static_cast<size_t>(index) >= proto.constants.size())
                return Const(0);

            const auto &constant = proto.constants[static_cast<size_t>(index)];
            if (constant.IsClassShape())
                return Const(0);

            switch (constant.kType) {
            case LUA_TNIL:
                return Const(0);
            case LUA_TBOOLEAN:
                return Const(std::get<bool>(constant.constantData) ? 1 : 0);
            case LUA_TNUMBER:
                return Float(std::get<double>(constant.constantData));
            case LUA_TINTEGER:
                return Const(std::get<int64_t>(constant.constantData));
            case LUA_TSTRING: {
                const auto &value = std::get<std::string>(constant.constantData);
                if (const auto address = m_image.StringAddress(value))
                    return ConstPointer(*address);
                return Const(0);
            }
            case LUA_TVECTOR:
                return Const(0);
            case LUA_TTABLE:
                return Const(0);
            case LUA_TFUNCTION:
                if (const auto address = ClosureAddress(proto, index))
                    return ConstPointer(*address);
                return Const(0);
            default:
                return Const(0);
            }
        }

        ExprId Lifter::OperandValue(const LiftedOperand &operand) {
            switch (operand.type) {
            case LiftedOperandType::Register:
                return Register(operand.value.reg);
            case LiftedOperandType::ImmediateNil:
                return Const(0);
            case LiftedOperandType::ImmediateInteger:
                return Const(operand.value.imm.n);
            case LiftedOperandType::ImmediateBool:
                return Const(operand.value.imm.b ? 1 : 0);
            case LiftedOperandType::ImmediateConstant:
                return ConstantValue(*m_proto.proto, operand.value.imm.k);
            case LiftedOperandType::ImmediateAux:
                return Const(static_cast<int64_t>(operand.value.imm.u));
            }
            return Const(0);
        }

        void Lifter::BranchIf(ExprId condition, uint64_t taken, uint64_t fallthrough) {
            BNLowLevelILLabel *trueLabel = m_il.GetLabelForAddress(m_arch, taken);
            BNLowLevelILLabel *falseLabel = m_il.GetLabelForAddress(m_arch, fallthrough);

            if (trueLabel && falseLabel) {
                Add(m_il.If(condition, *trueLabel, *falseLabel));
                return;
            }

            LowLevelILLabel trueCode, falseCode;
            if (trueLabel) {
                Add(m_il.If(condition, *trueLabel, falseCode));
                m_il.MarkLabel(falseCode);
                JumpTo(fallthrough);
                return;
            }
            if (falseLabel) {
                Add(m_il.If(condition, trueCode, *falseLabel));
                m_il.MarkLabel(trueCode);
                JumpTo(taken);
                return;
            }

            Add(m_il.If(condition, trueCode, falseCode));
            m_il.MarkLabel(trueCode);
            JumpTo(taken);
            m_il.MarkLabel(falseCode);
            JumpTo(fallthrough);
        }

        void Lifter::EmitPrologue() {
            const uint32_t count = std::min<uint32_t>(m_proto.proto->numparams, kArgumentCount);
            for (uint32_t i = 0; i < count; ++i)
                SetRegister(i, Register(ArgumentRegister(i)));
        }

        void Lifter::EmitCall(const LiftedInstruction &instruction) {
            if (instruction.operands.size() < 3)
                return;

            const auto base = static_cast<uint32_t>(instruction.operands[0].value.reg);
            const int argc = instruction.operands[1].value.imm.n;
            const int retc = instruction.operands[2].value.imm.n;
            const int maxStack = m_proto.proto->maxstacksize;

            int argumentCount = 0;
            if (argc == 0)
                argumentCount = std::min<int>(maxStack - static_cast<int>(base) - 1, kArgumentCount);
            else
                argumentCount = std::min<int>(argc - 1, kArgumentCount);

            int resultCount = 0;
            if (retc == 0)
                resultCount = std::min<int>(maxStack - static_cast<int>(base), kResultCount);
            else
                resultCount = std::min<int>(retc - 1, kResultCount);

            const std::string callee = CalleeGlobal(base);
            const auto emitSemanticCall = [&](Intrinsic intrinsic, uint32_t firstOutput, uint32_t secondOutput) {
                std::vector<ExprId> params;
                params.reserve(static_cast<size_t>(argumentCount));
                for (int i = 0; i < argumentCount; ++i) {
                    const uint32_t reg = base + 1 + static_cast<uint32_t>(i);
                    if (reg >= kRegisterCount)
                        break;
                    params.push_back(Register(reg));
                }

                std::vector<uint32_t> outputs;
                if (resultCount > 0)
                    outputs.push_back(firstOutput);
                if (resultCount > 1 && secondOutput != UINT32_MAX)
                    outputs.push_back(secondOutput);
                for (int i = 2; i < resultCount; ++i)
                    outputs.push_back(ResultRegister(static_cast<uint32_t>(i)));

                EmitIntrinsic(intrinsic, outputs, params);
                if (resultCount > 0)
                    SetRegister(base, Register(firstOutput));
                if (resultCount > 1 && secondOutput != UINT32_MAX)
                    SetRegister(base + 1, Register(secondOutput));
                for (int i = 2; i < resultCount; ++i)
                    SetRegister(base + static_cast<uint32_t>(i), Register(ResultRegister(static_cast<uint32_t>(i))));
            };

            if (callee == "pcall" || callee == "ypcall") {
                emitSemanticCall(Intrinsic::Pcall, kPcallOkRegister, kPcallResultRegister);
                return;
            }
            if (callee == "xpcall") {
                emitSemanticCall(Intrinsic::Xpcall, kPcallOkRegister, kPcallResultRegister);
                return;
            }
            if (callee == "coroutine.resume") {
                emitSemanticCall(Intrinsic::Resume, kPcallOkRegister, kPcallResultRegister);
                return;
            }
            if (callee == "coroutine.create") {
                emitSemanticCall(Intrinsic::CreateThread, kThreadRegister, UINT32_MAX);
                return;
            }

            const std::string method = CalleeMethod(base);
            const std::string resultName = SemanticResultName(callee, method, base);
            uint32_t namedResult = UINT32_MAX;
            if (!resultName.empty() && resultCount > 0) {
                if (auto *luau = dynamic_cast<LuauArchitecture *>(m_arch))
                    namedResult = luau->NamedResultRegister(resultName);
            }

            for (int i = 0; i < argumentCount; ++i) {
                const uint32_t reg = base + 1 + static_cast<uint32_t>(i);
                if (reg >= kRegisterCount)
                    break;
                SetRegister(ArgumentRegister(static_cast<uint32_t>(i)), Register(reg));
            }

            ExprId target = Register(base);
            if (m_index < m_proto.directCallTargets.size() && m_proto.directCallTargets[m_index] != 0)
                target = ConstPointer(m_proto.directCallTargets[m_index]);
            Add(m_il.Call(target));

            for (int i = 0; i < resultCount; ++i) {
                const uint32_t reg = base + static_cast<uint32_t>(i);
                if (reg >= kRegisterCount)
                    break;
                uint32_t resultRegister = ResultRegister(static_cast<uint32_t>(i));
                if (i == 0 && namedResult != UINT32_MAX) {
                    SetRegister(namedResult, Register(ResultRegister(0)));
                    resultRegister = namedResult;
                }
                SetRegister(reg, Register(resultRegister));
            }
        }

        void Lifter::EmitReturn(const LiftedInstruction &instruction) {
            if (instruction.operands.size() < 2)
                return;
            const auto base = static_cast<uint32_t>(instruction.operands[0].value.reg);
            const int count = instruction.operands[1].value.imm.n == 0 ? 1 : instruction.operands[1].value.imm.n - 1;
            for (int i = 0; i < count && i < static_cast<int>(kResultCount); ++i) {
                const uint32_t reg = base + static_cast<uint32_t>(i);
                if (reg >= kRegisterCount)
                    break;
                SetRegister(ResultRegister(static_cast<uint32_t>(i)), Register(reg));
            }
            Add(m_il.Return(Const(0)));
        }

        void Lifter::EmitNumericLoopPrep(const LiftedInstruction &instruction) {
            if (instruction.operands.empty())
                return;

            const auto base = static_cast<uint8_t>(instruction.operands[0].value.reg);
            if (static_cast<unsigned>(base) + 2 >= kRegisterCount)
                return;

            // Run the loop when step > 0 ? index <= limit : limit <= index. The branch
            // taken here is the loop-exit edge.
            const ExprId limit = Register(base);
            const ExprId step = Register(static_cast<uint8_t>(base + 1));
            const ExprId index = Register(static_cast<uint8_t>(base + 2));

            const ExprId positiveStep = m_il.FloatCompareGreaterThan(kRegisterSize, step, Float(0.0));
            const ExprId nonPositiveStep = m_il.FloatCompareLessEqual(kRegisterSize, step, Float(0.0));
            const ExprId indexAboveLimit = m_il.FloatCompareGreaterThan(kRegisterSize, index, limit);
            const ExprId limitAboveIndex = m_il.FloatCompareGreaterThan(kRegisterSize, limit, index);
            const ExprId skipPositive = m_il.And(kRegisterSize, positiveStep, indexAboveLimit);
            const ExprId skipNegative = m_il.And(kRegisterSize, nonPositiveStep, limitAboveIndex);
            const ExprId skip = m_il.Or(kRegisterSize, skipPositive, skipNegative);

            ConditionalBranch(skip);
        }

        void Lifter::EmitNumericLoopLatch(const LiftedInstruction &instruction) {
            if (instruction.operands.empty())
                return;

            const auto base = static_cast<uint8_t>(instruction.operands[0].value.reg);
            if (static_cast<unsigned>(base) + 2 >= kRegisterCount)
                return;

            const ExprId limit = Register(base);
            const ExprId step = Register(static_cast<uint8_t>(base + 1));
            const ExprId nextIndex = m_il.FloatAdd(kRegisterSize, Register(static_cast<uint8_t>(base + 2)), step);
            SetRegister(static_cast<uint8_t>(base + 2), nextIndex);

            const ExprId positiveStep = m_il.FloatCompareGreaterThan(kRegisterSize, step, Float(0.0));
            const ExprId nonPositiveStep = m_il.FloatCompareLessEqual(kRegisterSize, step, Float(0.0));
            const ExprId withinLimit = m_il.FloatCompareLessEqual(kRegisterSize, nextIndex, limit);
            const ExprId aboveLimit = m_il.FloatCompareGreaterEqual(kRegisterSize, nextIndex, limit);
            const ExprId continuePositive = m_il.And(kRegisterSize, positiveStep, withinLimit);
            const ExprId continueNegative = m_il.And(kRegisterSize, nonPositiveStep, aboveLimit);
            const ExprId continueLoop = m_il.Or(kRegisterSize, continuePositive, continueNegative);

            ConditionalBranch(continueLoop);
        }

        void Lifter::EmitGenericLoopLatch(const LiftedInstruction &instruction) {
            if (instruction.operands.size() < 3)
                return;

            const auto base = static_cast<uint8_t>(instruction.operands[0].value.reg);
            const int numVars = instruction.operands[2].value.imm.n & 0xFF;
            if (static_cast<unsigned>(base) + 2 >= kRegisterCount)
                return;

            std::vector<uint32_t> outputs;
            outputs.push_back(static_cast<uint32_t>(base) + 2);
            for (int i = 0; i < numVars; ++i) {
                const unsigned reg = static_cast<unsigned>(base) + 3 + static_cast<unsigned>(i);
                if (reg >= kRegisterCount)
                    break;
                outputs.push_back(reg);
            }

            const std::vector<ExprId> params = {Register(base), Register(static_cast<uint8_t>(base + 1)), Register(static_cast<uint8_t>(base + 2))};
            EmitIntrinsic(Intrinsic::IterNext, outputs, params);

            const unsigned firstVar = static_cast<unsigned>(base) + 3;
            if (firstVar < kRegisterCount)
                ConditionalBranch(m_il.CompareNotEqual(kRegisterSize, Register(static_cast<uint8_t>(firstVar)), Const(0)));
            else
                ConditionalBranch(Const(0));
        }

        void Lifter::EmitGenericLoopPrep(const LiftedInstruction &instruction) {
            if (instruction.operands.empty())
                return;

            const auto base = static_cast<uint8_t>(instruction.operands[0].value.reg);
            if (static_cast<unsigned>(base) + 2 >= kRegisterCount)
                return;

            const std::vector<ExprId> params = {Register(base), Register(static_cast<uint8_t>(base + 1)), Register(static_cast<uint8_t>(base + 2))};
            EmitIntrinsic(Intrinsic::IterPrepare, {}, params);
            UnconditionalBranch();
        }

        void Lifter::Run(const LiftedInstruction &instruction) {
            const auto &ops = instruction.operands;
            const auto reg = [&](size_t index) -> uint32_t { return index < ops.size() ? ops[index].value.reg : 0; };
            const auto integer = [&](size_t index) -> int32_t { return index < ops.size() ? ops[index].value.imm.n : 0; };

            if (m_index == 0)
                EmitPrologue();

            switch (instruction.operation) {
            case LiftedOperation::NOP:
                Add(m_il.Nop());
                break;

            case LiftedOperation::BREAK:
                Add(m_il.Breakpoint());
                break;

            case LiftedOperation::LOAD:
                if (ops.size() >= 2)
                    SetRegister(reg(0), OperandValue(ops[1]));
                break;

            case LiftedOperation::LOADNJUMP:
                if (ops.size() >= 2)
                    SetRegister(reg(0), OperandValue(ops[1]));
                UnconditionalBranch();
                break;

            case LiftedOperation::MOVE:
                if (ops.size() >= 2)
                    SetRegister(reg(0), OperandValue(ops[1]));
                break;

            case LiftedOperation::GETGLOBAL: {
                if (ops.size() >= 2) {
                    const uint64_t slot = GlobalSlot(*m_proto.proto, ops[1].value.imm.k);
                    SetRegister(reg(0), slot ? LoadMemory(ConstPointer(slot)) : Const(0));
                }
                break;
            }

            case LiftedOperation::SETGLOBAL:
                if (ops.size() >= 2) {
                    const uint64_t slot = GlobalSlot(*m_proto.proto, ops[1].value.imm.k);
                    if (slot)
                        StoreMemory(ConstPointer(slot), OperandValue(ops[0]));
                }
                break;

            case LiftedOperation::GETUPVAL: {
                if (ops.size() >= 2) {
                    const uint64_t slot = UpvalueSlot(static_cast<uint8_t>(integer(1)));
                    SetRegister(reg(0), slot ? LoadMemory(ConstPointer(slot)) : Const(0));
                }
                break;
            }

            case LiftedOperation::SETUPVAL:
                if (ops.size() >= 2) {
                    const uint64_t slot = UpvalueSlot(static_cast<uint8_t>(integer(1)));
                    if (slot)
                        StoreMemory(ConstPointer(slot), OperandValue(ops[0]));
                }
                break;

            case LiftedOperation::GETIMPORT: {
                if (ops.size() < 3)
                    break;
                const uint64_t slot = GlobalSlotForImport(*m_proto.proto, ops[2].value.imm.u);
                SetRegister(reg(0), slot ? LoadMemory(ConstPointer(slot)) : Const(0));
                break;
            }

            case LiftedOperation::GETTABLE: {
                if (ops.size() >= 3) {
                    const ExprId value = LoadMemory(Indexed(Register(reg(1)), Register(reg(2)), 8));
                    const std::string key = ConstantStringForRegister(reg(2)).value_or("");
                    if (m_image.IsMethodField(key) || !StoreNamedResult(SanitizeResultName(key), value, reg(0)))
                        SetRegister(reg(0), value);
                }
                break;
            }

            case LiftedOperation::SETTABLE:
                if (ops.size() >= 3)
                    StoreMemory(Indexed(Register(reg(1)), Register(reg(2)), 8), OperandValue(ops[0]));
                break;

            case LiftedOperation::GETTABLEKS: {
                if (ops.size() >= 3) {
                    const ExprId value = FieldLoad(Register(reg(1)), ops[2].value.imm.k);
                    const std::string key = ConstantString(*m_proto.proto, ops[2].value.imm.k).value_or("");
                    if (m_image.IsMethodField(key) || !StoreNamedResult(SanitizeResultName(key), value, reg(0)))
                        SetRegister(reg(0), value);
                }
                break;
            }

            case LiftedOperation::SETTABLEKS: {
                if (ops.size() >= 3) {
                    int32_t offset = FieldOffset(*m_proto.proto, ops[2].value.imm.k);
                    if (offset < 0)
                        offset = 0;
                    StoreMemory(Offset(Register(reg(1)), offset), OperandValue(ops[0]));
                }
                break;
            }

            case LiftedOperation::GETTABLEN:
                if (ops.size() >= 3)
                    SetRegister(reg(0), LoadMemory(Offset(Register(reg(1)), (integer(2) + 1) * 8)));
                break;

            case LiftedOperation::SETTABLEN:
                if (ops.size() >= 3)
                    StoreMemory(Offset(Register(reg(1)), (integer(2) + 1) * 8), OperandValue(ops[0]));
                break;

            case LiftedOperation::NEWTABLE:
            case LiftedOperation::DUPTABLE: {
                const uint64_t address = m_index < m_proto.tableAddresses.size() ? m_proto.tableAddresses[m_index] : 0;
                SetRegister(reg(0), address ? ConstPointer(address) : Const(0));

                // v7+ TABLE_WITH_CONSTANTS carries key/value pairs; materialize
                // them so a constructor reads like field initialization.
                if (instruction.operation == LiftedOperation::DUPTABLE && address && ops.size() >= 2) {
                    const int32_t index = ops[1].value.imm.k;
                    if (index >= 0 && static_cast<size_t>(index) < m_proto.proto->constants.size()) {
                        const auto &constant = m_proto.proto->constants[static_cast<size_t>(index)];
                        if (constant.kType == LUA_TTABLE) {
                            const auto &table = std::get<LuauTable>(constant.constantData);
                            if (table.valueConstantIndices.size() == table.keys.size()) {
                                for (size_t i = 0; i < table.keys.size(); ++i) {
                                    const int32_t offset = m_image.FieldOffset(table.keys[i]);
                                    if (offset < 0)
                                        continue;
                                    StoreMemory(Offset(ConstPointer(address), offset), ConstantValue(*m_proto.proto, table.valueConstantIndices[i]));
                                }
                            }
                        }
                    }
                }
                break;
            }

            case LiftedOperation::SETLIST: {
                if (ops.size() >= 4) {
                    const uint32_t tableBase = reg(0);
                    const uint32_t sourceBase = reg(1);
                    const int count = ops[2].value.imm.n == 0 ? 0 : ops[2].value.imm.n - 1;
                    const int start = integer(3);
                    for (int i = 0; i < count && i < 32; ++i) {
                        const uint32_t source = sourceBase + static_cast<uint32_t>(i);
                        if (source >= kRegisterCount)
                            break;
                        StoreMemory(Offset(Register(tableBase), static_cast<int64_t>(start + i) * 8), Register(source));
                    }
                }
                break;
            }

            case LiftedOperation::NEWCLOSURE:
                if (ops.size() >= 2) {
                    const auto address = ClosureAddressFromSubfunction(*m_proto.proto, ops[1].value.imm.k);
                    SetRegister(reg(0), ConstPointer(address.value_or(0)));
                }
                break;

            case LiftedOperation::DUPCLOSURE:
                if (ops.size() >= 2) {
                    const auto address = ClosureAddress(*m_proto.proto, ops[1].value.imm.k);
                    SetRegister(reg(0), ConstPointer(address.value_or(0)));
                }
                break;

            case LiftedOperation::CAPTURE:
                if (ops.size() >= 2 && m_index < m_proto.captureSlots.size()) {
                    const uint64_t slot = m_proto.captureSlots[m_index];
                    if (slot) {
                        const int32_t kind = integer(0);
                        ExprId value;
                        if (kind == 2) {
                            const uint64_t sourceSlot = UpvalueSlot(static_cast<uint8_t>(ops[1].value.reg));
                            value = sourceSlot ? LoadMemory(ConstPointer(sourceSlot)) : Const(0);
                        } else {
                            value = OperandValue(ops[1]);
                        }
                        StoreMemory(ConstPointer(slot), value);
                    }
                }
                break;

            case LiftedOperation::NAMECALL: {
                if (ops.size() >= 3) {
                    const uint32_t target = reg(0);
                    const uint32_t object = reg(1);
                    if (target + 1 < kRegisterCount)
                        SetRegister(target + 1, Register(object));
                    SetRegister(target, FieldLoad(Register(object), ops[2].value.imm.k));
                }
                break;
            }

            case LiftedOperation::CALL:
            case LiftedOperation::CALLFB:
                EmitCall(instruction);
                break;

            case LiftedOperation::RETURN:
                EmitReturn(instruction);
                break;

            case LiftedOperation::GETVARARGS: {
                if (ops.size() < 2)
                    break;
                const auto base = static_cast<uint8_t>(ops[0].value.reg);
                const int count = ops[1].value.imm.n == 0 ? 1 : ops[1].value.imm.n - 1;
                std::vector<uint32_t> outputs;
                for (int i = 0; i < count; ++i) {
                    const unsigned target = static_cast<unsigned>(base) + static_cast<unsigned>(i);
                    if (target >= kRegisterCount)
                        break;
                    outputs.push_back(target);
                }
                EmitIntrinsic(Intrinsic::Vararg, outputs, {Const(ops[1].value.imm.n)});
                break;
            }

            case LiftedOperation::PREPVARARGS:
                Add(m_il.Nop());
                break;

            case LiftedOperation::JUMP:
                UnconditionalBranch();
                break;

            case LiftedOperation::JUMPIF:
                if (!ops.empty())
                    ConditionalBranch(Truthy(OperandValue(ops[0])));
                break;

            case LiftedOperation::JUMPIFNOT:
                if (!ops.empty())
                    ConditionalBranch(Falsey(OperandValue(ops[0])));
                break;

            case LiftedOperation::JUMPIFEQ:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.CompareEqual(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPIFNOTEQ:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.CompareNotEqual(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPIFLE:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.FloatCompareLessEqual(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPIFLT:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.FloatCompareLessThan(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPIFNOTLE:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.FloatCompareGreaterThan(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPIFNOTLT:
                if (ops.size() >= 3)
                    ConditionalBranch(m_il.FloatCompareGreaterEqual(kRegisterSize, OperandValue(ops[0]), OperandValue(ops[2])));
                break;

            case LiftedOperation::JUMPXEQK: {
                if (ops.size() < 4)
                    break;
                ExprId condition = Const(0);
                const auto &constantOperand = ops[2];
                if (constantOperand.type == LiftedOperandType::ImmediateNil) {
                    condition = m_il.CompareEqual(kRegisterSize, OperandValue(ops[0]), Const(0));
                } else if (constantOperand.type == LiftedOperandType::ImmediateBool) {
                    condition = m_il.CompareEqual(kRegisterSize, OperandValue(ops[0]), Const(constantOperand.value.imm.b ? 1 : 0));
                } else if (constantOperand.type == LiftedOperandType::ImmediateConstant) {
                    const int32_t index = constantOperand.value.imm.k;
                    if (const auto number = ConstantDouble(*m_proto.proto, index))
                        condition = m_il.FloatCompareEqual(kRegisterSize, OperandValue(ops[0]), Float(*number));
                    else
                        condition = m_il.CompareEqual(kRegisterSize, OperandValue(ops[0]), ConstantValue(*m_proto.proto, index));
                }

                // The AUX flip bit means "jump when the comparison is false".
                // Emit the positive comparison and swap the branch targets so
                // HLIL structures it as `if (a == b)`, not `if (not(a != b))`.
                if (HasTarget()) {
                    if (ops[3].value.imm.b)
                        BranchIf(condition, FallthroughAddress(), TargetAddress());
                    else
                        BranchIf(condition, TargetAddress(), FallthroughAddress());
                }
                break;
            }

            case LiftedOperation::FORNPREP:
                EmitNumericLoopPrep(instruction);
                break;

            case LiftedOperation::FORNLOOP:
                EmitNumericLoopLatch(instruction);
                break;

            case LiftedOperation::FORGLOOP:
                EmitGenericLoopLatch(instruction);
                break;

            case LiftedOperation::FORGPREP:
            case LiftedOperation::FORGPREP_INEXT:
            case LiftedOperation::FORGPREP_NEXT:
                EmitGenericLoopPrep(instruction);
                break;

            case LiftedOperation::ADD:
            case LiftedOperation::SUB:
            case LiftedOperation::MUL:
            case LiftedOperation::DIV: {
                if (ops.size() < 3)
                    break;
                const ExprId lhs = OperandValue(ops[1]);
                const ExprId rhs = OperandValue(ops[2]);
                ExprId result = Const(0);
                switch (instruction.operation) {
                case LiftedOperation::ADD:
                    result = m_il.FloatAdd(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::SUB:
                    result = m_il.FloatSub(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::MUL:
                    result = m_il.FloatMult(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::DIV:
                    result = m_il.FloatDiv(kRegisterSize, lhs, rhs);
                    break;
                default:
                    break;
                }
                SetRegister(reg(0), result);
                break;
            }

            case LiftedOperation::ADDK:
            case LiftedOperation::SUBK:
            case LiftedOperation::MULK:
            case LiftedOperation::DIVK: {
                if (ops.size() < 3)
                    break;
                const auto constant = ConstantDouble(*m_proto.proto, ops[2].value.imm.k);
                if (!constant) {
                    EmitIntrinsic(
                        Intrinsic::Arithmetic, {reg(0)}, {Const(static_cast<int64_t>(instruction.operation)), OperandValue(ops[1]), OperandValue(ops[2])}
                    );
                    break;
                }
                const ExprId lhs = OperandValue(ops[1]);
                const ExprId rhs = Float(*constant);
                ExprId result = Const(0);
                switch (instruction.operation) {
                case LiftedOperation::ADDK:
                    result = m_il.FloatAdd(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::SUBK:
                    result = m_il.FloatSub(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::MULK:
                    result = m_il.FloatMult(kRegisterSize, lhs, rhs);
                    break;
                case LiftedOperation::DIVK:
                    result = m_il.FloatDiv(kRegisterSize, lhs, rhs);
                    break;
                default:
                    break;
                }
                SetRegister(reg(0), result);
                break;
            }

            case LiftedOperation::SUBRK:
            case LiftedOperation::DIVRK: {
                if (ops.size() < 3)
                    break;
                const auto constant = ConstantDouble(*m_proto.proto, ops[1].value.imm.k);
                if (!constant) {
                    EmitIntrinsic(
                        Intrinsic::Arithmetic, {reg(0)}, {Const(static_cast<int64_t>(instruction.operation)), OperandValue(ops[1]), OperandValue(ops[2])}
                    );
                    break;
                }
                const ExprId lhs = Float(*constant);
                const ExprId rhs = OperandValue(ops[2]);
                const ExprId result =
                    instruction.operation == LiftedOperation::SUBRK ? m_il.FloatSub(kRegisterSize, lhs, rhs) : m_il.FloatDiv(kRegisterSize, lhs, rhs);
                SetRegister(reg(0), result);
                break;
            }

            case LiftedOperation::MOD:
            case LiftedOperation::POW:
            case LiftedOperation::IDIV:
                if (ops.size() >= 3) {
                    const Intrinsic intrinsic = instruction.operation == LiftedOperation::MOD
                                                    ? Intrinsic::Modulo
                                                    : (instruction.operation == LiftedOperation::POW ? Intrinsic::Power : Intrinsic::IntegerDivide);
                    EmitIntrinsic(intrinsic, {reg(0)}, {OperandValue(ops[1]), OperandValue(ops[2])});
                }
                break;

            case LiftedOperation::MODK:
            case LiftedOperation::POWK:
            case LiftedOperation::IDIVK:
                if (ops.size() >= 3) {
                    const Intrinsic intrinsic = instruction.operation == LiftedOperation::MODK
                                                    ? Intrinsic::Modulo
                                                    : (instruction.operation == LiftedOperation::POWK ? Intrinsic::Power : Intrinsic::IntegerDivide);
                    EmitIntrinsic(intrinsic, {reg(0)}, {OperandValue(ops[1]), OperandValue(ops[2])});
                }
                break;

            case LiftedOperation::AND:
            case LiftedOperation::OR:
                if (ops.size() >= 3) {
                    const ExprId left = OperandValue(ops[1]);
                    const ExprId right = OperandValue(ops[2]);
                    const bool isAnd = instruction.operation == LiftedOperation::AND;
                    SetRegister(reg(0), isAnd ? SelectByTruthiness(Truthy(left), right, left) : SelectByTruthiness(Truthy(left), left, right));
                }
                break;

            case LiftedOperation::ANDK:
            case LiftedOperation::ORK:
                if (ops.size() >= 3) {
                    const ExprId left = OperandValue(ops[1]);
                    const ExprId right = OperandValue(ops[2]);
                    const bool isAnd = instruction.operation == LiftedOperation::ANDK;
                    SetRegister(reg(0), isAnd ? SelectByTruthiness(Truthy(left), right, left) : SelectByTruthiness(Truthy(left), left, right));
                }
                break;

            case LiftedOperation::CONCAT:
                if (ops.size() >= 3) {
                    std::vector<ExprId> params;
                    const unsigned begin = reg(1);
                    const unsigned end = reg(2);
                    for (unsigned i = begin; i <= end && i < kRegisterCount; ++i)
                        params.push_back(Register(static_cast<uint8_t>(i)));
                    EmitIntrinsic(Intrinsic::Concat, {reg(0)}, params);
                }
                break;

            case LiftedOperation::NOT:
                if (ops.size() >= 2)
                    SetRegister(reg(0), m_il.BoolToInt(kRegisterSize, Falsey(OperandValue(ops[1]))));
                break;

            case LiftedOperation::MINUS:
                if (ops.size() >= 2)
                    SetRegister(reg(0), m_il.FloatNeg(kRegisterSize, OperandValue(ops[1])));
                break;

            case LiftedOperation::LENGTH:
                if (ops.size() >= 2) {
                    if (auto *luau = dynamic_cast<LuauArchitecture *>(m_arch)) {
                        const uint32_t count = luau->NamedResultRegister("count");
                        EmitIntrinsic(Intrinsic::Length, {count}, {OperandValue(ops[1])});
                        SetRegister(reg(0), Register(count));
                    } else {
                        EmitIntrinsic(Intrinsic::Length, {reg(0)}, {OperandValue(ops[1])});
                    }
                }
                break;

            case LiftedOperation::NEWCLASS:
                if (ops.size() >= 4) {
                    const ExprId super = ops[1].type == LiftedOperandType::ImmediateNil ? Const(0) : OperandValue(ops[1]);
                    EmitIntrinsic(Intrinsic::NewClass, {reg(0)}, {super, OperandValue(ops[2]), OperandValue(ops[3])});
                }
                break;

            case LiftedOperation::NEWCLASSMEMBER:
                if (ops.size() >= 3)
                    EmitIntrinsic(Intrinsic::NewClassMember, {}, {OperandValue(ops[0]), OperandValue(ops[1]), KeyPointer(*m_proto.proto, ops[2].value.imm.k)});
                break;

            case LiftedOperation::FASTCALL:
            case LiftedOperation::FASTCALL1:
            case LiftedOperation::FASTCALL2:
            case LiftedOperation::FASTCALL2K:
            case LiftedOperation::FASTCALL3:
            case LiftedOperation::FASTPCALL:
            case LiftedOperation::CMPPROTO:
                Add(m_il.Nop());
                break;

            case LiftedOperation::CONDVALUE:
            case LiftedOperation::PHI:
            default:
                Add(m_il.Unimplemented());
                break;
            }
        }

    } // namespace

    LuauArchitecture::LuauArchitecture() : Architecture("luau-bytecode") {}

    BNEndianness LuauArchitecture::GetEndianness() const { return LittleEndian; }
    size_t LuauArchitecture::GetAddressSize() const { return 8; }
    size_t LuauArchitecture::GetDefaultIntegerSize() const { return 8; }
    // Proto instruction arrays begin at arbitrary file offsets; the view maps
    // them at those offsets, so BN must accept unaligned function starts.
    size_t LuauArchitecture::GetInstructionAlignment() const { return 1; }
    size_t LuauArchitecture::GetLinearSweepInitialAlignment() const { return 1; }
    size_t LuauArchitecture::GetMaxInstructionLength() const { return kInstructionSize * 2; }
    size_t LuauArchitecture::GetOpcodeDisplayLength() const { return kInstructionSize; }

    std::string LuauArchitecture::GetRegisterName(uint32_t reg) {
        if (reg < kRegisterCount)
            return std::format("R{}", reg);
        if (reg >= kArgumentBase && reg < kArgumentBase + kArgumentCount)
            return std::format("a{}", reg - kArgumentBase);
        if (reg >= kResultBase && reg < kResultBase + kResultCount)
            return std::format("v{}", reg - kResultBase);
        if (reg == kStackPointerRegister)
            return "sp";
        if (reg == kPcallOkRegister)
            return "ok";
        if (reg == kPcallResultRegister)
            return "result";
        if (reg == kThreadRegister)
            return "thread";
        if (reg >= kNamedResultBase && reg < kNamedResultBase + kNamedResultCount) {
            std::lock_guard lock(m_namedResultMutex);
            const size_t index = reg - kNamedResultBase;
            if (index < m_namedResultNames.size())
                return m_namedResultNames[index];
            return std::format("named{}", index);
        }
        return std::format("reg{}", reg);
    }

    uint32_t LuauArchitecture::NamedResultRegister(const std::string &name) {
        std::lock_guard lock(m_namedResultMutex);
        for (size_t i = 0; i < m_namedResultNames.size(); ++i) {
            if (m_namedResultNames[i] == name)
                return kNamedResultBase + static_cast<uint32_t>(i);
        }
        if (m_namedResultNames.size() >= kMaxNamedResults)
            return ResultRegister(0);
        m_namedResultNames.push_back(name);
        return kNamedResultBase + static_cast<uint32_t>(m_namedResultNames.size() - 1);
    }

    BNRegisterInfo LuauArchitecture::GetRegisterInfo(uint32_t reg) {
        BNRegisterInfo info{};
        info.fullWidthRegister = reg;
        info.offset = 0;
        info.size = kRegisterSize;
        info.extend = NoExtend;
        return info;
    }

    std::vector<uint32_t> LuauArchitecture::GetAllRegisters() {
        std::vector<uint32_t> registers(kTotalRegisters);
        for (uint32_t i = 0; i < kTotalRegisters; ++i)
            registers[i] = i;
        return registers;
    }

    std::vector<uint32_t> LuauArchitecture::GetFullWidthRegisters() { return GetAllRegisters(); }
    std::vector<uint32_t> LuauArchitecture::GetGlobalRegisters() { return {}; }
    uint32_t LuauArchitecture::GetStackPointerRegister() { return kStackPointerRegister; }
    std::string LuauArchitecture::GetIntrinsicName(uint32_t intrinsic) { return IntrinsicName(intrinsic); }

    std::vector<uint32_t> LuauArchitecture::GetAllIntrinsics() {
        std::vector<uint32_t> intrinsics(IntrinsicCount());
        for (uint32_t i = 0; i < intrinsics.size(); ++i)
            intrinsics[i] = i;
        return intrinsics;
    }

    BNIntrinsicClass LuauArchitecture::GetIntrinsicClass(uint32_t) { return GeneralIntrinsicClass; }

    std::vector<NameAndType> LuauArchitecture::GetIntrinsicInputs(uint32_t intrinsic) {
        // Numeric intrinsics operate on Luau numbers, which are IEEE doubles.
        // Declaring that here lets Binary Ninja render constants as floats and
        // propagate the double result instead of treating raw bit patterns as
        // integers.
        switch (static_cast<Intrinsic>(intrinsic)) {
        case Intrinsic::IntegerDivide:
        case Intrinsic::Modulo:
        case Intrinsic::Power:
            return {NameAndType(Type::FloatType(8)), NameAndType(Type::FloatType(8))};
        case Intrinsic::CreateThread:
            return {NameAndType(Type::PointerType(8, Type::VoidType()))};
        default:
            return {};
        }
    }

    std::vector<Confidence<Ref<Type>>> LuauArchitecture::GetIntrinsicOutputs(uint32_t intrinsic) {
        switch (static_cast<Intrinsic>(intrinsic)) {
        case Intrinsic::IntegerDivide:
        case Intrinsic::Modulo:
        case Intrinsic::Power:
            return {Type::FloatType(8)};
        case Intrinsic::Pcall:
        case Intrinsic::Xpcall:
        case Intrinsic::Resume:
            return {Type::BoolType(), Type::PointerType(8, Type::VoidType())};
        case Intrinsic::CreateThread:
            return {Type::PointerType(8, Type::VoidType())};
        default:
            return {};
        }
    }

    bool LuauArchitecture::GetInstructionInfo(const uint8_t *data, uint64_t addr, size_t maxLen, InstructionInfo &result) {
        static std::atomic<int> s_infoFailures{0};
        const auto fail = [&](const char *reason) {
            if (s_infoFailures.fetch_add(1) < 8)
                LogWarn("Fission.BinaryNinja: GetInstructionInfo(0x%llx): %s", static_cast<unsigned long long>(addr), reason);
            return false;
        };

        if (!data || maxLen < kInstructionSize)
            return fail("missing data or maxLen < 4");

        const auto image = ImageRegistry::Find(addr);
        if (!image)
            return fail("no image for address");
        const ProtoView *proto = image->ProtoAtAddress(addr);
        if (!proto)
            return fail("address outside any proto");

        const uint32_t raw = ReadRawInstruction(data);
        const auto opcode = static_cast<LuauOpcode>(raw & 0xFF);
        if (opcode >= LOP__COUNT)
            return fail("opcode >= LOP__COUNT");

        const size_t length = kInstructionSize * static_cast<size_t>(Luau::getOpLength(opcode));
        if (length > maxLen)
            return fail("instruction longer than maxLen");
        result.length = length;

        const auto index = static_cast<uint32_t>((addr - proto->address) / kInstructionSize);
        const int rawTarget = Luau::getJumpTarget(raw, index);
        const bool hasTarget = rawTarget >= 0 && static_cast<size_t>(rawTarget) < proto->instructionCount;
        const uint64_t target = hasTarget ? proto->address + static_cast<uint64_t>(rawTarget) * kInstructionSize : 0;
        const uint64_t fallthrough = addr + length;

        // JUMPXEQK* stores its comparison polarity in the AUX word's high bit:
        // set means "jump when the comparison is false".
        bool jumpOnFalse = false;
        switch (opcode) {
        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB:
        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            if (length >= 2 * kInstructionSize && maxLen >= 2 * kInstructionSize) {
                uint32_t aux = 0;
                std::memcpy(&aux, data + kInstructionSize, sizeof(aux));
                jumpOnFalse = (aux & 0x80000000u) != 0;
            }
            break;
        default:
            break;
        }

        // A "jump" whose target is the next instruction is a no-op; Fission lifts
        // those as NOP, so mirror that shape in the CFG instead of terminating the
        // block with a jump the IL lifter will not emit.
        const bool jumpsToNext = hasTarget && rawTarget == static_cast<int>(index + 1);
        const auto addUnconditionalTarget = [&]() {
            if (hasTarget && !jumpsToNext)
                result.AddBranch(UnconditionalBranch, target);
        };

        switch (opcode) {
        case LOP_JUMP:
        case LOP_JUMPBACK:
        case LOP_JUMPX:
        case LOP_FORGPREP:
        case LOP_FORGPREP_INEXT:
        case LOP_FORGPREP_NEXT:
            addUnconditionalTarget();
            break;

        case LOP_LOADB:
            if (LUAU_INSN_C(raw) != 0)
                addUnconditionalTarget();
            break;

        case LOP_JUMPIF:
        case LOP_JUMPIFNOT:
            // A D==0 truthiness branch is a no-op and Fission lifts it to NOP.
            // Comparison jumps are deliberately kept because they still evaluate
            // their operands (and can raise) even when both targets coincide.
            if (hasTarget && !jumpsToNext) {
                result.AddBranch(TrueBranch, target);
                result.AddBranch(FalseBranch, fallthrough);
            }
            break;

        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT:
        case LOP_FORNPREP:
        case LOP_FORNLOOP:
        case LOP_FORGLOOP:
            if (hasTarget) {
                result.AddBranch(TrueBranch, target);
                result.AddBranch(FalseBranch, fallthrough);
            }
            break;

        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB:
        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            if (hasTarget) {
                result.AddBranch(TrueBranch, jumpOnFalse ? fallthrough : target);
                result.AddBranch(FalseBranch, jumpOnFalse ? target : fallthrough);
            }
            break;

        case LOP_CALL:
        case LOP_CALLFB:
            if (index < proto->directCallTargets.size() && proto->directCallTargets[index] != 0)
                result.AddBranch(CallDestination, proto->directCallTargets[index]);
            break;

        case LOP_RETURN:
            result.AddBranch(FunctionReturn);
            break;

        // Native-codegen guard, never emitted by Luau::compile. Fission elides it and
        // so do we; keeping it a plain instruction keeps the CFG consistent with the
        // NOP emitted by the IL lifter.
        case LOP_CMPPROTO:
        default:
            break;
        }

        return true;
    }

    bool LuauArchitecture::GetInstructionText(const uint8_t *data, uint64_t addr, size_t &len, std::vector<InstructionTextToken> &result) {
        if (!data)
            return false;

        const auto image = ImageRegistry::Find(addr);
        if (!image)
            return false;
        const ProtoView *proto = image->ProtoAtAddress(addr);
        if (!proto)
            return false;

        const uint32_t raw = ReadRawInstruction(data);
        const auto opcode = static_cast<LuauOpcode>(raw & 0xFF);
        if (opcode >= LOP__COUNT)
            return false;

        len = kInstructionSize * static_cast<size_t>(Luau::getOpLength(opcode));
        result.emplace_back(InstructionToken, kOpcodeNames[opcode]);
        result.emplace_back(TextToken, " ");

        bool first = true;
        const auto separator = [&]() {
            if (!first)
                result.emplace_back(OperandSeparatorToken, ", ");
            first = false;
        };
        const auto reg = [&](uint32_t value) {
            separator();
            result.emplace_back(RegisterToken, std::format("R{}", value), value);
        };
        const auto integer = [&](int64_t value) {
            separator();
            result.emplace_back(IntegerToken, FormatInteger(value), static_cast<uint64_t>(value));
        };
        const auto constant = [&](int32_t value) {
            separator();
            result.emplace_back(IntegerToken, std::format("K{}", value), static_cast<uint64_t>(value));
        };
        const auto targetAddress = [&](uint64_t value) {
            separator();
            result.emplace_back(CodeRelativeAddressToken, std::format("0x{:x}", value), value);
        };
        const auto text = [&](const std::string &value) {
            separator();
            result.emplace_back(TextToken, value);
        };

        const uint8_t A = LUAU_INSN_A(raw);
        const uint8_t B = LUAU_INSN_B(raw);
        const uint8_t C = LUAU_INSN_C(raw);
        const int16_t D = LUAU_INSN_D(raw);
        const uint32_t aux = Luau::getOpLength(opcode) > 1 ? ReadRawInstruction(data + 4) : 0;

        const auto jumpTarget = [&]() {
            const auto index = static_cast<uint32_t>((addr - proto->address) / kInstructionSize);
            const int target = Luau::getJumpTarget(raw, index);
            if (target >= 0 && static_cast<size_t>(target) < proto->instructionCount)
                targetAddress(proto->address + static_cast<uint64_t>(target) * kInstructionSize);
            else
                integer(D);
        };

        const auto describeConstant = [&](int32_t index) {
            result.emplace_back(CommentToken, std::format(" ; {}", image->DescribeConstant(*proto->proto, index)));
        };
        const auto describeImport = [&]() {
            const auto path = image->ImportPath(*proto->proto, aux);
            if (path.empty())
                return;
            std::string joined;
            for (size_t i = 0; i < path.size(); ++i) {
                if (i)
                    joined += ".";
                joined += path[i];
            }
            result.emplace_back(CommentToken, std::format(" ; {}", joined));
        };

        switch (opcode) {
        case LOP_NOP:
        case LOP_BREAK:
        case LOP_NATIVECALL:
        case LOP_COVERAGE:
            break;

        case LOP_LOADNIL:
        case LOP_CLOSEUPVALS:
        case LOP_PREPVARARGS:
            reg(A);
            break;

        case LOP_LOADB:
            reg(A);
            integer(B);
            integer(C);
            break;

        case LOP_LOADN:
            reg(A);
            integer(D);
            break;

        case LOP_LOADK:
            reg(A);
            constant(D);
            describeConstant(D);
            break;

        case LOP_LOADKX:
            reg(A);
            constant(static_cast<int32_t>(aux));
            describeConstant(static_cast<int32_t>(aux));
            break;

        case LOP_MOVE:
            reg(A);
            reg(B);
            break;

        case LOP_GETGLOBAL:
        case LOP_SETGLOBAL:
            reg(A);
            constant(static_cast<int32_t>(aux));
            describeConstant(static_cast<int32_t>(aux));
            break;

        case LOP_GETUPVAL:
        case LOP_SETUPVAL:
            reg(A);
            integer(B);
            break;

        case LOP_GETIMPORT:
            reg(A);
            constant(D);
            describeImport();
            break;

        case LOP_GETTABLE:
        case LOP_SETTABLE:
            reg(A);
            reg(B);
            reg(C);
            break;

        case LOP_GETTABLEKS:
        case LOP_SETTABLEKS:
        case LOP_GETUDATAKS:
        case LOP_SETUDATAKS:
            reg(A);
            reg(B);
            constant(static_cast<int32_t>(aux & 0xFFFF));
            describeConstant(static_cast<int32_t>(aux & 0xFFFF));
            break;

        case LOP_GETTABLEN:
        case LOP_SETTABLEN:
            reg(A);
            reg(B);
            integer(C + 1);
            break;

        case LOP_NEWCLOSURE:
            reg(A);
            integer(D);
            result.emplace_back(CommentToken, std::format(" ; proto_{}", D));
            break;

        case LOP_DUPCLOSURE:
            reg(A);
            constant(D);
            describeConstant(D);
            break;

        case LOP_NAMECALL:
        case LOP_NAMECALLUDATA:
            reg(A);
            reg(B);
            constant(static_cast<int32_t>(aux & 0xFFFF));
            describeConstant(static_cast<int32_t>(aux & 0xFFFF));
            break;

        case LOP_CALL:
        case LOP_CALLFB:
            reg(A);
            integer(B);
            integer(C);
            break;

        case LOP_RETURN:
            reg(A);
            integer(B);
            break;

        case LOP_JUMP:
        case LOP_JUMPBACK:
        case LOP_JUMPX:
            jumpTarget();
            break;

        case LOP_JUMPIF:
        case LOP_JUMPIFNOT:
            reg(A);
            jumpTarget();
            break;

        case LOP_JUMPIFEQ:
        case LOP_JUMPIFLE:
        case LOP_JUMPIFLT:
        case LOP_JUMPIFNOTEQ:
        case LOP_JUMPIFNOTLE:
        case LOP_JUMPIFNOTLT:
            reg(A);
            jumpTarget();
            reg(LUAU_INSN_AUX_A(aux));
            break;

        case LOP_ADD:
        case LOP_SUB:
        case LOP_MUL:
        case LOP_DIV:
        case LOP_MOD:
        case LOP_POW:
        case LOP_AND:
        case LOP_OR:
        case LOP_IDIV:
            reg(A);
            reg(B);
            reg(C);
            break;

        case LOP_ADDK:
        case LOP_SUBK:
        case LOP_MULK:
        case LOP_DIVK:
        case LOP_MODK:
        case LOP_POWK:
        case LOP_ANDK:
        case LOP_ORK:
        case LOP_IDIVK:
            reg(A);
            reg(B);
            constant(C);
            describeConstant(C);
            break;

        case LOP_SUBRK:
        case LOP_DIVRK:
            reg(A);
            constant(B);
            describeConstant(B);
            reg(C);
            break;

        case LOP_CONCAT:
            reg(A);
            reg(B);
            reg(C);
            break;

        case LOP_NOT:
        case LOP_MINUS:
        case LOP_LENGTH:
            reg(A);
            reg(B);
            break;

        case LOP_NEWTABLE:
            reg(A);
            integer(B);
            integer(aux);
            break;

        case LOP_DUPTABLE:
            reg(A);
            constant(D);
            describeConstant(D);
            break;

        case LOP_SETLIST:
            reg(A);
            reg(B);
            integer(C);
            integer(aux);
            break;

        case LOP_FORNPREP:
        case LOP_FORNLOOP:
        case LOP_FORGLOOP:
        case LOP_FORGPREP:
        case LOP_FORGPREP_INEXT:
        case LOP_FORGPREP_NEXT:
            reg(A);
            jumpTarget();
            break;

        case LOP_FASTCALL:
            integer(A);
            integer(C);
            break;

        case LOP_FASTCALL1:
            integer(A);
            reg(B);
            integer(C);
            break;

        case LOP_FASTCALL2:
            integer(A);
            reg(B);
            integer(C);
            reg(LUAU_INSN_AUX_A(aux));
            break;

        case LOP_FASTCALL2K:
            integer(A);
            reg(B);
            integer(C);
            constant(static_cast<int32_t>(aux));
            break;

        case LOP_FASTCALL3:
            integer(A);
            reg(B);
            integer(C);
            reg(LUAU_INSN_AUX_A(aux));
            reg(LUAU_INSN_AUX_B(aux));
            break;

        case LOP_FASTPCALL:
            integer(A);
            integer(B);
            integer(C);
            break;

        case LOP_GETVARARGS:
            reg(A);
            integer(B);
            break;

        case LOP_CAPTURE:
            integer(A);
            reg(B);
            break;

        case LOP_JUMPXEQKNIL:
        case LOP_JUMPXEQKB:
            reg(A);
            jumpTarget();
            integer(LUAU_INSN_AUX_KB(aux) != 0 ? 1 : 0);
            break;

        case LOP_JUMPXEQKN:
        case LOP_JUMPXEQKS:
            reg(A);
            jumpTarget();
            constant(static_cast<int32_t>(LUAU_INSN_AUX_KV(aux)));
            describeConstant(static_cast<int32_t>(LUAU_INSN_AUX_KV(aux)));
            break;

        case LOP_NEWCLASSMEMBER:
            reg(A);
            reg(C);
            constant(static_cast<int32_t>(aux));
            describeConstant(static_cast<int32_t>(aux));
            break;

        case LOP_NEWCLASS:
            reg(A);
            if (B == 0xFF)
                text("nil");
            else
                reg(B);
            integer(C & 1);
            constant(static_cast<int32_t>(aux));
            describeConstant(static_cast<int32_t>(aux));
            break;

        case LOP_CMPPROTO:
            reg(A);
            jumpTarget();
            break;

        default:
            break;
        }

        // Carry the BytecodeLifter's remark through the disassembly. It contains the
        // resolved global/constant/import names and call arity computed by the
        // deserializer, which is the most useful annotation for a VM bytecode view.
        if (proto->lifted) {
            const uint64_t index = (addr - proto->address) / kInstructionSize;
            if (index < proto->lifted->instructions.size()) {
                const auto &remarks = proto->lifted->instructions[index].instructionRemarks;
                if (remarks && !remarks->empty()) {
                    std::string text = *remarks;
                    for (char &character : text) {
                        if (character == '\n' || character == '\r' || character == '\t')
                            character = ' ';
                    }
                    if (text.size() > 160) {
                        text.resize(157);
                        text += "...";
                    }
                    result.emplace_back(CommentToken, " ; " + text);
                }
            }
        }

        return true;
    }

    bool LuauArchitecture::GetInstructionLowLevelIL(const uint8_t *data, uint64_t addr, size_t &len, LowLevelILFunction &il) {
        static std::atomic<int> s_ilFailures{0};
        const auto fail = [&](const char *reason) {
            if (s_ilFailures.fetch_add(1) < 8)
                LogWarn("Fission.BinaryNinja: GetInstructionLowLevelIL(0x%llx): %s", static_cast<unsigned long long>(addr), reason);
            return false;
        };

        if (!data)
            return fail("missing data");

        const auto image = ImageRegistry::Find(addr);
        if (!image)
            return fail("no image for address");
        const ProtoView *proto = image->ProtoAtAddress(addr);
        if (!proto)
            return fail("address outside any proto");

        const uint32_t raw = ReadRawInstruction(data);
        const auto opcode = static_cast<LuauOpcode>(raw & 0xFF);
        if (opcode >= LOP__COUNT)
            return fail("opcode >= LOP__COUNT");

        len = kInstructionSize * static_cast<size_t>(Luau::getOpLength(opcode));
        const uint64_t index = (addr - proto->address) / kInstructionSize;
        if (!proto->lifted || index >= proto->lifted->instructions.size()) {
            il.AddInstruction(il.Unimplemented());
            return true;
        }

        Lifter lifter(il, this, *image, *proto, addr, raw);
        lifter.Run(proto->lifted->instructions[index]);
        return true;
    }

    namespace {

        // Luau's VM registers are frame-local: a callee does not clobber the caller's
        // registers except where the compiler explicitly places results. Marking them
        // callee-saved gives BN useful liveness information for the intrinsic calls.
        class LuauCallingConvention : public CallingConvention {
          public:
            explicit LuauCallingConvention(Architecture *arch) : CallingConvention(arch, "luau") {}

            std::vector<uint32_t> GetCallerSavedRegisters() override {
                std::vector<uint32_t> registers;
                registers.reserve(kArgumentCount + kResultCount + 3);
                for (uint32_t i = 0; i < kArgumentCount; ++i)
                    registers.push_back(ArgumentRegister(i));
                for (uint32_t i = 0; i < kResultCount; ++i)
                    registers.push_back(ResultRegister(i));
                registers.push_back(kPcallOkRegister);
                registers.push_back(kPcallResultRegister);
                registers.push_back(kThreadRegister);
                return registers;
            }
            std::vector<uint32_t> GetCalleeSavedRegisters() override {
                std::vector<uint32_t> registers(kRegisterCount);
                for (uint32_t i = 0; i < kRegisterCount; ++i)
                    registers[i] = i;
                return registers;
            }
            std::vector<uint32_t> GetIntegerArgumentRegisters() override {
                std::vector<uint32_t> registers(kArgumentCount);
                for (uint32_t i = 0; i < kArgumentCount; ++i)
                    registers[i] = ArgumentRegister(i);
                return registers;
            }
            uint32_t GetIntegerReturnValueRegister() override { return ResultRegister(0); }
            uint32_t GetHighIntegerReturnValueRegister() override { return ResultRegister(1); }
        };

    } // namespace

    void RegisterArchitecture() {
        auto *architecture = new LuauArchitecture();
        Architecture::Register(architecture);
        Ref<CallingConvention> convention = new LuauCallingConvention(architecture);
        architecture->RegisterCallingConvention(convention);
        architecture->SetDefaultCallingConvention(convention);
        if (Ref<Platform> platform = architecture->GetStandalonePlatform())
            platform->RegisterDefaultCallingConvention(convention);
    }

} // namespace fission::bn
