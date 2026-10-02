//
// Created by Dottik on 1/10/2026.
//

#include "InlineCallRecovery.hpp"

#include "SSABuilder.hpp"

#include <algorithm>
#include <bitset>
#include <boost/unordered/unordered_flat_set.hpp>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <span>

static std::optional<int32_t> SourceLine(const DeserializedFunction &function, int32_t pc) {
    if (function.lineinfo.empty() || pc < 0 || static_cast<size_t>(pc) >= function.instructions.size())
        return std::nullopt;
    const size_t offset = function.abslineinfoOffset + (static_cast<size_t>(pc) >> function.linegaplog2) * sizeof(int);
    if (offset + sizeof(int) > function.lineinfo.size())
        return std::nullopt;
    int base = 0;
    std::memcpy(&base, function.lineinfo.data() + offset, sizeof(int));
    return base + function.lineinfo[pc];
}

std::shared_ptr<const InlineSourceMap> BuildInlineSourceMap(const DeserializedBytecode &bytecode, Fission::InstructionDecoder *decoder, const LiftedFunction &root) {
    auto map = std::make_shared<InlineSourceMap>();
    std::vector<const LiftedFunction *> pending{&root};
    while (!pending.empty()) {
        const auto *function = pending.back();
        pending.pop_back();
        if (function->lpDeserialized)
            map->lifted[function->lpDeserialized] = function;
        for (const auto &child : function->subfunctions)
            pending.push_back(&child);
    }
    const auto &functions = bytecode.functions;
    map->sources.reserve(functions.size());
    for (const auto &function : functions) {
        int32_t lastLine = -1;
        if (!function.lineinfo.empty() && !function.instructions.empty()) {
            map->hasLineInfo = true;
            // the implicit `return` sits on the closing `end`, which the caller's next statement may share
            auto end = static_cast<int32_t>(function.instructions.size());
            const LuauInstruction tail{decoder ? decoder->DecodeInstruction(function.instructions.back().instruction) : function.instructions.back().instruction};
            if (tail.GetOpCode() == LOP_RETURN && tail.GetABCOperand(LuauInstruction::LuauOperand::B) == 1)
                --end;
            for (int32_t pc = 0; pc < end; ++pc)
                if (const auto line = SourceLine(function, pc))
                    lastLine = (std::max)(lastLine, *line);
        }
        map->sources.push_back({&function, -1, static_cast<int32_t>(function.lineDefined), lastLine});
    }
    for (size_t index = 0; index < functions.size(); ++index)
        for (const auto *child : functions[index].subfunctions)
            if (child >= functions.data() && child < functions.data() + functions.size())
                map->sources[child - functions.data()].parent = static_cast<int32_t>(index);

    // a body owns the lines after its header up to its last statement; spans nest, so painting widest first leaves the innermost
    constexpr int32_t kMaxTrackedLine = 1 << 22;
    std::vector<int32_t> order;
    for (int32_t index = 0; index < static_cast<int32_t>(map->sources.size()); ++index) {
        const auto &source = map->sources[index];
        if (!source.function->isvararg && source.firstLine >= 0 && source.lastLine > source.firstLine && source.lastLine < kMaxTrackedLine)
            order.push_back(index);
    }
    std::ranges::sort(order, [&](int32_t a, int32_t b) {
        const auto &left = map->sources[a], &right = map->sources[b];
        return left.lastLine - left.firstLine > right.lastLine - right.firstLine;
    });
    for (const int32_t index : order) {
        const auto &source = map->sources[index];
        if (static_cast<size_t>(source.lastLine) >= map->innermostByLine.size())
            map->innermostByLine.resize(source.lastLine + 1, -1);
        std::fill(map->innermostByLine.begin() + source.firstLine + 1, map->innermostByLine.begin() + source.lastLine + 1, index);
    }
    return map;
}

void AssignInlineOrigins(LiftedFunction &function, const InlineSourceMap &map) {
    function.inlineOrigin.assign(function.instructions.size(), -1);
    function.recoveredCallAt.assign(function.instructions.size(), -1);
    function.recoveredCalls.clear();
    for (auto &child : function.subfunctions)
        AssignInlineOrigins(child, map);
    if (!map.hasLineInfo || !function.lpDeserialized)
        return;
    const auto &sources = map.sources;
    std::vector<bool> enclosing(sources.size(), false);
    for (int32_t index = 0; index < static_cast<int32_t>(sources.size()); ++index)
        if (sources[index].function == function.lpDeserialized)
            for (int32_t at = index; at >= 0; at = sources[at].parent)
                enclosing[at] = true;
    const auto &byLine = map.innermostByLine;
    for (const auto &instruction : function.instructions) {
        const auto line = SourceLine(*function.lpDeserialized, instruction.instructionIndex);
        if (!line || *line < 0 || static_cast<size_t>(*line) >= byLine.size() || instruction.instructionIndex < 0 ||
            static_cast<size_t>(instruction.instructionIndex) >= function.inlineOrigin.size())
            continue;
        if (const int32_t source = byLine[*line]; source >= 0 && !enclosing[source])
            function.inlineOrigin[instruction.instructionIndex] = source;
    }
}

static LiftedOperation BaseOperation(LiftedOperation operation) {
    switch (operation) {
    case LiftedOperation::ADDK:
        return LiftedOperation::ADD;
    case LiftedOperation::SUBK:
        return LiftedOperation::SUB;
    case LiftedOperation::MULK:
        return LiftedOperation::MUL;
    case LiftedOperation::DIVK:
        return LiftedOperation::DIV;
    case LiftedOperation::MODK:
        return LiftedOperation::MOD;
    case LiftedOperation::POWK:
        return LiftedOperation::POW;
    case LiftedOperation::IDIVK:
        return LiftedOperation::IDIV;
    case LiftedOperation::ANDK:
        return LiftedOperation::AND;
    case LiftedOperation::ORK:
        return LiftedOperation::OR;
    default:
        return operation;
    }
}

static std::optional<std::string> ConstantKey(const LuauConstant &constant) {
    switch (constant.kType) {
    case LUA_TNIL:
        return "nil";
    case LUA_TBOOLEAN:
        return std::format("b{}", std::get<bool>(constant.constantData));
    case LUA_TNUMBER:
        return std::format("n{}", std::get<double>(constant.constantData));
    case LUA_TINTEGER:
        return std::format("i{}", std::get<int64_t>(constant.constantData));
    case LUA_TSTRING:
        return "s" + std::get<std::string>(constant.constantData);
    default:
        return std::nullopt;
    }
}

static LiftedOperand Register(int32_t reg) {
    LiftedOperand operand{};
    operand.type = LiftedOperandType::Register;
    operand.value.reg = static_cast<uint8_t>(reg);
    return operand;
}

static LiftedOperand Integer(int32_t value) {
    LiftedOperand operand{};
    operand.type = LiftedOperandType::ImmediateInteger;
    operand.value.imm.n = value;
    return operand;
}

static std::bitset<256> Writes(const LiftedInstruction &instruction) {
    std::bitset<256> written;
    for (size_t index = 0; index < instruction.operands.size(); ++index)
        if (const AccessType access = SSABuilder::GetRegisterAccess(instruction, index);
            instruction.operands[index].type == LiftedOperandType::Register && (access == AccessType::Write || access == AccessType::ReadWrite))
            written.set(instruction.operands[index].value.reg);
    for (const int reg : SSABuilder::GetImplicitDefinitions(instruction))
        if (reg >= 0 && reg < 256)
            written.set(reg);
    return written;
}

static std::optional<int32_t> JumpTarget(const LiftedInstruction &instruction) {
    const auto jump = ControlFlowAnalyzer::JumpOperand(instruction.operation);
    if (!jump || instruction.operands.size() <= jump->first)
        return std::nullopt;
    return instruction.instructionIndex + instruction.operands[jump->first].value.imm.n + jump->second;
}

static bool JumpsAlways(LiftedOperation operation) {
    return operation == LiftedOperation::JUMP || operation == LiftedOperation::LOADNJUMP || operation == LiftedOperation::FORGPREP ||
           operation == LiftedOperation::FORGPREP_INEXT || operation == LiftedOperation::FORGPREP_NEXT;
}

static bool CreatesClosure(const LiftedInstruction &instruction, const DeserializedFunction &owner, const DeserializedFunction *function) {
    if (instruction.operation == LiftedOperation::NEWCLOSURE)
        return instruction.operands.size() > 1 && instruction.operands[1].value.imm.k >= 0 &&
               static_cast<size_t>(instruction.operands[1].value.imm.k) < owner.subfunctions.size() && owner.subfunctions[instruction.operands[1].value.imm.k] == function;
    return instruction.operation == LiftedOperation::DUPCLOSURE && instruction.operands.size() > 2 &&
           instruction.operands[2].value.imm.n == static_cast<int32_t>(function->bytecodeId);
}

static std::optional<int32_t> SoleCreation(const LiftedFunction &owner, const DeserializedFunction *function) {
    std::optional<int32_t> creation;
    for (const auto &instruction : owner.instructions)
        if (CreatesClosure(instruction, *owner.lpDeserialized, function)) {
            if (creation)
                return std::nullopt;
            creation = instruction.instructionIndex;
        }
    return creation;
}

struct ClosureAccess {
    bool upvalue;
    int32_t index;
};

// Replaces [lo, hi] with `code`; hi = lo - 1 inserts before lo. A source of -1 marks code that is not a rebuilt call.
struct Recovery {
    int32_t lo, hi, source;
    std::vector<LiftedInstruction> code;
};

struct RecoveryState {
    // captures a recovered call needs in an enclosing function, spliced when that function is processed
    std::map<const LiftedFunction *, std::vector<Recovery>> insertions;
    // (parent, child creation pc, captured from an upvalue, register or upvalue index) -> the child's upvalue index
    std::map<std::tuple<const LiftedFunction *, int32_t, bool, int32_t>, int32_t> added;
};

// Where `chain.back()` can read the callee's closure before `until`: a register still holding it, or one of its upvalues. When every call was
// inlined the compiler never captured the closure; the capture is then added, but only when `commit` is set.
static std::optional<ClosureAccess> EnsureClosure(std::span<const LiftedFunction *const> chain, const DeserializedFunction *callee, int32_t until,
                                                  RecoveryState &state, bool commit) {
    const auto &function = *chain.back();
    if (const auto creation = SoleCreation(function, callee)) {
        const int32_t reg = function.instructions[*creation].operands[0].value.reg;
        if (*creation >= until)
            return std::nullopt;
        for (int32_t pc = *creation + 1; pc < until; ++pc)
            if (Writes(function.instructions[pc]).test(reg))
                return std::nullopt;
        return ClosureAccess{false, reg};
    }
    if (chain.size() < 2)
        return std::nullopt;
    const auto &parent = *chain[chain.size() - 2];
    const auto child = SoleCreation(parent, function.lpDeserialized);
    if (!child || (static_cast<size_t>(*child) < parent.inlineOrigin.size() && parent.inlineOrigin[*child] >= 0))
        return std::nullopt;
    const auto outer = EnsureClosure(chain.first(chain.size() - 1), callee, *child, state, commit);
    if (!outer)
        return std::nullopt;
    int32_t upvalue = 0;
    size_t pc = *child + 1;
    for (; pc < parent.instructions.size() && parent.instructions[pc].operation == LiftedOperation::CAPTURE; ++pc, ++upvalue) {
        const auto &capture = parent.instructions[pc];
        if ((capture.operands[0].value.imm.n == 2) == outer->upvalue && capture.operands[1].value.reg == outer->index)
            return ClosureAccess{true, upvalue};
    }
    const auto key = std::make_tuple(&parent, *child, outer->upvalue, outer->index);
    if (const auto added = state.added.find(key); added != state.added.end())
        return ClosureAccess{true, added->second};
    auto &deserialized = *function.lpDeserialized;
    if (deserialized.nups >= 200)
        return std::nullopt;
    const int32_t index = deserialized.nups;
    if (commit) {
        LiftedOperand mode{};
        mode.type = LiftedOperandType::ImmediateInteger;
        mode.value.imm.n = outer->upvalue ? 2 : 0;
        LiftedOperand captured = Register(outer->index);
        state.insertions[&parent].push_back(Recovery{static_cast<int32_t>(pc), static_cast<int32_t>(pc) - 1, -1,
                                                     {LiftedInstruction{LiftedOperation::CAPTURE, -1, {mode, captured}}}});
        state.added.emplace(key, index);
        ++deserialized.nups;
    }
    return ClosureAccess{true, index};
}

static std::optional<Recovery> RecoverRegion(const AnalyzedFunction &analyzed, std::span<const LiftedFunction *const> chain, const InlineSourceMap &map,
                                             RecoveryState &state, int32_t source, int32_t lo, int32_t hi, int32_t frame) {
    const auto &sources = map.sources;
    const auto *callee = sources[source].function;
    const auto liftedCallee = map.lifted.find(callee);
    if (liftedCallee == map.lifted.end() || callee->isvararg)
        return std::nullopt;
    const auto &function = *analyzed.lpLiftedFunction;
    const auto &instructions = function.instructions;

    // a callee that inlines other functions leaves their bodies beside this one as regions of their own
    const auto &byLine = map.innermostByLine;
    for (int32_t pc = 0; pc < static_cast<int32_t>(callee->instructions.size()); ++pc) {
        const auto line = SourceLine(*callee, pc);
        if (!line || *line < 0 || static_cast<size_t>(*line) >= byLine.size() || byLine[*line] < 0 || byLine[*line] == source)
            continue;
        bool enclosing = false;
        for (int32_t at = sources[source].parent; at >= 0 && !enclosing; at = sources[at].parent)
            enclosing = at == byLine[*line];
        if (!enclosing)
            return std::nullopt;
    }

    // the jump closing the caller's `if` arm can carry the body's last line
    while (hi >= lo && instructions[hi].operation == LiftedOperation::JUMP) {
        const auto target = JumpTarget(instructions[hi]);
        if (!target || (*target >= lo && *target <= hi + 1))
            break;
        --hi;
    }
    if (hi < lo)
        return std::nullopt;

    // a call carries no control: the body is entered at its head, leaves only to the instruction after it, and returns nothing of the caller's
    for (const auto &instruction : instructions) {
        const auto target = JumpTarget(instruction);
        const bool inside = instruction.instructionIndex >= lo && instruction.instructionIndex <= hi;
        if (inside && instruction.operation == LiftedOperation::RETURN)
            return std::nullopt;
        if (target && (inside ? (*target < lo || *target > hi + 1) : (*target > lo && *target <= hi)))
            return std::nullopt;
    }

    std::vector<const LiftedInstruction *> body;
    boost::unordered_flat_set<const LiftedInstruction *> inBody;
    for (int32_t pc = lo; pc <= hi; ++pc) {
        inBody.insert(&instructions[pc]);
        if (instructions[pc].operation != LiftedOperation::NOP)
            body.push_back(&instructions[pc]);
    }
    for (const auto &block : analyzed.basicBlocks) {
        if (!block.lpHead || block.lpHead->instructionIndex < lo || block.lpHead->instructionIndex > hi)
            continue;
        const bool reentered = std::ranges::any_of(block.predecessors, [&](uint32_t predecessor) {
            const auto *tail = analyzed.basicBlocks[predecessor].lpTail;
            return tail && tail->instructionIndex >= lo && tail->instructionIndex <= hi;
        });
        if (block.lpHead->instructionIndex > lo || reentered)
            for (const auto &phi : block.phiNodes)
                inBody.insert(&phi);
    }

    std::vector<const LiftedInstruction *> calleeBody;
    for (const auto &instruction : liftedCallee->second->instructions)
        if (instruction.operation != LiftedOperation::NOP && instruction.operation != LiftedOperation::PREPVARARGS)
            calleeBody.push_back(&instruction);

    // the inlined body is the callee's code with constant arguments folded and dead arms dropped: align the two by operation
    const size_t m = calleeBody.size(), n = body.size();
    if (n == 0 || m * n > (1u << 18))
        return std::nullopt;
    const auto same = [&](size_t a, size_t b) {
        return BaseOperation(calleeBody[a]->operation) == BaseOperation(body[b]->operation) && calleeBody[a]->operands.size() == body[b]->operands.size();
    };
    std::vector<uint16_t> lengths((m + 1) * (n + 1), 0);
    const auto length = [&](size_t a, size_t b) -> uint16_t & { return lengths[a * (n + 1) + b]; };
    for (size_t a = m; a-- > 0;)
        for (size_t b = n; b-- > 0;)
            length(a, b) = same(a, b) ? static_cast<uint16_t>(length(a + 1, b + 1) + 1) : (std::max)(length(a + 1, b), length(a, b + 1));
    std::vector<int32_t> matchOf(m, -1);
    std::vector<bool> bodyMatched(n, false);
    for (size_t a = 0, b = 0; a < m && b < n;) {
        if (same(a, b) && length(a, b) == length(a + 1, b + 1) + 1) {
            matchOf[a] = static_cast<int32_t>(b);
            bodyMatched[b] = true;
            ++a, ++b;
        } else if (length(a + 1, b) >= length(a, b + 1)) {
            ++a;
        } else {
            ++b;
        }
    }
    // returns become copies into the call's target and jumps past the body
    for (size_t b = 0; b < n; ++b)
        if (!bodyMatched[b] && body[b]->operation != LiftedOperation::MOVE && body[b]->operation != LiftedOperation::JUMP &&
            body[b]->operation != LiftedOperation::LOAD && body[b]->operation != LiftedOperation::LOADNJUMP)
            return std::nullopt;

    const auto &callerConstants = function.lpDeserialized->constants;
    struct Argument {
        std::string key;
        LiftedOperand operand;
    };
    const auto argumentFrom = [&](const LiftedOperand &operand) -> std::optional<Argument> {
        switch (operand.type) {
        case LiftedOperandType::Register:
            if (inBody.contains(analyzed.GetDefinition(operand)))
                return std::nullopt;
            return Argument{std::format("r{}.{}", operand.value.reg, operand.ssaVersion), Register(operand.value.reg)};
        case LiftedOperandType::ImmediateConstant:
            if (operand.value.imm.k >= 0 && static_cast<size_t>(operand.value.imm.k) < callerConstants.size())
                if (const auto key = ConstantKey(callerConstants[operand.value.imm.k]))
                    return Argument{*key, operand};
            return std::nullopt;
        case LiftedOperandType::ImmediateInteger:
            return Argument{std::format("n{}", static_cast<double>(operand.value.imm.n)), operand};
        case LiftedOperandType::ImmediateBool:
            return Argument{std::format("b{}", operand.value.imm.b), operand};
        case LiftedOperandType::ImmediateNil:
            return Argument{"nil", operand};
        default:
            return std::nullopt;
        }
    };
    // a callee constant the caller can load: an immediate, or an equal entry of the caller's own table
    const auto argumentOf = [&](const LuauConstant &constant) -> std::optional<Argument> {
        const auto key = ConstantKey(constant);
        if (!key)
            return std::nullopt;
        LiftedOperand operand{};
        switch (constant.kType) {
        case LUA_TNIL:
            operand.type = LiftedOperandType::ImmediateNil;
            return Argument{*key, operand};
        case LUA_TBOOLEAN:
            operand.type = LiftedOperandType::ImmediateBool;
            operand.value.imm.b = std::get<bool>(constant.constantData);
            return Argument{*key, operand};
        case LUA_TNUMBER:
            if (const double value = std::get<double>(constant.constantData);
                std::trunc(value) == value && value >= std::numeric_limits<int16_t>::min() && value <= std::numeric_limits<int16_t>::max() &&
                !(value == 0 && std::signbit(value)))
                return Argument{*key, Integer(static_cast<int32_t>(value))};
            break;
        default:
            break;
        }
        for (size_t k = 0; k < callerConstants.size(); ++k)
            if (ConstantKey(callerConstants[k]) == key) {
                operand.type = LiftedOperandType::ImmediateConstant;
                operand.value.imm.k = static_cast<int32_t>(k);
                return Argument{*key, operand};
            }
        return std::nullopt;
    };

    const int32_t parameters = callee->numparams;
    std::vector<std::optional<Argument>> bound(parameters);
    const auto bind = [&](int32_t parameter, const Argument &argument) {
        if (!bound[parameter])
            bound[parameter] = argument;
        return bound[parameter]->key == argument.key;
    };
    boost::unordered_flat_set<int32_t> written;
    int32_t maxReturns = 0;
    for (size_t a = 0; a < m; ++a) {
        const auto &instruction = *calleeBody[a];
        if (instruction.operation == LiftedOperation::RETURN && instruction.operands.size() > 1)
            maxReturns = instruction.operands[1].value.imm.n == 0 ? 255 : (std::max)(maxReturns, instruction.operands[1].value.imm.n - 1);
        for (size_t index = 0; index < instruction.operands.size(); ++index) {
            const auto &operand = instruction.operands[index];
            const AccessType access = SSABuilder::GetRegisterAccess(instruction, index);
            if (operand.type != LiftedOperandType::Register || operand.value.reg >= parameters || written.contains(operand.value.reg) ||
                (access != AccessType::Read && access != AccessType::ReadWrite) || matchOf[a] < 0)
                continue;
            const auto argument = argumentFrom(body[matchOf[a]]->operands[index]);
            if (!argument || !bind(operand.value.reg, *argument))
                return std::nullopt;
        }
        // `if parameter == K` folded away while its arm survived: the argument was K
        if (instruction.operation == LiftedOperation::JUMPXEQK && matchOf[a] < 0 && instruction.operands.size() == 4 &&
            instruction.operands[0].value.reg < parameters && !written.contains(instruction.operands[0].value.reg) &&
            instruction.operands[2].type == LiftedOperandType::ImmediateConstant && instruction.operands[3].value.imm.b) {
            const int32_t target = instruction.instructionIndex + instruction.operands[1].value.imm.n;
            size_t next = a + 1;
            while (next < m && matchOf[next] < 0)
                ++next;
            if (next < m && calleeBody[next]->instructionIndex < target && instruction.operands[2].value.imm.k >= 0 &&
                static_cast<size_t>(instruction.operands[2].value.imm.k) < callee->constants.size()) {
                const auto argument = argumentOf(callee->constants[instruction.operands[2].value.imm.k]);
                if (!argument || !bind(instruction.operands[0].value.reg, *argument))
                    return std::nullopt;
            }
        }
        const auto defined = Writes(instruction);
        for (int32_t reg = 0; reg < 256; ++reg)
            if (defined.test(reg))
                written.insert(reg);
    }
    if (std::ranges::any_of(bound, [](const auto &argument) { return !argument.has_value(); }))
        return std::nullopt;

    const auto closure = EnsureClosure(chain, callee, lo, state, false);
    if (!closure)
        return std::nullopt;

    // the caller's locals the closure captures are the callee's upvalues: writes to them are the body's effects, not results
    std::bitset<256> captured;
    if (!closure->upvalue)
        if (const auto creation = SoleCreation(function, callee))
            for (size_t pc = *creation + 1; pc < instructions.size() && instructions[pc].operation == LiftedOperation::CAPTURE; ++pc)
                if (instructions[pc].operands[0].value.imm.n != 2)
                    captured.set(instructions[pc].operands[1].value.reg);

    // a value the body defines and code after it reads, directly or through a merge, is a result
    boost::unordered_flat_map<const LiftedInstruction *, std::vector<SSARef>> definitions;
    for (const auto &[ref, definition] : analyzed.definitionMap)
        if (inBody.contains(definition))
            definitions[definition].push_back(ref);
    std::bitset<256> results;
    const std::function<void(const SSARef &, int)> follow = [&](const SSARef &ref, int depth) {
        const auto users = analyzed.users.find(ref);
        if (users == analyzed.users.end() || depth > 8)
            return;
        for (const auto *user : users->second) {
            if (inBody.contains(user))
                continue;
            if (user->operation == LiftedOperation::PHI && !user->operands.empty()) {
                follow(SSARef{static_cast<uint8_t>(user->operands[0].value.reg), user->operands[0].ssaVersion}, depth + 1);
                continue;
            }
            results.set(ref.regIndex);
        }
    };
    std::bitset<256> bodyWrites;
    for (const auto &[definition, refs] : definitions)
        for (const auto &ref : refs)
            if (!captured.test(ref.regIndex)) {
                follow(ref, 0);
                if (definition->operation != LiftedOperation::PHI)
                    bodyWrites.set(ref.regIndex);
            }

    int32_t base = 0, count = 0;
    if (results.any()) {
        if (bodyWrites.none())
            return std::nullopt;
        while (!bodyWrites.test(base))
            ++base;
        int32_t last = 255;
        while (!results.test(last))
            --last;
        count = last - base + 1;
        if (count < 1 || count > maxReturns)
            return std::nullopt;

        // the call writes every target: each must be written on every path through the body, or a caller value would survive it
        const int32_t exit = hi - lo + 1;
        std::vector<std::bitset<256>> reaching(exit + 1);
        std::vector<bool> reached(exit + 1, false);
        reached[0] = true;
        for (bool changed = true; changed;) {
            changed = false;
            for (int32_t pc = lo; pc <= hi; ++pc) {
                if (!reached[pc - lo])
                    continue;
                const auto &instruction = instructions[pc];
                const auto out = reaching[pc - lo] | Writes(instruction);
                const auto flow = [&](int32_t to) {
                    const int32_t at = to - lo;
                    const auto next = reached[at] ? reaching[at] & out : out;
                    if (!reached[at] || next != reaching[at]) {
                        reaching[at] = next;
                        reached[at] = true;
                        changed = true;
                    }
                };
                const auto target = JumpTarget(instruction);
                if (target)
                    flow(*target);
                if (!target || !JumpsAlways(instruction.operation))
                    flow(pc + 1);
            }
        }
        if (!reached[exit])
            return std::nullopt;
        for (int32_t reg = base; reg <= last; ++reg)
            if (!reaching[exit].test(reg))
                return std::nullopt;
    }

    while (!bound.empty() && bound.back()->operand.type == LiftedOperandType::ImmediateNil)
        bound.pop_back();
    const auto arguments = static_cast<int32_t>(bound.size());
    if (frame + arguments + 1 > 255 || frame + count > 255)
        return std::nullopt;
    EnsureClosure(chain, callee, lo, state, true);

    Recovery recovery{lo, hi, source, {}};
    auto &code = recovery.code;
    code.push_back(closure->upvalue ? LiftedInstruction{LiftedOperation::GETUPVAL, -1, {Register(frame), Integer(closure->index)}}
                                    : LiftedInstruction{LiftedOperation::MOVE, -1, {Register(frame), Register(closure->index)}});
    for (int32_t i = 0; i < arguments; ++i) {
        const auto &operand = bound[i]->operand;
        auto value = operand;
        value.ssaVersion = -1;
        code.push_back(LiftedInstruction{operand.type == LiftedOperandType::Register ? LiftedOperation::MOVE : LiftedOperation::LOAD, -1,
                                         {Register(frame + 1 + i), value}});
    }
    code.push_back(LiftedInstruction{LiftedOperation::CALL, -1, {Register(frame), Integer(arguments + 1), Integer(count + 1)}});
    for (int32_t j = 0; j < count; ++j)
        code.push_back(LiftedInstruction{LiftedOperation::MOVE, -1, {Register(base + j), Register(frame + j)}});
    return recovery;
}

// Replaces each recovered range with its call, renumbering the stream and every pc-keyed table that points into it.
static void Splice(LiftedFunction &function, const std::vector<Recovery> &recoveries) {
    auto &instructions = function.instructions;
    const auto size = static_cast<int32_t>(instructions.size());
    std::vector<int32_t> moved(size + 1, 0);
    std::vector<LiftedInstruction> rebuilt;
    std::vector<int32_t> origins, callAt;
    rebuilt.reserve(instructions.size());
    auto next = recoveries.begin();
    for (int32_t pc = 0; pc < size;) {
        if (next != recoveries.end() && next->lo == pc) {
            const auto call = next->source >= 0 ? static_cast<int32_t>(function.recoveredCalls.size()) : -1;
            if (next->source >= 0)
                function.recoveredCalls.push_back(next->source);
            for (int32_t inside = next->lo; inside <= next->hi; ++inside)
                moved[inside] = static_cast<int32_t>(rebuilt.size());
            for (const auto &instruction : next->code) {
                rebuilt.push_back(instruction);
                origins.push_back(-1);
                callAt.push_back(call);
            }
            pc = next->hi + 1;
            ++next;
            continue;
        }
        moved[pc] = static_cast<int32_t>(rebuilt.size());
        rebuilt.push_back(std::move(instructions[pc]));
        origins.push_back(pc < static_cast<int32_t>(function.inlineOrigin.size()) ? function.inlineOrigin[pc] : -1);
        callAt.push_back(pc < static_cast<int32_t>(function.recoveredCallAt.size()) ? function.recoveredCallAt[pc] : -1);
        ++pc;
    }
    moved[size] = static_cast<int32_t>(rebuilt.size());

    for (int32_t at = 0; at < static_cast<int32_t>(rebuilt.size()); ++at) {
        auto &instruction = rebuilt[at];
        if (instruction.instructionIndex >= 0)
            if (const auto target = JumpTarget(instruction); target && *target >= 0 && *target <= size) {
                const auto jump = *ControlFlowAnalyzer::JumpOperand(instruction.operation);
                instruction.operands[jump.first].value.imm.n = moved[*target] - at - jump.second;
            }
    }
    for (int32_t at = 0; at < static_cast<int32_t>(rebuilt.size()); ++at)
        rebuilt[at].instructionIndex = at;

    const auto insideRecovery = [&](int32_t pc) {
        return std::ranges::any_of(recoveries, [&](const Recovery &recovery) { return pc > recovery.lo && pc <= recovery.hi; });
    };
    std::erase_if(function.upvalueCloses, [&](const auto &close) { return close.first >= 0 && close.first < size && insideRecovery(close.first); });
    for (auto &close : function.upvalueCloses)
        close.first = moved[std::clamp(close.first, 0, size)];

    // the body's own locals end with it
    auto &locals = function.lpDeserialized->locvars;
    std::erase_if(locals, [&](const LuauLocalVar &local) {
        return std::ranges::any_of(recoveries, [&](const Recovery &recovery) {
            return recovery.hi >= recovery.lo && local.startpc >= recovery.lo && local.endpc <= recovery.hi + 1;
        });
    });
    for (auto &local : locals) {
        local.startpc = moved[std::clamp(local.startpc, 0, size)];
        local.endpc = moved[std::clamp(local.endpc, 0, size)];
    }

    int32_t frame = function.lpDeserialized->maxstacksize;
    for (const auto &recovery : recoveries)
        for (const auto &instruction : recovery.source >= 0 ? recovery.code : std::vector<LiftedInstruction>{})
            for (const auto &operand : instruction.operands)
                if (operand.type == LiftedOperandType::Register)
                    frame = (std::max)(frame, operand.value.reg + 1);
    for (const auto &recovery : recoveries)
        for (const auto &instruction : recovery.code)
            if (instruction.operation == LiftedOperation::CALL)
                frame = (std::max)(frame, instruction.operands[0].value.reg + (std::max)(instruction.operands[1].value.imm.n, instruction.operands[2].value.imm.n - 1));
    function.lpDeserialized->maxstacksize = static_cast<uint8_t>((std::min)(frame, 255));

    instructions = std::move(rebuilt);
    function.inlineOrigin = std::move(origins);
    function.recoveredCallAt = std::move(callAt);
}

static bool RecoverFunction(AnalyzedFunction &analyzed, std::vector<const LiftedFunction *> &chain, const InlineSourceMap &map, RecoveryState &state) {
    auto &function = *analyzed.lpLiftedFunction;
    chain.push_back(&function);
    bool changed = false;
    for (auto &inner : analyzed.innerFunctions)
        changed = RecoverFunction(inner, chain, map, state) || changed;

    std::vector<Recovery> recoveries;
    const auto &origin = function.inlineOrigin;
    const auto size = static_cast<int32_t>((std::min)(origin.size(), function.instructions.size()));
    const int32_t frame = function.lpDeserialized ? function.lpDeserialized->maxstacksize : 255;
    for (int32_t pc = 0; pc < size;) {
        if (origin[pc] < 0) {
            ++pc;
            continue;
        }
        int32_t hi = pc;
        while (hi + 1 < size && origin[hi + 1] == origin[pc])
            ++hi;
        if (auto recovery = RecoverRegion(analyzed, chain, map, state, origin[pc], pc, hi, frame)) {
            std::fill(function.inlineOrigin.begin() + recovery->hi + 1, function.inlineOrigin.begin() + hi + 1, -1);
            recoveries.push_back(std::move(*recovery));
        }
        pc = hi + 1;
    }
    if (const auto insertions = state.insertions.find(&function); insertions != state.insertions.end()) {
        recoveries.insert(recoveries.end(), insertions->second.begin(), insertions->second.end());
        std::ranges::stable_sort(recoveries, [](const Recovery &a, const Recovery &b) {
            return a.lo != b.lo ? a.lo < b.lo : (a.hi < a.lo) > (b.hi < b.lo);
        });
    }
    if (!recoveries.empty()) {
        Splice(function, recoveries);
        changed = true;
    }
    chain.pop_back();
    return changed;
}

bool RecoverInlinedCalls(AnalyzedFunction &root, const InlineSourceMap &map) {
    if (!map.hasLineInfo)
        return false;
    std::vector<const LiftedFunction *> chain;
    RecoveryState state;
    return RecoverFunction(root, chain, map, state);
}
