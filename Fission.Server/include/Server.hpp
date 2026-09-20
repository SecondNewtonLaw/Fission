#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

namespace Fission::Server {
    struct ServerConfig {
        uint16_t port = 8080;
        std::chrono::seconds maxTimeout{30};
        unsigned int threadCount = std::max(1u, std::thread::hardware_concurrency() / 2);
    };

    // Transport-independent response used by network code and tests.
    struct HttpResult {
        unsigned int status; // HTTP status code (200, 400, 404, 422, 500)
        std::string contentType;
        std::string body;
    };

    // Route one request without socket I/O.
    HttpResult HandleRequest(std::string_view method, std::string_view target, const std::string &body, const ServerConfig &config);

    void run_server(const ServerConfig &config);
} // namespace Fission::Server
