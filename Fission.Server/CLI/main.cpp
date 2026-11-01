#include "Server.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>

namespace {
    std::optional<long long> ParseInteger(const char *value) {
        if (value == nullptr || *value == '\0')
            return std::nullopt;
        errno = 0;
        char *end = nullptr;
        const long long parsed = std::strtoll(value, &end, 10);
        if (errno == ERANGE || end == value || *end != '\0')
            return std::nullopt;
        return parsed;
    }
} // namespace

int main(int argc, char *argv[]) {
    Fission::Server::ServerConfig config{};

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            const auto port = ParseInteger(argv[++i]);
            if (port && *port > 0 && *port <= 65535)
                config.port = static_cast<uint16_t>(*port);
            else
                std::cerr << "Warning: invalid port '" << argv[i] << "', using default " << config.port << std::endl;
        } else if (strcmp(argv[i], "--max-timeout") == 0 && i + 1 < argc) {
            const auto timeout = ParseInteger(argv[++i]);
            if (timeout && *timeout > 0)
                config.maxTimeout = std::chrono::seconds(*timeout);
            else
                std::cerr << "Warning: invalid timeout '" << argv[i] << "', using default " << config.maxTimeout.count() << "s" << std::endl;
        } else if (strcmp(argv[i], "--max-concurrency") == 0 && i + 1 < argc) {
            const auto concurrency = ParseInteger(argv[++i]);
            if (concurrency && *concurrency > 0 && *concurrency <= std::numeric_limits<unsigned int>::max())
                config.maxConcurrentDecompiles = static_cast<unsigned int>(*concurrency);
            else
                std::cerr << "Warning: invalid concurrency '" << argv[i] << "', using default " << config.maxConcurrentDecompiles << std::endl;
        } else if (strcmp(argv[i], "--max-queued") == 0 && i + 1 < argc) {
            const auto queued = ParseInteger(argv[++i]);
            if (queued && *queued >= 0 && *queued <= std::numeric_limits<unsigned int>::max())
                config.maxQueuedRequests = static_cast<unsigned int>(*queued);
            else
                std::cerr << "Warning: invalid queue size '" << argv[i] << "', using default " << config.maxQueuedRequests << std::endl;
        } else if (strcmp(argv[i], "--max-memory-mb") == 0 && i + 1 < argc) {
            const auto megabytes = ParseInteger(argv[++i]);
            if (megabytes && *megabytes >= 0 && *megabytes <= (1ll << 40))
                config.maxDecompileMemoryBytes = static_cast<std::uint64_t>(*megabytes) * 1024 * 1024;
            else
                std::cerr << "Warning: invalid memory budget '" << argv[i] << "', using default " << (config.maxDecompileMemoryBytes / (1024 * 1024)) << " MiB"
                          << std::endl;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "Fission.Server - Luau bytecode decompilation HTTP API\n"
                      << "Usage: Fission.Server.CLI [options]\n"
                      << "Options:\n"
                      << "  -p, --port <port>           Listen port (default: 8080)\n"
                      << "  --max-timeout <seconds>     Max decompilation timeout (default: 30)\n"
                      << "  --max-concurrency <count>   Max simultaneous decompiles (default: 4)\n"
                      << "  --max-queued <count>        Max requests waiting for a slot (default: 8)\n"
                      << "  --max-memory-mb <size>      Decompile working-set budget, 0 to disable (default: 2048)\n"
                      << "  -h, --help                  Show this help\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << ". Use --help for usage.\n";
            return 1;
        }
    }

    Fission::Server::run_server(config);
    return 0;
}
