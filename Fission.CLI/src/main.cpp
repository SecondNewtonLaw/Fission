//
// Created by Dottik on 5/10/2025.
//

#include "Deserializer.hpp"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "IRLifter.hpp"
#include "Luau/Compiler.h"
#include <cstdio>
#include <iostream>
#pragma clang diagnostic pop

int main() {
    Luau::CompileOptions compileOpts {0, 2};
    auto bytecode = Luau::compile(
        R"(
print("hi")
print = "i hate niggers"
)",
        compileOpts
    );

    Deserializer deserializer { };
    const auto deserializationResultOptional = deserializer.Deserialize(bytecode);

    // ASSERT(deserializationResultOptional.has_value(), "deserialization failed.");
    auto& deserializationResult = deserializationResultOptional.value();
    const auto liftedIR = LiftDeserializedBytecode(deserializationResult);

    return 0;
}