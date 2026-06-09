//
// Created by Dottik on 5/10/2025.
//

#include "Decompiler.hpp"
#include "libassert/assert.hpp"
#include "luacode.h"

#include <Windows.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

static void CliFailureHandler(const libassert::assertion_info &info) {
    libassert::enable_virtual_terminal_processing_if_needed();
    auto msg = info.to_string(0, libassert::color_scheme::blank);
    std::fprintf(stderr, "\n*** LIBASSERT ASSERTION FAILED ***\n%s\n", msg.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

struct InitCliHandler {
    InitCliHandler() { libassert::set_failure_handler(CliFailureHandler); }
};
static InitCliHandler g_cliHandlerInit;

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
    Decompiler decompiler{};

    std::string s = "____";
    uintptr_t size = 0;
    auto sz = luau_compile(
        "print(\"hi\", 0, vector.create(1,1,1), { a = 2 }, nil, 0.2)", sizeof("print(\"hi\", 0, vector.create(1,1,1), { a = 2 }, nil, 0.2)"), nullptr, &size
    );
    s.resize(size);
    memcpy(s.data(), sz, size);
    for (unsigned char c : s) {
        std::cout << std::format("0x{:02x}, ", c);
    }

    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
        if (strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true; // enable all fflags, because integer is experimental.

    Luau::CompileOptions tmpOpts{};
    tmpOpts.optimizationLevel = 2;
    tmpOpts.debugLevel = 0;
    auto decompileResult = decompiler.DecompileTestCodeFromFile(
        "test.txt", DecompilerFlags::PrintTimingBreakdown | DecompilerFlags::WriteIRToFile | DecompilerFlags::GenerateSSAIRGraph |
                        DecompilerFlags::GenerateIRGraph | DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes |
                        DecompilerFlags::AutoNameVariables,
        tmpOpts
    );
    ASSERT(decompileResult.resultCode == DecompileResult::Success, "Decompilation failed.");

    if (!decompileResult.timingStatistics.empty())
        std::cout << decompileResult.timingStatistics << std::endl;

    system("graph_generator.bat");
}