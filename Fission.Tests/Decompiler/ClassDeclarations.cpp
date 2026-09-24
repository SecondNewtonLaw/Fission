// Hand-build experimental V10 class opcodes and verify source reconstruction through the identity decoder.

#include "../../Fission.Fuzzing/include/SemanticOracle.hpp"
#include "Decompiler.hpp"
#include "Luau/BytecodeBuilder.h"
#include "Luau/Common.h"
#include "Luau/Compiler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <regex>
#include <string>

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

    // Decompile already-compiled, non-Roblox (identity-decoder) Luau bytecode.
    std::string DecompileVanillaOrFail(const std::string &bytecode) {
        Decompiler decompiler{};
        auto result = decompiler.DecompileVanillaBytecode(bytecode, static_cast<DecompilerFlags>(0));
        REQUIRE(result.resultCode == DecompileResult::Success);
        return std::move(result.decompilationOutput);
    }

    bool ContainsRegex(const std::string &haystack, const std::regex &pattern) { return std::regex_search(haystack, pattern); }

    bool Contains(const std::string &haystack, const std::string &needle) { return haystack.find(needle) != std::string::npos; }

    // true if `src` recompiles to valid bytecode (first byte 0 = Luau error marker).
    bool Recompiles(const std::string &src) {
        EnableLuauFFlagsOnce();
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string bc = Luau::compile(src, opts);
        return !bc.empty() && bc[0] != '\0';
    }

    // Hand-assemble bytecode for a v10 class-member registration:
    //   local C = {}              -- class table (NEWTABLE)
    //   C.greet = function() end  -- NEWCLASSMEMBER (A = class, C = closure, AUX = member name)
    //   return C
    std::string BuildClassMemberBytecode() {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        const std::string sGreet = "greet";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };

        const uint32_t child = bb.beginFunction(0, /*isvararg*/ false); // the method body
        bb.emitABC(LOP_RETURN, 0, 1, 0);
        bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);

        const uint32_t main = bb.beginFunction(0, /*isvararg*/ true);
        const int16_t childIdx = bb.addChildFunction(child);
        const int32_t cGreet = bb.addConstantString(sref(sGreet));
        bb.emitABC(LOP_NEWTABLE, 0, 0, 0); // R0 = {} (class table)
        bb.emitAux(0);
        bb.emitAD(LOP_NEWCLOSURE, 1, childIdx);  // R1 = function() end
        bb.emitABC(LOP_NEWCLASSMEMBER, 0, 0, 1); // R0.greet = R1
        bb.emitAux(static_cast<uint32_t>(cGreet));
        bb.emitABC(LOP_RETURN, 0, 2, 0); // return R0
        bb.endFunction(/*maxstacksize*/ 2, /*numupvalues*/ 0);
        bb.setMainFunction(main);
        bb.finalize();
        return bb.getBytecode();
    }

    // Hand-assemble bytecode for a full V10 class:
    //   class Animal
    //       public legs
    //       function speak(self) end
    //   end
    //   return Animal
    // The class value is a LOADKX of an LBC_CONSTANT_CLASS_SHAPE constant; each method is registered by a
    // NEWCLASSMEMBER on the class register.
    std::string BuildClassDeclarationBytecode() {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        const std::string sName = "Animal", sSpeak = "speak", sLegs = "legs";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };

        const uint32_t child = bb.beginFunction(1, /*isvararg*/ false); // method `speak(self)`
        bb.emitABC(LOP_RETURN, 0, 1, 0);
        bb.endFunction(/*maxstacksize*/ 1, /*numupvalues*/ 0);

        const uint32_t main = bb.beginFunction(0, /*isvararg*/ true);
        const int32_t cName = bb.addConstantString(sref(sName));
        const int32_t cLegs = bb.addConstantString(sref(sLegs));
        const int32_t cSpeak = bb.addConstantString(sref(sSpeak));
        Luau::BytecodeBuilder::ClassShape shape;
        shape.className = cName;
        shape.propertyNames = {cLegs};
        shape.methodNames = {cSpeak};
        const int32_t cShape = bb.addClassShape(std::move(shape));
        const int16_t childIdx = bb.addChildFunction(child);

        bb.emitAD(LOP_LOADKX, 0, 0); // R0 = class value (shape constant index in AUX)
        bb.emitAux(static_cast<uint32_t>(cShape));
        bb.emitAD(LOP_NEWCLOSURE, 1, childIdx);  // R1 = speak closure
        bb.emitABC(LOP_NEWCLASSMEMBER, 0, 0, 1); // R0.speak = R1
        bb.emitAux(static_cast<uint32_t>(cSpeak));
        bb.emitABC(LOP_RETURN, 0, 2, 0); // return R0
        bb.endFunction(/*maxstacksize*/ 2, /*numupvalues*/ 0);
        bb.setMainFunction(main);
        bb.finalize();
        return bb.getBytecode();
    }

    // class Animal { public legs; public name; function speak(self) end; function walk(self) end } ; return Animal
    std::string BuildMultiMemberClassBytecode() {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        const std::string sName = "Animal", sLegs = "legs", sNm = "name", sSpeak = "speak", sWalk = "walk";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };

        const uint32_t speakFn = bb.beginFunction(1, false);
        bb.emitABC(LOP_RETURN, 0, 1, 0);
        bb.endFunction(1, 0);
        const uint32_t walkFn = bb.beginFunction(1, false);
        bb.emitABC(LOP_RETURN, 0, 1, 0);
        bb.endFunction(1, 0);

        const uint32_t main = bb.beginFunction(0, true);
        const int32_t cName = bb.addConstantString(sref(sName));
        const int32_t cLegs = bb.addConstantString(sref(sLegs));
        const int32_t cNm = bb.addConstantString(sref(sNm));
        const int32_t cSpeak = bb.addConstantString(sref(sSpeak));
        const int32_t cWalk = bb.addConstantString(sref(sWalk));
        Luau::BytecodeBuilder::ClassShape shape;
        shape.className = cName;
        shape.propertyNames = {cLegs, cNm};
        shape.methodNames = {cSpeak, cWalk};
        const int32_t cShape = bb.addClassShape(std::move(shape));
        const int16_t iSpeak = bb.addChildFunction(speakFn);
        const int16_t iWalk = bb.addChildFunction(walkFn);

        bb.emitAD(LOP_LOADKX, 0, 0);
        bb.emitAux(static_cast<uint32_t>(cShape));
        bb.emitAD(LOP_NEWCLOSURE, 1, iSpeak); // R1 = speak (temp reused per method, as the compiler does)
        bb.emitABC(LOP_NEWCLASSMEMBER, 0, 0, 1);
        bb.emitAux(static_cast<uint32_t>(cSpeak));
        bb.emitAD(LOP_NEWCLOSURE, 1, iWalk); // R1 = walk
        bb.emitABC(LOP_NEWCLASSMEMBER, 0, 0, 1);
        bb.emitAux(static_cast<uint32_t>(cWalk));
        bb.emitABC(LOP_RETURN, 0, 2, 0);
        bb.endFunction(2, 0);
        bb.setMainFunction(main);
        bb.finalize();
        return bb.getBytecode();
    }

    // class Config { public host; public port } (no methods) ; return Config
    std::string BuildPropertiesOnlyClassBytecode() {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        const std::string sName = "Config", sHost = "host", sPort = "port";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };

        const uint32_t main = bb.beginFunction(0, true);
        const int32_t cName = bb.addConstantString(sref(sName));
        const int32_t cHost = bb.addConstantString(sref(sHost));
        const int32_t cPort = bb.addConstantString(sref(sPort));
        Luau::BytecodeBuilder::ClassShape shape;
        shape.className = cName;
        shape.propertyNames = {cHost, cPort};
        shape.methodNames = {};
        const int32_t cShape = bb.addClassShape(std::move(shape));
        bb.emitAD(LOP_LOADKX, 0, 0);
        bb.emitAux(static_cast<uint32_t>(cShape));
        bb.emitABC(LOP_RETURN, 0, 2, 0); // return R0
        bb.endFunction(1, 0);
        bb.setMainFunction(main);
        bb.finalize();
        return bb.getBytecode();
    }

    // class Box { function get(self) return self.value end } ; return Box  -- the method body reads the receiver.
    std::string BuildClassMethodSelfBodyBytecode() {
        EnableLuauFFlagsOnce();
        Luau::BytecodeBuilder bb{};
        const std::string sName = "Box", sGet = "get", sValue = "value";
        const auto sref = [](const std::string &s) { return Luau::BytecodeBuilder::StringRef{s.data(), s.size()}; };

        // method get(self): return self.value  (GETTABLEKS reads its key from the CHILD's own constants)
        const uint32_t getFn = bb.beginFunction(1, false);
        const int32_t cValue = bb.addConstantString(sref(sValue));
        bb.emitABC(LOP_GETTABLEKS, 1, 0, static_cast<uint8_t>(Luau::BytecodeBuilder::getStringHash(sref(sValue)))); // R1 = R0.value
        bb.emitAux(static_cast<uint32_t>(cValue));
        bb.emitABC(LOP_RETURN, 1, 2, 0); // return R1
        bb.endFunction(2, 0);

        const uint32_t main = bb.beginFunction(0, true);
        const int32_t cName = bb.addConstantString(sref(sName));
        const int32_t cGet = bb.addConstantString(sref(sGet));
        Luau::BytecodeBuilder::ClassShape shape;
        shape.className = cName;
        shape.propertyNames = {};
        shape.methodNames = {cGet};
        const int32_t cShape = bb.addClassShape(std::move(shape));
        const int16_t iGet = bb.addChildFunction(getFn);
        bb.emitAD(LOP_LOADKX, 0, 0);
        bb.emitAux(static_cast<uint32_t>(cShape));
        bb.emitAD(LOP_NEWCLOSURE, 1, iGet);
        bb.emitABC(LOP_NEWCLASSMEMBER, 0, 0, 1);
        bb.emitAux(static_cast<uint32_t>(cGet));
        bb.emitABC(LOP_RETURN, 0, 2, 0);
        bb.endFunction(2, 0);
        bb.setMainFunction(main);
        bb.finalize();
        return bb.getBytecode();
    }

    std::string BuildNewClassBytecode() {
        EnableLuauFFlagsOnce();
        Luau::FValue<bool> *classesFlag = nullptr;
        for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
            if (std::strcmp(flag->name, "DebugLuauUserDefinedClasses") == 0)
                classesFlag = flag;
        REQUIRE(classesFlag != nullptr);
        const bool previousClassesFlag = classesFlag->value;
        classesFlag->value = true;
        Luau::CompileOptions opts{};
        opts.optimizationLevel = 1;
        opts.debugLevel = 2;
        const std::string bytecode = Luau::compile(
            R"(
open class Animal
    public species: string
    function live(self)
        return "I am alive"
    end
end

export class Cat extends Animal
    public breed: string
    function describe(self)
        return self.breed
    end
end
)",
            opts
        );
        classesFlag->value = previousClassesFlag;
        return bytecode;
    }

    // Decompile identity-decoder bytecode with AST capture on, returning the whole result (astJson populated).
    DecompilationResult DecompileVanillaWithCaptures(const std::string &bytecode) {
        Decompiler decompiler{};
        return decompiler.DecompileVanillaBytecode(bytecode, DecompilerFlags::CaptureAST);
    }

    size_t CountMatches(const std::string &s, const std::regex &re) {
        return static_cast<size_t>(std::distance(std::sregex_iterator(s.begin(), s.end(), re), std::sregex_iterator()));
    }

    // Every {/[ closes with a matching }/]; strings are skipped. Enough to reject truncated/unbalanced JSON.
    bool BracesBalanced(const std::string &json) {
        int depth = 0;
        bool inStr = false, esc = false;
        for (char c : json) {
            if (inStr) {
                if (esc)
                    esc = false;
                else if (c == '\\')
                    esc = true;
                else if (c == '"')
                    inStr = false;
                continue;
            }
            if (c == '"')
                inStr = true;
            else if (c == '{' || c == '[')
                ++depth;
            else if (c == '}' || c == ']') {
                if (--depth < 0)
                    return false;
            }
        }
        return depth == 0 && !inStr;
    }

} // namespace

// NEWCLASSMEMBER (v10) registers a method on a class table. When the class register is a plain table (no
// class shape), it must decompile to a member assignment `<class>.greet = function ... end`, not be
// silently dropped.
TEST_CASE("Class: bare NEWCLASSMEMBER renders as a class member assignment", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildClassMemberBytecode());

    INFO("decompile:\n" << out);
    // Rendered with method-definition sugar: `function <class>.greet() ... end` (equivalent to
    // `<class>.greet = function ... end`). The key property is that the member registration survives.
    CHECK(ContainsRegex(out, std::regex(R"(function\s+\w+\.greet\s*\()")));
    CHECK(Recompiles(out));
}

// A LOADKX of a class-shape constant plus its NEWCLASSMEMBER methods must reconstruct a full
// `class Name ... end` declaration: the class name, its declared `public` properties, and each method.
// (No Recompiles() check: `class` is contextual syntax gated behind an experimental parser flag.)
TEST_CASE("Class: V10 class shape reconstructs a class declaration", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildClassDeclarationBytecode());

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(class\s+Animal)")));
    CHECK(ContainsRegex(out, std::regex(R"(public\s+legs)")));
    CHECK(ContainsRegex(out, std::regex(R"(function\s+speak\s*\()")));
    CHECK(ContainsRegex(out, std::regex(R"(return\s+Animal)")));
}

// Every property AND every method of a class must be reconstructed, in declaration order, all inside the
// class body; none dropped and none leaked back out as a top-level `local function` (the earlier defect).
TEST_CASE("Class: all properties and methods are reconstructed in order", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildMultiMemberClassBytecode());

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(class\s+Animal)")));
    // both properties present, in declaration order (legs before name).
    const auto legsPos = out.find("public legs");
    const auto namePos = out.find("public name");
    CHECK(legsPos != std::string::npos);
    CHECK(namePos != std::string::npos);
    CHECK(legsPos < namePos);
    // both methods present, in declaration order (speak before walk).
    const auto speakPos = out.find("function speak");
    const auto walkPos = out.find("function walk");
    CHECK(speakPos != std::string::npos);
    CHECK(walkPos != std::string::npos);
    CHECK(speakPos < walkPos);
    // exactly two method definitions and nothing leaked as a standalone `local function anon_N`.
    CHECK(CountMatches(out, std::regex(R"(\bfunction\s+\w+\s*\()")) == 2);
    CHECK_FALSE(Contains(out, "local function"));
}

// A class with only properties (no NEWCLASSMEMBER) still reconstructs a `class ... end` with each
// `public` line and no methods.
TEST_CASE("Class: properties-only class reconstructs with no methods", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildPropertiesOnlyClassBytecode());

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(class\s+Config)")));
    CHECK(ContainsRegex(out, std::regex(R"(public\s+host)")));
    CHECK(ContainsRegex(out, std::regex(R"(public\s+port)")));
    CHECK(ContainsRegex(out, std::regex(R"(\bend\b)")));
    CHECK_FALSE(Contains(out, "function")); // no methods
}

// A method that uses its receiver must name it `self` in BOTH the parameter list and the body; a single
// consistent identifier. Guards the desync where only the parameter was renamed while the body still read
// the raw register name.
TEST_CASE("Class: method receiver is `self` in both the signature and the body", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildClassMethodSelfBodyBytecode());

    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(function\s+get\s*\(\s*self\s*\))"))); // parameter is `self`
    CHECK(ContainsRegex(out, std::regex(R"(self\.value)")));                     // body reads `self.value`
    // no residual raw-register receiver leaked into the body (the desync symptom).
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\barg0\b)")));
    CHECK_FALSE(ContainsRegex(out, std::regex(R"(\bv0\b)")));
}

TEST_CASE("Class: NEWCLASS opcode decompiles compiler-emitted classes", "[Decompiler][Class]") {
    const auto out = DecompileVanillaOrFail(BuildNewClassBytecode());
    INFO("decompile:\n" << out);
    CHECK(ContainsRegex(out, std::regex(R"(open\s+class\s+Animal)")));
    CHECK(ContainsRegex(out, std::regex(R"(class\s+Cat\s+extends\s+Animal)")));
    CHECK(ContainsRegex(out, std::regex(R"(public\s+species)")));
    CHECK(ContainsRegex(out, std::regex(R"(public\s+breed)")));
    CHECK(ContainsRegex(out, std::regex(R"(function\s+live\s*\()")));
    CHECK(ContainsRegex(out, std::regex(R"(function\s+describe\s*\()")));
    CHECK(Contains(out, "table.freeze"));
}

TEST_CASE("Class: NEWCLASS preserves table exports and captured values", "[Decompiler][Class][ClassSemantics]") {
    EnableLuauFFlagsOnce();
    struct ClassFlags {
        std::vector<std::pair<Luau::FValue<bool> *, bool>> saved;
        ClassFlags() {
            for (auto *flag = Luau::FValue<bool>::list; flag; flag = flag->next)
                if (std::strcmp(flag->name, "DebugLuauUserDefinedClasses") == 0 || std::strcmp(flag->name, "DebugLuauUserDefinedClassesRuntime") == 0) {
                    saved.emplace_back(flag, flag->value);
                    flag->value = true;
                }
        }
        ~ClassFlags() {
            for (auto [flag, value] : saved)
                flag->value = value;
        }
    } flags;
    REQUIRE(flags.saved.size() == 2);
    const int optimization = GENERATE(0, 1, 2);
    const int debug = GENERATE(0, 2);
    std::string source;
    std::string expected;
    SECTION("ordinary class table member") {
        source = "class C end local t = {} t.C = C t.answer = 42 return t.answer";
        expected = "return: 42\n";
    }
    SECTION("mixed module exports") {
        source = "export class C end export local answer = 42";
        expected = "return: {\"C\"=<class>,\"answer\"=42}\n";
    }
    SECTION("multiple classes and value exports") {
        source = "export class C end export class D end export local answer = 42";
        expected = "return: {\"C\"=<class>,\"D\"=<class>,\"answer\"=42}\n";
    }
    SECTION("method captures table") {
        source = "local state = {} class C function get(self) return state end end return C.new({}):get()";
        expected = "return: {}\n";
    }
    SECTION("method preserves captured identity and mutation") {
        source = "local state = {n = 1} class C function get(self) return state end end "
                 "state.n = 42 local value = C.new({}):get() return value == state, value.n";
        expected = "return: true\t42\n";
    }
    SECTION("method retains binding before local shadow") {
        source = "local state = {} class C function get(self) return state end end "
                 "local state = {} return C.new({}):get() == state";
        expected = "return: false\n";
    }
    SECTION("method observes reassigned captured binding") {
        source = "local state = {} class C function get(self) return state end end "
                 "local previous = state state = {} return C.new({}):get() == state, C.new({}):get() == previous";
        expected = "return: true\tfalse\n";
    }
    SECTION("ordinary class table and unrelated frozen return") {
        source = "class C end local t = {} t.C = C local other = {answer = 42} return table.freeze(other)";
        expected = "return: {\"answer\"=42}\n";
    }
    SECTION("superclass read through table") {
        source = "open class Base function get(self) return 42 end end local t = {Base = Base} "
                 "class Child extends t.Base end return Child.new({}):get()";
        expected = "return: 42\n";
    }
    const Luau::CompileOptions options{optimization, debug};
    const auto bytecode = Luau::compile(source, options);
    REQUIRE(!bytecode.empty());
    REQUIRE(bytecode.front() != '\0');
    const auto output = DecompileVanillaOrFail(bytecode);
    INFO(output);
    const auto compiled = Luau::compile(output, options);
    REQUIRE(!compiled.empty());
    REQUIRE(compiled.front() != '\0');
    const auto prelude = Luau::compile("");
    const auto original = fuzz::RunLuauTrace(bytecode, prelude);
    const auto reconstructed = fuzz::RunLuauTrace(compiled, prelude);
    REQUIRE(original.status == fuzz::SemTrace::Status::Ok);
    REQUIRE(original.trace == expected);
    CHECK(reconstructed.status == original.status);
    CHECK(reconstructed.trace == original.trace);
}

TEST_CASE("Class: reused decompiler does not retain previous class registrations", "[Decompiler][Class][ClassSemantics]") {
    Decompiler decompiler;
    const auto first = decompiler.DecompileVanillaBytecode(BuildClassDeclarationBytecode());
    REQUIRE(first.resultCode == DecompileResult::Success);
    const auto second = decompiler.DecompileVanillaBytecode(BuildClassMemberBytecode());
    REQUIRE(second.resultCode == DecompileResult::Success);
    INFO(second.decompilationOutput);
    CHECK(ContainsRegex(second.decompilationOutput, std::regex(R"(function\s+\w+\.greet\s*\()")));
    CHECK(Recompiles(second.decompilationOutput));
}

// The reconstructed class must serialize through the AST-JSON path: a ClassDeclaration node with its name,
// a properties array, and a methods array carrying a FunctionDeclaration. Exercises
// AstJsonSerializer::Visit(ClassDeclarationNode) + its KindName case.
TEST_CASE("Class: class node serializes to well-formed AST JSON", "[Decompiler][Class][AST]") {
    const auto result = DecompileVanillaWithCaptures(BuildClassDeclarationBytecode());
    REQUIRE(result.resultCode == DecompileResult::Success);
    const std::string &json = result.astJson;

    INFO("astJson:\n" << json);
    REQUIRE_FALSE(json.empty());
    CHECK(BracesBalanced(json));
    CHECK(Contains(json, R"("kind":"ClassDeclaration")"));
    CHECK(Contains(json, R"("Animal")"));
    CHECK(Contains(json, R"("properties":)"));
    CHECK(Contains(json, R"("legs")"));
    CHECK(Contains(json, R"("methods":)"));
    CHECK(Contains(json, R"("kind":"FunctionDeclaration")"));
}
