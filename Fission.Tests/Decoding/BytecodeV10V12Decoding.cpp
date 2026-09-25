// Pin constant and proto framing for Luau bytecode versions 9 through 14 using hand-built fixtures.

#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

    void EnableLuauFFlagsOnce() {
        static bool enabled = false;
        if (enabled)
            return;
        enabled = true;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strncmp(flag->name, "Luau", 4) == 0)
                flag->value = true;
    }

    Luau::BytecodeBuilder::StringRef sref(const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; }

    void AppendVarint(std::string &out, std::uint32_t value) {
        do {
            auto byte = static_cast<std::uint8_t>(value & 0x7f);
            value >>= 7;
            if (value)
                byte |= 0x80;
            out.push_back(static_cast<char>(byte));
        } while (value);
    }

    void AppendU32(std::string &out, std::uint32_t value) {
        for (int i = 0; i < 4; ++i)
            out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
    }

    std::string MinimalBytecode(std::uint8_t version) {
        std::string proto;
        proto.append({char(1), char(0), char(0), char(0), char(0), char(0)}); // stack, params, upvalues, vararg, flags, empty type info
        AppendVarint(proto, 1);                                               // sizecode
        AppendU32(proto, static_cast<std::uint32_t>(LOP_RETURN) | (1u << 16));
        AppendVarint(proto, 0);   // constants
        AppendVarint(proto, 0);   // child protos
        AppendVarint(proto, 0);   // line defined
        AppendVarint(proto, 0);   // debug name
        proto.push_back(char(0)); // line info
        proto.push_back(char(0)); // debug info
        if (version >= 11)
            AppendVarint(proto, 0); // feedback entries

        std::string bytecode;
        bytecode.push_back(static_cast<char>(version));
        bytecode.push_back(char(3)); // type version
        AppendVarint(bytecode, 0);   // strings
        bytecode.push_back(char(0)); // no userdata mappings
        AppendVarint(bytecode, 1);   // one proto
        if (version >= 12)
            AppendVarint(bytecode, static_cast<std::uint32_t>(proto.size()));
        bytecode += proto;
        AppendVarint(bytecode, 0); // main proto id
        return bytecode;
    }

    std::string TypedUserdataBytecode() {
        std::string typeinfo;
        AppendVarint(typeinfo, 3);
        AppendVarint(typeinfo, 0);
        AppendVarint(typeinfo, 0);
        typeinfo.append({char(LBC_TYPE_FUNCTION), char(1), char(LBC_TYPE_TAGGED_USERDATA_BASE)});

        std::string proto;
        proto.append({char(1), char(1), char(0), char(0), char(0)});
        AppendVarint(proto, static_cast<std::uint32_t>(typeinfo.size()));
        proto += typeinfo;
        AppendVarint(proto, 1);
        AppendU32(proto, static_cast<std::uint32_t>(LOP_RETURN) | (2u << 16));
        AppendVarint(proto, 0);
        AppendVarint(proto, 0);
        AppendVarint(proto, 0);
        AppendVarint(proto, 0);
        proto.push_back(char(0));
        proto.push_back(char(0));
        AppendVarint(proto, 0);

        std::string bytecode;
        bytecode.append({char(12), char(3), char(1), char(7)});
        bytecode += "Vector3";
        bytecode.append({char(1), char(1), char(0), char(1)});
        AppendVarint(bytecode, static_cast<std::uint32_t>(proto.size()));
        bytecode += proto;
        AppendVarint(bytecode, 0);
        return bytecode;
    }

} // namespace

TEST_CASE("Deser: bytecode versions v9 through v14 parse minimal protos", "[BytecodeDecoder][Versions]") {
    Deserializer deserializer{};

    for (std::uint8_t version = 9; version <= 14; ++version) {
        INFO("bytecode version: " << static_cast<int>(version));
        auto dOpt = deserializer.Deserialize(MinimalBytecode(version));
        REQUIRE(dOpt.has_value());
        REQUIRE(dOpt->bytecodeVersion == version);
        REQUIRE(dOpt->functions.size() == 1);
        REQUIRE(dOpt->lpMainFunction == &dOpt->functions[0]);
        REQUIRE(dOpt->lpMainFunction->instructions.size() == 1);
        CHECK(dOpt->lpMainFunction->instructions[0].GetOpCode() == LOP_RETURN);
    }
}

TEST_CASE("Deser: tagged userdata arguments recover mapped type names", "[BytecodeDecoder][Types]") {
    Deserializer deserializer{};
    auto bytecode = deserializer.Deserialize(TypedUserdataBytecode());
    REQUIRE(bytecode.has_value());
    REQUIRE(bytecode->lpMainFunction != nullptr);
    CHECK(bytecode->userdataMappings[0] == 1);
    CHECK(Deserializer::TryGetTypeName(bytecode->lpMainFunction, 0) == "Vector3");
}

TEST_CASE("Deser: float and double vector constants preserve width", "[BytecodeDecoder][Vectors]") {
    EnableLuauFFlagsOnce();

    Luau::BytecodeBuilder bb{};
    const auto main = bb.beginFunction(0, true);
    const auto vf = bb.addConstantVectorf(0.03f, 0.5f, -2.0f, 0.0f);
    const auto vd = bb.addConstantVectord(0.03, 0.03333333, -2.0, 0.0);
    bb.emitAD(LOP_LOADK, 0, static_cast<std::int16_t>(vf));
    bb.emitAD(LOP_LOADK, 1, static_cast<std::int16_t>(vd));
    bb.emitABC(LOP_RETURN, 0, 3, 0);
    bb.endFunction(2, 0);
    bb.setMainFunction(main);
    bb.finalize();

    Deserializer deserializer{};
    auto bytecode = deserializer.Deserialize(bb.getBytecode());
    REQUIRE(bytecode.has_value());
    REQUIRE(bytecode->lpMainFunction->constants.size() == 2);

    const auto &floatVector = std::get<LuauVectorConstant>(bytecode->lpMainFunction->constants[0].constantData);
    const auto &doubleVector = std::get<LuauVectorConstant>(bytecode->lpMainFunction->constants[1].constantData);
    REQUIRE(std::holds_alternative<LuauVectorConstant::Float>(floatVector.components));
    REQUIRE(std::holds_alternative<LuauVectorConstant::Double>(doubleVector.components));
    CHECK(std::get<LuauVectorConstant::Float>(floatVector.components)[0] == 0.03f);
    CHECK(std::get<LuauVectorConstant::Double>(doubleVector.components)[1] == 0.03333333);

    Decompiler decompiler{};
    const auto result = decompiler.DecompileVanillaBytecode(bb.getBytecode(), static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    CHECK(result.decompilationOutput.find("local __fissionVectorCtor = Vector3.new") != std::string::npos);
    CHECK(result.decompilationOutput.find("__fissionVectorCtor(0.03, 0.5, -2)") != std::string::npos);
    CHECK(result.decompilationOutput.find("__fissionVectorCtor(0.03, 0.03333333, -2)") != std::string::npos);
}

TEST_CASE("Decompile: mapped userdata names replace generic userdata annotations", "[BytecodeDecoder][Types]") {
    EnableLuauFFlagsOnce();
    static const char *userdataTypes[] = {"Vector3", nullptr};
    Luau::CompileOptions options{};
    options.debugLevel = 2;
    options.typeInfoLevel = 1;
    options.userdataTypes = userdataTypes;
    const auto compiled = Luau::compile("return function(value: Vector3) return value end", options);

    Decompiler decompiler{};
    const auto result = decompiler.DecompileVanillaBytecode(compiled, static_cast<DecompilerFlags>(0));
    REQUIRE(result.resultCode == DecompileResult::Success);
    INFO("decompile:\n" << result.decompilationOutput);
    CHECK(result.decompilationOutput.find(": Vector3") != std::string::npos);
    CHECK(result.decompilationOutput.find(": userdata") == std::string::npos);
}

// The class-shape constant (LBC_CONSTANT_CLASS_SHAPE) must deserialize to the exact class name plus the
// declared property and method names, resolved from their constant-table string references and preserved
// in declaration order.
TEST_CASE("Deser: v10 class-shape constant parses name, properties and methods exactly", "[BytecodeDecoder][ClassShape]") {
    EnableLuauFFlagsOnce();

    // strings must outlive finalize(): BytecodeBuilder keeps the refs, not copies.
    const std::string sName = "Vector", sX = "x", sY = "y", sAdd = "add", sDot = "dot";

    Luau::BytecodeBuilder bb{};
    bb.beginFunction(0, /*isvararg*/ true);
    const int32_t cName = bb.addConstantString(sref(sName));
    const int32_t cX = bb.addConstantString(sref(sX));
    const int32_t cY = bb.addConstantString(sref(sY));
    const int32_t cAdd = bb.addConstantString(sref(sAdd));
    const int32_t cDot = bb.addConstantString(sref(sDot));
    Luau::BytecodeBuilder::ClassShape shape;
    shape.className = cName;
    shape.propertyNames = {cX, cY};   // declaration order: x, y
    shape.methodNames = {cAdd, cDot}; // declaration order: add, dot
    (void)bb.addClassShape(std::move(shape));
    bb.emitABC(LOP_RETURN, 0, 1, 0); // return
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);
    bb.setMainFunction(0);
    bb.finalize();

    Deserializer deserializer{};
    auto dOpt = deserializer.Deserialize(bb.getBytecode());
    REQUIRE(dOpt.has_value());
    REQUIRE(dOpt->lpMainFunction != nullptr);

    // locate the single class-shape constant.
    const LuauClassShape *found = nullptr;
    int shapeCount = 0;
    for (const auto &c : dOpt->lpMainFunction->constants) {
        if (c.IsClassShape()) {
            ++shapeCount;
            found = &std::get<LuauClassShape>(c.constantData);
            // a class shape carries no lua runtime type.
            CHECK(c.kType == LUA_TNIL);
        }
    }
    REQUIRE(shapeCount == 1);
    REQUIRE(found != nullptr);

    CHECK(found->className == "Vector");
    CHECK(found->propertyNames == std::vector<std::string>{"x", "y"});
    CHECK(found->methodNames == std::vector<std::string>{"add", "dot"});
}

// v12+ prefixes every proto with its size in bytes and appends a cost varint to INLINABLE protos. The
// deserializer must consume the size prefix and hard-resync past the cost so the NEXT proto's header is
// read at the right offset. A cost-bearing proto placed BEFORE another proto proves the resync: if the
// cost varint were misread, the following proto's numparams/maxstacksize/instruction-count would be
// corrupted (or the parse would fail outright).
TEST_CASE("Deser: v14 per-proto size prefix and INLINABLE cost varint resync correctly", "[BytecodeDecoder][V14]") {
    EnableLuauFFlagsOnce();

    Luau::BytecodeBuilder bb{};

    // child 0: INLINABLE + a cost -> its serialized proto ends with a cost varint that must be skipped.
    const uint32_t child0 = bb.beginFunction(0, /*isvararg*/ false);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0, /*flags*/ LPF_INLINABLE, /*cost*/ 42);

    // child 1: the proto that FOLLOWS the cost-bearing one; its header must land exactly. Distinctive
    // shape (1 param, maxstack 2, two instructions) so a misaligned resync is detectable.
    const uint32_t child1 = bb.beginFunction(1, /*isvararg*/ false);
    bb.emitABC(LOP_LOADNIL, 1, 0, 0);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 2, /*numupvalues*/ 0);

    // child 2: a third sibling so the per-proto size-prefix loop runs more than twice.
    const uint32_t child2 = bb.beginFunction(0, /*isvararg*/ true);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);

    const uint32_t main = bb.beginFunction(0, /*isvararg*/ true);
    const int16_t c0 = bb.addChildFunction(child0);
    const int16_t c1 = bb.addChildFunction(child1);
    const int16_t c2 = bb.addChildFunction(child2);
    bb.emitAD(LOP_NEWCLOSURE, 0, c0);
    bb.emitAD(LOP_NEWCLOSURE, 1, c1);
    bb.emitAD(LOP_NEWCLOSURE, 2, c2);
    bb.emitABC(LOP_RETURN, 0, 1, 0);
    bb.endFunction(/*maxstacksize*/ 3, /*numupvalues*/ 0);
    bb.setMainFunction(main);
    bb.finalize();

    Deserializer deserializer{};
    auto dOpt = deserializer.Deserialize(bb.getBytecode());
    REQUIRE(dOpt.has_value());

    CHECK(dOpt->bytecodeVersion == 14);
    REQUIRE(dOpt->functions.size() == 4); // child0, child1, child2, main

    // functions[0] is the INLINABLE cost-bearing proto; functions[1] is what follows it in the stream.
    const auto &following = dOpt->functions[1];
    CHECK(following.numparams == 1);
    CHECK(following.maxstacksize == 2);
    CHECK(following.instructions.size() == 2);
}
