#include "LuauAstGenerator.hpp"

// Luau's AST headers trip the project's -Werror=unused-parameter on their default visitor methods.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "Luau/Allocator.h"
#include "Luau/Ast.h"
#include "Luau/Location.h"
#include "Luau/Parser.h"
#pragma clang diagnostic pop

#include <algorithm>
#include <array>
#include <cstdio>
#include <deque>
#include <regex>
#include <set>
#include <string>
#include <vector>

namespace {
    using namespace Luau;

    // Owns the allocator + string arena for one chunk. AstName holds a bare const char*, so the
    // backing std::string must outlive the AST; the deque keeps every interned string stable.
    struct Builder {
        Allocator alloc;
        std::deque<std::string> strings;
        std::vector<AstLocal *> scope;
        bool inVararg = true; // top-level chunk is a vararg function; `...` is only valid where this holds
        uint32_t st;
        const Location L{};

        explicit Builder(uint32_t seed) : st(seed) {}

        uint32_t next() {
            uint32_t x = st;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            st = x;
            return x;
        }
        int rnd(int n) { return n <= 0 ? 0 : static_cast<int>(next() % static_cast<uint32_t>(n)); }
        bool chance(int p) { return rnd(100) < p; }

        AstName name(const std::string &s) {
            strings.push_back(s);
            return AstName(strings.back().c_str());
        }
        template <typename T> AstArray<T> arr(const std::vector<T> &v) {
            AstArray<T> a{};
            a.size = v.size();
            a.data = v.empty() ? nullptr : static_cast<T *>(alloc.allocate(sizeof(T) * v.size()));
            for (size_t i = 0; i < v.size(); ++i)
                a.data[i] = v[i];
            return a;
        }
        AstArray<char> chars(const std::string &s) {
            AstArray<char> a{};
            a.size = s.size();
            a.data = static_cast<char *>(alloc.allocate(s.empty() ? 1 : s.size()));
            for (size_t i = 0; i < s.size(); ++i)
                a.data[i] = s[i];
            return a;
        }
        template <typename T, typename... A> T *mk(A &&...args) { return alloc.alloc<T>(std::forward<A>(args)...); }

        AstLocal *mkLocal(const std::string &n) {
            return mk<AstLocal>(name(n), L, /*shadow*/ nullptr, /*fnDepth*/ 0u, /*loopDepth*/ 0u, /*annotation*/ nullptr);
        }

        AstExpr *strConst(const std::string &s) { return mk<AstExprConstantString>(L, chars(s), AstExprConstantString::QuoteStyle::QuotedSimple); }
        AstExpr *global() {
            static const std::array<const char *, 12> g = {"print", "math",   "table",  "string", "tostring", "tonumber",
                                                           "pairs", "ipairs", "select", "next",   "t",        "obj"};
            return mk<AstExprGlobal>(L, name(g[rnd(static_cast<int>(g.size()))]));
        }
        AstExpr *leaf() {
            static const std::array<const char *, 6> s = {"", "x", "hello", "a-b", "key", "value"};
            switch (rnd(7)) {
            case 0:
                return mk<AstExprConstantNumber>(L, static_cast<double>(rnd(1000)) / (chance(50) ? 1.0 : 4.0));
            case 1:
                return strConst(s[rnd(static_cast<int>(s.size()))]);
            case 2:
                return mk<AstExprConstantBool>(L, chance(50));
            case 3:
                return mk<AstExprConstantNil>(L);
            case 4:
                return (!scope.empty() && chance(70)) ? mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false) : global();
            default:
                return global();
            }
        }

        // A valid prefixexp (var | call | `(expr)`); required as the base of index/call.
        AstExpr *prefix(int depth) {
            if (depth <= 0)
                return (!scope.empty() && chance(60)) ? mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false) : global();
            switch (rnd(5)) {
            case 0:
                return global();
            case 1:
                return mk<AstExprIndexName>(L, prefix(depth - 1), name("field"), L, Position(0, 0), '.');
            case 2:
                return mk<AstExprIndexExpr>(L, prefix(depth - 1), expr(depth - 1));
            case 3:
                return mk<AstExprGroup>(L, expr(depth - 1));
            default:
                return (!scope.empty()) ? mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false) : global();
            }
        }

        AstExpr *call(int depth) {
            std::vector<AstExpr *> args;
            const int n = rnd(3);
            for (int i = 0; i < n; ++i)
                args.push_back(expr(depth - 1));
            AstArray<AstTypeOrPack> noTypes{}; // a406 AstExprCall carries explicit type args; we emit none
            return mk<AstExprCall>(L, prefix(depth - 1), arr(args), /*self*/ false, noTypes, L);
        }

        // method call `obj:m(args)`; AstExprCall with self=true and a `:`-indexed callee.
        AstExpr *methodCall(int depth) {
            static const std::array<const char *, 4> methods = {"method", "get", "set", "run"};
            AstExpr *callee = mk<AstExprIndexName>(L, prefix(depth - 1), name(methods[rnd(static_cast<int>(methods.size()))]), L, Position(0, 0), ':');
            std::vector<AstExpr *> args;
            const int n = rnd(3);
            for (int i = 0; i < n; ++i)
                args.push_back(expr(depth - 1));
            AstArray<AstTypeOrPack> noTypes{};
            return mk<AstExprCall>(L, callee, arr(args), /*self*/ true, noTypes, L);
        }

        // function body: statements + a trailing return. params/outer locals are in scope, so a
        // reference to an outer local prints as an upvalue capture (closure).
        AstStatBlock *funcBody(int depth, bool vararg) {
            const bool savedVararg = inVararg;
            inVararg = vararg;
            std::vector<AstStat *> stats;
            const int k = rnd(4);
            for (int i = 0; i < k; ++i)
                stats.push_back(stat(depth, /*inLoop*/ false));
            std::vector<AstExpr *> rets;
            const int n = rnd(3);
            for (int i = 0; i < n; ++i)
                rets.push_back(expr(depth - 1));
            stats.push_back(mk<AstStatReturn>(L, arr(rets)));
            inVararg = savedVararg;
            return mk<AstStatBlock>(L, arr(stats), /*hasEnd*/ true);
        }

        // anonymous function literal `function(params) ... end`; may be vararg and may capture upvalues.
        AstExprFunction *funcExpr(int depth) {
            const size_t mark = scope.size();
            std::vector<AstLocal *> params;
            const int np = rnd(4);
            for (int i = 0; i < np; ++i) {
                AstLocal *p = mkLocal("p" + std::to_string(scope.size()));
                params.push_back(p);
                scope.push_back(p);
            }
            const bool vararg = chance(40);
            AstStatBlock *body = funcBody(depth - 1, vararg);
            scope.resize(mark); // params + body locals leave scope at function end
            return mk<AstExprFunction>(
                L, AstArray<AstAttr *>{}, AstArray<AstGenericType *>{}, AstArray<AstGenericTypePack *>{},
                /*self*/ nullptr, arr(params), vararg, L, body, /*funcDepth*/ 0u, AstName{}, /*returnAnnotation*/ nullptr,
                /*varargAnnotation*/ nullptr, std::nullopt
            );
        }

        AstExpr *table(int depth) {
            static const std::array<const char *, 5> keys = {"x", "y", "field", "data", "k"};
            std::vector<AstExprTable::Item> items;
            const int count = rnd(5);
            for (int i = 0; i < count; ++i) {
                const int kind = rnd(3);
                AstExprTable::Item it{};
                if (kind == 0) {
                    it.kind = AstExprTable::Item::Kind::List;
                    it.key = nullptr;
                    it.value = expr(depth - 1);
                } else if (kind == 1) {
                    it.kind = AstExprTable::Item::Kind::Record;
                    it.key = strConst(keys[rnd(static_cast<int>(keys.size()))]);
                    it.value = expr(depth - 1);
                } else {
                    it.kind = AstExprTable::Item::Kind::General;
                    it.key = expr(depth - 1);
                    it.value = expr(depth - 1);
                }
                items.push_back(it);
            }
            return mk<AstExprTable>(L, arr(items));
        }

        AstExpr *expr(int depth) {
            if (depth <= 0)
                return leaf();
            switch (rnd(13)) {
            case 0:
            case 1:
                return leaf();
            case 2: {
                static const std::array<AstExprBinary::Op, 16> ops = {AstExprBinary::Add,       AstExprBinary::Sub,       AstExprBinary::Mul,
                                                                      AstExprBinary::Div,       AstExprBinary::FloorDiv,  AstExprBinary::Mod,
                                                                      AstExprBinary::Pow,       AstExprBinary::Concat,    AstExprBinary::CompareNe,
                                                                      AstExprBinary::CompareEq, AstExprBinary::CompareLt, AstExprBinary::CompareLe,
                                                                      AstExprBinary::CompareGt, AstExprBinary::CompareGe, AstExprBinary::And,
                                                                      AstExprBinary::Or};
                return mk<AstExprBinary>(L, ops[rnd(static_cast<int>(ops.size()))], expr(depth - 1), expr(depth - 1));
            }
            case 3: {
                if (chance(30)) // `- -x`: stresses the decompiler's double-minus comment guard on round-trip
                    return mk<AstExprUnary>(L, AstExprUnary::Op::Minus, mk<AstExprUnary>(L, AstExprUnary::Op::Minus, expr(depth - 1)));
                const AstExprUnary::Op o = rnd(3) == 0 ? AstExprUnary::Op::Not : (rnd(2) == 0 ? AstExprUnary::Op::Minus : AstExprUnary::Op::Len);
                return mk<AstExprUnary>(L, o, expr(depth - 1));
            }
            case 4:
                return mk<AstExprIndexName>(L, prefix(depth - 1), name("field"), L, Position(0, 0), '.');
            case 5:
                return mk<AstExprIndexExpr>(L, prefix(depth - 1), expr(depth - 1));
            case 6:
                return table(depth - 1);
            case 7:
                return call(depth - 1);
            case 8:
                return mk<AstExprGroup>(L, expr(depth - 1));
            case 9:
                return methodCall(depth - 1);
            case 10:
                return funcExpr(depth - 1);
            case 11: // `if c then a else b` expression (printer parenthesises it)
                return mk<AstExprIfElse>(L, expr(depth - 1), /*hasThen*/ true, expr(depth - 1), /*hasElse*/ true, expr(depth - 1));
            case 12:
                return inVararg ? static_cast<AstExpr *>(mk<AstExprVarargs>(L)) : leaf(); // `...` only in vararg scope
            default:
                return leaf();
            }
        }

        AstStatBlock *block(int depth, bool inLoop) {
            const size_t mark = scope.size();
            std::vector<AstStat *> stats;
            const int k = rnd(4);
            for (int i = 0; i < k; ++i)
                stats.push_back(stat(depth, inLoop));
            if (inLoop && chance(25)) {
                // Guard break/continue with a condition. A bare unconditional break/continue as a loop's
                // body makes the loop iterate zero/one time, so Luau prunes the back-edge and the loop
                // structure is gone from the bytecode; not faithfully recoverable (a degenerate input,
                // not appropriate code). `if <cond> then break end` keeps the loop real and reconstructable.
                AstStat *bc = chance(50) ? static_cast<AstStat *>(mk<AstStatBreak>(L)) : static_cast<AstStat *>(mk<AstStatContinue>(L));
                auto *thenB = mk<AstStatBlock>(L, arr<AstStat *>({bc}), /*hasEnd*/ true);
                stats.push_back(mk<AstStatIf>(L, expr(depth), thenB, nullptr, std::optional<Location>{L}, std::nullopt));
            }
            scope.resize(mark); // locals declared in this block leave scope at its end
            return mk<AstStatBlock>(L, arr(stats), /*hasEnd*/ true);
        }

        AstStat *stat(int depth, bool inLoop) {
            if (depth <= 0)
                return mk<AstStatExpr>(L, call(1));
            switch (rnd(15)) {
            case 0:
            case 1: { // local v = expr
                AstExpr *val = expr(depth);
                AstLocal *var = mkLocal("v" + std::to_string(scope.size()));
                auto *s = mk<AstStatLocal>(L, arr<AstLocal *>({var}), arr<AstExpr *>({val}), std::optional<Location>{L});
                scope.push_back(var);
                return s;
            }
            case 2: { // assignment to an in-scope local or a global
                AstExpr *target =
                    (!scope.empty() && chance(60)) ? static_cast<AstExpr *>(mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false)) : global();
                return mk<AstStatAssign>(L, arr<AstExpr *>({target}), arr<AstExpr *>({expr(depth)}));
            }
            case 3:
                return mk<AstStatExpr>(L, call(depth)); // call statement
            case 4: {                                   // if
                AstStatBlock *thenB = block(depth - 1, inLoop);
                AstStat *elseB = chance(45) ? static_cast<AstStat *>(block(depth - 1, inLoop)) : nullptr;
                return mk<AstStatIf>(L, expr(depth), thenB, elseB, std::optional<Location>{L}, elseB ? std::optional<Location>{L} : std::nullopt);
            }
            case 5:
                return mk<AstStatWhile>(L, expr(depth), block(depth - 1, true), /*hasDo*/ true, L);
            case 6: { // numeric for; the loop var is in scope only inside the body
                AstLocal *var = mkLocal("i" + std::to_string(scope.size()));
                AstExpr *from = expr(depth - 1);
                AstExpr *to = expr(depth - 1);
                AstExpr *step = chance(50) ? expr(depth - 1) : nullptr;
                scope.push_back(var);
                AstStatBlock *body = block(depth - 1, true);
                scope.pop_back();
                return mk<AstStatFor>(L, var, from, to, step, body, /*hasDo*/ true, L);
            }
            case 7: { // local function; name is in scope inside its own body (recursion) and after
                AstLocal *fn = mkLocal("f" + std::to_string(scope.size()));
                scope.push_back(fn);
                AstExprFunction *body = funcExpr(depth - 1);
                return mk<AstStatLocalFunction>(L, fn, body, /*isConst*/ false, /*constKeywordBegin*/ Position(0, 0));
            }
            case 8: { // generic for: `for k, v in <iterator> do ... end`
                std::vector<AstLocal *> vars;
                const int nv = 1 + rnd(2);
                for (int i = 0; i < nv; ++i)
                    vars.push_back(mkLocal("g" + std::to_string(scope.size()) + "_" + std::to_string(i)));
                std::vector<AstExpr *> values;
                values.push_back(call(depth - 1)); // an iterator expression, e.g. pairs(t)
                const size_t mark = scope.size();
                for (auto *v : vars)
                    scope.push_back(v);
                AstStatBlock *body = block(depth - 1, /*inLoop*/ true);
                scope.resize(mark);
                return mk<AstStatForIn>(L, arr(vars), arr(values), body, /*hasIn*/ true, L, /*hasDo*/ true, L);
            }
            case 9: { // repeat ... until <cond>
                AstStatBlock *body = block(depth - 1, /*inLoop*/ true);
                return mk<AstStatRepeat>(L, expr(depth), body, /*DEPRECATED_hasUntil*/ true);
            }
            case 10: { // local v1, v2[, v3] = e1, e2[, e3]; multiple locals + values (register fan-out)
                const int n = 2 + rnd(2);
                std::vector<AstLocal *> vars;
                std::vector<AstExpr *> vals;
                for (int i = 0; i < n; ++i) {
                    vars.push_back(mkLocal("v" + std::to_string(scope.size() + i)));
                    // last value is sometimes a call, so the locals absorb a multi-return expansion.
                    vals.push_back((i == n - 1 && chance(40)) ? call(depth - 1) : expr(depth - 1));
                }
                auto *s = mk<AstStatLocal>(L, arr(vars), arr(vals), std::optional<Location>{L});
                for (auto *v : vars)
                    scope.push_back(v);
                return s;
            }
            case 11: { // a, b = e1, e2; parallel assignment (stresses temp/ordering during stores)
                if (scope.size() < 2)
                    return mk<AstStatExpr>(L, call(depth));
                std::vector<AstExpr *> targets, vals;
                const int n = 2;
                for (int i = 0; i < n; ++i) {
                    targets.push_back(mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false));
                    vals.push_back(expr(depth - 1));
                }
                return mk<AstStatAssign>(L, arr(targets), arr(vals));
            }
            case 12: { // a += e; compound assignment (ADDK/SUBK/.../CONCAT store paths)
                static const std::array<AstExprBinary::Op, 8> ops = {AstExprBinary::Add,      AstExprBinary::Sub, AstExprBinary::Mul, AstExprBinary::Div,
                                                                     AstExprBinary::FloorDiv, AstExprBinary::Mod, AstExprBinary::Pow, AstExprBinary::Concat};
                AstExpr *target =
                    (!scope.empty() && chance(70)) ? static_cast<AstExpr *>(mk<AstExprLocal>(L, scope[rnd(static_cast<int>(scope.size()))], false)) : global();
                return mk<AstStatCompoundAssign>(L, ops[rnd(static_cast<int>(ops.size()))], target, expr(depth - 1));
            }
            default:
                return mk<AstStatExpr>(L, call(depth));
            }
        }

        AstStatBlock *sugarChunk(bool methods, bool interactions = false) {
            const auto num = [&](double v) { return mk<AstExprConstantNumber>(L, v); };
            const auto local = [&](AstLocal *v) { return mk<AstExprLocal>(L, v, false); };
            const auto block = [&](std::vector<AstStat *> body) { return mk<AstStatBlock>(L, arr(body), true); };
            const auto field = [&](AstLocal *v, const char *key, char op = '.') { return mk<AstExprIndexName>(L, local(v), name(key), L, Position(0, 0), op); };
            const auto call = [&](AstExpr *callee, std::vector<AstExpr *> args, bool self = false) {
                return mk<AstExprCall>(L, callee, arr(args), self, AstArray<AstTypeOrPack>{}, L);
            };
            const auto print = [&](std::vector<AstExpr *> args) { return mk<AstStatExpr>(L, call(mk<AstExprGlobal>(L, name("print")), args)); };
            const auto decl = [&](AstLocal *v, AstExpr *value) {
                return mk<AstStatLocal>(L, arr<AstLocal *>({v}), arr<AstExpr *>({value}), std::optional<Location>{L});
            };
            const auto function = [&](AstLocal *self, std::vector<AstLocal *> args, std::vector<AstStat *> body) {
                return mk<AstExprFunction>(
                    L, AstArray<AstAttr *>{}, AstArray<AstGenericType *>{}, AstArray<AstGenericTypePack *>{}, self, arr(args), false, L, block(body), 0u,
                    AstName{}, nullptr, nullptr, std::nullopt
                );
            };
            const auto table = [&](AstExpr *value) {
                AstExprTable::Item record{}, item{};
                record.kind = AstExprTable::Item::Kind::Record;
                record.key = strConst("value");
                record.value = value;
                item.kind = AstExprTable::Item::Kind::List;
                item.value = value;
                return mk<AstExprTable>(L, arr<AstExprTable::Item>({record, item}));
            };
            auto *value = mkLocal("value"), *step = mkLocal("step"), *state = mkLocal("state");
            const int initial = 2 + rnd(8), delta = 1 + rnd(3);
            std::vector<AstStat *> stats;
            if (interactions) {
                auto *backing = mkLocal("backing"), *proxy = mkLocal("proxy"), *total = mkLocal("total");
                auto *ignored = mkLocal("ignored"), *key = mkLocal("key"), *assigned = mkLocal("assigned");
                const auto slot = [&](AstLocal *v, AstExpr *index) { return mk<AstExprIndexExpr>(L, local(v), index); };
                std::vector<AstExprTable::Item> hooks;
                const auto hook = [&](const char *name, AstExprFunction *fn) {
                    AstExprTable::Item item{};
                    item.kind = AstExprTable::Item::Kind::Record;
                    item.key = strConst(name);
                    item.value = fn;
                    hooks.push_back(item);
                };
                hook(
                    "__index",
                    function(nullptr, {ignored, key}, {print({strConst("get"), local(key)}), mk<AstStatReturn>(L, arr<AstExpr *>({slot(backing, local(key))}))})
                );
                hook(
                    "__newindex", function(
                                      nullptr, {ignored, key, assigned},
                                      {print({strConst("set"), local(key), local(assigned)}),
                                       mk<AstStatAssign>(L, arr<AstExpr *>({slot(backing, local(key))}), arr<AstExpr *>({local(assigned)}))}
                                  )
                );
                stats.push_back(decl(backing, table(num(initial))));
                stats.push_back(decl(
                    proxy,
                    call(mk<AstExprGlobal>(L, name("setmetatable")), {mk<AstExprTable>(L, AstArray<AstExprTable::Item>{}), mk<AstExprTable>(L, arr(hooks))})
                ));
                auto *receiver = mkLocal("receiver"), *operand = mkLocal("operand"), *capture = mkLocal("capture");
                stats.push_back(
                    mk<AstStatLocalFunction>(
                        L, receiver, function(nullptr, {}, {print({strConst("receiver")}), mk<AstStatReturn>(L, arr<AstExpr *>({local(proxy)}))}), false,
                        Position(0, 0)
                    )
                );
                stats.push_back(
                    mk<AstStatLocalFunction>(
                        L, operand,
                        function(nullptr, {value}, {print({strConst("rhs"), local(value)}), mk<AstStatReturn>(L, arr<AstExpr *>({local(value), num(999)}))}),
                        false, Position(0, 0)
                    )
                );
                stats.push_back(decl(total, num(0)));
                auto *outer = mkLocal("outer"), *inner = mkLocal("inner");
                auto *skip = mk<AstExprBinary>(L, AstExprBinary::CompareEq, mk<AstExprBinary>(L, AstExprBinary::Mod, local(inner), num(2)), num(0));
                auto *target = mk<AstExprIndexName>(L, call(local(receiver), {}), name("value"), L, Position(0, 0), '.');
                auto *innerBody = block(
                    {mk<AstStatIf>(L, skip, block({mk<AstStatContinue>(L)}), nullptr, std::optional<Location>{L}, std::nullopt),
                     mk<AstStatCompoundAssign>(L, AstExprBinary::Add, target, call(local(operand), {local(inner)})),
                     mk<AstStatCompoundAssign>(L, AstExprBinary::Add, local(total), field(backing, "value"))}
                );
                stats.push_back(
                    mk<AstStatFor>(
                        L, outer, num(1), num(2), nullptr, block({mk<AstStatFor>(L, inner, num(1), num(2 + rnd(5)), nullptr, innerBody, true, L)}), true, L
                    )
                );
                stats.push_back(
                    mk<AstStatLocalFunction>(
                        L, capture,
                        function(
                            nullptr, {step},
                            {mk<AstStatCompoundAssign>(L, AstExprBinary::Add, local(total), local(step)),
                             mk<AstStatReturn>(L, arr<AstExpr *>({local(total), field(backing, "value")}))}
                        ),
                        false, Position(0, 0)
                    )
                );
                stats.push_back(print({call(local(capture), {num(delta)})}));
                stats.push_back(mk<AstStatReturn>(L, arr<AstExpr *>({local(total), field(backing, "value")})));
                return block(stats);
            }
            if (!methods) {
                const AstExprBinary::Op ops[] = {AstExprBinary::Add,      AstExprBinary::Sub, AstExprBinary::Mul, AstExprBinary::Div,
                                                 AstExprBinary::FloorDiv, AstExprBinary::Mod, AstExprBinary::Pow};
                const auto op = ops[rnd(7)];
                auto *text = mkLocal("text"), *update = mkLocal("update");
                auto *slot = mk<AstExprIndexExpr>(L, local(state), num(1));
                std::vector<AstStat *> body{decl(state, table(local(value)))};
                AstExpr *target = slot, *rhs = local(step);
                if (chance(50)) {
                    auto *key = mkLocal("key"), *operand = mkLocal("operand");
                    body.push_back(
                        mk<AstStatLocalFunction>(
                            L, key, function(nullptr, {}, {print({strConst("key")}), mk<AstStatReturn>(L, arr<AstExpr *>({num(1)}))}), false, Position(0, 0)
                        )
                    );
                    body.push_back(
                        mk<AstStatLocalFunction>(
                            L, operand, function(nullptr, {}, {print({strConst("rhs")}), mk<AstStatReturn>(L, arr<AstExpr *>({local(step)}))}), false,
                            Position(0, 0)
                        )
                    );
                    target = mk<AstExprIndexExpr>(L, local(state), call(local(key), {}));
                    rhs = call(local(operand), {});
                }
                const std::vector<AstStat *> updates = {
                    mk<AstStatCompoundAssign>(L, op, local(value), local(step)), mk<AstStatCompoundAssign>(L, op, field(state, "value"), local(step)),
                    mk<AstStatCompoundAssign>(L, op, target, rhs), mk<AstStatCompoundAssign>(L, AstExprBinary::Concat, local(text), strConst("!")),
                    mk<AstStatReturn>(L, arr<AstExpr *>({local(value), field(state, "value"), slot, local(text)}))
                };
                body.insert(body.end(), updates.begin(), updates.end());
                auto *fn = function(nullptr, {value, step, text}, body);
                stats.push_back(mk<AstStatLocalFunction>(L, update, fn, false, Position(0, 0)));
                stats.push_back(print({call(local(update), {num(initial), num(delta), strConst("start")})}));
                stats.push_back(print({call(local(update), {num(initial + 1), num(delta + 1), strConst("next")})}));
            } else {
                auto *self = mkLocal("self"), *result = mkLocal("result");
                auto *positive = mk<AstExprBinary>(L, AstExprBinary::CompareGt, local(step), num(0));
                auto *tail = mk<AstStatIf>(
                    L, positive, block({print({strConst("up")})}), block({print({strConst("down")})}), std::optional<Location>{L}, std::optional<Location>{L}
                );
                auto *branch = mk<AstStatIf>(
                    L, mk<AstExprBinary>(L, AstExprBinary::CompareEq, local(step), num(0)), block({print({strConst("zero")})}), tail,
                    std::optional<Location>{L}, std::optional<Location>{L}
                );
                auto *fn = function(
                    self, {step},
                    {mk<AstStatCompoundAssign>(L, AstExprBinary::Add, field(self, "value"), local(step)),
                     decl(
                         result,
                         mk<AstExprIfElse>(L, positive, true, field(self, "value"), true, mk<AstExprUnary>(L, AstExprUnary::Op::Minus, field(self, "value")))
                     ),
                     branch, mk<AstStatReturn>(L, arr<AstExpr *>({local(result)}))}
                );
                stats.push_back(decl(state, table(num(initial))));
                stats.push_back(mk<AstStatFunction>(L, field(state, "bump", ':'), fn));
                for (int argument : {0, delta, -delta})
                    stats.push_back(print({call(field(state, "bump", ':'), {num(argument)}, true)}));
            }
            return block(stats);
        }

        AstStatBlock *guardChunk() {
            const double n = static_cast<double>(1 + rnd(20));
            auto *run = mkLocal("run");
            auto *p = mkLocal("p");
            auto *ds = mkLocal("ds");
            auto *body = mk<AstStatBlock>(
                L,
                arr<AstStat *>({mk<AstStatIf>(
                    L,
                    mk<AstExprBinary>(
                        L, AstExprBinary::Or,
                        mk<AstExprBinary>(
                            L, AstExprBinary::And, mk<AstExprUnary>(L, AstExprUnary::Op::Not, mk<AstExprLocal>(L, p, false)),
                            mk<AstExprIndexExpr>(L, mk<AstExprLocal>(L, ds, false), mk<AstExprConstantNumber>(L, 1))
                        ),
                        mk<AstExprBinary>(
                            L, AstExprBinary::And,
                            mk<AstExprBinary>(
                                L, AstExprBinary::And, mk<AstExprLocal>(L, p, false),
                                mk<AstExprIndexExpr>(L, mk<AstExprLocal>(L, ds, false), mk<AstExprConstantNumber>(L, 2))
                            ),
                            mk<AstExprBinary>(
                                L, AstExprBinary::CompareGt, mk<AstExprLocal>(L, p, false),
                                mk<AstExprIndexExpr>(L, mk<AstExprLocal>(L, ds, false), mk<AstExprConstantNumber>(L, 2))
                            )
                        )
                    ),
                    mk<AstStatBlock>(
                        L,
                        arr<AstStat *>({mk<AstStatExpr>(
                            L,
                            mk<AstExprCall>(
                                L, mk<AstExprGlobal>(L, name("print")), arr<AstExpr *>({mk<AstExprConstantNumber>(L, n)}), false, AstArray<AstTypeOrPack>{}, L
                            )
                        )}),
                        true
                    ),
                    nullptr, std::optional<Location>{L}, std::nullopt
                )}),
                true
            );
            auto *fn = mk<AstExprFunction>(
                L, AstArray<AstAttr *>{}, AstArray<AstGenericType *>{}, AstArray<AstGenericTypePack *>{}, nullptr, arr<AstLocal *>({p, ds}), false, L, body, 0u,
                AstName{}, nullptr, nullptr, std::nullopt
            );
            std::vector<AstStat *> stats{mk<AstStatLocalFunction>(L, run, fn, false, Position(0, 0))};
            for (int i = 0; i < 3; ++i) {
                std::vector<AstExprTable::Item> items;
                for (int j = 0; j < 2; ++j) {
                    AstExprTable::Item item{};
                    item.kind = AstExprTable::Item::Kind::List;
                    item.value = mk<AstExprConstantNumber>(L, n + j);
                    items.push_back(item);
                }
                AstExpr *value = i == 0 ? static_cast<AstExpr *>(mk<AstExprConstantNil>(L)) : mk<AstExprConstantNumber>(L, n + (i == 1 ? 2 : 0));
                stats.push_back(
                    mk<AstStatExpr>(
                        L, mk<AstExprCall>(
                               L, mk<AstExprLocal>(L, run, false), arr<AstExpr *>({value, mk<AstExprTable>(L, arr(items))}), false, AstArray<AstTypeOrPack>{}, L
                           )
                    )
                );
            }
            return mk<AstStatBlock>(L, arr(stats), true);
        }

        AstStatBlock *chunk() {
            std::vector<AstStat *> stats;
            const int k = 3 + rnd(8);
            for (int i = 0; i < k; ++i)
                stats.push_back(stat(2 + rnd(3), /*inLoop*/ false));
            std::vector<AstExpr *> rets;
            const int n = 1 + rnd(2);
            for (int i = 0; i < n; ++i)
                rets.push_back(expr(2));
            stats.push_back(mk<AstStatReturn>(L, arr(rets)));
            return mk<AstStatBlock>(L, arr(stats), /*hasEnd*/ true);
        }

        std::string structuredChunk(int variant) {
            const int value = 2 + rnd(7);
            switch (variant) {
            case 0:
                return "-- fuzz-case: branches\n"
                       "local value = " +
                       std::to_string(value) +
                       "\nlocal trace = {}\n"
                       "local guard = type(t) == \"table\"\n"
                       "if guard then\n"
                       "    trace[1] = value\n"
                       "else\n"
                       "    trace[1] = value + 1\n"
                       "end\n"
                       "return trace[1], guard\n";
            case 1:
                return "-- fuzz-case: loop-control\n"
                       "local total = 0\n"
                       "for i = 1, " +
                       std::to_string(value + 5) +
                       " do\n"
                       "    if i % 2 == 0 then\n"
                       "        continue\n"
                       "    end\n"
                       "    total += i\n"
                       "    if total > " +
                       std::to_string(value + 7) +
                       " then\n"
                       "        break\n"
                       "    end\n"
                       "end\n"
                       "return total\n";
            case 2:
                return "-- fuzz-case: multivalue\n"
                       "local function pair(x)\n"
                       "    return x, x + 1\n"
                       "end\n"
                       "local first, second = pair(" +
                       std::to_string(value) +
                       ")\n"
                       "first, second = second, first\n"
                       "return first, second, pair(first + second)\n";
            default:
                return "-- fuzz-case: closure-state\n"
                       "local state = { value = " +
                       std::to_string(value) +
                       " }\n"
                       "local function makeCounter(step)\n"
                       "    return function(extra)\n"
                       "        state.value += step\n"
                       "        return state.value + extra\n"
                       "    end\n"
                       "end\n"
                       "local counter = makeCounter(2)\n"
                       "local first = counter(1)\n"
                       "local second = counter(0)\n"
                       "return first, second, state.value\n";
            }
        }
    };

    // This Luau version has no transpiler, so print the generated subset directly.
    // Binary/unary/group are fully parenthesised so precedence is always correct without tracking it;
    // the decompiler re-parses the source, so redundant parens are harmless.
    const char *BinOp(AstExprBinary::Op op) {
        switch (op) {
        case AstExprBinary::Add:
            return "+";
        case AstExprBinary::Sub:
            return "-";
        case AstExprBinary::Mul:
            return "*";
        case AstExprBinary::Div:
            return "/";
        case AstExprBinary::FloorDiv:
            return "//";
        case AstExprBinary::Mod:
            return "%";
        case AstExprBinary::Pow:
            return "^";
        case AstExprBinary::Concat:
            return "..";
        case AstExprBinary::CompareNe:
            return "~=";
        case AstExprBinary::CompareEq:
            return "==";
        case AstExprBinary::CompareLt:
            return "<";
        case AstExprBinary::CompareLe:
            return "<=";
        case AstExprBinary::CompareGt:
            return ">";
        case AstExprBinary::CompareGe:
            return ">=";
        case AstExprBinary::And:
            return "and";
        case AstExprBinary::Or:
            return "or";
        default:
            return "+";
        }
    }
    const char *UnOp(AstExprUnary::Op op) { return op == AstExprUnary::Op::Not ? "not " : (op == AstExprUnary::Op::Len ? "#" : "-"); }

    void PrintStr(std::string &out, const AstArray<char> &v) {
        out += '"';
        for (size_t i = 0; i < v.size; ++i) {
            const char ch = v.data[i];
            switch (ch) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            default:
                out += ch;
                break;
            }
        }
        out += '"';
    }

    bool IsIdentKey(const AstArray<char> &v) {
        if (v.size == 0 || (v.data[0] >= '0' && v.data[0] <= '9'))
            return false;
        for (size_t i = 0; i < v.size; ++i) {
            const char ch = v.data[i];
            if (!(ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')))
                return false;
        }
        return true;
    }

    void PrintExpr(std::string &out, AstExpr *e);
    void PrintBlock(std::string &out, AstStatBlock *b, int indent); // PrintExpr (function literals) recurses into blocks

    void PrintExprList(std::string &out, const AstArray<AstExpr *> &xs) {
        for (size_t i = 0; i < xs.size; ++i) {
            if (i)
                out += ", ";
            PrintExpr(out, xs.data[i]);
        }
    }

    void PrintExpr(std::string &out, AstExpr *e) {
        if (e->is<AstExprConstantNil>()) {
            out += "nil";
        } else if (auto *n = e->as<AstExprConstantBool>()) {
            out += n->value ? "true" : "false";
        } else if (auto *n = e->as<AstExprConstantNumber>()) {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", n->value);
            out += buf;
        } else if (auto *n = e->as<AstExprConstantString>()) {
            PrintStr(out, n->value);
        } else if (auto *n = e->as<AstExprGlobal>()) {
            out += n->name.value;
        } else if (auto *n = e->as<AstExprLocal>()) {
            out += n->local->name.value;
        } else if (auto *n = e->as<AstExprGroup>()) {
            out += '(';
            PrintExpr(out, n->expr);
            out += ')';
        } else if (auto *n = e->as<AstExprBinary>()) {
            out += '(';
            PrintExpr(out, n->left);
            out += ' ';
            out += BinOp(n->op);
            out += ' ';
            PrintExpr(out, n->right);
            out += ')';
        } else if (auto *n = e->as<AstExprUnary>()) {
            out += '(';
            out += UnOp(n->op);
            PrintExpr(out, n->expr);
            out += ')';
        } else if (auto *n = e->as<AstExprIndexName>()) {
            PrintExpr(out, n->expr);
            out += (n->op == ':' ? ':' : '.'); // `:` appears only as a method-call callee
            out += n->index.value;
        } else if (auto *n = e->as<AstExprIndexExpr>()) {
            PrintExpr(out, n->expr);
            out += '[';
            PrintExpr(out, n->index);
            out += ']';
        } else if (auto *n = e->as<AstExprCall>()) {
            PrintExpr(out, n->func);
            out += '(';
            PrintExprList(out, n->args);
            out += ')';
        } else if (e->is<AstExprVarargs>()) {
            out += "...";
        } else if (auto *n = e->as<AstExprIfElse>()) {
            out += "(if ";
            PrintExpr(out, n->condition);
            out += " then ";
            PrintExpr(out, n->trueExpr);
            out += " else ";
            PrintExpr(out, n->falseExpr);
            out += ')';
        } else if (auto *n = e->as<AstExprFunction>()) {
            out += "function(";
            for (size_t i = 0; i < n->args.size; ++i) {
                if (i)
                    out += ", ";
                out += n->args.data[i]->name.value;
            }
            if (n->vararg) {
                if (n->args.size)
                    out += ", ";
                out += "...";
            }
            out += ")\n";
            PrintBlock(out, n->body, 0); // body flush-left: ugly but valid (luau ignores indentation)
            out += "end";
        } else if (auto *n = e->as<AstExprTable>()) {
            out += "{ ";
            for (size_t i = 0; i < n->items.size; ++i) {
                const auto &it = n->items.data[i];
                if (i)
                    out += ", ";
                if (it.kind == AstExprTable::Item::Kind::Record && it.key) {
                    auto *ks = it.key->as<AstExprConstantString>();
                    if (ks && IsIdentKey(ks->value)) {
                        out.append(ks->value.data, ks->value.size);
                        out += " = ";
                    } else {
                        out += '[';
                        PrintExpr(out, it.key);
                        out += "] = ";
                    }
                } else if (it.kind == AstExprTable::Item::Kind::General && it.key) {
                    out += '[';
                    PrintExpr(out, it.key);
                    out += "] = ";
                }
                PrintExpr(out, it.value);
            }
            out += " }";
        } else {
            out += "nil"; // any node the generator does not emit prints as a safe placeholder
        }
    }

    void PrintBlock(std::string &out, AstStatBlock *b, int indent);

    void Indent(std::string &out, int indent) { out.append(static_cast<size_t>(indent) * 4, ' '); }

    void PrintStat(std::string &out, AstStat *s, int indent) {
        // expression-ending statements close with `;` so the next line cannot glue on as a call:
        // `local v = foo` + `(bar)()` would otherwise parse as `local v = foo(bar)()`.
        if (auto *n = s->as<AstStatLocal>()) {
            Indent(out, indent);
            out += "local ";
            for (size_t i = 0; i < n->vars.size; ++i) {
                if (i)
                    out += ", ";
                out += n->vars.data[i]->name.value;
            }
            out += " = ";
            PrintExprList(out, n->values);
            out += ";\n";
        } else if (auto *n = s->as<AstStatAssign>()) {
            Indent(out, indent);
            PrintExprList(out, n->vars);
            out += " = ";
            PrintExprList(out, n->values);
            out += ";\n";
        } else if (auto *n = s->as<AstStatCompoundAssign>()) {
            Indent(out, indent);
            PrintExpr(out, n->var);
            out += " ";
            out += BinOp(n->op);
            out += "= ";
            PrintExpr(out, n->value);
            out += ";\n";
        } else if (auto *n = s->as<AstStatExpr>()) {
            Indent(out, indent);
            PrintExpr(out, n->expr);
            out += ";\n";
        } else if (auto *n = s->as<AstStatReturn>()) {
            Indent(out, indent);
            out += "return ";
            PrintExprList(out, n->list);
            out += ";\n";
        } else if (s->is<AstStatBreak>()) {
            Indent(out, indent);
            out += "break\n";
        } else if (s->is<AstStatContinue>()) {
            Indent(out, indent);
            out += "continue\n";
        } else if (auto *n = s->as<AstStatIf>()) {
            Indent(out, indent);
            out += "if ";
            while (true) {
                PrintExpr(out, n->condition);
                out += " then\n";
                PrintBlock(out, n->thenbody, indent + 1);
                if (n->elsebody && n->elsebody->is<AstStatIf>()) {
                    Indent(out, indent);
                    out += "elseif ";
                    n = n->elsebody->as<AstStatIf>();
                    continue;
                }
                if (n->elsebody) {
                    Indent(out, indent);
                    out += "else\n";
                    if (auto *eb = n->elsebody->as<AstStatBlock>())
                        PrintBlock(out, eb, indent + 1);
                }
                break;
            }
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatWhile>()) {
            Indent(out, indent);
            out += "while ";
            PrintExpr(out, n->condition);
            out += " do\n";
            PrintBlock(out, n->body, indent + 1);
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatFor>()) {
            Indent(out, indent);
            out += "for ";
            out += n->var->name.value;
            out += " = ";
            PrintExpr(out, n->from);
            out += ", ";
            PrintExpr(out, n->to);
            if (n->step) {
                out += ", ";
                PrintExpr(out, n->step);
            }
            out += " do\n";
            PrintBlock(out, n->body, indent + 1);
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatFunction>()) {
            Indent(out, indent);
            out += "function ";
            PrintExpr(out, n->name);
            out += "(";
            for (size_t i = 0; i < n->func->args.size; ++i) {
                if (i)
                    out += ", ";
                out += n->func->args.data[i]->name.value;
            }
            out += ")\n";
            PrintBlock(out, n->func->body, indent + 1);
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatLocalFunction>()) {
            Indent(out, indent);
            out += "local function ";
            out += n->name->name.value;
            out += "(";
            for (size_t i = 0; i < n->func->args.size; ++i) {
                if (i)
                    out += ", ";
                out += n->func->args.data[i]->name.value;
            }
            if (n->func->vararg) {
                if (n->func->args.size)
                    out += ", ";
                out += "...";
            }
            out += ")\n";
            PrintBlock(out, n->func->body, indent + 1);
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatForIn>()) {
            Indent(out, indent);
            out += "for ";
            for (size_t i = 0; i < n->vars.size; ++i) {
                if (i)
                    out += ", ";
                out += n->vars.data[i]->name.value;
            }
            out += " in ";
            PrintExprList(out, n->values);
            out += " do\n";
            PrintBlock(out, n->body, indent + 1);
            Indent(out, indent);
            out += "end\n";
        } else if (auto *n = s->as<AstStatRepeat>()) {
            Indent(out, indent);
            out += "repeat\n";
            PrintBlock(out, n->body, indent + 1);
            Indent(out, indent);
            out += "until ";
            PrintExpr(out, n->condition);
            // terminate with `;`: the until-condition is a trailing value, so a following `(`-statement
            // would otherwise glue (`until c (f)()` parses as a call `c(f)`). `;` is a valid terminator.
            out += ";\n";
        } else if (auto *n = s->as<AstStatBlock>()) {
            Indent(out, indent);
            out += "do\n";
            PrintBlock(out, n, indent + 1);
            Indent(out, indent);
            out += "end\n";
        }
    }

    void PrintBlock(std::string &out, AstStatBlock *b, int indent) {
        for (size_t i = 0; i < b->body.size; ++i)
            PrintStat(out, b->body.data[i], indent);
    }
} // namespace

// Locals the compiler propagates as constants: initialized with a constant and never assigned.
using ConstantLocals = std::map<const Luau::AstLocal *, Luau::AstExpr *>;

// The compiler folds a constant condition, so its if-expression leaves no branch to recover.
static bool FoldsToConstant(Luau::AstExpr *expr, const ConstantLocals &constants) {
    if (auto *n = expr->as<Luau::AstExprGroup>())
        return FoldsToConstant(n->expr, constants);
    if (expr->is<Luau::AstExprConstantNil>() || expr->is<Luau::AstExprConstantBool>() || expr->is<Luau::AstExprConstantNumber>() ||
        expr->is<Luau::AstExprConstantInteger>() || expr->is<Luau::AstExprConstantString>())
        return true;
    if (auto *n = expr->as<Luau::AstExprLocal>()) {
        const auto found = constants.find(n->local);
        return found != constants.end() && FoldsToConstant(found->second, constants);
    }
    if (auto *n = expr->as<Luau::AstExprUnary>())
        return n->op == Luau::AstExprUnary::Op::Not && FoldsToConstant(n->expr, constants);
    if (auto *n = expr->as<Luau::AstExprBinary>())
        return (n->op == Luau::AstExprBinary::And || n->op == Luau::AstExprBinary::Or || n->op == Luau::AstExprBinary::CompareEq ||
                n->op == Luau::AstExprBinary::CompareNe) &&
               FoldsToConstant(n->left, constants) && FoldsToConstant(n->right, constants);
    return false;
}

// The truthiness of a folded condition, when it is plain to see.
static std::optional<bool> ConstantTruth(Luau::AstExpr *expr, const ConstantLocals &constants) {
    if (auto *n = expr->as<Luau::AstExprGroup>())
        return ConstantTruth(n->expr, constants);
    if (expr->is<Luau::AstExprConstantNil>())
        return false;
    if (auto *n = expr->as<Luau::AstExprConstantBool>())
        return n->value;
    if (expr->is<Luau::AstExprConstantNumber>() || expr->is<Luau::AstExprConstantInteger>() || expr->is<Luau::AstExprConstantString>())
        return true;
    if (auto *n = expr->as<Luau::AstExprLocal>()) {
        const auto found = constants.find(n->local);
        return found == constants.end() ? std::nullopt : ConstantTruth(found->second, constants);
    }
    if (auto *n = expr->as<Luau::AstExprUnary>(); n && n->op == Luau::AstExprUnary::Op::Not) {
        const auto operand = ConstantTruth(n->expr, constants);
        return operand ? std::optional<bool>(!*operand) : std::nullopt;
    }
    if (auto *n = expr->as<Luau::AstExprBinary>(); n && (n->op == Luau::AstExprBinary::And || n->op == Luau::AstExprBinary::Or)) {
        const auto left = ConstantTruth(n->left, constants);
        if (!left)
            return std::nullopt;
        return *left == (n->op == Luau::AstExprBinary::Or) ? left : ConstantTruth(n->right, constants);
    }
    return std::nullopt;
}

static ConstantLocals CollectConstantLocals(Luau::AstStatBlock *root) {
    struct Collector : Luau::AstVisitor {
        ConstantLocals initial;
        std::set<const Luau::AstLocal *> written;
        bool visit(Luau::AstStatLocal *node) override {
            for (size_t i = 0; i < node->vars.size && i < node->values.size; ++i)
                initial[node->vars.data[i]] = node->values.data[i];
            return true;
        }
        bool visit(Luau::AstStatAssign *node) override {
            for (auto *var : node->vars)
                if (auto *local = var->as<Luau::AstExprLocal>())
                    written.insert(local->local);
            return true;
        }
        bool visit(Luau::AstStatCompoundAssign *node) override {
            if (auto *local = node->var->as<Luau::AstExprLocal>())
                written.insert(local->local);
            return true;
        }
    } collector;
    root->visit(&collector);
    for (const auto *local : collector.written)
        collector.initial.erase(local);
    return collector.initial;
}

std::optional<std::map<std::string, int>> LuauAstGenerator::MeasureSugar(const std::string &source, bool scoped) {
    Luau::Allocator allocator;
    Luau::AstNameTable names(allocator);
    auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
    if (!parsed.errors.empty() || !parsed.root)
        return std::nullopt;
    struct Visitor : Luau::AstVisitor {
        std::map<std::string, int> counts;
        bool scoped = false;
        ConstantLocals constants;
        std::string scope = "chunk";
        std::map<const Luau::AstLocal *, std::string> locals;
        std::map<std::string, int> functionInstances;
        int nextLocal = 0, nextLambda = 0;
        // a local's number follows declaration order and a loop variable keeps its source name, neither of which the output
        // preserves (hoisted declarations shift the order); sites compare locals by kind only
        void add(const std::string &kind, const std::string &target = "") {
            static const std::regex local(R"(\blocal\d+\b|\bcapture:[A-Za-z_][A-Za-z0-9_]*)");
            counts[scoped ? scope + "|" + kind + "|" + std::regex_replace(target, local, "local") : kind]++;
        }
        std::string shape(Luau::AstExpr *expr) {
            if (auto *n = expr->as<Luau::AstExprGroup>())
                return shape(n->expr);
            if (auto *n = expr->as<Luau::AstExprLocal>()) {
                auto found = locals.find(n->local);
                return found == locals.end() ? "capture:" + std::string(n->local->name.value) : found->second;
            }
            if (auto *n = expr->as<Luau::AstExprGlobal>())
                return "global:" + std::string(n->name.value);
            if (auto *n = expr->as<Luau::AstExprIndexName>())
                return shape(n->expr) + "." + n->index.value;
            if (auto *n = expr->as<Luau::AstExprIndexExpr>())
                return shape(n->expr) + "[" + shape(n->index) + "]";
            if (auto *n = expr->as<Luau::AstExprConstantNumber>())
                return std::to_string(n->value);
            if (auto *n = expr->as<Luau::AstExprConstantString>()) {
                std::string result = "str:";
                for (unsigned char c : n->value) {
                    if (c < 0x20 || c == '\\' || c == '|') {
                        char escaped[5];
                        std::snprintf(escaped, sizeof(escaped), "\\x%02x", c);
                        result += escaped;
                    } else
                        result += static_cast<char>(c);
                }
                return result;
            }
            if (auto *n = expr->as<Luau::AstExprCall>()) {
                std::string result = shape(n->func) + "(";
                for (auto *arg : n->args)
                    result += shape(arg) + ",";
                return result + ")";
            }
            if (auto *n = expr->as<Luau::AstExprBinary>())
                return "(" + shape(n->left) + Luau::toString(n->op) + shape(n->right) + ")";
            if (auto *n = expr->as<Luau::AstExprUnary>())
                return Luau::toString(n->op) + shape(n->expr);
            return "expr";
        }
        void function(Luau::AstExprFunction *node, const std::string &name) {
            const auto parent = scope;
            const int savedLocal = nextLocal, savedLambda = nextLambda;
            scope += "/" + name;
            const int instance = ++functionInstances[scope];
            if (instance > 1)
                scope += "#" + std::to_string(instance);
            nextLocal = nextLambda = 0;
            if (node->self)
                locals[node->self] = "self";
            for (size_t i = 0; i < node->args.size; ++i)
                locals[node->args.data[i]] = "arg" + std::to_string(i);
            node->body->visit(this);
            scope = parent;
            nextLocal = savedLocal;
            nextLambda = savedLambda;
        }
        bool visit(Luau::AstStatLocal *node) override {
            for (size_t i = 0; i < node->vars.size; ++i) {
                const auto symbol = "local" + std::to_string(nextLocal++);
                locals[node->vars.data[i]] = i < node->values.size && node->values.data[i]->is<Luau::AstExprCall>() ? shape(node->values.data[i]) : symbol;
            }
            return true;
        }
        bool visit(Luau::AstStatCompoundAssign *node) override {
            // `t.k ..= v` compiles exactly like `t.k = t.k .. v`; only a local's `..=` is told apart (by rendering)
            if (node->op != Luau::AstExprBinary::Concat || node->var->is<Luau::AstExprLocal>())
                add("compound-" + Luau::toString(node->op) + "=", shape(node->var));
            return true;
        }
        // `if c then continue end` directly in a loop body compiles exactly like `if not c then <rest> end`
        std::set<const Luau::AstStat *> guardContinues;
        void markGuardContinues(Luau::AstStatBlock *body) {
            for (auto *stat : body->body)
                if (auto *branch = stat->as<Luau::AstStatIf>(); branch && !branch->elsebody && branch->thenbody->body.size == 1 &&
                                                                  branch->thenbody->body.data[0]->is<Luau::AstStatContinue>())
                    guardContinues.insert(branch->thenbody->body.data[0]);
        }
        bool visit(Luau::AstStatWhile *node) override {
            markGuardContinues(node->body);
            return true;
        }
        bool visit(Luau::AstStatRepeat *node) override {
            markGuardContinues(node->body);
            return true;
        }
        bool visit(Luau::AstStatFor *node) override {
            markGuardContinues(node->body);
            return true;
        }
        bool visit(Luau::AstStatForIn *node) override {
            markGuardContinues(node->body);
            return true;
        }
        bool visit(Luau::AstStatFunction *node) override {
            auto *member = node->name->as<Luau::AstExprIndexName>();
            const std::string name = member ? member->index.value : shape(node->name);
            add(node->func->self ? "method-definition" : "function-definition", name);
            function(node->func, name);
            return false;
        }
        bool visit(Luau::AstStatLocalFunction *node) override {
            const std::string name = node->name->name.value;
            locals[node->name] = "function:" + name;
            add("local-function", name);
            function(node->func, name);
            return false;
        }
        bool visit(Luau::AstExprFunction *node) override {
            function(node, "lambda" + std::to_string(nextLambda++));
            return false;
        }
        bool visit(Luau::AstStatReturn *node) override {
            if (scoped)
                for (auto *expr : node->list) {
                    auto *binary = expr->as<Luau::AstExprBinary>();
                    if (binary && binary->left->is<Luau::AstExprLocal>())
                        add("inlined-compound-" + Luau::toString(binary->op) + "=", shape(binary->left));
                }
            return true;
        }
        bool visit(Luau::AstExprCall *node) override {
            if (node->self)
                add("method-call", shape(node->func));
            return true;
        }
        bool visit(Luau::AstExprIfElse *node) override {
            if (!FoldsToConstant(node->condition, constants)) {
                add("if-expression");
                return true;
            }
            // the compiler keeps only the branch the constant takes
            if (const auto truth = ConstantTruth(node->condition, constants)) {
                (*truth ? node->trueExpr : node->falseExpr)->visit(this);
                return false;
            }
            return true;
        }
        bool visit(Luau::AstStatIf *node) override {
            if (node->elsebody && node->elsebody->is<Luau::AstStatIf>())
                add("elseif");
            return true;
        }
        bool visit(Luau::AstExprTable *node) override {
            for (const auto &item : node->items)
                if (item.kind == Luau::AstExprTable::Item::Kind::Record)
                    add("record-field", shape(item.key));
            return true;
        }
        bool visit(Luau::AstStatContinue *node) override {
            if (!guardContinues.contains(node))
                add("continue");
            return true;
        }
    } visitor;
    visitor.scoped = scoped;
    visitor.constants = CollectConstantLocals(parsed.root);
    parsed.root->visit(&visitor);
    return visitor.counts;
}

std::optional<std::string> LuauAstGenerator::RecoveryLoss(const std::string &source, const std::string &output, bool allowInlining, bool localFunctionsInlined) {
    const auto before = MeasureSugar(source, true), after = MeasureSugar(output, true);
    if (!before || !after)
        return std::nullopt;
    std::string loss;
    for (const auto &[site, amount] : *before) {
        if (site.find("|inlined-") != std::string::npos)
            continue;
        // an inlined local function's call leaves no expression to recover
        if (localFunctionsInlined && site.find("function:") != std::string::npos)
            continue;
        const auto found = after->find(site);
        int recovered = found == after->end() ? 0 : found->second;
        if (allowInlining) {
            const auto split = site.find("|compound-");
            if (split != std::string::npos) {
                auto alternative = after->find(site.substr(0, split + 1) + "inlined-" + site.substr(split + 1));
                if (alternative != after->end())
                    recovered += alternative->second;
            }
        }
        if (recovered < amount)
            loss += site + ": " + std::to_string(amount) + " -> " + std::to_string(recovered) + "\n";
    }
    return loss;
}

std::string LuauAstGenerator::Minimize(std::string source, const std::function<bool(const std::string &)> &interesting, int budget, int &attempts) {
    attempts = 0;
    while (attempts < budget) {
        Luau::Allocator allocator;
        Luau::AstNameTable names(allocator);
        auto parsed = Luau::Parser::parse(source.data(), source.size(), names, allocator);
        if (!parsed.root || !parsed.errors.empty())
            break;
        struct Edit {
            size_t begin, end;
            std::string replacement;
        };
        struct Visitor : Luau::AstVisitor {
            const std::string &source;
            std::vector<size_t> lines{0};
            std::vector<Edit> edits;
            explicit Visitor(const std::string &text) : source(text) {
                for (size_t i = 0; i < source.size(); ++i)
                    if (source[i] == '\n')
                        lines.push_back(i + 1);
            }
            size_t offset(const Luau::Position &p) const { return p.line < lines.size() ? lines[p.line] + p.column : source.size(); }
            void replace(Luau::AstNode *node, std::string text) {
                const size_t begin = offset(node->location.begin), end = offset(node->location.end);
                if (begin <= end && end <= source.size() && text.size() < end - begin)
                    edits.push_back({begin, end, std::move(text)});
            }
            void child(Luau::AstExpr *node, Luau::AstExpr *value) {
                const size_t begin = offset(value->location.begin), end = offset(value->location.end);
                if (begin <= end && end <= source.size())
                    replace(node, "(" + source.substr(begin, end - begin) + ")");
            }
            bool visit(Luau::AstStat *node) override {
                if (!node->is<Luau::AstStatBlock>())
                    replace(node, "");
                return true;
            }
            bool visit(Luau::AstExprBinary *node) override {
                child(node, node->left);
                child(node, node->right);
                return true;
            }
            bool visit(Luau::AstExprIfElse *node) override {
                child(node, node->trueExpr);
                child(node, node->falseExpr);
                return true;
            }
            bool visit(Luau::AstExprConstantNumber *node) override {
                replace(node, "0");
                replace(node, "1");
                return true;
            }
        } visitor(source);
        parsed.root->visit(&visitor);
        std::stable_sort(visitor.edits.begin(), visitor.edits.end(), [](const Edit &a, const Edit &b) {
            return a.end - a.begin - a.replacement.size() > b.end - b.begin - b.replacement.size();
        });
        bool changed = false;
        for (const auto &edit : visitor.edits) {
            if (attempts >= budget)
                break;
            std::string candidate = source;
            candidate.replace(edit.begin, edit.end - edit.begin, edit.replacement);
            ++attempts;
            if (interesting(candidate)) {
                source = std::move(candidate);
                changed = true;
                break;
            }
        }
        if (!changed)
            break;
    }
    return source;
}

std::string LuauAstGenerator::Generate(bool sugarOnly) {
    Builder b{m_state};
    const int structuredVariant = sugarOnly ? 12 + b.rnd(3) : b.rnd(15);
    if (structuredVariant >= 12) {
        const bool methods = structuredVariant == 13;
        const bool interactions = structuredVariant == 14;
        std::string out = interactions ? "-- fuzz-case: sugar-interactions\n" : methods ? "-- fuzz-case: sugar-methods\n" : "-- fuzz-case: sugar-compound\n";
        PrintBlock(out, b.sugarChunk(methods, interactions), 0);
        m_state = b.st;
        return out;
    }
    if (structuredVariant == 4) {
        std::string out = "-- fuzz-case: mixed-guard\n";
        PrintBlock(out, b.guardChunk(), 0);
        m_state = b.st;
        return out;
    }
    if (structuredVariant < 4) {
        std::string out = b.structuredChunk(structuredVariant);
        m_state = b.st;
        return out;
    }
    Luau::AstStatBlock *root = b.chunk();
    m_state = b.st; // advance the seed so successive Generate() calls differ
    std::string out;
    PrintBlock(out, root, 0);
    return out;
}
