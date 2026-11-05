//
// Parsed Luau bytecode image shared by the Binary Ninja view and architecture.
//

#pragma once

#include "BytecodeLifter.hpp"
#include "Deserializer.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fission::bn {

    // Serializes plugin-side Fission operations. libassert's failure handler is
    // process-global, so overlapping validation, parsing and decompilation could
    // restore the wrong handler and turn a recoverable assertion into an abort.
    std::mutex &FissionOperationMutex();

    // One Luau proto, placed at a synthetic Binary Ninja address. Every address is
    // imageBase + the file offset of the proto's instruction array, so the raw bytes
    // Binary Ninja hands back to the architecture are exactly the Luau instruction
    // words.
    struct ProtoView {
        const DeserializedFunction *proto = nullptr;
        const LiftedFunction *lifted = nullptr;
        std::string name;
        uint64_t fileOffset = 0;
        uint64_t address = 0;
        size_t instructionCount = 0;
        // Per-instruction synthetic addresses. 0 means "not applicable".
        std::vector<uint64_t> tableAddresses;    // NEWTABLE / DUPTABLE allocations
        std::vector<uint64_t> captureSlots;      // CAPTURE target upvalue slots
        std::vector<uint64_t> directCallTargets; // resolved closure call targets

        [[nodiscard]] uint64_t EndAddress() const { return address + instructionCount * sizeof(uint32_t); }
        [[nodiscard]] bool Contains(uint64_t candidate) const { return candidate >= address && candidate < EndAddress(); }
    };

    // True when the lifted instruction definitely overwrites `reg`.
    [[nodiscard]] bool InstructionWritesRegister(const LiftedInstruction &instruction, uint8_t reg);

    struct GlobalSlotInfo {
        std::string name;
        uint64_t address = 0;
    };

    struct TableSiteInfo {
        uint64_t address = 0;
        uint32_t protoId = 0;
        uint32_t instructionIndex = 0;
    };

    class Image {
      public:
        enum class UpvalueKind : uint8_t { Unknown = 0, Number = 1, Other = 2 };
        enum class FieldKind : uint8_t { Unknown = 0, Table, Number, Integer, Boolean, String, Function };

        // Parses, lifts and indexes a complete Luau bytecode file. Returns nullptr when
        // the input is not valid Luau bytecode or the lifter rejects it. The image is
        // automatically registered with ImageRegistry at a unique base address.
        static std::shared_ptr<Image> Parse(std::string bytes);

        [[nodiscard]] const std::string &Bytes() const { return m_bytes; }
        [[nodiscard]] const DeserializedBytecode &Bytecode() const { return m_bytecode; }
        [[nodiscard]] uint64_t BaseAddress() const { return m_baseAddress; }
        [[nodiscard]] uint64_t FileSize() const { return m_bytes.size(); }
        [[nodiscard]] uint64_t AddressSpaceSize() const { return m_addressSpaceSize; }
        [[nodiscard]] const std::vector<ProtoView> &Protos() const { return m_protos; }
        [[nodiscard]] const std::vector<GlobalSlotInfo> &GlobalSlots() const { return m_globalSlots; }
        [[nodiscard]] const std::vector<TableSiteInfo> &TableSites() const { return m_tableSites; }

        // Synthetic data regions used to give globals, upvalues and tables real
        // addresses. Binary Ninja maps them as zero-filled readable data.
        [[nodiscard]] uint64_t CodeBase() const { return m_codeBase; }
        [[nodiscard]] uint64_t CodeEnd() const { return m_codeEnd; }
        [[nodiscard]] uint64_t SyntheticBase() const { return m_syntheticBase; }
        [[nodiscard]] uint64_t SyntheticEnd() const { return m_syntheticEnd; }
        [[nodiscard]] uint64_t FunctionBase() const { return m_functionBase; }
        [[nodiscard]] uint64_t FunctionSlot(uint32_t protoId) const {
            return protoId < m_bytecode.functions.size() ? m_functionBase + static_cast<uint64_t>(protoId) * 8 : 0;
        }
        [[nodiscard]] uint64_t GlobalBase() const { return m_globalBase; }
        [[nodiscard]] uint64_t UpvalueBase(uint32_t protoId) const { return protoId < m_upvalueOffsets.size() ? m_upvalueBase + m_upvalueOffsets[protoId] : 0; }
        [[nodiscard]] uint64_t UpvalueSize() const { return m_upvalueSize; }
        [[nodiscard]] uint64_t TableBase() const { return m_tableBase; }
        [[nodiscard]] uint64_t GlobalSlot(std::string_view name) const;
        [[nodiscard]] uint64_t UpvalueSlot(uint32_t protoId, uint8_t index) const;
        [[nodiscard]] bool IsNumericUpvalue(uint32_t protoId, uint8_t index) const;
        [[nodiscard]] bool IsMethodField(std::string_view name) const;
        [[nodiscard]] FieldKind FieldValueKind(std::string_view name) const;
        [[nodiscard]] FieldKind FieldResultKind(std::string_view name) const;
        [[nodiscard]] int32_t FieldOffset(std::string_view name) const;
        [[nodiscard]] const std::unordered_map<std::string, int32_t> &FieldOffsets() const { return m_fieldOffsets; }

        static constexpr uint64_t kTableStride = 0x1000;
        static constexpr uint64_t kUpvalueSlotSize = 8;
        static constexpr uint32_t kFieldBase = 0;

        // Main proto is the file's entry function (`bytecode.lpMainFunction`).
        [[nodiscard]] const ProtoView *MainProto() const { return m_mainProto; }

        // Resolves an address inside a proto's instruction array. The address may point
        // at an AUX word; callers that need an instruction start should round down.
        [[nodiscard]] const ProtoView *ProtoAtAddress(uint64_t address) const;
        [[nodiscard]] const ProtoView *ProtoByIndex(uint32_t protoId) const;

        // File-offset address of an interned string payload, if it exists in this image.
        [[nodiscard]] std::optional<uint64_t> StringAddress(std::string_view value) const;
        [[nodiscard]] std::optional<uint64_t> ConstantStringAddress(const DeserializedFunction &proto, int32_t constantIndex) const;
        [[nodiscard]] std::optional<std::string> ConstantString(const DeserializedFunction &proto, int32_t constantIndex) const;

        // `GETIMPORT` stores a chain of up to three string constants; returns the dotted path.
        [[nodiscard]] std::vector<std::string> ImportPath(const DeserializedFunction &proto, uint32_t aux) const;

        [[nodiscard]] std::string DescribeConstant(const DeserializedFunction &proto, int32_t constantIndex) const;

      private:
        Image() = default;

        std::string m_bytes;
        DeserializedBytecode m_bytecode;
        Fission::InstructionDecoder m_decoder;
        std::unique_ptr<LiftedFunction> m_lifted;
        std::vector<ProtoView> m_protos;
        std::vector<const ProtoView *> m_protoById;
        std::unordered_map<std::string, uint64_t> m_stringAddresses;
        std::vector<GlobalSlotInfo> m_globalSlots;
        std::unordered_map<std::string, uint64_t> m_globalSlotByName;
        std::vector<TableSiteInfo> m_tableSites;
        std::unordered_map<std::string, int32_t> m_fieldOffsets;
        std::unordered_set<std::string> m_methodFields;
        std::unordered_map<std::string, FieldKind> m_fieldValueKinds;
        std::unordered_map<std::string, FieldKind> m_fieldResultKinds;
        const ProtoView *m_mainProto = nullptr;
        uint64_t m_baseAddress = 0;
        uint64_t m_codeBase = 0;
        uint64_t m_codeEnd = 0;
        uint64_t m_syntheticBase = 0;
        uint64_t m_functionBase = 0;
        uint64_t m_globalBase = 0;
        uint64_t m_upvalueBase = 0;
        uint64_t m_upvalueSize = 0;
        std::vector<uint64_t> m_upvalueOffsets;
        std::vector<std::vector<UpvalueKind>> m_upvalueKinds;
        uint64_t m_tableBase = 0;
        uint64_t m_syntheticEnd = 0;
        uint64_t m_addressSpaceSize = 0;
    };

    // A single global image table keyed by non-overlapping address ranges. Binary Ninja
    // asks the architecture for IL by address only, so the parsed images must be
    // discoverable without a view pointer.
    class ImageRegistry {
      public:
        // Registers the image. Images are kept alive for the lifetime of the process
        // because Binary Ninja may keep views open for a long time.
        static void Add(const std::shared_ptr<Image> &image);

        [[nodiscard]] static std::shared_ptr<Image> Find(uint64_t address);
        [[nodiscard]] static std::shared_ptr<Image> FindExactBase(uint64_t base);

        // Deterministic address assignment, called by Image::Parse before parsing so
        // every emitted pointer is final.
        [[nodiscard]] static uint64_t AllocateBase(uint64_t size);

        // Prevents colliding with the small synthetic bases used by tests.
        static constexpr uint64_t kInitialBase = 0x4C55415500000000ull;

      private:
        static uint64_t s_nextBase;
    };

} // namespace fission::bn
