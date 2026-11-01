#pragma once

#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

#include <chrono>

namespace Fission::Server {
    // Read one HTTP request, enforcing an absolute deadline across every receive. A client that
    // dribbles bytes slowly cannot extend the read past this deadline.
    boost::beast::error_code ReadHttpRequest(
        boost::asio::ip::tcp::socket &socket, boost::beast::flat_buffer &buffer, boost::beast::http::request_parser<boost::beast::http::string_body> &parser,
        std::chrono::steady_clock::time_point deadline
    );
} // namespace Fission::Server
