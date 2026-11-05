//
// Standalone verifier for the bytecode image layer (no Binary Ninja required).
//

#include "BytecodeLifter.hpp"
#include "Decompiler.hpp"
#include "Image.hpp"

#include "Luau/Bytecode.h"
#include "Luau/BytecodeUtils.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <variant>

namespace {

    std::string ReadFile(const std::string &path, bool &ok) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) {
            ok = false;
            return {};
        }
        std::ostringstream buffer;
        buffer << stream.rdbuf();
        ok = true;
        return buffer.str();
    }

    uint32_t ReadWord(const std::string &bytes, uint64_t offset) {
        uint32_t word = 0;
        if (offset + sizeof(word) <= bytes.size())
            std::memcpy(&word, bytes.data() + offset, sizeof(word));
        return word;
    }

} // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <file.luac>\n";
        return 1;
    }

    bool readOk = false;
    const std::string bytes = ReadFile(argv[1], readOk);
    if (!readOk) {
        std::cerr << "error: could not read '" << argv[1] << "'\n";
        return 1;
    }

    auto image = fission::bn::Image::Parse(bytes);
    if (!image) {
        std::cerr << "error: image parse/lift failed\n";
        return 1;
    }

    std::cout << std::format(
        "image: base=0x{:x} size={} bytecode=v{} types=v{} strings={} protos={}\n", image->BaseAddress(), image->FileSize(),
        static_cast<int>(image->Bytecode().bytecodeVersion), static_cast<int>(image->Bytecode().typesVersion), image->Bytecode().stringTable.size(),
        image->Protos().size()
    );

    if (argc >= 4 && std::string(argv[2]) == "--proto") {
        const uint32_t id = static_cast<uint32_t>(std::stoul(argv[3]));
        if (id >= image->Bytecode().functions.size()) {
            std::cerr << "error: no proto " << id << "\n";
            return 1;
        }
        const auto &proto = image->Bytecode().functions[id];
        std::cout << std::format("proto {} constants={} nups={} params={}\n", id, proto.constants.size(), proto.nups, proto.numparams);
        for (uint8_t index = 0; index < proto.nups; ++index)
            std::cout << std::format("  upval_{} numeric={}\n", index, image->IsNumericUpvalue(id, index));
        for (size_t i = 0; i < proto.constants.size(); ++i) {
            const auto &constant = proto.constants[i];
            std::cout << std::format("  [{}] type={} {}", i, static_cast<int>(constant.kType), image->DescribeConstant(proto, static_cast<int32_t>(i)));
            if (constant.kType == LUA_TNUMBER && std::holds_alternative<double>(constant.constantData))
                std::cout << std::format(" (0x{:016x})", std::bit_cast<uint64_t>(std::get<double>(constant.constantData)));
            std::cout << "\n";
        }
        if (const fission::bn::ProtoView *view = image->ProtoByIndex(id)) {
            if (view->lifted) {
                for (const auto &instruction : view->lifted->instructions) {
                    std::cout << std::format("  {:4} {}\n", instruction.instructionIndex, OperationToString(instruction.operation));
                    for (const auto &operand : instruction.operands) {
                        switch (operand.type) {
                        case LiftedOperandType::Register:
                            std::cout << std::format("        reg r{}\n", operand.value.reg);
                            break;
                        case LiftedOperandType::ImmediateConstant:
                            std::cout << std::format("        const k{}\n", operand.value.imm.k);
                            break;
                        case LiftedOperandType::ImmediateInteger:
                            std::cout << std::format("        imm {}\n", operand.value.imm.n);
                            break;
                        case LiftedOperandType::ImmediateBool:
                            std::cout << std::format("        bool {}\n", operand.value.imm.b);
                            break;
                        case LiftedOperandType::ImmediateNil:
                            std::cout << "        nil\n";
                            break;
                        case LiftedOperandType::ImmediateAux:
                            std::cout << std::format("        aux 0x{:x}\n", operand.value.imm.u);
                            break;
                        }
                    }
                }
            }
        }
        return 0;
    }

    if (argc >= 4 && std::string(argv[2]) == "--pseudo-function") {
        const uint32_t id = static_cast<uint32_t>(std::stoul(argv[3]));
        Decompiler decompiler;
        const auto result = decompiler.DecompileVanillaFunction(bytes, id);
        std::cout << result.decompilationOutput;
        if (result.resultCode != DecompileResult::Success) {
            std::cerr << "\nerror: " << result.errorMessage << "\n";
            return 1;
        }
        return 0;
    }

    if (argc >= 3 && std::string(argv[2]) == "--pseudo") {
        Decompiler decompiler;
        const auto result = decompiler.DecompileVanillaBytecode(bytes);
        std::cout << result.decompilationOutput;
        if (result.resultCode != DecompileResult::Success) {
            std::cerr << "\nerror: " << result.errorMessage << "\n";
            return 1;
        }
        return 0;
    }

    if (argc >= 3 && std::string(argv[2]) == "--fields") {
        const auto kindName = [](fission::bn::Image::FieldKind kind) {
            switch (kind) {
            case fission::bn::Image::FieldKind::Table:
                return "table";
            case fission::bn::Image::FieldKind::Number:
                return "number";
            case fission::bn::Image::FieldKind::Integer:
                return "integer";
            case fission::bn::Image::FieldKind::Boolean:
                return "boolean";
            case fission::bn::Image::FieldKind::String:
                return "string";
            case fission::bn::Image::FieldKind::Function:
                return "function";
            default:
                return "unknown";
            }
        };
        std::vector<std::pair<int32_t, std::string>> fields;
        for (const auto &[name, offset] : image->FieldOffsets())
            fields.emplace_back(offset, name);
        std::sort(fields.begin(), fields.end());
        for (const auto &[offset, name] : fields)
            std::cout << std::format(
                "  +0x{:03x} {:20s} method={} value={} result={}\n", offset, name, image->IsMethodField(name), kindName(image->FieldValueKind(name)),
                kindName(image->FieldResultKind(name))
            );
        return 0;
    }

    size_t totalUpvalues = 0;
    size_t numericUpvalues = 0;
    for (const auto &proto : image->Bytecode().functions) {
        totalUpvalues += proto.nups;
        for (uint8_t index = 0; index < proto.nups; ++index) {
            if (image->IsNumericUpvalue(proto.bytecodeId, index))
                ++numericUpvalues;
        }
        const uint64_t base = image->UpvalueBase(proto.bytecodeId);
        if (proto.nups == 0)
            continue;
        if (base == 0 || base % fission::bn::Image::kUpvalueSlotSize != 0 ||
            base + proto.nups * fission::bn::Image::kUpvalueSlotSize > image->UpvalueBase(0) + image->UpvalueSize()) {
            std::cerr << std::format("problem: proto {} upvalue storage out of range\n", proto.bytecodeId);
            return 1;
        }
        for (uint8_t index = 0; index < proto.nups; ++index) {
            if (image->UpvalueSlot(proto.bytecodeId, index) != base + index * fission::bn::Image::kUpvalueSlotSize) {
                std::cerr << std::format("problem: proto {} upvalue {} misaddressed\n", proto.bytecodeId, index);
                return 1;
            }
        }
    }
    std::cout << std::format(
        "upvalues: base=0x{:x} size={} slots={} numeric={} bytes-per-slot={}\n", image->UpvalueBase(0), image->UpvalueSize(), totalUpvalues, numericUpvalues,
        fission::bn::Image::kUpvalueSlotSize
    );

    size_t problems = 0;
    size_t totalInstructions = 0;
    size_t totalLifted = 0;
    std::map<std::string, size_t> histogram;

    for (const auto &proto : image->Protos()) {
        const auto &deserialized = *proto.proto;
        if (proto.fileOffset + proto.instructionCount * sizeof(uint32_t) > image->FileSize()) {
            std::cerr << std::format("problem: proto {} instruction range exceeds file\n", deserialized.bytecodeId);
            ++problems;
            continue;
        }
        if (!proto.lifted) {
            std::cerr << std::format("problem: proto {} has no lifted IR\n", deserialized.bytecodeId);
            ++problems;
            continue;
        }
        if (proto.lifted->instructions.size() != deserialized.instructions.size()) {
            std::cerr << std::format(
                "problem: proto {} lifted {} instructions, raw {}\n", deserialized.bytecodeId, proto.lifted->instructions.size(),
                deserialized.instructions.size()
            );
            ++problems;
        }

        totalInstructions += deserialized.instructions.size();
        totalLifted += proto.lifted->instructions.size();

        size_t nextInstruction = 0;
        for (size_t i = 0; i < proto.lifted->instructions.size(); ++i) {
            const auto &instruction = proto.lifted->instructions[i];
            histogram[std::string(OperationToString(instruction.operation))]++;

            if (instruction.instructionIndex != static_cast<int32_t>(i)) {
                std::cerr << std::format("problem: proto {} instruction {} reports index {}\n", deserialized.bytecodeId, i, instruction.instructionIndex);
                ++problems;
            }

            if (i >= deserialized.instructions.size())
                continue;

            // AUX words are not instructions; skip them using the preceding
            // instruction's length, exactly as Binary Ninja's analysis does.
            if (i < nextInstruction)
                continue;

            const uint32_t raw = ReadWord(bytes, proto.fileOffset + i * sizeof(uint32_t));
            const auto opcode = static_cast<LuauOpcode>(raw & 0xFF);
            if (opcode >= LOP__COUNT) {
                std::cerr << std::format("problem: proto {} slot {} has invalid opcode {}\n", deserialized.bytecodeId, i, raw & 0xFF);
                ++problems;
                continue;
            }

            nextInstruction = i + static_cast<size_t>(Luau::getOpLength(opcode));

            const int target = Luau::getJumpTarget(raw, static_cast<uint32_t>(i));
            if (target >= 0 && static_cast<size_t>(target) > deserialized.instructions.size()) {
                std::cerr << std::format(
                    "problem: proto {} slot {} (opcode {}) target {} outside {} instructions\n", deserialized.bytecodeId, i, raw & 0xFF, target,
                    deserialized.instructions.size()
                );
                ++problems;
            }

            if (!proto.Contains(proto.address + i * sizeof(uint32_t))) {
                std::cerr << std::format("problem: proto {} slot {} outside address range\n", deserialized.bytecodeId, i);
                ++problems;
            }
        }
    }

    if (const auto *mainProto = image->MainProto()) {
        std::cout << std::format("main: {} at 0x{:x}\n", mainProto->name, mainProto->address);
    } else {
        std::cerr << "problem: no main proto\n";
        ++problems;
    }

    std::cout << std::format("instructions: raw={} lifted={} distinct-ops={}\n", totalInstructions, totalLifted, histogram.size());
    for (const auto &[name, count] : histogram) {
        if (count >= 16)
            std::cout << std::format("  {:20s} {}\n", name, count);
    }

    if (problems != 0) {
        std::cerr << std::format("FAILED: {} problem(s)\n", problems);
        return 1;
    }
    std::cout << "OK\n";
    return 0;
}
