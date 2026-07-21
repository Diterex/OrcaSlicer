#pragma once

#include <string>
#include <map>
#include <functional>
#include <memory>
#include <vector>
#include <algorithm>

#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/beast/core.hpp>

#define MCP_API_PORT 13619

namespace Slic3r { namespace GUI {

// A minimal standalone HTTP server for the MCP API.
// Completely independent from the existing OAuth HttpServer.
class McpApiServer {
public:
    struct Response {
        int status_code = 200;
        std::string status_text = "OK";
        std::string content_type = "application/json";
        std::string body;
        std::vector<unsigned char> binary_body;
        bool is_binary = false;
        std::map<std::string, std::string> extra_headers;
    };

    using request_handler_fn = std::function<Response(
        const std::string& method,
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& headers)>;

    explicit McpApiServer(int port = MCP_API_PORT);
    ~McpApiServer();

    void set_handler(request_handler_fn handler);
    void start();
    void stop();
    bool is_running() const { return m_running; }
    int  port() const { return m_port; }
    void set_port(int port) { if (!m_running) m_port = port; }
    // Shared secret required on every non-preflight request (Authorization:
    // Bearer <token>, or X-Mcp-Token). Empty token disables the check (not
    // recommended). Set before start().
    void set_auth_token(const std::string& t) { if (!m_running) m_auth_token = t; }

private:
    class Session;
    class Listener;

    void run_io();

    int m_port;
    bool m_running = false;
    std::string m_auth_token;
    request_handler_fn m_handler;
    boost::asio::io_context m_ioc;
    std::unique_ptr<Listener> m_listener;
    boost::thread m_thread;
};

// Internal: TCP listener that accepts connections
class McpApiServer::Listener {
public:
    Listener(boost::asio::io_context& ioc, int port, request_handler_fn& handler,
             std::string auth_token);
    void start_accept();
    void stop();

private:
    boost::asio::io_context& m_ioc;
    boost::asio::ip::tcp::acceptor m_acceptor;
    request_handler_fn& m_handler;
    int m_port;
    std::string m_auth_token;
};

// Internal: HTTP session that handles one request
class McpApiServer::Session : public std::enable_shared_from_this<Session> {
public:
    Session(boost::asio::ip::tcp::socket socket, request_handler_fn& handler,
            int port, std::string auth_token);
    void start();

private:
    void read_request_line();
    void read_headers();
    void read_body(int content_length);
    void process_request(const std::string& body);
    void send_response(const Response& resp);

    // Security helpers (block cross-origin/DNS-rebinding and unauthenticated use).
    bool host_is_local() const;      // Host header must be localhost:port (or absent)
    bool origin_is_local() const;    // Origin, if present, must be a localhost origin
    bool is_authorized() const;      // token must match (unless no token configured)
    std::string cors_origin() const; // echo a validated localhost Origin, else ""

    boost::asio::ip::tcp::socket m_socket;
    boost::asio::streambuf m_buf;
    request_handler_fn& m_handler;
    int m_port;
    std::string m_auth_token;

    std::string m_method;
    std::string m_url;
    std::map<std::string, std::string> m_headers;
};

}} // namespace Slic3r::GUI
