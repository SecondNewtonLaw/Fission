//
// Binary Ninja architecture that lifts Luau bytecode to BNIL.
//

#pragma once

#include <binaryninjaapi.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fission::bn {

    class LuauArchitecture : public BinaryNinja::Architecture {
      public:
        LuauArchitecture();

        // Returns a stable virtual return register for a semantic call result,
        // e.g. `workspace` for GetService("Workspace"). The pool is finite and
        // shared by every view because Binary Ninja addresses registers by
        // architecture-global index.
        uint32_t NamedResultRegister(const std::string &name);

        BNEndianness GetEndianness() const override;
        size_t GetAddressSize() const override;
        size_t GetDefaultIntegerSize() const override;
        size_t GetInstructionAlignment() const override;
        size_t GetLinearSweepInitialAlignment() const override;
        size_t GetMaxInstructionLength() const override;
        size_t GetOpcodeDisplayLength() const override;

        bool GetInstructionInfo(const uint8_t *data, uint64_t addr, size_t maxLen, BinaryNinja::InstructionInfo &result) override;
        bool GetInstructionText(const uint8_t *data, uint64_t addr, size_t &len, std::vector<BinaryNinja::InstructionTextToken> &result) override;
        bool GetInstructionLowLevelIL(const uint8_t *data, uint64_t addr, size_t &len, BinaryNinja::LowLevelILFunction &il) override;

        std::string GetRegisterName(uint32_t reg) override;
        BNRegisterInfo GetRegisterInfo(uint32_t reg) override;
        uint32_t GetStackPointerRegister() override;
        std::vector<uint32_t> GetAllRegisters() override;
        std::vector<uint32_t> GetFullWidthRegisters() override;
        std::vector<uint32_t> GetGlobalRegisters() override;

        std::string GetIntrinsicName(uint32_t intrinsic) override;
        std::vector<uint32_t> GetAllIntrinsics() override;
        BNIntrinsicClass GetIntrinsicClass(uint32_t intrinsic) override;
        std::vector<BinaryNinja::NameAndType> GetIntrinsicInputs(uint32_t intrinsic) override;
        std::vector<BinaryNinja::Confidence<BinaryNinja::Ref<BinaryNinja::Type>>> GetIntrinsicOutputs(uint32_t intrinsic) override;

        static constexpr uint32_t kMaxNamedResults = 256;

      private:
        std::mutex m_namedResultMutex;
        std::vector<std::string> m_namedResultNames;
    };

    void RegisterArchitecture();

} // namespace fission::bn
