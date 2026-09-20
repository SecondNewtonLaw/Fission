#include "Server.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

int main(int argc, char *argv[]) {
    Fission::Server::ServerConfig config{};

    for (int i = 1; i < argc; ++i) {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            int port = std::atoi(argv[++i]);
            if (port > 0 && port <= 65535)
                config.port = static_cast<uint16_t>(port);
            else
                std::cerr << "Warning: invalid port '" << argv[i] << "', using default " << config.port << std::endl;
        } else if (strcmp(argv[i], "--max-timeout") == 0 && i + 1 < argc) {
            int timeout = std::atoi(argv[++i]);
            if (timeout > 0)
                config.maxTimeout = std::chrono::seconds(timeout);
            else
                std::cerr << "Warning: invalid timeout '" << argv[i] << "', using default " << config.maxTimeout.count() << "s" << std::endl;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "Fission.Server - Luau bytecode decompilation HTTP API\n"
                      << "Usage: Fission.Server.CLI [options]\n"
                      << "Options:\n"
                      << "  -p, --port <port>        Listen port (default: 8080)\n"
                      << "  --max-timeout <seconds>   Max decompilation timeout (default: 30)\n"
                      << "  -h, --help                Show this help\n";
            return 0;
        } else {
            std::cerr << "Unknown option: " << argv[i] << ". Use --help for usage.\n";
            return 1;
        }
    }

    Fission::Server::run_server(config);
    return 0;
}
