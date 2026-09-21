//
// Created by Dottik on 28/5/2025.
//

#include "Deserializer.hpp"
#include <cstring>
#include <sstream>

std::optional<DeserializedBytecode> Deserializer::Deserialize(const std::string &bytecode) {
    BinaryReader reader{bytecode};
    DeserializedBytecode result;
    result.bytecodeVersion = reader.Read<uint8_t>();

    if (result.bytecodeVersion == 0) {
        return std::nullopt;
    }

    if ((result.bytecodeVersion < LBC_VERSION_MIN || result.bytecodeVersion > LBC_VERSION_MAX) && result.bytecodeVersion != LBC_VERSION_CLASSES)
        return std::nullopt; // deserializer does not support this bytecode version.

    if (result.bytecodeVersion >= 4) {
        result.typesVersion = reader.Read<uint8_t>();

        if (result.typesVersion < LBC_TYPE_VERSION_MIN || result.typesVersion > LBC_TYPE_VERSION_MAX)
            return std::nullopt;
    }

    auto stringCount = reader.ReadVariableInteger32();
    // Bound attacker-controlled counts by remaining input bytes.
    if (reader.HasFailed() || stringCount > reader.Remaining())
        return std::nullopt;

    for (unsigned int i = 0; i < stringCount; i++) {
        auto stringLength = reader.ReadVariableInteger32();
        if (reader.HasFailed() || stringLength > reader.Remaining())
            return std::nullopt;

        // Preserve raw bytes; SourceGenerator owns escaping and UTF-8 emission.
        auto rS = reader.ReadString(stringLength);
        result.stringTable.emplace_back(rS);
    }

    auto userdataTypeNames = std::make_shared<LuauUserdataTypeNames>();
    if (result.typesVersion == 3) {
        std::uint8_t index = reader.Read<uint8_t>();
        while (index != 0) {
            const auto stringId = reader.ReadVariableInteger32();
            const auto name = result.ReadFromStringTable(stringId);
            if (index - 1 < USERDATA_TYPE_LIMIT && name && Deserializer::IsValidTypeName(*name)) {
                result.userdataMappings[index - 1] = stringId;
                (*userdataTypeNames)[index - 1] = *name;
            }

            if (reader.HasFailed()) // ran off the end mid-list: malformed.
                return std::nullopt;

            index = reader.Read<uint8_t>(); // next index.
        }
    }

    auto protoCount = reader.ReadVariableInteger32();
    if (reader.HasFailed() || protoCount > reader.Remaining())
        return std::nullopt;
    result.functions.resize(protoCount);

    for (auto i = 0llu; i < protoCount; i++) {
        DeserializedFunction function{};
        function.uTypeVersion = result.typesVersion;
        function.uBytecodeVersion = result.bytecodeVersion;
        function.bytecodeId = int(i);
        function.userdataTypeNames = userdataTypeNames;

        // V12 proto sizes include trailing cost and future extension data; resynchronize at the recorded end.
        const std::uint8_t *protoStart = nullptr;
        std::uint32_t protoSize = 0;
        if (result.bytecodeVersion >= 12) {
            protoSize = reader.ReadVariableInteger32();
            if (reader.HasFailed() || protoSize > reader.Remaining())
                return std::nullopt;
            protoStart = reader.GetCurrentReaderPosition();
        }

        function.maxstacksize = reader.Read<uint8_t>();
        function.numparams = reader.Read<uint8_t>();
        function.nups = reader.Read<uint8_t>();
        function.isvararg = reader.Read<uint8_t>();

        // SSA register arrays cannot represent parameters beyond maxstacksize.
        if (reader.HasFailed() || function.numparams > function.maxstacksize)
            return std::nullopt;

        if (result.bytecodeVersion >= 4u) {
            function.flags = reader.Read<uint8_t>();

            if (result.typesVersion == 1) {
                auto typeSize = reader.ReadVariableInteger32();

                if (typeSize) {
                    // memcpy needs an explicit bound before AdvanceBy can set the reader failure state.
                    if (reader.HasFailed() || typeSize > reader.Remaining())
                        return std::nullopt;
                    const uint8_t *types = reader.GetCurrentReaderPosition();
                    int headerSize = typeSize > 127 ? 4 : 3;

                    function.typeinfo.resize(headerSize + typeSize);

                    if (headerSize == 4) {
                        function.typeinfo[0] = (typeSize & 127) | (1 << 7);
                        function.typeinfo[1] = typeSize >> 7;
                        function.typeinfo[2] = 0;
                        function.typeinfo[3] = 0;
                    } else {
                        function.typeinfo[0] = static_cast<uint8_t>(typeSize);
                        function.typeinfo[1] = 0;
                        function.typeinfo[2] = 0;
                    }

                    memcpy(function.typeinfo.data() + headerSize, types, typeSize);
                    reader.AdvanceBy(headerSize + typeSize);
                } else {
                    reader.AdvanceBy(typeSize);
                }
            } else if (result.typesVersion == 2 || result.typesVersion == 3) {
                uint32_t typesize = reader.ReadVariableInteger32();

                if (typesize) {
                    if (reader.HasFailed() || typesize > reader.Remaining())
                        return std::nullopt;
                    const uint8_t *types = reader.GetCurrentReaderPosition();

                    function.typeinfo.resize(typesize);

                    memcpy(function.typeinfo.data(), types, typesize);
                    reader.AdvanceBy(typesize);
                }
            }
        }

        const auto sizecode = reader.ReadVariableInteger32();
        // Every valid proto contains at least RETURN; bound instruction count before allocation.
        if (reader.HasFailed() || sizecode == 0 || sizecode > reader.Remaining())
            return std::nullopt;
        function.instructions.resize(sizecode);

        for (auto j = 0llu; j < sizecode; j++)
            function.instructions[j] = reader.Read<uint32_t>();
        if (reader.HasFailed())
            return std::nullopt;

        const auto sizek = reader.ReadVariableInteger32();
        if (reader.HasFailed() || sizek > reader.Remaining())
            return std::nullopt;
        function.constants.resize(sizek);

        for (auto j = 0llu; j < sizek; j++) {
            function.constants[j] = LuauConstant{};
            auto lbcConstant = reader.Read<uint8_t>();
            switch (lbcConstant) {
            case LBC_CONSTANT_NIL:
                // Constants are preinitialized to nil.
                break;

            case LBC_CONSTANT_BOOLEAN: {
                uint8_t v = reader.Read<uint8_t>();
                function.constants[j].kType = LUA_TBOOLEAN;
                function.constants[j].constantData = static_cast<bool>(v);
                break;
            }

            case LBC_CONSTANT_NUMBER: {
                double v = reader.Read<double>();
                function.constants[j].kType = LUA_TNUMBER;
                function.constants[j].constantData = v;
                break;
            }

            case LBC_CONSTANT_VECTOR: {
                auto x = reader.Read<float>();
                auto y = reader.Read<float>();
                auto z = reader.Read<float>();
                auto w = reader.Read<float>();
                function.constants[j].kType = LUA_TVECTOR;
                function.constants[j].constantData = LuauVectorConstant{x, y, z, w};
                break;
            }

            case LBC_CONSTANT_VECTORD: {
                auto x = reader.Read<double>();
                auto y = reader.Read<double>();
                auto z = reader.Read<double>();
                auto w = reader.Read<double>();
                function.constants[j].kType = LUA_TVECTOR;
                function.constants[j].constantData = LuauVectorConstant{x, y, z, w};
                break;
            }

            case LBC_CONSTANT_STRING: {
                function.constants[j].kType = LUA_TSTRING;
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger32());
                // Preserve an empty value for malformed string references.
                function.constants[j].constantData = str.value_or(std::string{});

                break;
            }

            case LBC_CONSTANT_IMPORT: {
                function.constants[j].kType = LUA_TNIL;
                /*uint32_t iid =*/
                (void)reader.Read<uint32_t>();
                break;
            }

            case LBC_CONSTANT_TABLE: {
                uint32_t keyCount = reader.ReadVariableInteger32();
                if (reader.HasFailed() || keyCount > reader.Remaining())
                    return std::nullopt;
                std::vector<std::string> vec;
                vec.reserve(keyCount);
                for (uint32_t k = 0; k < keyCount; ++k) {
                    uint32_t key = reader.ReadVariableInteger32();
                    // Table keys must reference earlier string constants.
                    if (reader.HasFailed() || key >= function.constants.size())
                        return std::nullopt;
                    const auto &constant = function.constants[key];
                    if (!std::holds_alternative<std::string>(constant.constantData))
                        return std::nullopt;
                    vec.emplace_back(std::get<std::string>(constant.constantData));
                }
                function.constants[j].kType = LUA_TTABLE;
                function.constants[j].constantData = LuauTable{vec};
                break;
            }

            case LBC_CONSTANT_TABLE_WITH_CONSTANTS: {
                // bytecode v7 ewww
                uint32_t keyCount = reader.ReadVariableInteger32();
                if (reader.HasFailed() || keyCount > reader.Remaining())
                    return std::nullopt;
                std::vector<std::string> vec;
                vec.reserve(keyCount);
                std::vector<int32_t> tableValues;
                tableValues.reserve(keyCount);
                for (uint32_t k = 0; k < keyCount; ++k) {
                    uint32_t key = reader.ReadVariableInteger32();
                    if (reader.HasFailed() || key >= function.constants.size())
                        return std::nullopt;
                    const auto &constant = function.constants[key];
                    auto valIdx = reader.Read<int32_t>();
                    if (!std::holds_alternative<std::string>(constant.constantData))
                        return std::nullopt;
                    vec.emplace_back(std::get<std::string>(constant.constantData));
                    tableValues.emplace_back(valIdx);
                }
                function.constants[j].kType = LUA_TTABLE;
                function.constants[j].constantData = LuauTable{vec, tableValues};
                break;
            }

            case LBC_CONSTANT_CLOSURE: {
                function.constants[j].kType = LUA_TFUNCTION;
                uint32_t fid = reader.ReadVariableInteger32();
                // Later stages dereference closure function pointers.
                if (reader.HasFailed() || fid >= result.functions.size())
                    return std::nullopt;
                function.constants[j].constantData = result.functions.data() + fid;
                break;
            }

            case LBC_CONSTANT_INTEGER: {
                bool isNegative = reader.Read<uint8_t>();
                uint64_t magnitude = reader.ReadVariableInteger64();
                function.constants[j].kType = LUA_TINTEGER;
                function.constants[j].constantData = isNegative ? static_cast<int64_t>(~magnitude + 1) : static_cast<int64_t>(magnitude);
                break;
            }

            case LBC_CONSTANT_CLASS_SHAPE: {
                // V10 stores class name, property names, and method names as earlier string constants.
                LuauClassShape shape{};
                const auto classNameId = reader.ReadVariableInteger32();
                const auto propCount = reader.ReadVariableInteger32();
                const auto methodCount = reader.ReadVariableInteger32();
                if (reader.HasFailed() || propCount > reader.Remaining() || methodCount > reader.Remaining())
                    return std::nullopt;

                // Invalid name references remain empty.
                const auto resolveName = [&](uint32_t cid) -> std::string {
                    if (cid >= function.constants.size())
                        return {};
                    const auto &c = function.constants[cid];
                    if (!std::holds_alternative<std::string>(c.constantData))
                        return {};
                    return std::get<std::string>(c.constantData);
                };

                shape.className = resolveName(classNameId);
                shape.propertyNames.reserve(propCount);
                for (auto p = 0u; p < propCount; ++p)
                    shape.propertyNames.emplace_back(resolveName(reader.ReadVariableInteger32()));
                shape.methodNames.reserve(methodCount);
                for (auto m = 0u; m < methodCount; ++m)
                    shape.methodNames.emplace_back(resolveName(reader.ReadVariableInteger32()));
                if (reader.HasFailed())
                    return std::nullopt;

                function.constants[j].kType = LUA_TNIL; // no lua runtime type; identified via LuauConstant::IsClassShape().
                function.constants[j].constantData = std::move(shape);
                break;
            }

            default:
                break;
            }
        }

        if (reader.HasFailed()) // a constant read ran off the end
            return std::nullopt;

        auto sizep = reader.ReadVariableInteger32();
        if (reader.HasFailed() || sizep > reader.Remaining())
            return std::nullopt;
        function.subfunctions.resize(sizep);
        for (auto j = 0llu; j < sizep; j++) {
            auto fid = reader.ReadVariableInteger32();
            // Later stages dereference subfunction pointers.
            if (reader.HasFailed() || fid >= result.functions.size())
                return std::nullopt;
            function.subfunctions[j] = result.functions.data() + fid;
        }

        function.lineDefined = reader.ReadVariableInteger32();
        function.debugName = result.ReadFromStringTable(reader.ReadVariableInteger32());
        auto lineinfo = reader.Read<uint8_t>();

        if (lineinfo) {
            function.linegaplog2 = reader.Read<uint8_t>();

            // Guard empty bodies and shifts wider than size_t.
            if (reader.HasFailed() || function.instructions.empty() || function.linegaplog2 >= 32)
                return std::nullopt;

            const size_t instrCount = function.instructions.size();
            const size_t intervals = ((instrCount - 1) >> function.linegaplog2) + 1;
            const size_t absoffset = (instrCount + 3) & ~static_cast<size_t>(3);
            const size_t sizelineinfo = absoffset + intervals * sizeof(int);
            function.lineinfo = {};
            function.lineinfo.resize(sizelineinfo);

            function.abslineinfoOffset = absoffset;
            // A pointer stored before copying function into result.functions would dangle.
            auto *abslineinfo = reinterpret_cast<int *>(function.lineinfo.data() + absoffset);

            uint8_t lastoffset = 0;
            for (size_t j = 0; j < instrCount; ++j) {
                lastoffset += reader.Read<uint8_t>();
                function.lineinfo[j] = lastoffset;
            }

            auto lastline = 0;
            for (size_t j = 0; j < intervals; ++j) {
                lastline += reader.Read<int32_t>();
                abslineinfo[j] = lastline;
            }
            if (reader.HasFailed())
                return std::nullopt;
        }

        uint8_t debuginfo = reader.Read<uint8_t>();

        if (debuginfo) {
            const uint32_t sizelocvars = reader.ReadVariableInteger32();
            if (reader.HasFailed() || sizelocvars > reader.Remaining())
                return std::nullopt;
            function.locvars = {};
            function.locvars.resize(sizelocvars);

            for (uint32_t j = 0; j < sizelocvars; ++j) {
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger32());
                if (str.has_value())
                    function.locvars[j].varname = str.value();
                else
                    function.locvars[j].varname = "";

                function.locvars[j].startpc = reader.ReadVariableInteger32();
                function.locvars[j].endpc = reader.ReadVariableInteger32();
                function.locvars[j].reg = reader.Read<uint8_t>();
            }

            const uint32_t sizeupvalues = reader.ReadVariableInteger32();
            if (reader.HasFailed() || sizeupvalues > reader.Remaining())
                return std::nullopt;

            function.upvalueNames = {};
            function.upvalueNames.resize(sizeupvalues);

            for (uint32_t j = 0; j < sizeupvalues; ++j) {
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger32());
                function.upvalueNames[j] = str.has_value() ? str.value() : "";
            }
        }

        if (result.bytecodeVersion >= 11) {
            auto fbSize = reader.ReadVariableInteger32();
            if (reader.HasFailed() || fbSize > reader.Remaining())
                return std::nullopt;
            for (uint32_t j = 0; j < fbSize; ++j) {
                (void)reader.Read<uint8_t>();
                (void)reader.ReadVariableInteger32();
            }
        }

        if (result.bytecodeVersion >= 12) {
            // Match lvmload by skipping per-proto cost and unknown trailing bytes.
            const std::uint8_t *protoEnd = protoStart + protoSize;
            const std::uint8_t *cur = reader.GetCurrentReaderPosition();
            if (cur > protoEnd) // we over-read past the declared proto extent: malformed.
                return std::nullopt;
            reader.AdvanceBy(static_cast<std::size_t>(protoEnd - cur));
        }

        if (reader.HasFailed()) // ran off the end while parsing this function
            return std::nullopt;

        result.functions[i] = function;
    }

    auto mainfid = reader.ReadVariableInteger32();
    // bIsMain assignment dereferences the main function pointer.
    if (reader.HasFailed() || mainfid >= result.functions.size())
        return std::nullopt;

    result.lpMainFunction = result.functions.data() + mainfid;
    result.lpMainFunction->bIsMain = true;

    return result;
}
