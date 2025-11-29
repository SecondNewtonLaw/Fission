//
// Created by Dottik on 28/5/2025.
//


#pragma once
#include <string>

class BinaryReader {
    std::string m_backingBuffer;
    const std::uint8_t *m_lpBufferStart;
    const std::uint8_t *m_lpBufferEnd;
    const std::uint8_t *m_lpCurrentBufferPointer;

public:
    explicit BinaryReader(const std::string &buffer) {
        this->m_backingBuffer = buffer;
        this->m_lpBufferStart = reinterpret_cast<const std::uint8_t *>(this->m_backingBuffer.data());
        this->m_lpBufferEnd =
            reinterpret_cast<const std::uint8_t *>(this->m_backingBuffer.data()) + this->m_backingBuffer.size();
        this->m_lpCurrentBufferPointer = this->m_lpBufferStart;
    }

    unsigned int ReadVariableInteger() {
        unsigned int result = 0;
        unsigned int shift = 0;

        uint8_t byte { };

        do {
            byte = this->Read<uint8_t>();
            result |= (byte & 127) << shift;
            shift += 7;
        } while (byte & 128);

        return result;
    }

    template <typename T>
    T Read(const bool advance = true) {
        const auto advanceBy = sizeof(T);
        // ASSERT(
        //     this->m_lpCurrentBufferPointer + advanceBy <= this->m_lpBufferEnd,
        //     "structure is too big for the given buffer, cannot read!"
        // );
        T tmp { };
        memcpy(&tmp, this->m_lpCurrentBufferPointer, advanceBy);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += advanceBy;

        return tmp;
    }

    std::string ReadString(std::size_t stringLength, bool advance = true) {
        // ASSERT(
        //     this->m_lpCurrentBufferPointer + stringLength <= this->m_lpBufferEnd,
        //     "string is too long for the given buffer, cannot read!"
        // );
        std::string tmp(stringLength, '\0');

        memcpy(tmp.data(), this->m_lpCurrentBufferPointer, stringLength);

        if (advance) [[likely]]
            this->m_lpCurrentBufferPointer += stringLength;

        return tmp;
    }

    void AdvanceBy(std::size_t offset) {
        // ASSERT(
        //     this->m_lpCurrentBufferPointer + offset <= this->m_lpBufferEnd,
        //     "advancing by that amount would lead us to exit the buffer bounds."
        // );
        this->m_lpCurrentBufferPointer += offset;
    }

    const std::uint8_t *GetCurrentReaderPosition() const { return this->m_lpCurrentBufferPointer; }

    const std::uint8_t *GetEndPosition() const { return this->m_lpBufferEnd; }
};