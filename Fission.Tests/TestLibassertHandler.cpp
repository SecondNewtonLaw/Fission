#include <cstdio>
#include <cstdlib>
#include <libassert/assert.hpp>

static void TestFailureHandler(const libassert::assertion_info &info) {
    libassert::enable_virtual_terminal_processing_if_needed();
    auto msg = info.to_string(0, libassert::color_scheme::blank);
    std::fprintf(stderr, "\n*** LIBASSERT ASSERTION FAILED ***\n%s\n", msg.c_str());
    std::fflush(stderr);
    std::_Exit(1);
}

namespace {
    struct InitHandler {
        InitHandler() { libassert::set_failure_handler(TestFailureHandler); }
    } g_handlerInit;
} // namespace
