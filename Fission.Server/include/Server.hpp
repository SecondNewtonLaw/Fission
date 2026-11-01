#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace Fission::Server {
    struct ServerConfig {
        uint16_t port = 8080;
        std::chrono::seconds maxTimeout{30};
        unsigned int threadCount = std::max(1u, std::thread::hardware_concurrency() / 2);
        unsigned int maxConcurrentDecompiles = std::min(4u, std::max(1u, std::thread::hardware_concurrency() / 2));
        unsigned int maxQueuedRequests = 8;
        std::chrono::milliseconds queueWait{2000};
        // Aggregate decompile working set; zero disables the byte budget.
        std::uint64_t maxDecompileMemoryBytes = 2ull * 1024 * 1024 * 1024;
    };

    inline std::uint64_t EstimateDecompileFootprint(std::uint64_t requestBodyBytes) {
        // Measured peak is about 240x the decoded bytecode size.
        constexpr std::uint64_t kFootprintMultiplier = 256;
        const std::uint64_t decodedBytes = (requestBodyBytes / 4) * 3;
        return std::max<std::uint64_t>(1, decodedBytes * kFootprintMultiplier);
    }

    class DecompileGate {
        mutable std::mutex m_mutex;
        std::condition_variable m_released;
        unsigned int m_active = 0;
        unsigned int m_waiting = 0;
        std::uint64_t m_reservedBytes = 0;
        const unsigned int m_maxActive;
        const unsigned int m_maxWaiting;
        const std::uint64_t m_byteBudget;

        bool Fits(std::uint64_t estimate) const { return m_active < m_maxActive && (m_byteBudget == 0 || m_reservedBytes + estimate <= m_byteBudget); }

      public:
        DecompileGate(unsigned int maxActive, unsigned int maxWaiting, std::uint64_t byteBudget)
            : m_maxActive(std::max(1u, maxActive)), m_maxWaiting(std::max(0u, maxWaiting)), m_byteBudget(byteBudget) {}

        bool Acquire(std::uint64_t estimate, std::chrono::milliseconds maxWait) {
            std::unique_lock lock(m_mutex);
            if (Fits(estimate)) {
                ++m_active;
                m_reservedBytes += estimate;
                return true;
            }
            if (m_waiting >= m_maxWaiting)
                return false;
            ++m_waiting;
            const bool acquired = m_released.wait_for(lock, maxWait, [this, estimate] { return Fits(estimate); });
            --m_waiting;
            if (!acquired)
                return false;
            ++m_active;
            m_reservedBytes += estimate;
            return true;
        }

        void Release(std::uint64_t estimate) {
            {
                std::lock_guard lock(m_mutex);
                if (m_active > 0)
                    --m_active;
                m_reservedBytes = m_reservedBytes > estimate ? m_reservedBytes - estimate : 0;
            }
            m_released.notify_all();
        }

        unsigned int Active() const {
            std::lock_guard lock(m_mutex);
            return m_active;
        }
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
