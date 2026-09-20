// Compare normalized execution traces from original and decompiled bytecode in a sandboxed Luau VM.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "lua.h"
#include "lualib.h"

namespace fuzz {

    struct SemTrace {
        enum class Status { Ok, Error, Timeout, LoadFailed };
        Status status = Status::LoadFailed;
        std::string trace;
        bool comparable = true;
        std::vector<std::optional<std::string>> prints;
        std::optional<std::string> error;
    };

    namespace detail {
        struct RunBudget {
            std::chrono::steady_clock::time_point deadline;
            bool expired = false;
            bool comparable = true;
            std::set<const void *> seenObjects;
            std::vector<std::optional<std::string>> prints;
        };

        // interrupt fires on VM back-edges/calls; only raise outside a GC step (gc < 0).
        inline void InterruptHook(lua_State *L, int gc) {
            if (gc >= 0)
                return;
            auto *budget = static_cast<RunBudget *>(lua_callbacks(L)->userdata);
            if (budget && std::chrono::steady_clock::now() > budget->deadline) {
                budget->expired = true;
                luaL_errorL(L, "FUZZ_TIMEOUT"); // not the luaL_error macro: ##__VA_ARGS__ trips -Werror=gnu-zero-variadic-macro-arguments
            }
        }

        inline void SerializeString(const char *s, size_t len, std::string &out) {
            constexpr char hex[] = "0123456789abcdef";
            out += '"';
            for (size_t i = 0; i < len; ++i) {
                const auto ch = static_cast<unsigned char>(s[i]);
                if (ch == '"' || ch == '\\') {
                    out += '\\';
                    out += static_cast<char>(ch);
                } else if (ch < 32 || ch >= 127) {
                    out += "\\x";
                    out += hex[ch >> 4];
                    out += hex[ch & 15];
                } else {
                    out += static_cast<char>(ch);
                }
            }
            out += '"';
        }

        inline void MarkIncomparable(lua_State *L) {
            auto *budget = static_cast<RunBudget *>(lua_callbacks(L)->userdata);
            if (budget)
                budget->comparable = false;
        }

        inline void SerializeValue(lua_State *L, int idx, std::string &out, int depth, std::set<const void *> &visited) {
            idx = lua_absindex(L, idx);
            switch (lua_type(L, idx)) {
            case LUA_TNIL:
                out += "nil";
                break;
            case LUA_TBOOLEAN:
                out += lua_toboolean(L, idx) ? "true" : "false";
                break;
            case LUA_TNUMBER: {
                char buf[40];
                std::snprintf(buf, sizeof(buf), "%.17g", lua_tonumber(L, idx));
                out += buf;
                break;
            }
            case LUA_TSTRING: {
                size_t len = 0;
                const char *s = lua_tolstring(L, idx, &len);
                SerializeString(s, len, out);
                break;
            }
            case LUA_TINTEGER:
                out += "i64(" + std::to_string(lua_tointeger64(L, idx, nullptr)) + ')';
                break;
            case LUA_TBUFFER: {
                auto *budget = static_cast<RunBudget *>(lua_callbacks(L)->userdata);
                if (budget && !budget->seenObjects.insert(lua_topointer(L, idx)).second)
                    MarkIncomparable(L);
                size_t len = 0;
                const auto *data = static_cast<const char *>(lua_tobuffer(L, idx, &len));
                out += "buffer:";
                SerializeString(data, len, out);
                break;
            }
            case LUA_TVECTOR: {
                const float *v = lua_tovector(L, idx);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "vec(%.9g,%.9g,%.9g)", v[0], v[1], v[2]);
                out += buf;
                break;
            }
            case LUA_TTABLE: {
                const void *ptr = lua_topointer(L, idx);
                auto *budget = static_cast<RunBudget *>(lua_callbacks(L)->userdata);
                if (budget && !budget->seenObjects.insert(ptr).second)
                    MarkIncomparable(L);
                if (lua_getmetatable(L, idx)) {
                    MarkIncomparable(L);
                    lua_pop(L, 1);
                }
                if (depth <= 0 || visited.count(ptr)) {
                    MarkIncomparable(L);
                    out += depth <= 0 ? "{...}" : "{cycle}";
                    break;
                }
                visited.insert(ptr);
                out += '{';
                std::vector<std::string> entries;
                lua_pushnil(L);
                while (lua_next(L, idx) != 0) {
                    std::string e;
                    SerializeValue(L, -2, e, 1, visited);
                    e += '=';
                    SerializeValue(L, -1, e, depth - 1, visited);
                    entries.push_back(std::move(e));
                    lua_pop(L, 1);
                }
                std::sort(entries.begin(), entries.end());
                for (const auto &e : entries) {
                    if (out.back() != '{')
                        out += ',';
                    out += e;
                }
                out += '}';
                visited.erase(ptr);
                break;
            }
            default:
                MarkIncomparable(L);
                out += '<';
                out += lua_typename(L, lua_type(L, idx));
                out += '>';
                break;
            }
        }

        // print override: serialize every arg deterministically, tab-separated, into the trace buffer
        // stashed in an upvalue-held lightuserdata.
        inline int TracePrint(lua_State *L) {
            auto *trace = static_cast<std::string *>(lua_tolightuserdata(L, lua_upvalueindex(1)));
            auto *budget = static_cast<RunBudget *>(lua_callbacks(L)->userdata);
            const bool earlierComparable = budget->comparable;
            budget->comparable = true;
            const size_t start = trace->size();
            const int argc = lua_gettop(L);
            std::set<const void *> visited;
            for (int i = 1; i <= argc; ++i) {
                if (i > 1)
                    *trace += '\t';
                SerializeValue(L, i, *trace, 4, visited);
            }
            *trace += '\n';
            budget->prints.push_back(budget->comparable ? std::optional(trace->substr(start)) : std::nullopt);
            budget->comparable &= earlierComparable;
            return 0;
        }

        // strip "[string \"...\"]:<line>:" location prefixes: original and decompiled sources have
        // different line numbers for the same semantic error.
        inline std::string NormalizeError(const std::string &msg) {
            static const std::regex locRe(R"((\[string \"[^\"]*\"\]|\bchunk):\d+:?\s*)");
            static const std::regex addressRe(R"((table|function|userdata|thread): 0x[0-9a-fA-F]+)");
            return std::regex_replace(std::regex_replace(msg, locRe, ""), addressRe, "$1: <address>");
        }
    } // namespace detail

    // Fixture preludes. A single environment only exercises one path per program; running under
    // several; plain data, an instrumented callable object, and hostile scalars; flips branches,
    // reaches method bodies, and makes effect ORDER observable (every metamethod hit prints a
    // marker). A faithful decompilation must trace-match under every fixture.
    inline const char *kSemPreludes[] = {
        // P0: plain tables (baseline; matches the original single-fixture oracle).
        "t = { 1, 2, 3, key = 'value', x = 5 }\n"
        "obj = { x = 1, y = 'hello', items = { 4, 5 } }\n"
        "math.randomseed(0)\n",
        // P1: instrumented; callable, method-bearing, and __index-traced, so `t(...)`,
        // `t:set(...)`, `obj:method()` etc. run instead of erroring, and every dynamic lookup
        // lands in the trace as an ordered marker.
        "local function mk(name)\n"
        "    local fixture = { 1, 2, 3, key = 'value', x = 5 }\n"
        "    return setmetatable(fixture, {\n"
        "        __call = function(self, ...) print('<call>', name, ...) return 1, 'r' end,\n"
        "        __index = function(self, k)\n"
        "            print('<index>', name, k)\n"
        "            return function(...) print('<method>', name, k, ...) return 2 end\n"
        "        end,\n"
        "    })\n"
        "end\n"
        "t = mk('t')\n"
        "obj = mk('obj')\n"
        "math.randomseed(0)\n",
        // P2: hostile scalars; forces the error/short-circuit paths from a different angle.
        "t = 0\n"
        "obj = 'hostile'\n"
        "math.randomseed(0)\n",
    };
    inline constexpr size_t kSemPreludeCount = sizeof(kSemPreludes) / sizeof(kSemPreludes[0]);

    // Execute one bytecode blob in a fresh sandboxed state; capture prints, returns, and errors.
    // preludeBc must be the compiled kSemPrelude (same compiler settings; callers reuse one blob).
    inline SemTrace
    RunLuauTrace(const std::string &bytecode, const std::string &preludeBc, std::chrono::milliseconds budget = std::chrono::milliseconds(1000)) {
        SemTrace result{};
        lua_State *L = luaL_newstate();
        if (!L)
            return result;
        luaL_openlibs(L);

        std::string trace;
        lua_pushlightuserdata(L, &trace);
        lua_pushcclosure(L, detail::TracePrint, "print", 1);
        lua_setglobal(L, "print");

        detail::RunBudget runBudget{};
        runBudget.deadline = std::chrono::steady_clock::now() + budget;
        lua_callbacks(L)->userdata = &runBudget;
        lua_callbacks(L)->interrupt = detail::InterruptHook;

        std::string errorMsg;
        const auto runChunk = [&](const std::string &bc, bool captureReturns) -> int {
            if (luau_load(L, "=chunk", bc.data(), bc.size(), 0) != 0) {
                lua_pop(L, 1);
                return -1; // load failure, not a runtime error
            }
            const int base = lua_gettop(L) - 1;
            const int status = lua_pcall(L, 0, LUA_MULTRET, 0);
            if (status == 0 && captureReturns) {
                const int nret = lua_gettop(L) - base;
                std::set<const void *> visited;
                for (int i = 0; i < nret; ++i) {
                    trace += i == 0 ? "return: " : "\t";
                    detail::SerializeValue(L, base + 1 + i, trace, 4, visited);
                }
                if (nret > 0)
                    trace += '\n';
            } else if (status != 0) {
                size_t len = 0;
                const char *msg = lua_tolstring(L, -1, &len); // must read before settop pops the error object
                if (!msg)
                    runBudget.comparable = false;
                errorMsg = msg ? std::string(msg, len) : "<non-string error>";
            }
            lua_settop(L, base);
            return status;
        };

        if (runChunk(preludeBc, false) != 0) {
            lua_close(L);
            return result; // fixture must run clean; anything else is a harness bug
        }

        const int status = runChunk(bytecode, true);
        if (status == -1) {
            result.status = SemTrace::Status::LoadFailed;
        } else if (status != 0) {
            const std::string err = detail::NormalizeError(errorMsg);
            if (runBudget.expired) {
                result.status = SemTrace::Status::Timeout;
            } else {
                result.status = SemTrace::Status::Error;
                if (errorMsg != "<non-string error>")
                    result.error = err;
                trace += "error: " + err + '\n';
            }
        } else {
            result.status = SemTrace::Status::Ok;
        }
        result.trace = std::move(trace);
        result.comparable = runBudget.comparable;
        result.prints = std::move(runBudget.prints);
        lua_close(L);
        return result;
    }

    struct SemVerdict {
        enum class Kind { Match, Diverge, Unrunnable } kind = Kind::Unrunnable;
        SemTrace original, decompiled; // for Diverge: the first diverging fixture's traces
        size_t fixture = 0;            // index of the fixture that diverged (or last judged)
    };

    // Equal errors alone do not establish coverage of the program body.
    inline SemVerdict CompareSemantics(const std::string &originalBc, const std::string &decompiledBc, const std::vector<std::string> &preludeBcs) {
        SemVerdict v{};
        bool judgedAny = false;
        bool incomplete = false;
        for (size_t i = 0; i < preludeBcs.size(); ++i) {
            SemTrace orig = RunLuauTrace(originalBc, preludeBcs[i]);
            if (orig.status == SemTrace::Status::Timeout || orig.status == SemTrace::Status::LoadFailed) {
                incomplete = true;
                continue;
            }
            SemTrace dec = RunLuauTrace(decompiledBc, preludeBcs[i]);
            v.fixture = i;
            bool printsDiffer = orig.prints.size() != dec.prints.size();
            for (size_t p = 0; !printsDiffer && p < orig.prints.size(); ++p)
                printsDiffer = orig.prints[p] && dec.prints[p] && orig.prints[p] != dec.prints[p];
            if (dec.status != orig.status || printsDiffer || (orig.error && dec.error && orig.error != dec.error) ||
                (orig.comparable && dec.comparable && dec.trace != orig.trace)) {
                v.kind = SemVerdict::Kind::Diverge;
                v.original = std::move(orig);
                v.decompiled = std::move(dec);
                return v;
            }
            incomplete |= !orig.comparable || !dec.comparable;
            judgedAny |= orig.status == SemTrace::Status::Ok && orig.comparable && dec.comparable;
            v.original = std::move(orig);
            v.decompiled = std::move(dec);
        }
        v.kind = judgedAny && !incomplete ? SemVerdict::Kind::Match : SemVerdict::Kind::Unrunnable;
        return v;
    }

    // compile all fixture preludes once (thread-safe local-static init in callers).
    inline std::vector<std::string> CompilePreludes(const std::function<bool(const std::string &, std::string *)> &compile) {
        std::vector<std::string> out;
        for (size_t i = 0; i < kSemPreludeCount; ++i) {
            std::string bc;
            if (!compile(kSemPreludes[i], &bc))
                return {};
            out.push_back(std::move(bc));
        }
        return out;
    }
} // namespace fuzz
