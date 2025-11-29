//
// Created by Dottik on 5/10/2025.
//

#include "Deserializer.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Compiler.h"
#include <cstdio>
#include <iostream>
#pragma clang diagnostic pop

int main() {
    Luau::CompileOptions compileOpts {0, 2};
    auto bytecode = Luau::compile(
        R"(
print("hi")
)",
        compileOpts
    );

    Deserializer deserializer { };
    const auto deserializationResultOptional = deserializer.Deserialize(bytecode);

    // ASSERT(deserializationResultOptional.has_value(), "deserialization failed.");
    auto deserializationResult = deserializationResultOptional.value();

    // Decompiler::Lifter::BytecodeLifter lifter { };
    // auto ast = lifter.Lift(deserializationResult);
    //
    // Decompiler::SourceCodeGenerator generator { };
    // generator.VisitNodeTree(ast);

    std::cout << "Output: \r\n" << deserializationResult.stringTable[0] << std::endl;
    return 0;
}