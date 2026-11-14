#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

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

    // Resident set size of this process, or zero when the platform cannot report it.
    // Admission uses it instead of trusting the estimate alone: allocator arenas and capture
    // buffers keep freed pages resident, so a model-only limit can be exceeded without ever
    // reserving past the budget.
    std::uint64_t CurrentResidentBytes();

    class DecompileGate {
        mutable std::mutex m_mutex;
        std::condition_variable m_released;
        unsigned int m_active = 0;
        unsigned int m_waiting = 0;
        std::uint64_t m_reservedBytes = 0;
        const unsigned int m_maxActive;
        const unsigned int m_maxWaiting;
        const std::uint64_t m_byteBudget;
        // Empty keeps the gate reservation-only, which is what embedders and unit tests use.
        const std::function<std::uint64_t()> m_memoryProbe;

        // Wake waiters periodically so a probe-observed drop in resident memory is noticed even
        // when no decompile finishes (a phase releasing a large graph can lower RSS mid-request).
        static constexpr std::chrono::milliseconds kMemoryPollInterval{20};

        bool Fits(std::uint64_t estimate) const {
            if (m_active >= m_maxActive)
                return false;
            if (m_byteBudget == 0)
                return true;
            // Nothing can fit a budget smaller than a single request.
            if (estimate > m_byteBudget)
                return false;
            if (!m_memoryProbe)
                return m_reservedBytes + estimate <= m_byteBudget;
            const std::uint64_t resident = m_memoryProbe();
            if (resident == 0)
                return m_reservedBytes + estimate <= m_byteBudget;
            // Idle is never refused: retained pages are reusable by the next decompile, so waiting
            // cannot lower them and refusal would stall the server on its own allocator high-water.
            if (m_active == 0)
                return true;
            return resident + estimate <= m_byteBudget;
        }

        void Grant(std::uint64_t estimate) {
            ++m_active;
            m_reservedBytes += estimate;
        }

      public:
        DecompileGate(unsigned int maxActive, unsigned int maxWaiting, std::uint64_t byteBudget, std::function<std::uint64_t()> memoryProbe = {})
            : m_maxActive(std::max(1u, maxActive)), m_maxWaiting(std::max(0u, maxWaiting)), m_byteBudget(byteBudget), m_memoryProbe(std::move(memoryProbe)) {}

        bool Acquire(std::uint64_t estimate, std::chrono::milliseconds maxWait) {
            const auto deadline = std::chrono::steady_clock::now() + maxWait;
            std::unique_lock lock(m_mutex);
            if (Fits(estimate)) {
                Grant(estimate);
                return true;
            }
            if (m_waiting >= m_maxWaiting)
                return false;
            ++m_waiting;
            struct WaitingExit {
                unsigned int &waiting;
                ~WaitingExit() { --waiting; }
            } waitingExit{m_waiting};
            for (;;) {
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline)
                    return false;
                // Wake on completion; poll so measured memory dropping below the budget is observed.
                m_released.wait_until(lock, std::min(deadline, now + kMemoryPollInterval), [this, estimate] { return Fits(estimate); });
                if (Fits(estimate)) {
                    Grant(estimate);
                    return true;
                }
            }
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
