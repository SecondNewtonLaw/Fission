//
// Created by Dottik on 22/9/2026.
//

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class FissionDebugStage : uint8_t { Pipeline, CFA, SSA, AST, Count };

class FissionDebugNotes {
    static constexpr size_t kStageCount = static_cast<size_t>(FissionDebugStage::Count);
    static constexpr std::array<size_t, kStageCount> kStageLimits{12, 32, 24, 16};
    static constexpr size_t kMaxNoteLength = 512;
    static constexpr size_t kBlockNoteLimit = 6;

    struct Note {
        std::string message;
        bool decision;
    };

    bool m_enabled = false;
    std::array<std::vector<Note>, kStageCount> m_notes{};
    std::array<size_t, kStageCount> m_omitted{};

    static constexpr std::string_view StageName(FissionDebugStage stage) {
        switch (stage) {
        case FissionDebugStage::Pipeline:
            return "Pipeline";
        case FissionDebugStage::CFA:
            return "CFA";
        case FissionDebugStage::SSA:
            return "SSA";
        case FissionDebugStage::AST:
            return "AST";
        default:
            return "Unknown";
        }
    }

    void Record(FissionDebugStage stage, std::string message, bool decision) {
        if (!m_enabled)
            return;
        const size_t index = static_cast<size_t>(stage);
        if (index >= kStageCount)
            return;
        if (m_notes[index].size() >= kStageLimits[index]) {
            ++m_omitted[index];
            if (!decision)
                return;
            auto &notes = m_notes[index];
            const auto routine = std::find_if(notes.begin(), notes.end(), [](const Note &note) { return !note.decision; });
            notes.erase(routine == notes.end() ? notes.begin() : routine);
        }
        if (message.size() > kMaxNoteLength)
            message.replace(kMaxNoteLength - 3, std::string::npos, "...");
        m_notes[index].push_back({std::move(message), decision});
    }

  public:
    void Reset(bool enabled) {
        m_enabled = enabled;
        for (auto &notes : m_notes)
            notes.clear();
        m_omitted.fill(0);
    }

    [[nodiscard]] bool Enabled() const { return m_enabled; }

    void Add(FissionDebugStage stage, std::string message) { Record(stage, std::move(message), false); }
    void AddDecision(FissionDebugStage stage, std::string message) { Record(stage, std::move(message), true); }

    template <typename... Args> void Add(FissionDebugStage stage, std::format_string<Args...> format, Args &&...args) {
        if (m_enabled)
            Add(stage, std::format(format, std::forward<Args>(args)...));
    }

    template <typename... Args> void AddDecision(FissionDebugStage stage, std::format_string<Args...> format, Args &&...args) {
        if (m_enabled)
            AddDecision(stage, std::format(format, std::forward<Args>(args)...));
    }

    void AddBlock(FissionDebugStage stage, std::string_view function, uint32_t blockId, std::vector<std::string> &blockNotes,
                  std::string message, bool decision = true) {
        if (!m_enabled)
            return;
        if (message.size() > kMaxNoteLength)
            message.replace(kMaxNoteLength - 3, std::string::npos, "...");
        Record(stage, std::format("{} B{}: {}", function, blockId, message), decision);
        const auto prefix = std::format("{}: ", StageName(stage));
        const auto omitted = prefix + "additional decisions omitted";
        const auto count = std::count_if(blockNotes.begin(), blockNotes.end(), [&](const std::string &note) {
            return note.starts_with(prefix) && note != omitted;
        });
        if (count >= static_cast<std::ptrdiff_t>(kBlockNoteLimit)) {
            if (!decision) {
                if (std::find(blockNotes.begin(), blockNotes.end(), omitted) == blockNotes.end())
                    blockNotes.push_back(omitted);
                return;
            }
            const auto oldest = std::find_if(blockNotes.begin(), blockNotes.end(), [&](const std::string &note) {
                return note.starts_with(prefix) && note != omitted;
            });
            blockNotes.erase(oldest);
            if (std::find(blockNotes.begin(), blockNotes.end(), omitted) == blockNotes.end())
                blockNotes.push_back(omitted);
        }
        blockNotes.push_back(prefix + message);
    }

    [[nodiscard]] std::string Render() const {
        if (!m_enabled)
            return {};
        std::stringstream output;
        for (size_t i = 0; i < kStageCount; ++i) {
            if (m_notes[i].empty() && m_omitted[i] == 0)
                continue;
            output << '[' << StageName(static_cast<FissionDebugStage>(i)) << "]\n";
            for (const auto &note : m_notes[i])
                output << "- " << note.message << '\n';
            if (m_omitted[i] != 0)
                output << "- " << m_omitted[i] << " additional notes omitted\n";
        }
        return output.str();
    }
};
