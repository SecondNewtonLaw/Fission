//
// Created by Dottik on 5/10/2025.
//

#include "Decompiler.hpp"
#include "libassert/assert.hpp"
#include <Windows.h>
#include <filesystem>
#include <fstream>

#pragma comment(lib, "crypt32.lib")

std::string DecodeBase64FileToBinary(const std::wstring &filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
        throw std::runtime_error("Failed to open Base64 file");

    std::stringstream ss;
    ss << file.rdbuf();
    std::string rawContent = ss.str();

    std::string base64Content;
    base64Content.reserve(rawContent.size());

    for (unsigned char c : rawContent) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/' || c == '=') {
            base64Content.push_back(c);
        }
    }

    if (base64Content.empty())
        throw std::runtime_error("Base64 content is empty after sanitization");

    while (base64Content.size() % 4 != 0) {
        base64Content.push_back('=');
    }

    DWORD decodedLength = 0;
    if (!CryptStringToBinaryA(
            base64Content.c_str(), static_cast<DWORD>(base64Content.size()), CRYPT_STRING_BASE64, nullptr, &decodedLength, nullptr, nullptr
        )) {
        throw std::runtime_error("CryptStringToBinaryA (Size) failed with error: " + std::to_string(GetLastError()));
    }

    std::vector<BYTE> decodedData(decodedLength);
    if (!CryptStringToBinaryA(
            base64Content.c_str(), static_cast<DWORD>(base64Content.size()), CRYPT_STRING_BASE64, decodedData.data(), &decodedLength, nullptr, nullptr
        )) {
        throw std::runtime_error("CryptStringToBinaryA (Decode) failed with error: " + std::to_string(GetLastError()));
    }

    decodedData.resize(decodedLength);
    return std::string(reinterpret_cast<const char *>(decodedData.data()), decodedData.size());
}

int main() {
    auto data = DecodeBase64FileToBinary(L"bytecode_encoded.txt");
    Decompiler decompiler{};
    ASSERT(
        decompiler.DecompileRobloxBytecode(
            data,
            DecompilerFlags::PrintTimingBreakdown | DecompilerFlags::WriteIRToFile | DecompilerFlags::GenerateSSAIRGraph | DecompilerFlags::GenerateIRGraph
        ) == DecompileResult::Success,
        "Decompilation failed."
    );
    return 0;
}