//
// Created by Dottik on 28/5/2025.
//

#pragma once
#include <cstring>
#include <string>

// Reads untrusted bytecode without throwing or advancing after a bounds failure.
class BinaryReader {
    std::string m_backingBuffer;
    const std::uint8_t *m_lpBufferStart;
    const std::uint8_t *m_lpBufferEnd;
    const std::uint8_t *m_lpCurrentBufferPointer;
    bool m_bOutOfBounds = false; // sticky: set the moment any read would have crossed the end.

    /// Maximum continuation bytes for a valid varint: 9 for uint64, 4 for uint32.
    static constexpr unsigned int kMaxVarintBytes64 = 10;
    static constexpr unsigned int kMaxVarintBytes32 = 5;

  public:
    explicit BinaryReader(uint8_t *bufferStart, size_t size) {
        this->m_lpBufferStart = reinterpret_cast<const std::uint8_t *>(bufferStart);
        this->m_lpBufferEnd = reinterpret_cast<const std::uint8_t *>(bufferStart) + size;
        this->m_lpCurrentBufferPointer = this->m_lpBufferStart;
    }

    explicit BinaryReader(const std::string &buffer) {
        this->m_backingBuffer = buffer;
        this->m_lpBufferStart = reinterpret_cast<const std::uint8_t *>(this->m_backingBuffer.data());
        this->m_lpBufferEnd = reinterpret_cast<const std::uint8_t *>(this->m_backingBuffer.data()) + this->m_backingBuffer.size();
        this->m_lpCurrentBufferPointer = this->m_lpBufferStart;
    }

    // Cached pointers refer into m_backingBuffer and cannot survive copies or moves.
    BinaryReader(const BinaryReader &) = delete;
    BinaryReader &operator=(const BinaryReader &) = delete;
    BinaryReader(BinaryReader &&) = delete;
    BinaryReader &operator=(BinaryReader &&) = delete;

    // Subtraction avoids overflow from adding an untrusted length to a pointer.
    [[nodiscard]] std::size_t Remaining() const { return static_cast<std::size_t>(this->m_lpBufferEnd - this->m_lpCurrentBufferPointer); }
    [[nodiscard]] bool CanRead(std::size_t count) const { return count <= this->Remaining(); }

    // Any failed read remains visible to the deserializer.
    [[nodiscard]] bool HasFailed() const { return this->m_bOutOfBounds; }

    uint64_t ReadVariableInteger64() {
        uint64_t result = 0;
        unsigned int shift = 0;

        for (unsigned int i = 0; i < kMaxVarintBytes64; ++i) {
            uint8_t byte = this->Read<uint8_t>();
            if (this->m_bOutOfBounds)
                return result;
            result |= ((uint64_t)(byte & 127)) << shift;
            if (!(byte & 128))
                return result;
            shift += 7;
        }

        // Reject overlong varints.
        this->m_bOutOfBounds = true;
        return result;
    }

    unsigned int ReadVariableInteger32() {
        unsigned int result = 0;
        unsigned int shift = 0;

        for (unsigned int i = 0; i < kMaxVarintBytes32; ++i) {
            uint8_t byte = this->Read<uint8_t>();
            if (this->m_bOutOfBounds)
                return result;
            result |= (byte & 127) << shift;
            if (!(byte & 128))
                return result;
            shift += 7;
        }

        // Reject overlong varints.
        this->m_bOutOfBounds = true;
        return result;
    }

    template <typename T> T Read(const bool advance = true) {
        const auto advanceBy = sizeof(T);
        if (!this->CanRead(advanceBy)) [[unlikely]] {
            this->m_bOutOfBounds = true;
            return T{};
        }
        T tmp{};
        memcpy(&tmp, this->m_lpCurrentBufferPointer, advanceBy);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += advanceBy;

        return tmp;
    }

    std::string ReadString(std::size_t stringLength, bool advance = true) {
        if (!this->CanRead(stringLength)) [[unlikely]] {
            this->m_bOutOfBounds = true;
            return std::string{};
        }
        std::string tmp(stringLength, '\0');

        memcpy(tmp.data(), this->m_lpCurrentBufferPointer, stringLength);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += stringLength;

        return tmp;
    }

    void AdvanceBy(std::size_t offset) {
        if (!this->CanRead(offset)) [[unlikely]] {
            this->m_bOutOfBounds = true;
            return;
        }
        this->m_lpCurrentBufferPointer += offset;
    }

    const std::uint8_t *GetCurrentReaderPosition() const { return this->m_lpCurrentBufferPointer; }
    std::uint8_t *GetCurrentReaderPositionMut() { return const_cast<uint8_t *>(this->m_lpCurrentBufferPointer); }

    const std::uint8_t *GetEndPosition() const { return this->m_lpBufferEnd; }
};
