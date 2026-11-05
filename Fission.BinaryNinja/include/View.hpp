//
// Binary Ninja BinaryView that maps a Luau bytecode file into a synthetic
// address space so the Luau architecture can lift its instruction arrays.
//

#pragma once

#include <binaryninjaapi.h>

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fission::bn {

    class Image;

    class LuauBinaryView : public BinaryNinja::BinaryView {
      public:
        explicit LuauBinaryView(BinaryNinja::BinaryView *data);
        ~LuauBinaryView() override;

        bool Init() override;

        bool PerformIsExecutable() const override { return true; }
        bool PerformIsRelocatable() const override { return false; }
        BNEndianness PerformGetDefaultEndianness() const override { return LittleEndian; }
        size_t PerformGetAddressSize() const override { return 8; }
        uint64_t PerformGetEntryPoint() const override;

        [[nodiscard]] std::shared_ptr<Image> GetImage() const { return m_image; }
        [[nodiscard]] std::string DecompilePseudoLuau() const;

      private:
        BinaryNinja::Ref<BinaryNinja::BinaryView> m_raw;
        BinaryNinja::Ref<BinaryNinja::Architecture> m_architecture;
        std::shared_ptr<Image> m_image;
        mutable std::mutex m_pseudoLuauMutex;
        mutable std::string m_pseudoLuau;
        mutable bool m_pseudoLuauReady = false;
    };

    class LuauBinaryViewType : public BinaryNinja::BinaryViewType {
      public:
        LuauBinaryViewType();

        BinaryNinja::Ref<BinaryNinja::BinaryView> Create(BinaryNinja::BinaryView *data) override;
        bool IsTypeValidForData(BinaryNinja::BinaryView *data) override;
    };

    void RegisterView();

} // namespace fission::bn
