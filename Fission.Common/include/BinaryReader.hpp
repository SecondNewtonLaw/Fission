//
// Created by Dottik on 28/5/2025.
//

#pragma once
#include <libassert/assert.hpp>
#include <cstring>
#include <string>

class BinaryReader {
    std::string m_backingBuffer;
    const std::uint8_t *m_lpBufferStart;
    const std::uint8_t *m_lpBufferEnd;
    const std::uint8_t *m_lpCurrentBufferPointer;

    /// Max continuation bytes for a valid varint: 9 for uint64, 4 for uint32.
    static constexpr unsigned int kMaxVarintBytes64 = 10;
    static constexpr unsigned int kMaxVarintBytes32 = 5;

    void AssertBoundsOrFail(std::size_t advanceBy) const {
        ASSERT(this->m_lpCurrentBufferPointer + advanceBy <= this->m_lpBufferEnd, "Bounds check out: reading past end of buffer");
    }

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

    uint64_t ReadVariableInteger64() {
        uint64_t result = 0;
        unsigned int shift = 0;

        uint8_t byte;

        for (unsigned int i = 0; i < kMaxVarintBytes64; ++i) {
            byte = this->Read<uint8_t>();
            result |= ((uint64_t)(byte & 127)) << shift;
            if (!(byte & 128))
                return result;
            shift += 7;
        }

        ASSERT(false, "ReadVariableInteger64: malformed varint (exceeded max continuation bytes)");
        return result;
    }

    unsigned int ReadVariableInteger32() {
        unsigned int result = 0;
        unsigned int shift = 0;

        uint8_t byte{};

        for (unsigned int i = 0; i < kMaxVarintBytes32; ++i) {
            byte = this->Read<uint8_t>();
            result |= (byte & 127) << shift;
            if (!(byte & 128))
                return result;
            shift += 7;
        }

        ASSERT(false, "ReadVariableInteger32: malformed varint (exceeded max continuation bytes)");
        return result;
    }

    template <typename T> T Read(const bool advance = true) {
        const auto advanceBy = sizeof(T);
        this->AssertBoundsOrFail(advanceBy);
        T tmp{};
        memcpy(&tmp, this->m_lpCurrentBufferPointer, advanceBy);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += advanceBy;

        return tmp;
    }

    std::string ReadString(std::size_t stringLength, bool advance = true) {
        this->AssertBoundsOrFail(stringLength);
        std::string tmp(stringLength, '\0');

        memcpy(tmp.data(), this->m_lpCurrentBufferPointer, stringLength);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += stringLength;

        return tmp;
    }

    void AdvanceBy(std::size_t offset) {
        this->AssertBoundsOrFail(offset);
        this->m_lpCurrentBufferPointer += offset;
    }

    const std::uint8_t *GetCurrentReaderPosition() const { return this->m_lpCurrentBufferPointer; }
    std::uint8_t *GetCurrentReaderPositionMut() { return const_cast<uint8_t *>(this->m_lpCurrentBufferPointer); }

    const std::uint8_t *GetEndPosition() const { return this->m_lpBufferEnd; }
};
