// Generate well-formed Luau parser ASTs and transpile them to source.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

class LuauAstGenerator {
  public:
    explicit LuauAstGenerator(uint32_t seed) : m_state(seed ? seed : 0x9e3779b9u) {}

    std::string Generate(bool sugarOnly = false);
    static std::optional<std::map<std::string, int>> MeasureSugar(const std::string &source, bool scoped = false);
    static std::optional<std::string> RecoveryLoss(const std::string &source, const std::string &output, bool allowInlining);
    static std::string Minimize(std::string source, const std::function<bool(const std::string &)> &interesting, int budget, int &attempts);

  private:
    uint32_t m_state;
};
