//
// Binary Ninja BinaryView that maps a Luau bytecode file into a synthetic
// address space so the Luau architecture can lift its instruction arrays.
//

#include "View.hpp"

#include "Decompiler.hpp"
#include "Deserializer.hpp"
#include "Image.hpp"
#include "SafetyGuard.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "highlevelilinstruction.h"

namespace fission::bn {

    using namespace BinaryNinja;

    namespace {

        constexpr uint64_t kMaximumImageSize = 64ull * 1024ull * 1024ull;

        struct ByteRange {
            uint64_t begin = 0;
            uint64_t end = 0;
        };

        std::vector<ByteRange> CodeRanges(const Image &image) {
            std::vector<ByteRange> ranges;
            ranges.reserve(image.Protos().size());
            for (const auto &proto : image.Protos()) {
                ByteRange range{proto.fileOffset, proto.fileOffset + proto.instructionCount * sizeof(uint32_t)};
                if (range.end <= range.begin)
                    continue;
                ranges.push_back(range);
            }
            std::sort(ranges.begin(), ranges.end(), [](const ByteRange &a, const ByteRange &b) { return a.begin < b.begin; });

            std::vector<ByteRange> merged;
            for (const auto &range : ranges) {
                if (merged.empty() || range.begin > merged.back().end) {
                    merged.push_back(range);
                    continue;
                }
                merged.back().end = std::max(merged.back().end, range.end);
            }
            return merged;
        }

        void DebugLog(const std::string &message) {
            try {
                std::ofstream log(std::filesystem::temp_directory_path() / "Fission.BinaryNinja.log", std::ios::app);
                log << message << "\n";
            } catch (...) {
            }
        }

        std::string SanitizeFieldName(std::string name, const std::string &fallback) {
            if (name.empty())
                name = fallback;
            for (char &character : name) {
                const auto byte = static_cast<unsigned char>(character);
                if (!std::isalnum(byte) && character != '_')
                    character = '_';
            }
            if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
                name = "_" + name;
            return name;
        }

        bool LooksLikeFunction(std::string_view name) {
            static constexpr std::string_view kPrefixes[] = {"table.",  "string.", "math.", "coroutine.", "bit32.",
                                                             "buffer.", "vector.", "utf8.", "os.",        "debug."};
            for (const auto prefix : kPrefixes) {
                if (name.starts_with(prefix))
                    return true;
            }
            static constexpr std::string_view kNames[] = {"pcall",    "xpcall",   "print",        "warn",         "error",   "assert", "type",   "typeof",
                                                          "tostring", "tonumber", "select",       "next",         "pairs",   "ipairs", "rawget", "rawset",
                                                          "rawequal", "rawlen",   "setmetatable", "getmetatable", "require", "unpack"};
            for (const auto candidate : kNames) {
                if (name == candidate)
                    return true;
            }
            return false;
        }

        Ref<Type> DefineStructType(BinaryView &view, const std::string &name, const std::vector<std::pair<std::string, Ref<Type>>> &fields) {
            StructureBuilder builder;
            uint64_t offset = 0;
            for (const auto &[fieldName, fieldType] : fields) {
                builder.AddMemberAtOffset(fieldType, fieldName, offset);
                offset += 8;
            }
            Ref<Structure> structure = builder.Finalize();
            Ref<Type> type = Type::StructureType(structure);
            QualifiedName qualified(name);
            view.DefineType(Type::GenerateAutoTypeId("Fission.BinaryNinja", qualified), qualified, type);
            return type;
        }

        Ref<Type> DefineTableType(BinaryView &view, Architecture &architecture, const Image &image) {
            std::vector<std::pair<int32_t, std::string>> ordered;
            ordered.reserve(image.FieldOffsets().size());
            for (const auto &[name, offset] : image.FieldOffsets())
                ordered.emplace_back(offset, name);
            std::sort(ordered.begin(), ordered.end(), [](const auto &left, const auto &right) { return left.first < right.first; });

            const QualifiedName qualified("LuauTable");
            const std::string id = Type::GenerateAutoTypeId("Fission.BinaryNinja", qualified);
            Ref<NamedTypeReference> tableReference = new NamedTypeReference(StructNamedTypeClass, id, qualified);
            Ref<Type> tableType = Type::NamedType(tableReference);
            Ref<Type> tablePointer = Type::PointerType(8, tableType);
            Ref<CallingConvention> convention = architecture.GetDefaultCallingConvention();

            const auto kindType = [&](Image::FieldKind kind) -> Ref<Type> {
                switch (kind) {
                case Image::FieldKind::Table:
                    return tablePointer;
                case Image::FieldKind::Number:
                    return Type::FloatType(8);
                case Image::FieldKind::Integer:
                    return Type::IntegerType(8, true);
                case Image::FieldKind::Boolean:
                    return Type::BoolType();
                case Image::FieldKind::String:
                    return Type::PointerType(8, Type::IntegerType(1, true));
                case Image::FieldKind::Function:
                    return Type::PointerType(8, Type::VoidType());
                default:
                    return Type::IntegerType(8, true);
                }
            };

            const auto methodType = [&](const std::string &name) -> Ref<Type> {
                const Image::FieldKind resultKind = image.FieldResultKind(name);
                Ref<Type> returnType = resultKind == Image::FieldKind::Unknown ? Type::PointerType(8, Type::VoidType()) : kindType(resultKind);
                if (!returnType)
                    returnType = Type::PointerType(8, Type::VoidType());

                std::vector<FunctionParameter> parameters;
                parameters.emplace_back("self", tablePointer);
                Ref<Type> function = Type::FunctionType(ReturnValue(returnType), convention, parameters, true);
                return Type::PointerType(8, function);
            };

            StructureBuilder builder;
            for (const auto &[offset, name] : ordered) {
                Ref<Type> fieldType = image.IsMethodField(name) ? methodType(name) : kindType(image.FieldValueKind(name));
                builder.AddMemberAtOffset(fieldType, SanitizeFieldName(name, "field"), static_cast<uint64_t>(offset));
            }

            Ref<Structure> structure = builder.Finalize();
            const QualifiedName actual = view.DefineType(id, qualified, Type::StructureType(structure));
            if (Ref<Type> named = view.GetTypeByName(actual))
                return named;
            return tableType;
        }

        std::string SanitizeLuauIdentifier(std::string value, const std::string &fallback) {
            if (value.empty())
                value = fallback;
            for (char &character : value) {
                const auto byte = static_cast<unsigned char>(character);
                if (!std::isalnum(byte) && character != '_')
                    character = '_';
            }
            if (!value.empty() && std::isdigit(static_cast<unsigned char>(value[0])))
                value = "_" + value;
            return value;
        }

        std::string JoinNames(const std::vector<std::string> &items) {
            std::string text;
            for (size_t i = 0; i < items.size(); ++i) {
                if (i)
                    text += ", ";
                text += items[i];
            }
            return text;
        }

        // Signature of a proto, derived purely from bytecode metadata. This never
        // runs the decompiler so it is safe to call while Binary Ninja lists
        // functions during analysis.
        struct ProtoSignature {
            std::string name;
            std::string header; // "function name(args)"
            std::vector<std::string> upvalues;
        };

        ProtoSignature BuildProtoSignature(const Image &image, uint32_t bytecodeId) {
            ProtoSignature signature;
            const ProtoView *proto = image.ProtoByIndex(bytecodeId);
            signature.name = SanitizeLuauIdentifier(proto ? proto->name : std::string(), std::format("proto_{}", bytecodeId));
            if (!proto || !proto->proto) {
                signature.header = std::format("function {}()", signature.name);
                return signature;
            }

            const auto &function = *proto->proto;
            std::vector<std::string> parameters;
            parameters.reserve(function.numparams);
            for (uint8_t index = 0; index < function.numparams; ++index) {
                std::string name = std::format("arg{}", static_cast<unsigned>(index) + 1);
                for (const auto &local : function.locvars) {
                    if (local.reg == index && local.startpc == 0 && !local.varname.empty()) {
                        name = SanitizeLuauIdentifier(local.varname, name);
                        break;
                    }
                }
                parameters.push_back(std::move(name));
            }
            if (function.isvararg)
                parameters.emplace_back("...");

            for (uint8_t index = 0; index < function.nups; ++index) {
                const std::string fallback = std::format("upvalue_{}", static_cast<unsigned>(index));
                if (index < function.upvalueNames.size() && !function.upvalueNames[index].empty())
                    signature.upvalues.push_back(SanitizeLuauIdentifier(function.upvalueNames[index], fallback));
                else
                    signature.upvalues.push_back(fallback);
            }

            signature.header = std::format("function {}({})", signature.name, JoinNames(parameters));
            return signature;
        }

        std::string BuildPseudoLuauFunction(const Image &image, uint32_t bytecodeId) {
            const ProtoSignature signature = BuildProtoSignature(image, bytecodeId);

            Decompiler decompiler;
            DecompilationResult result{};
            {
                std::lock_guard fissionLock(FissionOperationMutex());
                result = decompiler.DecompileVanillaFunction(image.Bytes(), bytecodeId);
            }
            std::string body = result.decompilationOutput;
            if (result.resultCode != DecompileResult::Success && body.empty())
                body = std::format("--[[ Fission decompilation failed ({}): {} ]]", static_cast<int>(result.resultCode), result.errorMessage);

            std::string source;
            if (!signature.upvalues.empty())
                source += "local " + JoinNames(signature.upvalues) + "\n\n";
            source += signature.header + "\n";

            std::istringstream stream(body);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty())
                    source += "    " + line + "\n";
                else
                    source += "\n";
            }
            source += "end\n";
            return source;
        }

        std::mutex g_pseudoSourceMutex;
        std::unordered_map<std::string, std::string> g_pseudoSourceCache;

        std::string PseudoLuauSource(const Image &image, uint32_t bytecodeId) {
            const std::string key = std::format("{}:{}", image.BaseAddress(), bytecodeId);
            std::lock_guard lock(g_pseudoSourceMutex);
            if (const auto it = g_pseudoSourceCache.find(key); it != g_pseudoSourceCache.end())
                return it->second;
            std::string source = BuildPseudoLuauFunction(image, bytecodeId);
            g_pseudoSourceCache.emplace(key, source);
            return source;
        }

        std::string TrimSourceLine(std::string line) {
            const auto first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos)
                return {};
            const auto last = line.find_last_not_of(" \t\r\n");
            return line.substr(first, last - first + 1);
        }

        // Returns the index just past a quoted string starting at `index`.
        size_t SkipQuotedString(const std::string &line, size_t index, char quote) {
            ++index;
            while (index < line.size()) {
                if (line[index] == '\\') {
                    index += 2;
                    continue;
                }
                if (line[index] == quote)
                    return index + 1;
                ++index;
            }
            return line.size();
        }

        // Returns the index just past a backtick string starting at `index`.
        // Interpolation expressions are scanned structurally so nested strings
        // and braces cannot terminate the outer literal early.
        size_t SkipInterpolatedString(const std::string &line, size_t index) {
            ++index;
            while (index < line.size()) {
                const char character = line[index];
                if (character == '\\') {
                    index += 2;
                    continue;
                }
                if (character == '`')
                    return index + 1;
                if (character == '$' && index + 1 < line.size() && line[index + 1] == '{') {
                    index += 2;
                    int depth = 1;
                    while (index < line.size() && depth > 0) {
                        const char inner = line[index];
                        if (inner == '"' || inner == '\'') {
                            index = SkipQuotedString(line, index, inner);
                            continue;
                        }
                        if (inner == '`') {
                            index = SkipInterpolatedString(line, index);
                            continue;
                        }
                        if (inner == '{') {
                            ++depth;
                            ++index;
                            continue;
                        }
                        if (inner == '}') {
                            --depth;
                            ++index;
                            continue;
                        }
                        ++index;
                    }
                    continue;
                }
                ++index;
            }
            return line.size();
        }

        // Removes an inline Luau comment, leaving "--" inside quoted,
        // interpolated and long-bracket strings untouched. Source text is
        // generated code, not instruction-aligned, so a naive find would corrupt
        // print("a--b") or `a--{value}`.
        std::string StripLuauInlineComment(const std::string &line) {
            size_t index = 0;
            while (index < line.size()) {
                const char character = line[index];
                if (character == '"' || character == '\'') {
                    index = SkipQuotedString(line, index, character);
                    continue;
                }
                if (character == '`') {
                    index = SkipInterpolatedString(line, index);
                    continue;
                }
                if (character == '[') {
                    size_t cursor = index + 1;
                    size_t level = 0;
                    while (cursor < line.size() && line[cursor] == '=') {
                        ++level;
                        ++cursor;
                    }
                    if (cursor < line.size() && line[cursor] == '[') {
                        const std::string closing = "]" + std::string(level, '=') + "]";
                        const size_t end = line.find(closing, cursor + 1);
                        if (end == std::string::npos)
                            return line;
                        index = end + closing.size();
                        continue;
                    }
                }
                if (character == '-' && index + 1 < line.size() && line[index + 1] == '-')
                    return line.substr(0, index);
                ++index;
            }
            return line;
        }

        bool IsLuauKeyword(std::string_view word) {
            static const std::unordered_set<std::string_view> keywords = {"local",    "function", "end",    "if",     "then",  "else",   "elseif",
                                                                          "for",      "while",    "do",     "repeat", "until", "return", "break",
                                                                          "continue", "not",      "and",    "or",     "in",    "const",  "true",
                                                                          "false",    "nil",      "export", "type",   "typeof"};
            return keywords.contains(word);
        }

        // Emits one line of Luau text as typed tokens so Binary Ninja applies its
        // syntax colors: keywords, numbers, strings, operators and braces.
        template <typename Append> void TokenizeLuauSourceLine(const std::string &line, Append &&append) {
            size_t index = 0;
            const auto appendRange = [&](size_t begin, size_t end, BNInstructionTextTokenType type) {
                if (end > begin)
                    append(type, line.substr(begin, end - begin));
            };

            while (index < line.size()) {
                const char character = line[index];
                if (std::isspace(static_cast<unsigned char>(character))) {
                    size_t end = index + 1;
                    while (end < line.size() && std::isspace(static_cast<unsigned char>(line[end])))
                        ++end;
                    appendRange(index, end, TextToken);
                    index = end;
                    continue;
                }

                if (character == '-' && index + 1 < line.size() && line[index + 1] == '-') {
                    appendRange(index, line.size(), CommentToken);
                    break;
                }

                if (character == '"' || character == '\'') {
                    const size_t end = SkipQuotedString(line, index, character);
                    appendRange(index, end, StringToken);
                    index = end;
                    continue;
                }

                if (character == '`') {
                    const size_t end = SkipInterpolatedString(line, index);
                    appendRange(index, end, StringToken);
                    index = end;
                    continue;
                }

                if (std::isdigit(static_cast<unsigned char>(character))) {
                    size_t end = index + 1;
                    bool floating = false;
                    while (end < line.size()) {
                        const char next = line[end];
                        if (std::isalnum(static_cast<unsigned char>(next)) || next == '_') {
                            ++end;
                            continue;
                        }
                        if (next == '.' && end + 1 < line.size() && std::isdigit(static_cast<unsigned char>(line[end + 1]))) {
                            floating = true;
                            ++end;
                            continue;
                        }
                        break;
                    }
                    appendRange(index, end, floating ? FloatingPointToken : IntegerToken);
                    index = end;
                    continue;
                }

                if (std::isalpha(static_cast<unsigned char>(character)) || character == '_') {
                    size_t end = index + 1;
                    while (end < line.size() && (std::isalnum(static_cast<unsigned char>(line[end])) || line[end] == '_'))
                        ++end;
                    const std::string_view word(line.data() + index, end - index);
                    appendRange(index, end, IsLuauKeyword(word) ? KeywordToken : TextToken);
                    index = end;
                    continue;
                }

                if (character == '(' || character == ')' || character == '[' || character == ']' || character == '{' || character == '}') {
                    appendRange(index, index + 1, BraceToken);
                    ++index;
                    continue;
                }

                if (std::string_view("+-*/%^#<>=~&|!?:").find(character) != std::string_view::npos) {
                    size_t end = index + 1;
                    if (end < line.size() &&
                        (line.substr(index, 2) == "==" || line.substr(index, 2) == "~=" || line.substr(index, 2) == "<=" || line.substr(index, 2) == ">=" ||
                         line.substr(index, 2) == "//" || line.substr(index, 2) == ".." || line.substr(index, 2) == "::" || line.substr(index, 2) == "->" ||
                         line.substr(index, 2) == "+=" || line.substr(index, 2) == "-=" || line.substr(index, 2) == "*=" || line.substr(index, 2) == "/="))
                        ++end;
                    appendRange(index, end, OperationToken);
                    index = end;
                    continue;
                }

                appendRange(index, index + 1, TextToken);
                ++index;
            }
        }

        void AppendSourceLine(const std::string &line, HighLevelILTokenEmitter &tokens) {
            TokenizeLuauSourceLine(line, [&](BNInstructionTextTokenType type, const std::string &text) { tokens.Append(type, text); });
        }

        void AppendSourceLine(const std::string &line, DisassemblyTextLine &target) {
            TokenizeLuauSourceLine(line, [&](BNInstructionTextTokenType type, const std::string &text) { target.tokens.emplace_back(type, text); });
        }

        const ProtoView *FunctionProto(Function *function) {
            if (!function)
                return nullptr;
            const uint64_t address = function->GetStart();
            const auto image = ImageRegistry::Find(address);
            return image ? image->ProtoAtAddress(address) : nullptr;
        }

        class PseudoLuauFunction final : public LanguageRepresentationFunction {
          public:
            PseudoLuauFunction(LanguageRepresentationFunctionType *type, Architecture *arch, Function *owner, HighLevelILFunction *highLevelIL)
                : LanguageRepresentationFunction(type, arch, owner, highLevelIL) {}

          protected:
            // Renders the Fission source of this proto. Binary Ninja calls
            // GetExprText once per instruction in graph views and once for the
            // root instruction in the linear view. The full block is therefore
            // emitted for the root only, and every call is idempotent: no line
            // cursor, so a second render pass produces the same text.
            void LoadLines() {
                if (m_loaded)
                    return;
                m_loaded = true;
                Ref<Function> function = GetFunction();
                if (!function)
                    return;
                m_proto = FunctionProto(function);
                if (!m_proto || !m_proto->proto)
                    return;
                const auto image = ImageRegistry::Find(m_proto->address);
                if (!image)
                    return;

                std::istringstream stream(PseudoLuauSource(*image, m_proto->proto->bytecodeId));
                std::vector<std::string> lines;
                std::string text;
                while (std::getline(stream, text))
                    lines.push_back(std::move(text));

                // Drop the upvalue declarations and the "function ..." header;
                // the header is supplied by GetFunctionTypeTokens.
                size_t headerIndex = std::string::npos;
                for (size_t i = 0; i < lines.size(); ++i) {
                    if (TrimSourceLine(lines[i]).starts_with("function ")) {
                        headerIndex = i;
                        break;
                    }
                }
                if (headerIndex == std::string::npos)
                    return;
                lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(headerIndex) + 1);
                if (!lines.empty() && TrimSourceLine(lines.back()) == "end")
                    lines.pop_back();

                bool inBlockComment = false;
                for (std::string &line : lines) {
                    const std::string trimmed = TrimSourceLine(line);
                    if (inBlockComment) {
                        if (trimmed.find("]]") != std::string::npos)
                            inBlockComment = false;
                        continue;
                    }
                    if (trimmed.starts_with("--[[")) {
                        if (trimmed.find("]]") == std::string::npos)
                            inBlockComment = true;
                        continue;
                    }
                    if (trimmed.starts_with("--"))
                        continue;
                    std::string content = StripLuauInlineComment(line);
                    if (TrimSourceLine(content).empty())
                        continue;
                    if (content.find_first_not_of(" \t\r\n") == std::string::npos)
                        m_lines.emplace_back();
                    else
                        m_lines.push_back(std::move(content));
                }
            }

            bool IsRoot(const HighLevelILInstruction &instr) const {
                try {
                    Ref<HighLevelILFunction> highLevelIL = GetHighLevelILFunction();
                    if (!highLevelIL || highLevelIL->GetInstructionCount() == 0)
                        return false;
                    return instr.exprIndex == highLevelIL->GetRootExpr().exprIndex;
                } catch (...) {
                    return false;
                }
            }

            void EmitSourceLine(const std::string &line, HighLevelILTokenEmitter &tokens) {
                tokens.InitLine();
                AppendSourceLine(line, tokens);
                tokens.NewLine();
            }

            // Graph/basic-block rendering: Fission emits AST-shaped source text,
            // not instruction-aligned lines, so there is no reliable mapping from
            // a block address to a line. Point at the root node instead of
            // guessing a line from the instruction index.
            void EmitLineForAddress(uint64_t, HighLevelILTokenEmitter &tokens) {
                tokens.InitLine();
                tokens.Append(CommentToken, "-- Pseudo Luau source is shown on the function root");
                tokens.NewLine();
            }

            void BeginLines(const HighLevelILInstruction &, HighLevelILTokenEmitter &) override {}

            void GetExprText(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, DisassemblySettings *, BNOperatorPrecedence, bool) override {
                try {
                    LoadLines();
                    if (IsRoot(instr)) {
                        for (const std::string &line : m_lines)
                            EmitSourceLine(line, tokens);
                        return;
                    }
                    EmitLineForAddress(instr.address, tokens);
                } catch (...) {
                    try {
                        tokens.InitLine();
                        tokens.Append(CommentToken, "-- Pseudo Luau: failed to render this function");
                        tokens.NewLine();
                    } catch (...) {
                    }
                }
            }

            void EndLines(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens) override {
                try {
                    LoadLines();
                    if (!IsRoot(instr))
                        return;
                    tokens.InitLine();
                    tokens.NoIndentForThisLine();
                    tokens.Append(KeywordToken, "end");
                    tokens.NewLine();
                } catch (...) {
                }
            }

          private:
            const ProtoView *m_proto = nullptr;
            bool m_loaded = false;
            std::vector<std::string> m_lines;
        };

        // LLIL/HLIL -> Luau language representation. Fission's own text is a
        // separate mode ("Pseudo Luau (Fission)"); this one keeps the view driven
        // by the IL that Binary Ninja actually analyzed, and covers every HLIL
        // operation so no instruction degrades to an "unsupported" marker.
        class PseudoLuauILFunction final : public LanguageRepresentationFunction {
          public:
            PseudoLuauILFunction(LanguageRepresentationFunctionType *type, Architecture *arch, Function *owner, HighLevelILFunction *highLevelIL)
                : LanguageRepresentationFunction(type, arch, owner, highLevelIL) {}

          protected:
            void GetExprText(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, DisassemblySettings *, BNOperatorPrecedence, bool) override {
                // Binary Ninja invokes this from C: an exception escaping it
                // terminates the process, so every render is contained.
                try {
                    // The header comes from GetFunctionTypeTokens; statements are
                    // the function body, so they start at indentation level 1.
                    EmitStatement(instr, tokens, 1);
                } catch (...) {
                    try {
                        tokens.InitLine();
                        tokens.Append(CommentToken, "-- Pseudo Luau: failed to render this instruction");
                        tokens.NewLine();
                    } catch (...) {
                    }
                }
            }

            void BeginLines(const HighLevelILInstruction &, HighLevelILTokenEmitter &) override {}

            void EndLines(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens) override {
                try {
                    if (!IsRoot(instr))
                        return;
                    tokens.InitLine();
                    tokens.Append(KeywordToken, "end");
                    tokens.NewLine();
                } catch (...) {
                }
            }

          private:
            // ---- token helpers -------------------------------------------------

            static void Keyword(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(KeywordToken, text); }
            static void Text(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(TextToken, text); }
            static void Op(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(OperationToken, text); }
            static void Brace(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(BraceToken, text); }
            static void Field(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(FieldNameToken, text); }
            static void Comment(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(CommentToken, text); }
            static void Var(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(LocalVariableToken, text); }
            static void Str(HighLevelILTokenEmitter &tokens, const std::string &text) { tokens.Append(StringToken, text); }

            static void Integer(HighLevelILTokenEmitter &tokens, int64_t value) { tokens.Append(IntegerToken, std::format("{}", value)); }

            static void Float(HighLevelILTokenEmitter &tokens, double value) { tokens.Append(FloatingPointToken, std::format("{:g}", value)); }

            static void Hex(HighLevelILTokenEmitter &tokens, uint64_t value) { tokens.Append(IntegerToken, std::format("0x{:x}", value)); }

            bool IsRoot(const HighLevelILInstruction &instr) const {
                try {
                    Ref<HighLevelILFunction> highLevelIL = GetHighLevelILFunction();
                    if (!highLevelIL || highLevelIL->GetInstructionCount() == 0)
                        return false;
                    return instr.exprIndex == highLevelIL->GetRootExpr().exprIndex;
                } catch (...) {
                    return false;
                }
            }

            std::string VariableName(const Variable &variable) const {
                try {
                    if (Ref<Function> function = GetFunction()) {
                        const std::string name = function->GetVariableNameOrDefault(variable);
                        if (!name.empty())
                            return name;
                    }
                } catch (...) {
                }
                return std::format("v{}", variable.storage);
            }

            std::string VariableName(const SSAVariable &variable) const { return VariableName(variable.var); }

            std::string SymbolName(uint64_t address) const {
                try {
                    if (Ref<Function> function = GetFunction(); function && function->GetView()) {
                        if (Ref<Symbol> symbol = function->GetView()->GetSymbolByAddress(address))
                            return symbol->GetShortName();
                    }
                } catch (...) {
                }
                return std::format("0x{:x}", address);
            }

            void EmitSymbolOrAddress(HighLevelILTokenEmitter &tokens, uint64_t address) {
                if (address == 0) {
                    Integer(tokens, 0);
                    return;
                }
                try {
                    if (Ref<Function> function = GetFunction(); function && function->GetView()) {
                        if (Ref<Symbol> symbol = function->GetView()->GetSymbolByAddress(address)) {
                            Text(tokens, symbol->GetShortName());
                            return;
                        }
                    }
                } catch (...) {
                }
                Hex(tokens, address);
            }

            void LabelName(HighLevelILTokenEmitter &tokens, uint64_t target) { tokens.Append(GotoLabelToken, std::format("label_{:x}", target)); }

            // ---- statements ----------------------------------------------------

            // Indentation is emitted as literal spaces so the rendered source is
            // stable regardless of how Binary Ninja groups its token lines.
            void EmitIndent(HighLevelILTokenEmitter &tokens, int depth) {
                if (depth > 0)
                    tokens.Append(TextToken, std::string(static_cast<size_t>(depth) * 4, ' '));
            }

            void BeginStatementLine(HighLevelILTokenEmitter &tokens, int depth) {
                tokens.InitLine();
                EmitIndent(tokens, depth);
            }

            void EmitStatement(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, int depth) {
                // BLOCK is transparent, CASE is rendered by its switch, FOR starts
                // with its init statement, and NOP contributes no line at all.
                const bool beginsLine = instr.operation != HLIL_BLOCK && instr.operation != HLIL_CASE && instr.operation != HLIL_FOR &&
                                        instr.operation != HLIL_FOR_SSA && instr.operation != HLIL_NOP;
                if (beginsLine)
                    BeginStatementLine(tokens, depth);

                switch (instr.operation) {
                case HLIL_NOP:
                    break;

                case HLIL_BLOCK:
                    for (const auto &statement : instr.GetBlockExprs<HLIL_BLOCK>())
                        EmitStatement(statement, tokens, depth);
                    break;

                case HLIL_IF:
                    EmitIf(instr, tokens, depth);
                    break;

                case HLIL_WHILE: {
                    Keyword(tokens, "while ");
                    EmitExpr(instr.GetConditionExpr<HLIL_WHILE>(), tokens, false);
                    Keyword(tokens, " do");
                    tokens.NewLine();
                    EmitNestedStatement(instr.GetLoopExpr<HLIL_WHILE>(), tokens, depth + 1);
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "end");
                    tokens.NewLine();
                    break;
                }

                case HLIL_WHILE_SSA: {
                    Keyword(tokens, "while ");
                    EmitExpr(instr.GetConditionExpr<HLIL_WHILE_SSA>(), tokens, false);
                    Keyword(tokens, " do");
                    tokens.NewLine();
                    EmitNestedStatement(instr.GetLoopExpr<HLIL_WHILE_SSA>(), tokens, depth + 1);
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "end");
                    tokens.NewLine();
                    break;
                }

                case HLIL_DO_WHILE: {
                    Keyword(tokens, "repeat");
                    tokens.NewLine();
                    EmitNestedStatement(instr.GetLoopExpr<HLIL_DO_WHILE>(), tokens, depth + 1);
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "until ");
                    EmitExpr(instr.GetConditionExpr<HLIL_DO_WHILE>(), tokens, false);
                    tokens.NewLine();
                    break;
                }

                case HLIL_DO_WHILE_SSA: {
                    Keyword(tokens, "repeat");
                    tokens.NewLine();
                    EmitNestedStatement(instr.GetLoopExpr<HLIL_DO_WHILE_SSA>(), tokens, depth + 1);
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "until ");
                    EmitExpr(instr.GetConditionExpr<HLIL_DO_WHILE_SSA>(), tokens, false);
                    tokens.NewLine();
                    break;
                }

                case HLIL_FOR:
                    EmitFor(instr, tokens, depth, false);
                    break;

                case HLIL_FOR_SSA:
                    EmitFor(instr, tokens, depth, true);
                    break;

                case HLIL_SWITCH:
                    EmitSwitch(instr, tokens, depth);
                    break;

                case HLIL_CASE:
                    // Cases are rendered by HLIL_SWITCH.
                    break;

                case HLIL_BREAK:
                    Keyword(tokens, "break");
                    tokens.NewLine();
                    break;

                case HLIL_CONTINUE:
                    Keyword(tokens, "continue");
                    tokens.NewLine();
                    break;

                case HLIL_JUMP:
                    Keyword(tokens, "goto ");
                    EmitExpr(instr.GetDestExpr<HLIL_JUMP>(), tokens, false);
                    tokens.NewLine();
                    break;

                case HLIL_GOTO:
                    Keyword(tokens, "goto ");
                    LabelName(tokens, instr.GetTarget<HLIL_GOTO>());
                    tokens.NewLine();
                    break;

                case HLIL_LABEL:
                    Op(tokens, "::");
                    LabelName(tokens, instr.GetTarget<HLIL_LABEL>());
                    Op(tokens, "::");
                    tokens.NewLine();
                    break;

                case HLIL_RET: {
                    Keyword(tokens, "return");
                    bool first = true;
                    for (const auto &value : instr.GetSourceExprs<HLIL_RET>()) {
                        Text(tokens, first ? " " : ", ");
                        EmitExpr(value, tokens, false);
                        first = false;
                    }
                    tokens.NewLine();
                    break;
                }

                case HLIL_NORET:
                    Keyword(tokens, "error");
                    Brace(tokens, "(");
                    Str(tokens, "\"noreturn\"");
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;

                case HLIL_VAR_DECLARE:
                    Keyword(tokens, "local ");
                    Var(tokens, VariableName(instr.GetVariable<HLIL_VAR_DECLARE>()));
                    tokens.NewLine();
                    break;

                case HLIL_VAR_INIT:
                    Keyword(tokens, "local ");
                    Var(tokens, VariableName(instr.GetDestVariable<HLIL_VAR_INIT>()));
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_VAR_INIT>(), tokens, false);
                    tokens.NewLine();
                    break;

                case HLIL_VAR_INIT_SSA:
                    Keyword(tokens, "local ");
                    Var(tokens, VariableName(instr.GetDestSSAVariable<HLIL_VAR_INIT_SSA>()));
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_VAR_INIT_SSA>(), tokens, false);
                    tokens.NewLine();
                    break;

                case HLIL_ASSIGN:
                    EmitExpr(instr.GetDestExpr<HLIL_ASSIGN>(), tokens, false);
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_ASSIGN>(), tokens, false);
                    tokens.NewLine();
                    break;

                case HLIL_ASSIGN_MEM_SSA:
                    EmitExpr(instr.GetDestExpr<HLIL_ASSIGN_MEM_SSA>(), tokens, false);
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_ASSIGN_MEM_SSA>(), tokens, false);
                    tokens.NewLine();
                    break;

                case HLIL_ASSIGN_UNPACK: {
                    bool first = true;
                    for (const auto &dest : instr.GetDestExprs<HLIL_ASSIGN_UNPACK>()) {
                        if (!first)
                            Text(tokens, ", ");
                        EmitExpr(dest, tokens, false);
                        first = false;
                    }
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_ASSIGN_UNPACK>(), tokens, false);
                    tokens.NewLine();
                    break;
                }

                case HLIL_ASSIGN_UNPACK_MEM_SSA: {
                    bool first = true;
                    for (const auto &dest : instr.GetDestExprs<HLIL_ASSIGN_UNPACK_MEM_SSA>()) {
                        if (!first)
                            Text(tokens, ", ");
                        EmitExpr(dest, tokens, false);
                        first = false;
                    }
                    Op(tokens, " = ");
                    EmitExpr(instr.GetSourceExpr<HLIL_ASSIGN_UNPACK_MEM_SSA>(), tokens, false);
                    tokens.NewLine();
                    break;
                }

                case HLIL_FORCE_VER:
                    Var(tokens, VariableName(instr.GetDestVariable<HLIL_FORCE_VER>()));
                    Op(tokens, " = ");
                    Var(tokens, VariableName(instr.GetVariable<HLIL_FORCE_VER>()));
                    tokens.NewLine();
                    break;

                case HLIL_FORCE_VER_SSA:
                    Var(tokens, VariableName(instr.GetDestSSAVariable<HLIL_FORCE_VER_SSA>()));
                    Op(tokens, " = ");
                    Var(tokens, VariableName(instr.GetSSAVariable<HLIL_FORCE_VER_SSA>()));
                    tokens.NewLine();
                    break;

                case HLIL_ASSERT: {
                    Keyword(tokens, "assert");
                    Brace(tokens, "(");
                    Var(tokens, VariableName(instr.GetVariable<HLIL_ASSERT>()));
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;
                }

                case HLIL_ASSERT_SSA: {
                    Keyword(tokens, "assert");
                    Brace(tokens, "(");
                    Var(tokens, VariableName(instr.GetSSAVariable<HLIL_ASSERT_SSA>()));
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;
                }

                case HLIL_TAILCALL: {
                    Keyword(tokens, "return ");
                    EmitCall(instr.GetDestExpr<HLIL_TAILCALL>(), instr.GetParameterExprs<HLIL_TAILCALL>(), tokens);
                    tokens.NewLine();
                    break;
                }

                case HLIL_SYSCALL:
                case HLIL_SYSCALL_SSA: {
                    Keyword(tokens, "syscall");
                    EmitArgumentList(instr.GetParameterExprs<HLIL_SYSCALL>(), tokens);
                    tokens.NewLine();
                    break;
                }

                case HLIL_UNREACHABLE:
                    Keyword(tokens, "error");
                    Brace(tokens, "(");
                    Str(tokens, "\"unreachable\"");
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;

                case HLIL_BP:
                    Keyword(tokens, "error");
                    Brace(tokens, "(");
                    Str(tokens, "\"breakpoint\"");
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;

                case HLIL_TRAP:
                    Keyword(tokens, "error");
                    Brace(tokens, "(");
                    Str(tokens, "\"trap\"");
                    Brace(tokens, ")");
                    tokens.NewLine();
                    break;

                case HLIL_UNDEF:
                    Keyword(tokens, "nil");
                    tokens.NewLine();
                    break;

                case HLIL_UNIMPL:
                case HLIL_UNIMPL_MEM:
                    Comment(tokens, "-- unimplemented instruction");
                    tokens.NewLine();
                    break;

                case HLIL_VAR_PHI:
                    Var(tokens, VariableName(instr.GetDestSSAVariable<HLIL_VAR_PHI>()));
                    Op(tokens, " = ");
                    {
                        bool first = true;
                        for (const auto &source : instr.GetSourceSSAVariables<HLIL_VAR_PHI>()) {
                            if (!first)
                                Keyword(tokens, " or ");
                            Var(tokens, VariableName(source));
                            first = false;
                        }
                    }
                    tokens.NewLine();
                    break;

                case HLIL_MEM_PHI:
                    Comment(tokens, "-- memory phi");
                    tokens.NewLine();
                    break;

                default:
                    // Expressions used as statements (calls, struct inits, ...).
                    EmitExpr(instr, tokens, false);
                    tokens.NewLine();
                    break;
                }
            }

            // Emits a statement that is nested inside the current one. A
            // HLIL_BLOCK is transparent, so its children receive `depth`.
            void EmitNestedStatement(const HighLevelILInstruction &statement, HighLevelILTokenEmitter &tokens, int depth) {
                if (statement.operation == HLIL_NOP || statement.operation == HLIL_UNDEF)
                    return;
                EmitStatement(statement, tokens, depth);
            }

            void EmitFor(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, int depth, bool ssa) {
                EmitStatement(ssa ? instr.GetInitExpr<HLIL_FOR_SSA>() : instr.GetInitExpr<HLIL_FOR>(), tokens, depth);
                BeginStatementLine(tokens, depth);
                Keyword(tokens, "while ");
                EmitExpr(ssa ? instr.GetConditionExpr<HLIL_FOR_SSA>() : instr.GetConditionExpr<HLIL_FOR>(), tokens, false);
                Keyword(tokens, " do");
                tokens.NewLine();
                EmitStatement(ssa ? instr.GetLoopExpr<HLIL_FOR_SSA>() : instr.GetLoopExpr<HLIL_FOR>(), tokens, depth + 1);
                EmitStatement(ssa ? instr.GetUpdateExpr<HLIL_FOR_SSA>() : instr.GetUpdateExpr<HLIL_FOR>(), tokens, depth + 1);
                BeginStatementLine(tokens, depth);
                Keyword(tokens, "end");
                tokens.NewLine();
            }

            void EmitIf(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, int depth) {
                Keyword(tokens, "if ");
                EmitExpr(instr.GetConditionExpr<HLIL_IF>(), tokens, false);
                Keyword(tokens, " then");
                tokens.NewLine();
                EmitNestedStatement(instr.GetTrueExpr<HLIL_IF>(), tokens, depth + 1);

                HighLevelILInstruction otherwise = instr.GetFalseExpr<HLIL_IF>();
                while (otherwise.operation == HLIL_IF) {
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "elseif ");
                    EmitExpr(otherwise.GetConditionExpr<HLIL_IF>(), tokens, false);
                    Keyword(tokens, " then");
                    tokens.NewLine();
                    EmitNestedStatement(otherwise.GetTrueExpr<HLIL_IF>(), tokens, depth + 1);
                    otherwise = otherwise.GetFalseExpr<HLIL_IF>();
                }

                if (otherwise.operation != HLIL_NOP && otherwise.operation != HLIL_UNDEF && otherwise.operation != HLIL_UNREACHABLE) {
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "else");
                    tokens.NewLine();
                    EmitNestedStatement(otherwise, tokens, depth + 1);
                }
                BeginStatementLine(tokens, depth);
                Keyword(tokens, "end");
                tokens.NewLine();
            }

            void EmitSwitch(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, int depth) {
                const HighLevelILInstruction condition = instr.GetConditionExpr<HLIL_SWITCH>();
                bool first = true;
                for (const auto &caseInstr : instr.GetCases<HLIL_SWITCH>()) {
                    const auto values = caseInstr.GetValueExprs<HLIL_CASE>();
                    if (values.size() == 0)
                        continue;
                    if (!first)
                        BeginStatementLine(tokens, depth);
                    Keyword(tokens, first ? "if " : "elseif ");
                    bool firstValue = true;
                    for (const auto &value : values) {
                        if (!firstValue)
                            Keyword(tokens, " or ");
                        EmitExpr(condition, tokens, true);
                        Op(tokens, " == ");
                        EmitExpr(value, tokens, true);
                        firstValue = false;
                    }
                    Keyword(tokens, " then");
                    tokens.NewLine();
                    EmitNestedStatement(caseInstr.GetTrueExpr<HLIL_CASE>(), tokens, depth + 1);
                    first = false;
                }

                const HighLevelILInstruction otherwise = instr.GetDefaultExpr<HLIL_SWITCH>();
                const bool hasDefault = otherwise.operation != HLIL_NOP && otherwise.operation != HLIL_UNDEF && otherwise.operation != HLIL_UNREACHABLE;
                if (first) {
                    // No usable cases: still show the default body once.
                    if (hasDefault) {
                        Keyword(tokens, "do");
                        tokens.NewLine();
                        EmitNestedStatement(otherwise, tokens, depth + 1);
                        BeginStatementLine(tokens, depth);
                        Keyword(tokens, "end");
                        tokens.NewLine();
                    } else {
                        Comment(tokens, "-- switch");
                        tokens.NewLine();
                    }
                    return;
                }
                if (hasDefault) {
                    BeginStatementLine(tokens, depth);
                    Keyword(tokens, "else");
                    tokens.NewLine();
                    EmitNestedStatement(otherwise, tokens, depth + 1);
                }
                BeginStatementLine(tokens, depth);
                Keyword(tokens, "end");
                tokens.NewLine();
            }
            // ---- expressions ---------------------------------------------------

            void EmitArgumentList(const HighLevelILInstructionList &parameters, HighLevelILTokenEmitter &tokens) {
                Brace(tokens, "(");
                bool first = true;
                for (const auto &parameter : parameters) {
                    if (!first)
                        Text(tokens, ", ");
                    EmitExpr(parameter, tokens, false);
                    first = false;
                }
                Brace(tokens, ")");
            }

            void EmitCall(const HighLevelILInstruction &dest, const HighLevelILInstructionList &parameters, HighLevelILTokenEmitter &tokens) {
                EmitExpr(dest, tokens, true);
                EmitArgumentList(parameters, tokens);
            }

            void EmitBinary(
                const std::string &op, const HighLevelILInstruction &left, const HighLevelILInstruction &right, HighLevelILTokenEmitter &tokens,
                bool parenthesize
            ) {
                if (parenthesize)
                    Brace(tokens, "(");
                EmitExpr(left, tokens, true);
                Op(tokens, " " + op + " ");
                EmitExpr(right, tokens, true);
                if (parenthesize)
                    Brace(tokens, ")");
            }

            void EmitUnary(const std::string &op, const HighLevelILInstruction &source, HighLevelILTokenEmitter &tokens, bool parenthesize) {
                if (parenthesize)
                    Brace(tokens, "(");
                Op(tokens, op);
                EmitExpr(source, tokens, true);
                if (parenthesize)
                    Brace(tokens, ")");
            }

            void EmitCallLike(const std::string &name, const HighLevelILInstructionList &parameters, HighLevelILTokenEmitter &tokens) {
                Text(tokens, name);
                EmitArgumentList(parameters, tokens);
            }

            void EmitOneArgCall(const std::string &name, const HighLevelILInstruction &source, HighLevelILTokenEmitter &tokens, bool parenthesize) {
                if (parenthesize)
                    Brace(tokens, "(");
                Text(tokens, name);
                Brace(tokens, "(");
                EmitExpr(source, tokens, false);
                Brace(tokens, ")");
                if (parenthesize)
                    Brace(tokens, ")");
            }

            void EmitTwoArgCall(
                const std::string &name, const HighLevelILInstruction &left, const HighLevelILInstruction &right, HighLevelILTokenEmitter &tokens,
                bool parenthesize
            ) {
                if (parenthesize)
                    Brace(tokens, "(");
                Text(tokens, name);
                Brace(tokens, "(");
                EmitExpr(left, tokens, false);
                Text(tokens, ", ");
                EmitExpr(right, tokens, false);
                Brace(tokens, ")");
                if (parenthesize)
                    Brace(tokens, ")");
            }

            std::string FieldName(const HighLevelILInstruction &instr, size_t memberIndex, uint64_t offset) const {
                try {
                    const auto type = instr.GetType().GetValue();
                    if (type && type->IsStructure()) {
                        const auto members = type->GetStructure()->GetMembers();
                        if (memberIndex < members.size() && !members[memberIndex].name.empty())
                            return members[memberIndex].name;
                    }
                } catch (...) {
                }
                return std::format("field_{:x}", offset);
            }

            void EmitFloatConstant(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens) {
                const uint64_t bits = static_cast<uint64_t>(instr.GetConstant<HLIL_FLOAT_CONST>());
                if (instr.size == 4)
                    Float(tokens, static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(bits))));
                else
                    Float(tokens, std::bit_cast<double>(bits));
            }

            void EmitIntrinsic(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, bool parenthesize) {
                const uint32_t intrinsic = instr.GetIntrinsic<HLIL_INTRINSIC>();
                std::string name;
                try {
                    if (Ref<Architecture> architecture = GetArchitecture())
                        name = architecture->GetIntrinsicName(intrinsic);
                } catch (...) {
                }
                if (name.empty())
                    name = std::format("intrinsic_{}", intrinsic);

                const auto parameters = instr.GetParameterExprs<HLIL_INTRINSIC>();
                const char *op = name == "luau_mod" ? "%" : (name == "luau_idiv" ? "//" : (name == "luau_pow" ? "^" : nullptr));
                if (op && parameters.size() >= 2) {
                    EmitBinary(op, parameters[0], parameters[1], tokens, parenthesize);
                    return;
                }
                EmitCallLike(name, parameters, tokens);
            }

            void EmitStructInit(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens) {
                Brace(tokens, "{");
                bool first = true;
                for (const auto &field : instr.GetFieldExprs<HLIL_STRUCT_INIT>()) {
                    if (!first)
                        Text(tokens, ", ");
                    const std::string name = FieldName(field, field.GetMemberIndex<HLIL_STRUCT_INIT_FIELD>(), field.GetOffset<HLIL_STRUCT_INIT_FIELD>());
                    Field(tokens, name);
                    Op(tokens, " = ");
                    EmitExpr(field.GetSourceExpr<HLIL_STRUCT_INIT_FIELD>(), tokens, false);
                    first = false;
                }
                Brace(tokens, "}");
            }

            void EmitExpr(const HighLevelILInstruction &instr, HighLevelILTokenEmitter &tokens, bool parenthesize) {
                switch (instr.operation) {
                case HLIL_CONST:
                    Integer(tokens, instr.GetConstant<HLIL_CONST>());
                    break;

                case HLIL_CONST_DATA: {
                    const ConstantData data = instr.GetConstantData<HLIL_CONST_DATA>();
                    tokens.Append(IntegerToken, std::format("0x{:x}", static_cast<uint64_t>(data.value)));
                    break;
                }

                case HLIL_CONST_PTR:
                    EmitSymbolOrAddress(tokens, static_cast<uint64_t>(instr.GetConstant<HLIL_CONST_PTR>()));
                    break;

                case HLIL_EXTERN_PTR:
                    Op(tokens, "*");
                    Brace(tokens, "(");
                    EmitSymbolOrAddress(tokens, static_cast<uint64_t>(instr.GetConstant<HLIL_EXTERN_PTR>()));
                    Op(tokens, " + ");
                    Hex(tokens, static_cast<uint64_t>(instr.GetOffset<HLIL_EXTERN_PTR>()));
                    Brace(tokens, ")");
                    break;

                case HLIL_IMPORT:
                    EmitSymbolOrAddress(tokens, static_cast<uint64_t>(instr.GetConstant<HLIL_IMPORT>()));
                    break;

                case HLIL_FLOAT_CONST:
                    EmitFloatConstant(instr, tokens);
                    break;

                case HLIL_VAR:
                    Var(tokens, VariableName(instr.GetVariable<HLIL_VAR>()));
                    break;

                case HLIL_VAR_SSA:
                    Var(tokens, VariableName(instr.GetSSAVariable<HLIL_VAR_SSA>()));
                    break;

                case HLIL_VAR_SSA_PARTIAL:
                    Var(tokens, VariableName(instr.GetDestSSAVariable<HLIL_VAR_SSA_PARTIAL>()));
                    break;

                case HLIL_ADDRESS_OF:
                    EmitUnary("&", instr.GetSourceExpr<HLIL_ADDRESS_OF>(), tokens, parenthesize);
                    break;

                case HLIL_DEREF:
                    EmitUnary("*", instr.GetSourceExpr<HLIL_DEREF>(), tokens, parenthesize);
                    break;

                case HLIL_DEREF_SSA:
                    EmitUnary("*", instr.GetSourceExpr<HLIL_DEREF_SSA>(), tokens, parenthesize);
                    break;

                case HLIL_DEREF_FIELD:
                case HLIL_DEREF_FIELD_SSA: {
                    const auto &source =
                        instr.operation == HLIL_DEREF_FIELD ? instr.GetSourceExpr<HLIL_DEREF_FIELD>() : instr.GetSourceExpr<HLIL_DEREF_FIELD_SSA>();
                    const size_t memberIndex =
                        instr.operation == HLIL_DEREF_FIELD ? instr.GetMemberIndex<HLIL_DEREF_FIELD>() : instr.GetMemberIndex<HLIL_DEREF_FIELD_SSA>();
                    const uint64_t offset = instr.operation == HLIL_DEREF_FIELD ? instr.GetOffset<HLIL_DEREF_FIELD>() : instr.GetOffset<HLIL_DEREF_FIELD_SSA>();
                    EmitExpr(source, tokens, true);
                    Op(tokens, ".");
                    Field(tokens, FieldName(instr, memberIndex, offset));
                    break;
                }

                case HLIL_STRUCT_FIELD:
                    EmitExpr(instr.GetSourceExpr<HLIL_STRUCT_FIELD>(), tokens, true);
                    Op(tokens, ".");
                    Field(tokens, FieldName(instr, instr.GetMemberIndex<HLIL_STRUCT_FIELD>(), instr.GetOffset<HLIL_STRUCT_FIELD>()));
                    break;

                case HLIL_ARRAY_INDEX:
                    EmitExpr(instr.GetSourceExpr<HLIL_ARRAY_INDEX>(), tokens, true);
                    Brace(tokens, "[");
                    EmitExpr(instr.GetIndexExpr<HLIL_ARRAY_INDEX>(), tokens, false);
                    Brace(tokens, "]");
                    break;

                case HLIL_ARRAY_INDEX_SSA:
                    EmitExpr(instr.GetSourceExpr<HLIL_ARRAY_INDEX_SSA>(), tokens, true);
                    Brace(tokens, "[");
                    EmitExpr(instr.GetIndexExpr<HLIL_ARRAY_INDEX_SSA>(), tokens, false);
                    Brace(tokens, "]");
                    break;

                case HLIL_SPLIT:
                    Brace(tokens, "{");
                    EmitExpr(instr.GetHighExpr<HLIL_SPLIT>(), tokens, false);
                    Text(tokens, ", ");
                    EmitExpr(instr.GetLowExpr<HLIL_SPLIT>(), tokens, false);
                    Brace(tokens, "}");
                    break;

                case HLIL_PASS_BY_REF:
                    EmitExpr(instr.GetSourceExpr<HLIL_PASS_BY_REF>(), tokens, parenthesize);
                    break;

                case HLIL_RETURN_BY_REF:
                    EmitExpr(instr.GetSourceExpr<HLIL_RETURN_BY_REF>(), tokens, parenthesize);
                    break;

                case HLIL_CALL:
                    EmitCall(instr.GetDestExpr<HLIL_CALL>(), instr.GetParameterExprs<HLIL_CALL>(), tokens);
                    break;

                case HLIL_CALL_SSA:
                    EmitCall(instr.GetDestExpr<HLIL_CALL_SSA>(), instr.GetParameterExprs<HLIL_CALL_SSA>(), tokens);
                    break;

                case HLIL_SYSCALL:
                case HLIL_SYSCALL_SSA:
                    EmitCallLike("syscall", instr.GetParameterExprs<HLIL_SYSCALL>(), tokens);
                    break;

                case HLIL_TAILCALL:
                    EmitCall(instr.GetDestExpr<HLIL_TAILCALL>(), instr.GetParameterExprs<HLIL_TAILCALL>(), tokens);
                    break;

                case HLIL_INTRINSIC:
                case HLIL_INTRINSIC_SSA:
                    EmitIntrinsic(instr, tokens, parenthesize);
                    break;

                case HLIL_ADD:
                case HLIL_FADD:
                    EmitBinary("+", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_ADC:
                    EmitExpr(instr.GetLeftExpr<HLIL_ADC>(), tokens, true);
                    Op(tokens, " + ");
                    EmitExpr(instr.GetRightExpr<HLIL_ADC>(), tokens, true);
                    Op(tokens, " + ");
                    EmitExpr(instr.GetCarryExpr<HLIL_ADC>(), tokens, true);
                    break;

                case HLIL_SUB:
                case HLIL_FSUB:
                    EmitBinary("-", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_SBB:
                    EmitExpr(instr.GetLeftExpr<HLIL_SBB>(), tokens, true);
                    Op(tokens, " - ");
                    EmitExpr(instr.GetRightExpr<HLIL_SBB>(), tokens, true);
                    Op(tokens, " - ");
                    EmitExpr(instr.GetCarryExpr<HLIL_SBB>(), tokens, true);
                    break;

                case HLIL_MUL:
                case HLIL_FMUL:
                case HLIL_MULU_DP:
                case HLIL_MULS_DP:
                    EmitBinary("*", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_DIVU:
                case HLIL_DIVS:
                case HLIL_DIVU_DP:
                case HLIL_DIVS_DP:
                case HLIL_FDIV:
                    EmitBinary("/", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_MODU:
                case HLIL_MODS:
                case HLIL_MODU_DP:
                case HLIL_MODS_DP:
                    EmitBinary("%", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_AND:
                    EmitTwoArgCall("bit32.band", instr.GetLeftExpr<HLIL_AND>(), instr.GetRightExpr<HLIL_AND>(), tokens, parenthesize);
                    break;
                case HLIL_OR:
                    EmitTwoArgCall("bit32.bor", instr.GetLeftExpr<HLIL_OR>(), instr.GetRightExpr<HLIL_OR>(), tokens, parenthesize);
                    break;
                case HLIL_XOR:
                    EmitTwoArgCall("bit32.bxor", instr.GetLeftExpr<HLIL_XOR>(), instr.GetRightExpr<HLIL_XOR>(), tokens, parenthesize);
                    break;
                case HLIL_LSL:
                    EmitTwoArgCall("bit32.lshift", instr.GetLeftExpr<HLIL_LSL>(), instr.GetRightExpr<HLIL_LSL>(), tokens, parenthesize);
                    break;
                case HLIL_LSR:
                    EmitTwoArgCall("bit32.rshift", instr.GetLeftExpr<HLIL_LSR>(), instr.GetRightExpr<HLIL_LSR>(), tokens, parenthesize);
                    break;
                case HLIL_ASR:
                    EmitTwoArgCall("bit32.arshift", instr.GetLeftExpr<HLIL_ASR>(), instr.GetRightExpr<HLIL_ASR>(), tokens, parenthesize);
                    break;
                case HLIL_ROL:
                case HLIL_RLC:
                    EmitTwoArgCall("bit32.lrotate", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_ROR:
                case HLIL_RRC:
                    EmitTwoArgCall("bit32.rrotate", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_TEST_BIT:
                    EmitTwoArgCall("bit32.btest", instr.GetLeftExpr<HLIL_TEST_BIT>(), instr.GetRightExpr<HLIL_TEST_BIT>(), tokens, parenthesize);
                    break;

                case HLIL_ADD_OVERFLOW:
                    EmitBinary("+", instr.GetLeftExpr<HLIL_ADD_OVERFLOW>(), instr.GetRightExpr<HLIL_ADD_OVERFLOW>(), tokens, parenthesize);
                    break;

                case HLIL_NEG:
                case HLIL_FNEG:
                    EmitUnary("-", instr.GetSourceExpr(), tokens, parenthesize);
                    break;

                case HLIL_NOT:
                    EmitUnary("not ", instr.GetSourceExpr<HLIL_NOT>(), tokens, parenthesize);
                    break;

                case HLIL_SX:
                case HLIL_ZX:
                case HLIL_LOW_PART:
                case HLIL_INT_TO_FLOAT:
                case HLIL_FLOAT_CONV:
                    EmitExpr(instr.GetSourceExpr(), tokens, parenthesize);
                    break;

                case HLIL_BOOL_TO_INT:
                    Brace(tokens, "(");
                    EmitExpr(instr.GetSourceExpr<HLIL_BOOL_TO_INT>(), tokens, false);
                    Keyword(tokens, " and ");
                    Integer(tokens, 1);
                    Keyword(tokens, " or ");
                    Integer(tokens, 0);
                    Brace(tokens, ")");
                    break;

                case HLIL_FLOAT_TO_INT:
                case HLIL_ROUND_TO_INT:
                    EmitOneArgCall("math.round", instr.GetSourceExpr(), tokens, parenthesize);
                    break;

                case HLIL_FLOOR:
                    EmitOneArgCall("math.floor", instr.GetSourceExpr<HLIL_FLOOR>(), tokens, parenthesize);
                    break;
                case HLIL_CEIL:
                    EmitOneArgCall("math.ceil", instr.GetSourceExpr<HLIL_CEIL>(), tokens, parenthesize);
                    break;
                case HLIL_FTRUNC:
                    EmitOneArgCall("math.trunc", instr.GetSourceExpr<HLIL_FTRUNC>(), tokens, parenthesize);
                    break;
                case HLIL_FSQRT:
                    EmitOneArgCall("math.sqrt", instr.GetSourceExpr<HLIL_FSQRT>(), tokens, parenthesize);
                    break;
                case HLIL_FABS:
                case HLIL_ABS:
                    EmitOneArgCall("math.abs", instr.GetSourceExpr(), tokens, parenthesize);
                    break;

                case HLIL_CMP_E:
                case HLIL_FCMP_E:
                    EmitBinary("==", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_CMP_NE:
                case HLIL_FCMP_NE:
                    EmitBinary("~=", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_CMP_SLT:
                case HLIL_CMP_ULT:
                case HLIL_FCMP_LT:
                    EmitBinary("<", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_CMP_SLE:
                case HLIL_CMP_ULE:
                case HLIL_FCMP_LE:
                    EmitBinary("<=", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_CMP_SGT:
                case HLIL_CMP_UGT:
                case HLIL_FCMP_GT:
                    EmitBinary(">", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_CMP_SGE:
                case HLIL_CMP_UGE:
                case HLIL_FCMP_GE:
                    EmitBinary(">=", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_FCMP_O:
                    // Ordered: neither operand is NaN.
                    Brace(tokens, "(");
                    EmitExpr(instr.GetLeftExpr<HLIL_FCMP_O>(), tokens, true);
                    Op(tokens, " == ");
                    EmitExpr(instr.GetLeftExpr<HLIL_FCMP_O>(), tokens, true);
                    Keyword(tokens, " and ");
                    EmitExpr(instr.GetRightExpr<HLIL_FCMP_O>(), tokens, true);
                    Op(tokens, " == ");
                    EmitExpr(instr.GetRightExpr<HLIL_FCMP_O>(), tokens, true);
                    Brace(tokens, ")");
                    break;

                case HLIL_FCMP_UO:
                    // Unordered: at least one operand is NaN.
                    Keyword(tokens, "not ");
                    Brace(tokens, "(");
                    EmitExpr(instr.GetLeftExpr<HLIL_FCMP_UO>(), tokens, true);
                    Op(tokens, " == ");
                    EmitExpr(instr.GetLeftExpr<HLIL_FCMP_UO>(), tokens, true);
                    Keyword(tokens, " and ");
                    EmitExpr(instr.GetRightExpr<HLIL_FCMP_UO>(), tokens, true);
                    Op(tokens, " == ");
                    EmitExpr(instr.GetRightExpr<HLIL_FCMP_UO>(), tokens, true);
                    Brace(tokens, ")");
                    break;

                case HLIL_BSWAP:
                    EmitOneArgCall("bit32.byteswap", instr.GetSourceExpr<HLIL_BSWAP>(), tokens, parenthesize);
                    break;
                case HLIL_POPCNT:
                    EmitOneArgCall("bit32.countbits", instr.GetSourceExpr<HLIL_POPCNT>(), tokens, parenthesize);
                    break;
                case HLIL_CLZ:
                    EmitOneArgCall("bit32.countlz", instr.GetSourceExpr<HLIL_CLZ>(), tokens, parenthesize);
                    break;
                case HLIL_CTZ:
                    EmitOneArgCall("bit32.countrz", instr.GetSourceExpr<HLIL_CTZ>(), tokens, parenthesize);
                    break;
                case HLIL_RBIT:
                    EmitOneArgCall("bit32.rbit", instr.GetSourceExpr<HLIL_RBIT>(), tokens, parenthesize);
                    break;
                case HLIL_CLS:
                    EmitOneArgCall("bit32.cls", instr.GetSourceExpr<HLIL_CLS>(), tokens, parenthesize);
                    break;

                case HLIL_MINS:
                case HLIL_MINU:
                    EmitTwoArgCall("math.min", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;
                case HLIL_MAXS:
                case HLIL_MAXU:
                    EmitTwoArgCall("math.max", instr.GetLeftExpr(), instr.GetRightExpr(), tokens, parenthesize);
                    break;

                case HLIL_STRUCT_INIT:
                    EmitStructInit(instr, tokens);
                    break;
                case HLIL_STRUCT_INIT_FIELD:
                    EmitExpr(instr.GetSourceExpr<HLIL_STRUCT_INIT_FIELD>(), tokens, parenthesize);
                    break;

                case HLIL_VAR_PHI:
                    Var(tokens, VariableName(instr.GetDestSSAVariable<HLIL_VAR_PHI>()));
                    break;
                case HLIL_MEM_PHI:
                    Keyword(tokens, "nil");
                    break;

                case HLIL_UNREACHABLE:
                    Keyword(tokens, "error");
                    Brace(tokens, "(");
                    Str(tokens, "\"unreachable\"");
                    Brace(tokens, ")");
                    break;

                case HLIL_UNDEF:
                case HLIL_UNIMPL:
                case HLIL_UNIMPL_MEM:
                    Keyword(tokens, "nil");
                    break;

                case HLIL_NOP:
                    break;

                default:
                    Comment(tokens, "-- HLIL op " + std::to_string(static_cast<int>(instr.operation)));
                    break;
                }
            }
        };

        class PseudoLuauLanguageType final : public LanguageRepresentationFunctionType {
          public:
            PseudoLuauLanguageType() : LanguageRepresentationFunctionType("Pseudo Luau") {}

            Ref<LanguageRepresentationFunction> Create(Architecture *arch, Function *owner, HighLevelILFunction *highLevelIL) override {
                try {
                    // The base constructor dereferences all three objects, so never
                    // construct a representation with a missing high-level IL.
                    if (!arch || !owner || !highLevelIL)
                        return nullptr;
                    if (Ref<BinaryView> view = owner->GetView(); view && view->GetTypeName() == "LuauBytecode")
                        return new PseudoLuauFunction(this, arch, owner, highLevelIL);
                    return new PseudoLuauILFunction(this, arch, owner, highLevelIL);
                } catch (...) {
                    return nullptr;
                }
            }

            // One entry in the dropdown. For Luau bytecode it renders Fission IR;
            // for every other view it renders the LLIL/HLIL mapping.
            bool IsValid(BinaryView *view) override { return view != nullptr; }

            std::vector<DisassemblyTextLine> GetFunctionTypeTokens(Function *func, DisassemblySettings *settings = nullptr) override {
                (void)settings;
                std::vector<DisassemblyTextLine> lines;
                try {
                    if (!func)
                        return lines;

                    const ProtoView *proto = FunctionProto(func);
                    if (proto && proto->proto) {
                        const auto image = ImageRegistry::Find(proto->address);
                        if (image) {
                            DisassemblyTextLine line;
                            AppendSourceLine(BuildProtoSignature(*image, proto->proto->bytecodeId).header, line);
                            lines.push_back(std::move(line));
                            return lines;
                        }
                    }

                    std::vector<std::string> parameters;
                    for (const auto &variable : func->GetParameterVariables().GetValue())
                        parameters.push_back(func->GetVariableNameOrDefault(variable));
                    if (func->HasVariableArguments().GetValue())
                        parameters.push_back("...");

                    std::string signature = "function " + (func->GetSymbol() ? func->GetSymbol()->GetShortName() : std::string("function")) + "(";
                    for (size_t i = 0; i < parameters.size(); ++i) {
                        if (i)
                            signature += ", ";
                        signature += parameters[i];
                    }
                    signature += ")";

                    DisassemblyTextLine line;
                    AppendSourceLine(signature, line);
                    lines.push_back(std::move(line));
                    return lines;
                } catch (...) {
                    return lines;
                }
            }
        };

    } // namespace

    LuauBinaryView::LuauBinaryView(BinaryView *data) : BinaryView("LuauBytecode", data->GetFile(), data), m_raw(data) {}

    uint64_t LuauBinaryView::PerformGetEntryPoint() const {
        if (!m_image)
            return 0;
        const ProtoView *main = m_image->MainProto();
        return main ? main->address : 0;
    }

    std::string LuauBinaryView::DecompilePseudoLuau() const {
        if (!m_image)
            return "-- no Luau image loaded\n";

        std::lock_guard lock(m_pseudoLuauMutex);
        if (m_pseudoLuauReady)
            return m_pseudoLuau;

        Decompiler decompiler;
        DecompilationResult result{};
        {
            std::lock_guard fissionLock(FissionOperationMutex());
            result = decompiler.DecompileVanillaBytecode(m_image->Bytes());
        }
        if (result.resultCode != DecompileResult::Success) {
            if (!result.decompilationOutput.empty())
                m_pseudoLuau = result.decompilationOutput;
            else
                m_pseudoLuau = std::format("--[[ Fission decompilation failed ({}): {} ]]", static_cast<int>(result.resultCode), result.errorMessage);
        } else {
            m_pseudoLuau = result.decompilationOutput;
        }
        m_pseudoLuauReady = true;
        return m_pseudoLuau;
    }

    bool LuauBinaryView::Init() {
        try {
            const uint64_t size = m_raw->GetLength();
            if (size < 3 || size > kMaximumImageSize)
                return false;

            std::string bytes(static_cast<size_t>(size), '\0');
            if (m_raw->Read(bytes.data(), 0, size) != size)
                return false;

            auto image = Image::Parse(std::move(bytes));
            if (!image)
                return false;
            m_image = image;

            const uint64_t base = image->BaseAddress();
            const auto codeRanges = CodeRanges(*image);

            // Split the file into executable instruction arrays and readable metadata.
            // Proto starts keep their file offsets; the architecture reports alignment 1
            // so BN accepts the unaligned starts instead of shifting them.
            uint64_t cursor = 0;
            for (const auto &range : codeRanges) {
                if (range.begin > cursor) {
                    const uint64_t length = range.begin - cursor;
                    AddAutoSegment(base + cursor, length, cursor, length, SegmentReadable | SegmentContainsData | SegmentDenyExecute);
                }
                const uint64_t length = range.end - range.begin;
                AddAutoSegment(
                    base + range.begin, length, range.begin, length, SegmentReadable | SegmentExecutable | SegmentContainsData | SegmentContainsCode
                );
                cursor = range.end;
            }
            if (cursor < image->FileSize())
                AddAutoSegment(
                    base + cursor, image->FileSize() - cursor, cursor, image->FileSize() - cursor, SegmentReadable | SegmentContainsData | SegmentDenyExecute
                );

            Ref<Architecture> architecture = Architecture::GetByName("luau-bytecode");
            if (!architecture)
                return false;
            m_architecture = architecture;
            SetDefaultArchitecture(architecture);

            Ref<Platform> platform = architecture->GetStandalonePlatform();
            if (!platform)
                return false;
            SetDefaultPlatform(platform);

            DebugLog(
                std::format(
                    "Init base=0x{:x} file=0x{:x} code=[0x{:x},0x{:x}) protos={} align={} platform={}", image->BaseAddress(), image->FileSize(),
                    image->CodeBase(), image->CodeEnd(), image->Protos().size(), architecture->GetInstructionAlignment(), platform->GetName()
                )
            );

            // Name the interned strings and give them real data variables so pointer
            // operands in the lifted IL resolve to readable text. `char` arrays are
            // rendered as strings by Binary Ninja instead of raw byte blobs.
            for (size_t i = 0; i < image->Bytecode().stringTable.size(); ++i) {
                if (i >= image->Bytecode().stringTableOffsets.size())
                    break;
                const auto &value = image->Bytecode().stringTable[i];
                const uint64_t offset = image->Bytecode().stringTableOffsets[i];
                if (value.empty() || offset >= image->FileSize() || offset + value.size() > image->FileSize())
                    continue;
                const uint64_t address = base + offset;
                DefineDataVariable(address, Type::ArrayType(Type::IntegerType(1, true), value.size()));
                DefineAutoSymbol(new Symbol(DataSymbol, std::format("luau_str_{:x}", offset), address));
            }

            size_t functionsAdded = 0;
            size_t functionsFailed = 0;
            for (const auto &proto : image->Protos()) {
                DefineAutoSymbol(new Symbol(FunctionSymbol, proto.name, proto.address));
                uint32_t firstWord = 0;
                m_raw->Read(&firstWord, proto.fileOffset, sizeof(firstWord));
                const uint8_t firstOpcode = static_cast<uint8_t>(firstWord & 0xFF);
                if (firstOpcode >= LOP__COUNT && functionsFailed < 5) {
                    DebugLog(
                        std::format(
                            "proto {} at 0x{:x} (file 0x{:x}) has invalid first opcode 0x{:x}", proto.proto->bytecodeId, proto.address, proto.fileOffset,
                            firstOpcode
                        )
                    );
                }

                // autoDiscovered=true makes Binary Ninja apply its "don't analyze
                // automatically discovered functions" setting, leaving every proto
                // with no disassembly. These are explicit view functions.
                Ref<Function> function = AddFunctionForAnalysis(platform, proto.address, false);
                if (!function) {
                    ++functionsFailed;
                    if (functionsFailed <= 5)
                        DebugLog(
                            std::format(
                                "AddFunctionForAnalysis failed for proto {} at 0x{:x} (file 0x{:x}, opcode 0x{:x})", proto.proto->bytecodeId, proto.address,
                                proto.fileOffset, firstOpcode
                            )
                        );
                    continue;
                }
                ++functionsAdded;
                const auto &deserialized = *proto.proto;
                function->SetComment(
                    std::format(
                        "Luau proto {}: {} params, {} upvalues, {} instructions{}", deserialized.bytecodeId, deserialized.numparams, deserialized.nups,
                        proto.instructionCount, deserialized.isvararg ? ", vararg" : ""
                    )
                );
            }
            DebugLog(std::format("function creation: added={} failed={}", functionsAdded, functionsFailed));

            if (const ProtoView *main = image->MainProto())
                AddEntryPointForAnalysis(platform, main->address);

            // Synthetic environment image. The function table is read-only; globals,
            // upvalues and table objects live in a writable .data region because the
            // VM can mutate them after load.
            const size_t functionCount = image->Bytecode().functions.size();
            const size_t globalCount = image->GlobalSlots().size();
            const size_t tableCount = image->TableSites().size();
            const uint64_t functionSize = functionCount * 8;
            const uint64_t globalSize = globalCount * 8;
            const uint64_t upvalueSize = image->UpvalueSize();
            const uint64_t tableSize = tableCount * Image::kTableStride;
            const Ref<Type> opaquePointer = Type::PointerType(8, Type::VoidType());

            if (functionSize) {
                AddAutoSegment(image->FunctionBase(), functionSize, 0, 0, SegmentReadable | SegmentContainsData | SegmentDenyExecute);
                AddAutoSection(".luau_functions", image->FunctionBase(), functionSize, ReadOnlyDataSectionSemantics);

                std::vector<std::pair<std::string, Ref<Type>>> fields;
                fields.reserve(functionCount);
                std::unordered_set<std::string> used;
                for (uint32_t protoId = 0; protoId < functionCount; ++protoId) {
                    const ProtoView *proto = image->ProtoByIndex(protoId);
                    std::string name = SanitizeFieldName(proto ? proto->name : std::string{}, std::format("proto_{}", protoId));
                    if (!used.insert(name).second)
                        name = SanitizeFieldName(std::format("{}_{}", name, protoId), "proto");
                    fields.emplace_back(name, opaquePointer);
                }
                DefineDataVariable(image->FunctionBase(), DefineStructType(*this, "LuauFunctionTable", fields));
                DefineAutoSymbol(new Symbol(DataSymbol, "luau_functions", image->FunctionBase()));
            }

            const Ref<Type> tableType = DefineTableType(*this, *m_architecture, *image);
            const Ref<Type> tablePointer = Type::PointerType(8, tableType);

            if (globalSize) {
                AddAutoSegment(image->GlobalBase(), globalSize, 0, 0, SegmentReadable | SegmentWritable | SegmentContainsData | SegmentDenyExecute);
                AddAutoSection(".luau_globals", image->GlobalBase(), globalSize, ReadWriteDataSectionSemantics);

                std::vector<std::pair<std::string, Ref<Type>>> fields;
                fields.reserve(globalCount);
                std::unordered_set<std::string> used;
                for (const auto &slot : image->GlobalSlots()) {
                    std::string name = slot.name;
                    for (char &character : name) {
                        const auto byte = static_cast<unsigned char>(character);
                        if (character != '.' && !std::isalnum(byte) && character != '_')
                            character = '_';
                    }
                    if (name.empty())
                        name = "global";
                    if (std::isdigit(static_cast<unsigned char>(name[0])))
                        name = "_" + name;
                    if (!used.insert(name).second)
                        name = SanitizeFieldName(std::format("{}_{}", name, fields.size()), "global");
                    const bool isTableGlobal = slot.name == "game" || slot.name == "workspace" || slot.name == "script";
                    fields.emplace_back(name, isTableGlobal ? tablePointer : (LooksLikeFunction(slot.name) ? opaquePointer : Type::IntegerType(8, true)));
                }
                DefineDataVariable(image->GlobalBase(), DefineStructType(*this, "LuauGlobals", fields));
                DefineAutoSymbol(new Symbol(DataSymbol, "luau_globals", image->GlobalBase()));
            }

            if (upvalueSize) {
                AddAutoSegment(image->UpvalueBase(0), upvalueSize, 0, 0, SegmentReadable | SegmentWritable | SegmentContainsData | SegmentDenyExecute);
                AddAutoSection(".luau_upvalues", image->UpvalueBase(0), upvalueSize, ReadWriteDataSectionSemantics);

                for (size_t protoId = 0; protoId < functionCount; ++protoId) {
                    const auto &proto = image->Bytecode().functions[protoId];
                    if (proto.nups == 0)
                        continue;
                    std::vector<std::pair<std::string, Ref<Type>>> fields;
                    fields.reserve(proto.nups);
                    for (uint8_t index = 0; index < proto.nups; ++index) {
                        fields.emplace_back(
                            std::format("upval_{}", index),
                            image->IsNumericUpvalue(static_cast<uint32_t>(protoId), index) ? Type::FloatType(8) : Type::IntegerType(8, true)
                        );
                    }
                    const uint64_t address = image->UpvalueBase(static_cast<uint32_t>(protoId));
                    DefineDataVariable(address, DefineStructType(*this, std::format("LuauUpvalues_{}", protoId), fields));
                    DefineAutoSymbol(new Symbol(DataSymbol, std::format("upvalues_{}", protoId), address));
                }
            }

            if (tableSize) {
                AddAutoSegment(image->TableBase(), tableSize, 0, 0, SegmentReadable | SegmentWritable | SegmentContainsData | SegmentDenyExecute);
                AddAutoSection(".luau_tables", image->TableBase(), tableSize, ReadWriteDataSectionSemantics);
                for (const auto &site : image->TableSites()) {
                    DefineDataVariable(site.address, tableType);
                    DefineAutoSymbol(new Symbol(DataSymbol, std::format("table_{}_{}", site.protoId, site.instructionIndex), site.address));
                }
            }

            DebugLog(
                std::format(
                    "synthetic functions=[0x{:x},+0x{:x}) globals=[0x{:x},+0x{:x}) tables={} upvalues=[0x{:x},+0x{:x})", image->FunctionBase(), functionSize,
                    image->GlobalBase(), globalSize, tableCount, image->UpvalueBase(0), upvalueSize
                )
            );

            return true;
        } catch (const std::exception &error) {
            DebugLog(std::format("Init failed: {}", error.what()));
            return false;
        } catch (...) {
            DebugLog("Init failed with a non-standard exception");
            return false;
        }
    }

    LuauBinaryViewType::LuauBinaryViewType() : BinaryViewType("LuauBytecode", "Luau Bytecode") {}

    Ref<BinaryView> LuauBinaryViewType::Create(BinaryView *data) {
        try {
            return new LuauBinaryView(data);
        } catch (...) {
            return nullptr;
        }
    }

    bool LuauBinaryViewType::IsTypeValidForData(BinaryView *data) {
        if (!data)
            return false;
        const uint64_t size = data->GetLength();
        if (size < 3 || size > kMaximumImageSize)
            return false;

        uint8_t header[2] = {};
        if (data->Read(header, 0, sizeof(header)) != sizeof(header))
            return false;

        // Cheap rejection for common file formats before paying for a full parse.
        const uint8_t version = header[0];
        if (version < LBC_VERSION_MIN || (version > LBC_VERSION_MAX && version != LBC_VERSION_CLASSES))
            return false;
        if (version >= 4 && (header[1] < LBC_TYPE_VERSION_MIN || header[1] > LBC_TYPE_VERSION_MAX))
            return false;

        std::string bytes(static_cast<size_t>(size), '\0');
        if (data->Read(bytes.data(), 0, size) != size)
            return false;

        try {
            // The assertion handler this installs is process-global, so the
            // plugin serializes every Fission entry point that uses it.
            std::lock_guard fissionLock(FissionOperationMutex());
            Fission::ScopedThrowingAssertHandler assertHandler;
            Fission::ScopedDecompileBudget budget(std::chrono::seconds(10));
            Deserializer deserializer;
            return deserializer.Deserialize(bytes).has_value();
        } catch (...) {
            return false;
        }
    }

    namespace {

        // Supplies Binary Ninja with a real prototype for each proto so HLIL shows
        // named parameters instead of a bare "arg @ R0" signature.
        class LuauDebugInfoParser : public CustomDebugInfoParser {
          public:
            LuauDebugInfoParser() : CustomDebugInfoParser("LuauBytecode") {}

            bool IsValid(Ref<BinaryView> view) override { return view && view->GetTypeName() == "LuauBytecode"; }

            bool ParseInfo(Ref<DebugInfo> info, Ref<BinaryView> view, Ref<BinaryView>, ProgressFunction) override {
                try {
                    auto *luauView = dynamic_cast<LuauBinaryView *>(view.operator->());
                    if (!luauView)
                        return false;
                    const auto image = luauView->GetImage();
                    if (!image)
                        return false;

                    Ref<Platform> platform = view->GetDefaultPlatform();
                    Ref<CallingConvention> convention = platform ? platform->GetDefaultCallingConvention() : nullptr;

                    for (const auto &proto : image->Protos()) {
                        if (!proto.proto)
                            continue;

                        std::vector<FunctionParameter> parameters;
                        parameters.reserve(proto.proto->numparams);
                        for (uint32_t i = 0; i < proto.proto->numparams; ++i) {
                            std::string name = std::format("arg{}", i);
                            for (const auto &local : proto.proto->locvars) {
                                if (local.reg == i && local.startpc == 0 && !local.varname.empty()) {
                                    name = local.varname;
                                    break;
                                }
                            }
                            parameters.emplace_back(name, Type::IntegerType(8, true));
                        }

                        Ref<Type> returnType = Type::IntegerType(8, true);
                        Ref<Type> functionType = Type::FunctionType(ReturnValue(returnType), convention, parameters, Confidence<bool>(proto.proto->isvararg));
                        DebugFunctionInfo function(proto.name, proto.name, proto.name, proto.address, functionType, platform, {}, {});
                        info->AddFunction(function);
                    }
                    return true;
                } catch (...) {
                    return false;
                }
            }
        };

    } // namespace

    void RegisterView() {
        DebugLog("Fission.BinaryNinja: registering binary view and Pseudo Luau language representation");
        BinaryViewType::Register(new LuauBinaryViewType());
        LanguageRepresentationFunctionType::Register(new PseudoLuauLanguageType());
        PluginCommand::Register(
            "Fission\\Show Pseudo-Luau (Whole Chunk)", "Decompile this bytecode with Fission and show the Luau source",
            [](BinaryView *view) {
                auto *luauView = dynamic_cast<LuauBinaryView *>(view);
                if (!luauView)
                    return;
                ShowPlainTextReport("Pseudo-Luau", luauView->DecompilePseudoLuau());
            },
            [](BinaryView *view) { return view && view->GetTypeName() == "LuauBytecode"; }
        );
        PluginCommand::RegisterForFunction(
            "Fission\\Show Pseudo-Luau (Function)", "Decompile this function with Fission and show only its Luau definition",
            [](BinaryView *, Function *function) {
                if (!function)
                    return;
                const ProtoView *proto = FunctionProto(function);
                if (!proto || !proto->proto)
                    return;
                const auto image = ImageRegistry::Find(proto->address);
                if (!image)
                    return;
                const std::string name = function->GetSymbol() ? function->GetSymbol()->GetShortName() : std::format("proto_{}", proto->proto->bytecodeId);
                ShowPlainTextReport("Pseudo-Luau: " + name, PseudoLuauSource(*image, proto->proto->bytecodeId));
            }
        );
        PluginCommand::RegisterForAddress(
            "Fission\\Show Pseudo-Luau (Address)", "Decompile the proto at the selected address and show only its Luau definition",
            [](BinaryView *, uint64_t address) {
                const auto image = ImageRegistry::Find(address);
                if (!image)
                    return;
                const ProtoView *proto = image->ProtoAtAddress(address);
                if (!proto || !proto->proto)
                    return;
                ShowPlainTextReport("Pseudo-Luau: " + proto->name, PseudoLuauSource(*image, proto->proto->bytecodeId));
            }
        );
        // Prototype injection is temporarily disabled while validating base
        // analysis; the body prologue still copies a0.. into R0...

        BinaryViewType::RegisterBinaryViewInitialAnalysisCompletionEvent([](BinaryView *view) {
            if (!view || view->GetTypeName() != "LuauBytecode")
                return;
            size_t functionCount = 0;
            BNFunction **functions = BNGetAnalysisFunctionList(view->GetObject(), &functionCount);
            size_t withBlocks = 0;
            size_t totalBlocks = 0;
            for (size_t i = 0; i < functionCount; ++i) {
                size_t blockCount = 0;
                BNBasicBlock **blocks = BNGetFunctionBasicBlockList(functions[i], &blockCount);
                if (blockCount)
                    ++withBlocks;
                totalBlocks += blockCount;
                BNFreeBasicBlockList(blocks, blockCount);
            }
            DebugLog(std::format("analysis complete: functions={} withBlocks={} totalBlocks={}", functionCount, withBlocks, totalBlocks));
            BNFreeFunctionList(functions, functionCount);
        });
    }

} // namespace fission::bn
