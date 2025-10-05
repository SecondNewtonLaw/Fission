//
// Created by Dottik on 5/10/2025.
//

#pragma once
#include <string_view>
#include <string>


namespace Fission {
    class BytecodeDecoder {
    public:
        virtual std::string DecodeBytecode(std::string_view bytecode) = 0;

        virtual bool CanDecode(std::string_view bytecode) = 0;
    };
}
