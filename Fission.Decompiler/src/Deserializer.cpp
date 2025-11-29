//
// Created by Dottik on 28/5/2025.
//

#include "Deserializer.hpp"

std::optional<DeserializedBytecode> Deserializer::Deserialize(const std::string &bytecode) {
    BinaryReader reader {bytecode};
    DeserializedBytecode result;
    result.bytecodeVersion = reader.Read<uint8_t>();

    if (result.bytecodeVersion == 0) {
        // Compiler error;
        return std::nullopt;
    }

    if (result.bytecodeVersion < LBC_VERSION_MIN || result.bytecodeVersion > LBC_VERSION_MAX)
        return std::nullopt; // deserializer does not support this bytecode version.

    if (result.bytecodeVersion >= 4) {
        result.typesVersion = reader.Read<uint8_t>();

        if (result.typesVersion < LBC_TYPE_VERSION_MIN || result.typesVersion > LBC_TYPE_VERSION_MAX)
            return std::nullopt;
    }

    auto stringCount = reader.ReadVariableInteger();

    for (unsigned int i = 0; i < stringCount; i++) {
        auto stringLength = reader.ReadVariableInteger();
        result.stringTable.emplace_back(reader.ReadString(stringLength));
    }

    if (result.typesVersion == 3) {
        // This is runtime information, we do not need this when decompiling (very likely)
        std::uint8_t index = reader.Read<uint8_t>();
        while (index != 0) {
            auto str = result.ReadFromStringTable(reader.ReadVariableInteger());
            // ASSERT(str.has_value(), "malformed bytecode");

            if (index - 1 < USERDATA_TYPE_LIMIT) {
                // TODO: check if this is an error, and raise accordingly.
            }

            index = reader.Read<uint8_t>(); // next index.
        }
    }

    auto protoCount = reader.ReadVariableInteger();
    result.functions.resize(protoCount);

    for (auto i = 0llu; i < protoCount; i++) {
        DeserializedFunction function { };
        function.bytecodeId = int(i);
        function.maxstacksize = reader.Read<uint8_t>();
        function.numparams = reader.Read<uint8_t>();
        function.nups = reader.Read<uint8_t>();
        function.isvararg = reader.Read<uint8_t>();

        if (result.bytecodeVersion >= 4u) {
            // Parse type information.
            function.flags = reader.Read<uint8_t>();

            if (result.typesVersion == 1) {
                auto typeSize = reader.ReadVariableInteger();

                if (typeSize) {
                    const uint8_t *types = reader.GetCurrentReaderPosition();
                    // ASSERT(typeSize == unsigned(2 + function.numparams));
                    // ASSERT(types[0] == LBC_TYPE_FUNCTION);
                    // ASSERT(types[1] == function.numparams);

                    // transform v1 into v2 format
                    int headerSize = typeSize > 127 ? 4 : 3;

                    // function.typeinfo;
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
                uint32_t typesize = reader.ReadVariableInteger();

                if (typesize) {
                    const uint8_t *types = reader.GetCurrentReaderPosition();

                    function.typeinfo.resize(typesize);

                    memcpy(function.typeinfo.data(), types, typesize);
                    reader.AdvanceBy(typesize);

                    if (result.typesVersion == 3) {
                        // nothing to do here, becuase it's simply runtime-userdata remapping.
                    }
                }
            }
        }

        const auto sizecode = reader.ReadVariableInteger();
        function.instructions.resize(sizecode);

        for (auto j = 0llu; j < sizecode; j++)
            function.instructions[j] = reader.Read<uint32_t>();

        const auto sizek = reader.ReadVariableInteger();
        function.constants.resize(sizek);

        for (auto j = 0llu; j < sizek; j++) {
            function.constants[j] = LuauConstant { };
            auto lbcConstant = reader.Read<uint8_t>();
            switch (lbcConstant) {
            case LBC_CONSTANT_NIL:
                // All constants have already been pre-initialized to nil
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
                function.constants[j].constantData = LuauVector {x, y, z, w};
                break;
            }

            case LBC_CONSTANT_STRING: {
                function.constants[j].kType = LUA_TSTRING;
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger());
                // ASSERT(str.has_value(), "malformed bytecode");
                function.constants[j].constantData = str.value();

                break;
            }

            case LBC_CONSTANT_IMPORT: {
                function.constants[j].kType = LUA_TNIL;
                /*uint32_t iid =*/
                (void)reader.Read<uint32_t>();
                break;
            }

            case LBC_CONSTANT_TABLE: {
                int keyCount = reader.ReadVariableInteger();
                auto vec = std::vector<std::string>(keyCount);
                for (auto k = 0; k < keyCount; ++k) {
                    int key = reader.ReadVariableInteger();
                    const auto &constant = function.constants[key];
                    // ASSERT(constant.kType == LUA_TSTRING, "kString isn't correct.");
                    vec.emplace_back(std::get<std::string>(constant.constantData));
                }
                function.constants[j].kType = LUA_TTABLE;
                function.constants[j].constantData = LuauTable {vec};
                break;
            }

            case LBC_CONSTANT_CLOSURE: {
                function.constants[j].kType = LUA_TFUNCTION;
                uint32_t fid = reader.ReadVariableInteger();
                function.constants[j].constantData = result.functions.data() + fid;
                break;
            }

            default:
                // ASSERT(false, "WARNING! Unknown constant!", lbcConstant);
                break;

            }
        }

        auto sizep = reader.ReadVariableInteger();
        function.subfunctions.resize(sizep);
        for (auto j = 0llu; j < sizep; j++) {
            auto fid = reader.ReadVariableInteger();
            function.subfunctions[j] = result.functions.data() + fid;
        }

        function.lineDefined = reader.ReadVariableInteger();
        function.debugName = result.ReadFromStringTable(reader.ReadVariableInteger());
        auto lineinfo = reader.Read<uint8_t>();

        if (lineinfo) {
            function.linegaplog2 = reader.Read<uint8_t>();

            int intervals = ((function.instructions.size() - 1) >> function.linegaplog2) + 1;
            int absoffset = (function.instructions.size() + 3) & ~3;

            const int sizelineinfo = absoffset + intervals * sizeof(int);
            function.lineinfo = { };
            function.lineinfo.resize(sizelineinfo);

            function.abslineinfo = reinterpret_cast<int *>(function.lineinfo.data() + absoffset);

            uint8_t lastoffset = 0;
            for (auto j = 0llu; j < function.instructions.size(); ++j) {
                lastoffset += reader.Read<uint8_t>();
                function.lineinfo[j] = lastoffset;
            }

            auto lastline = 0;
            for (auto j = 0; j < intervals; ++j) {
                lastline += reader.Read<int32_t>();
                function.abslineinfo[j] = lastline;
            }
        }

        uint8_t debuginfo = reader.Read<uint8_t>();

        if (debuginfo) {
            const int sizelocvars = reader.ReadVariableInteger();
            function.locvars = { };
            function.locvars.resize(sizelocvars);

            for (int j = 0; j < sizelocvars; ++j) {
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger());
                if (str.has_value())
                    function.locvars[j].varname = str.value();
                else
                    function.locvars[j].varname = "";

                function.locvars[j].startpc = reader.ReadVariableInteger();
                function.locvars[j].endpc = reader.ReadVariableInteger();
                function.locvars[j].reg = reader.Read<uint8_t>();
            }

            const int sizeupvalues = reader.ReadVariableInteger();
            // ASSERT(sizeupvalues == function.nups, "nups != sizeupvalues");

            function.upvalueNames = { };
            function.upvalueNames.resize(sizeupvalues);

            for (int j = 0; j < sizeupvalues; ++j) {
                auto str = result.ReadFromStringTable(reader.ReadVariableInteger());
                function.upvalueNames[j] = str.has_value() ? str.value() : "";
            }
        }

        result.functions[i] = function;
    }

    auto mainfid = reader.ReadVariableInteger();

    result.lpMainFunction = result.functions.data() + mainfid;

    // ASSERT(
    //     reader.GetCurrentReaderPosition() == reader.GetEndPosition(),
    //     "bytecode was not properly parsed. EndPosition != CurrentPosition", reader
    // );
    return result;
}