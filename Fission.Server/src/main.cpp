#include "Decompiler.hpp"
#include "libassert/assert.hpp"
#include "luacode.h"

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
using tcp = asio::ip::tcp;

static void ServerFailureHandler(const libassert::assertion_info &info) {
    libassert::enable_virtual_terminal_processing_if_needed();
    auto msg = info.to_string(0, libassert::color_scheme::blank);
    std::fprintf(stderr, "\n*** LIBASSERT ASSERTION FAILED ***\n%s\n", msg.c_str());
    std::fflush(stderr);
    throw std::runtime_error(msg);
}

struct InitServerHandler {
    InitServerHandler() { libassert::set_failure_handler(ServerFailureHandler); }
};
static InitServerHandler g_serverHandlerInit;

struct ServerOptions {
    std::string host = "127.0.0.1";
    unsigned short port = 3001;
};

static std::mutex g_decompileMutex;

class CoutCapture {
  public:
    explicit CoutCapture(std::ostream &stream)
        : stream(stream), original(stream.rdbuf(buffer.rdbuf())) {}

    ~CoutCapture() { stream.rdbuf(original); }

  private:
    std::ostream &stream;
    std::ostringstream buffer;
    std::streambuf *original;
};

static void EnableLuauFFlags() {
    static bool enabled = false;
    if (enabled)
        return;

    for (Luau::FValue<bool> *flag = Luau::FValue<bool>::list; flag; flag = flag->next) {
        if (std::strncmp(flag->name, "Luau", 4) == 0)
            flag->value = true;
    }

    enabled = true;
}

static std::string DecodeBase64(const std::string &input) {
    static constexpr unsigned char invalid = 255;
    static unsigned char table[256];
    static bool initialized = false;

    if (!initialized) {
        std::fill(table, table + 256, invalid);
        for (int i = 0; i < 26; ++i) {
            table[static_cast<unsigned char>('A' + i)] = static_cast<unsigned char>(i);
            table[static_cast<unsigned char>('a' + i)] = static_cast<unsigned char>(26 + i);
        }
        for (int i = 0; i < 10; ++i)
            table[static_cast<unsigned char>('0' + i)] = static_cast<unsigned char>(52 + i);
        table[static_cast<unsigned char>('+')] = 62;
        table[static_cast<unsigned char>('/')] = 63;
        initialized = true;
    }

    std::string out;
    out.reserve((input.size() * 3) / 4);
    std::uint32_t value = 0;
    int bits = -8;
    bool sawData = false;

    for (unsigned char c : input) {
        if (c == '=')
            break;
        const unsigned char decoded = table[c];
        if (decoded == invalid)
            continue;

        sawData = true;
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }

    if (!sawData)
        throw std::runtime_error("Request body did not contain base64 bytecode.");

    return out;
}

static std::string DecompileBase64(const std::string &base64Body) {
    const auto bytecode = DecodeBase64(base64Body);
    std::lock_guard lock(g_decompileMutex);
    EnableLuauFFlags();

    Decompiler decompiler{};
    const auto flags = DecompilerFlags::InferRobloxTypes | DecompilerFlags::InferTypes |
                       DecompilerFlags::AutoNameVariables | DecompilerFlags::OmitFissionComments;

    CoutCapture capture(std::cout);
    const auto result = decompiler.DecompileRobloxBytecode(bytecode, flags);
    if (result.resultCode != DecompileResult::Success) {
        if (!result.decompilationOutput.empty())
            throw std::runtime_error(result.decompilationOutput);
        throw std::runtime_error("Fission failed to decompile bytecode.");
    }

    return result.decompilationOutput;
}

static http::response<http::string_body>
MakeResponse(http::status status, unsigned int version, std::string body) {
    http::response<http::string_body> response{status, version};
    response.set(http::field::server, "Fission.Server");
    response.set(http::field::content_type, "text/plain; charset=utf-8");
    response.keep_alive(false);
    response.body() = std::move(body);
    response.prepare_payload();
    return response;
}

static void HandleSession(tcp::socket socket) {
    beast::flat_buffer buffer;
    http::request_parser<http::string_body> parser;
    parser.body_limit(boost::none);
    http::read(socket, buffer, parser);
    const auto request = parser.release();
    const auto target = std::string(request.target());

    http::response<http::string_body> response;
    if (request.method() == http::verb::get && target == "/") {
        response = MakeResponse(http::status::ok, request.version(), "Fission server is running.");
    } else if (request.method() == http::verb::post && target.starts_with("/luau/decompile")) {
        try {
            response = MakeResponse(http::status::ok, request.version(), DecompileBase64(request.body()));
        } catch (const std::exception &error) {
            response = MakeResponse(http::status::internal_server_error, request.version(), error.what());
        }
    } else {
        response = MakeResponse(http::status::not_found, request.version(), "Not found.");
    }

    http::write(socket, response);
    beast::error_code ignored;
    socket.shutdown(tcp::socket::shutdown_send, ignored);
}

static ServerOptions ParseArgs(int argc, char **argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            options.port = static_cast<unsigned short>(std::stoi(argv[++i]));
        } else if ((arg == "--host" || arg == "-h") && i + 1 < argc) {
            options.host = argv[++i];
        } else if (arg == "--help") {
            std::cout << "Usage: Fission.Server [--host 127.0.0.1] [--port 3001]\n";
            std::exit(0);
        }
    }
    return options;
}

int main(int argc, char **argv) {
    try {
        const auto options = ParseArgs(argc, argv);
        asio::io_context io;
        const auto address = asio::ip::make_address(options.host);
        tcp::acceptor acceptor{io, tcp::endpoint{address, options.port}};

        std::cout << "Fission server listening on http://" << options.host << ":" << options.port
                  << "/luau/decompile\n";

        for (;;) {
            tcp::socket socket{io};
            acceptor.accept(socket);
            try {
                HandleSession(std::move(socket));
            } catch (const std::exception &error) {
                std::cerr << "HTTP session failed: " << error.what() << "\n";
            }
        }
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
