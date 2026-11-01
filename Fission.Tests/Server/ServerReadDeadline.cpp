// Exercise the total request read deadline over real loopback sockets. A client that dribbles one
// byte at a time must not be able to extend the read past the absolute deadline.

#ifdef __clang__
#pragma clang diagnostic push
#if __has_warning("-Wc2y-extensions")
#pragma clang diagnostic ignored "-Wc2y-extensions"
#endif
#pragma clang diagnostic ignored "-Wlanguage-extension-token"
#pragma clang diagnostic ignored "-Wgnu-anonymous-struct"
#pragma clang diagnostic ignored "-Wnested-anon-types"
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#pragma clang diagnostic ignored "-Wmicrosoft-cpp-macro"
#pragma clang diagnostic ignored "-Wunused-parameter"
#endif
#include "ServerTransport.hpp"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

namespace {
    struct DribbleOutcome {
        beast::error_code error;
        std::chrono::milliseconds elapsed{0};
    };

    // Send `initial` immediately, then one byte every 100 ms. With `dribbleBytes == 0`, keep the
    // connection open without sending anything so the read must expire on its deadline. The client
    // stops after five seconds, or as soon as the server completes, so a regression cannot hang the
    // test process.
    DribbleOutcome ReadDribbledRequest(const std::string &initial, std::size_t dribbleBytes, std::chrono::milliseconds deadline) {
        net::io_context ioc;
        tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), 0}};
        const auto port = acceptor.local_endpoint().port();

        DribbleOutcome outcome;
        std::atomic<bool> serverDone{false};
        std::thread server([&] {
            struct DoneFlag {
                std::atomic<bool> &flag;
                ~DoneFlag() { flag.store(true, std::memory_order_release); }
            } done{serverDone};

            tcp::socket peer{ioc};
            beast::error_code ec;
            acceptor.accept(peer, ec);
            if (ec) {
                outcome.error = ec;
                return;
            }

            beast::flat_buffer buffer;
            http::request_parser<http::string_body> parser;
            parser.body_limit(32ull * 1024 * 1024);
            const auto started = std::chrono::steady_clock::now();
            outcome.error = Fission::Server::ReadHttpRequest(peer, buffer, parser, started + deadline);
            outcome.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            peer.shutdown(tcp::socket::shutdown_both, ec);
        });

        tcp::socket client{ioc};
        beast::error_code ec;
        client.connect({net::ip::make_address("127.0.0.1"), port}, ec);
        REQUIRE_FALSE(ec);

        net::write(client, net::buffer(initial), ec);
        REQUIRE_FALSE(ec);

        client.non_blocking(true);
        const auto started = std::chrono::steady_clock::now();
        std::size_t sent = 0;
        while (!serverDone.load(std::memory_order_acquire) && std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            if (sent >= dribbleBytes) {
                if (dribbleBytes == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const char byte = 'x';
            net::write(client, net::buffer(&byte, 1), ec);
            if (ec && ec != net::error::would_block && ec != net::error::try_again)
                break;
            ec = {};
            ++sent;
        }

        // Closing first lets a regressed unbounded read finish via EOF so the elapsed-time check can
        // report the failure instead of the join blocking forever.
        client.close(ec);
        server.join();
        return outcome;
    }

    void CheckStoppedAtDeadline(const DribbleOutcome &outcome) {
        CHECK(outcome.error);
        CHECK(outcome.elapsed >= std::chrono::milliseconds(600));
        CHECK(outcome.elapsed < std::chrono::seconds(2));
    }
} // namespace

TEST_CASE("Server: a dribbling body is cut off at the total read deadline", "[Server][Transport]") {
    // A complete header that promises 50 body bytes, then dribble those bytes slowly.
    const auto outcome = ReadDribbledRequest("POST /decompile HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 50\r\n\r\n", 50, std::chrono::milliseconds(800));
    CheckStoppedAtDeadline(outcome);
}

TEST_CASE("Server: an incomplete dribbled header is cut off at the total read deadline", "[Server][Transport]") {
    // A header that never finishes must still be closed at the absolute deadline, not extended by
    // each arriving byte.
    const auto outcome = ReadDribbledRequest("POST /decompile HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Drip: ", 50, std::chrono::milliseconds(800));
    CheckStoppedAtDeadline(outcome);
}

TEST_CASE("Server: a silent partial request is cut off at the total read deadline", "[Server][Transport]") {
    // On POSIX, a blocking receive can hide inside Asio's EAGAIN retry loop. Keep the connection
    // open and send nothing after the partial header; the read must return on the deadline.
    const auto outcome = ReadDribbledRequest("POST /decompile HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Silent: ", 0, std::chrono::milliseconds(800));
    CheckStoppedAtDeadline(outcome);
}

TEST_CASE("Server: a complete request sent in one write finishes before the deadline", "[Server][Transport]") {
    net::io_context ioc;
    tcp::acceptor acceptor{ioc, {net::ip::make_address("127.0.0.1"), 0}};
    const auto port = acceptor.local_endpoint().port();

    beast::error_code readError;
    bool requestDone = false;
    std::chrono::milliseconds elapsed{0};
    std::thread server([&] {
        tcp::socket peer{ioc};
        beast::error_code ec;
        acceptor.accept(peer, ec);
        if (ec) {
            readError = ec;
            return;
        }

        beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(32ull * 1024 * 1024);
        const auto started = std::chrono::steady_clock::now();
        readError = Fission::Server::ReadHttpRequest(peer, buffer, parser, started + std::chrono::seconds(2));
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
        requestDone = parser.is_done();
        peer.shutdown(tcp::socket::shutdown_both, ec);
    });

    tcp::socket client{ioc};
    beast::error_code ec;
    client.connect({net::ip::make_address("127.0.0.1"), port}, ec);
    REQUIRE_FALSE(ec);

    // Beast's parser is not eager: header and body in one receive can leave the body buffered.
    const std::string request = "POST /decompile HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 4\r\n\r\nbody";
    net::write(client, net::buffer(request), ec);
    REQUIRE_FALSE(ec);

    server.join();
    client.close(ec);

    CHECK_FALSE(readError);
    CHECK(requestDone);
    CHECK(elapsed < std::chrono::seconds(1));
}
