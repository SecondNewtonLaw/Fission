//
// Created by Dottik on 2/6/2026.
//

#pragma once
#include <libassert/assert.hpp>

#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>

namespace Fission {
    // thrown by the scoped handler so the decompiler boundary recovers instead of abort()ing.
    struct DecompilerError : std::runtime_error {
        using std::runtime_error::runtime_error;
    };

    // wall-clock budget for one decompilation. hot loops call CheckDecompileDeadline(); a hostile input
    // (unbounded CFG growth or runaway lifting that balloons RAM) then throws at the boundary instead of
    // hanging or exhausting memory. thread-local since the pipeline runs one decompile at a time.
    class ScopedDecompileBudget {
        static inline thread_local std::optional<std::chrono::steady_clock::time_point> s_deadline;
        std::optional<std::chrono::steady_clock::time_point> m_previous;

      public:
        explicit ScopedDecompileBudget(std::chrono::steady_clock::duration budget) : m_previous(s_deadline) {
            s_deadline = std::chrono::steady_clock::now() + budget;
        }
        ~ScopedDecompileBudget() { s_deadline = m_previous; }

        ScopedDecompileBudget(const ScopedDecompileBudget &) = delete;
        ScopedDecompileBudget &operator=(const ScopedDecompileBudget &) = delete;

        static void Check() {
            if (s_deadline && std::chrono::steady_clock::now() > *s_deadline) {
                throw DecompilerError("decompilation exceeded its time budget");
            }
        }
    };

    inline void CheckDecompileDeadline() { ScopedDecompileBudget::Check(); }

    // RAII: routes libassert failures through a throwing handler for the lifetime of the
    // scope, restoring the previous handler on destruction. lets a bad bytecode unwind to
    // the top-level catch instead of taking the whole process down. handler is process
    // global, so one decompile at a time (the lifter isn't reentrant anyway).
    class ScopedThrowingAssertHandler {
        libassert::handler_ptr m_previousHandler;

        [[noreturn]] static void ThrowingHandler(const libassert::assertion_info &info) {
#ifndef PRODUCTION_BUILD
            libassert::enable_virtual_terminal_processing_if_needed();
            const std::string message = info.to_string(0, libassert::color_scheme::blank);
            std::fprintf(stderr, "\n*** assertion failed (recovered) ***\n%s\n", message.c_str());
            throw DecompilerError(message);
#else
            (void)info; // don't leak internal layout in prod.
            throw DecompilerError("internal decompiler error");
#endif
        }

      public:
        ScopedThrowingAssertHandler() : m_previousHandler(libassert::get_failure_handler()) { libassert::set_failure_handler(&ThrowingHandler); }
        ~ScopedThrowingAssertHandler() { libassert::set_failure_handler(m_previousHandler); }

        ScopedThrowingAssertHandler(const ScopedThrowingAssertHandler &) = delete;
        ScopedThrowingAssertHandler &operator=(const ScopedThrowingAssertHandler &) = delete;
        ScopedThrowingAssertHandler(ScopedThrowingAssertHandler &&) = delete;
        ScopedThrowingAssertHandler &operator=(ScopedThrowingAssertHandler &&) = delete;
    };
} // namespace Fission
