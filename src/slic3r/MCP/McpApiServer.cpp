#include "McpApiServer.h"

#include <iostream>
#include <sstream>
#include <boost/log/trivial.hpp>

using boost::asio::ip::tcp;

namespace Slic3r { namespace GUI {

namespace {
// Split an authority "host[:port]" or an origin "scheme://host[:port]" into a
// lowercased host and its (optional) port string.
void split_authority(std::string s, std::string& host, std::string& port) {
    host.clear();
    port.clear();
    auto scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    auto at = s.rfind('@');                          // drop any userinfo (user:pass@)
    if (at != std::string::npos) s = s.substr(at + 1);
    if (!s.empty() && s.front() == '[') {            // IPv6 literal: [::1]:port
        auto rb = s.find(']');
        if (rb == std::string::npos) { host = s; }
        else {
            host = s.substr(1, rb - 1);
            if (rb + 1 < s.size() && s[rb + 1] == ':') port = s.substr(rb + 2);
        }
    } else {
        auto colon = s.rfind(':');
        if (colon != std::string::npos) { host = s.substr(0, colon); port = s.substr(colon + 1); }
        else host = s;
    }
    std::transform(host.begin(), host.end(), host.begin(), ::tolower);
}

bool host_is_loopback(const std::string& host) {
    return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Constant-time string compare so token validation doesn't leak the secret via
// response timing. Length is not secret (fixed-size token), so an early length
// check is fine.
bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char r = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        r |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    return r == 0;
}
} // namespace

// ---------------------------------------------------------------------------
// McpApiServer
// ---------------------------------------------------------------------------

McpApiServer::McpApiServer(int port) : m_port(port) {}

McpApiServer::~McpApiServer() { stop(); }

void McpApiServer::set_handler(request_handler_fn handler) {
    m_handler = std::move(handler);
}

void McpApiServer::start() {
    if (m_running) return;
    if (!m_handler) return;

    m_listener = std::make_unique<Listener>(m_ioc, m_port, m_handler, m_auth_token);
    m_listener->start_accept();
    m_running = true;
    m_thread = boost::thread([this]() { run_io(); });

    BOOST_LOG_TRIVIAL(info) << "MCP API server started on port " << m_port;
}

void McpApiServer::stop() {
    if (!m_running) return;
    m_running = false;
    if (m_listener) m_listener->stop();
    m_ioc.stop();
    if (m_thread.joinable()) m_thread.join();
    BOOST_LOG_TRIVIAL(info) << "MCP API server stopped";
}

void McpApiServer::run_io() {
    try {
        m_ioc.run();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MCP API server error: " << e.what();
    }
}

// ---------------------------------------------------------------------------
// Listener
// ---------------------------------------------------------------------------

McpApiServer::Listener::Listener(boost::asio::io_context& ioc, int port, request_handler_fn& handler,
                                 std::string auth_token)
    : m_ioc(ioc)
    , m_acceptor(ioc, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port))
    , m_handler(handler)
    , m_port(port)
    , m_auth_token(std::move(auth_token))
{
    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true));
}

void McpApiServer::Listener::start_accept() {
    m_acceptor.async_accept(
        [this](const boost::system::error_code& ec, tcp::socket socket) {
            if (!ec) {
                auto sess = std::make_shared<Session>(std::move(socket), m_handler, m_port, m_auth_token);
                sess->start();
            }
            if (m_acceptor.is_open()) {
                start_accept();
            }
        });
}

void McpApiServer::Listener::stop() {
    boost::system::error_code ec;
    m_acceptor.close(ec);
}

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

McpApiServer::Session::Session(tcp::socket socket, request_handler_fn& handler,
                               int port, std::string auth_token)
    : m_socket(std::move(socket))
    , m_handler(handler)
    , m_port(port)
    , m_auth_token(std::move(auth_token)) {}

void McpApiServer::Session::start() {
    read_request_line();
}

// --- Security helpers --------------------------------------------------------

bool McpApiServer::Session::host_is_local() const {
    auto it = m_headers.find("host");
    // HTTP/1.1 requires Host, and legitimate local MCP clients always send it.
    // Fail closed on an absent Host so this stays a real second layer rather
    // than leaving the token as the only control.
    if (it == m_headers.end()) return false;
    std::string host, port;
    split_authority(it->second, host, port);
    if (!host_is_loopback(host)) return false;
    if (!port.empty() && port != std::to_string(m_port)) return false;
    return true;
}

bool McpApiServer::Session::origin_is_local() const {
    auto it = m_headers.find("origin");
    if (it == m_headers.end()) return true;   // non-browser clients send no Origin
    if (it->second == "null") return false;   // opaque origin (sandboxed/file)
    std::string host, port;
    split_authority(it->second, host, port);
    return host_is_loopback(host);
}

bool McpApiServer::Session::is_authorized() const {
    if (m_auth_token.empty()) return true;    // no token configured (discouraged)
    auto get = [&](const char* k) -> std::string {
        auto it = m_headers.find(k);
        return it == m_headers.end() ? std::string() : it->second;
    };
    std::string auth = get("authorization");
    if (!auth.empty()) {
        auto sp = auth.find(' ');
        std::string scheme = (sp == std::string::npos) ? auth : auth.substr(0, sp);
        std::string cred   = (sp == std::string::npos) ? std::string() : auth.substr(sp + 1);
        std::transform(scheme.begin(), scheme.end(), scheme.begin(), ::tolower);
        if (scheme == "bearer" && constant_time_eq(cred, m_auth_token)) return true;
    }
    return constant_time_eq(get("x-mcp-token"), m_auth_token);
}

std::string McpApiServer::Session::cors_origin() const {
    auto it = m_headers.find("origin");
    if (it == m_headers.end()) return "";
    return origin_is_local() ? it->second : std::string();
}

void McpApiServer::Session::read_request_line() {
    auto self = shared_from_this();
    boost::asio::async_read_until(m_socket, m_buf, "\r\n",
        [this, self](const boost::system::error_code& ec, std::size_t) {
            if (ec) return;
            std::istream stream(&m_buf);
            std::string line;
            std::getline(stream, line);
            // Remove trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            std::istringstream iss(line);
            std::string version;
            iss >> m_method >> m_url >> version;

            read_headers();
        });
}

void McpApiServer::Session::read_headers() {
    auto self = shared_from_this();
    boost::asio::async_read_until(m_socket, m_buf, "\r\n",
        [this, self](const boost::system::error_code& ec, std::size_t) {
            if (ec) return;
            std::istream stream(&m_buf);
            std::string line;
            std::getline(stream, line);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty()) {
                // End of headers
                auto it = m_headers.find("content-length");
                int cl = 0;
                if (it != m_headers.end()) {
                    try { cl = std::stoi(it->second); } catch (...) {}
                }
                if (cl > 0) {
                    read_body(cl);
                } else {
                    process_request("");
                }
                return;
            }

            // Parse header: "Name: Value"
            auto colon = line.find(':');
            if (colon != std::string::npos) {
                std::string name = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                // Trim leading space from value
                if (!value.empty() && value[0] == ' ')
                    value = value.substr(1);
                // Lowercase the header name for case-insensitive lookup
                std::transform(name.begin(), name.end(), name.begin(), ::tolower);
                m_headers[name] = value;
            }

            // Read next header
            read_headers();
        });
}

void McpApiServer::Session::read_body(int content_length) {
    auto self = shared_from_this();

    // Some data may already be in the buffer from header reading
    size_t already = m_buf.size();
    if ((int)already >= content_length) {
        std::istream stream(&m_buf);
        std::string body(content_length, '\0');
        stream.read(&body[0], content_length);
        process_request(body);
        return;
    }

    int remaining = content_length - (int)already;
    boost::asio::async_read(m_socket, m_buf,
        boost::asio::transfer_at_least(remaining),
        [this, self, content_length](const boost::system::error_code& ec, std::size_t) {
            if (ec && ec != boost::asio::error::eof) return;
            std::istream stream(&m_buf);
            std::string body(content_length, '\0');
            stream.read(&body[0], content_length);
            process_request(body);
        });
}

void McpApiServer::Session::process_request(const std::string& body) {
    auto self = shared_from_this();

    BOOST_LOG_TRIVIAL(debug) << "MCP API: " << m_method << " " << m_url
                             << " body=" << body.size() << " bytes";

    // Reject cross-origin and DNS-rebinding attempts before doing any work: a
    // web page must not be able to reach this localhost server. Non-browser
    // clients (no Origin/Host) are unaffected.
    if (!host_is_local() || !origin_is_local()) {
        Response resp;
        resp.status_code = 403;
        resp.status_text = "Forbidden";
        resp.content_type = "application/json";
        resp.body = "{\"ok\":false,\"error\":\"forbidden: non-local Host/Origin\"}";
        send_response(resp);
        return;
    }

    // CORS preflight -- answered only after the origin check above passes.
    if (m_method == "OPTIONS") {
        Response resp;
        resp.status_code = 204;
        resp.status_text = "No Content";
        resp.body = "";
        send_response(resp);
        return;
    }

    // Every real request must carry the shared token.
    if (!is_authorized()) {
        Response resp;
        resp.status_code = 401;
        resp.status_text = "Unauthorized";
        resp.content_type = "application/json";
        resp.extra_headers["WWW-Authenticate"] = "Bearer";
        resp.body = "{\"ok\":false,\"error\":\"unauthorized: missing or invalid MCP token\"}";
        send_response(resp);
        return;
    }

    Response resp;
    try {
        resp = m_handler(m_method, m_url, body, m_headers);
    } catch (const std::exception& e) {
        resp.status_code = 500;
        resp.status_text = "Internal Server Error";
        resp.content_type = "application/json";
        resp.body = "{\"ok\":false,\"error\":\"" + std::string(e.what()) + "\"}";
    }

    send_response(resp);
}

void McpApiServer::Session::send_response(const Response& resp) {
    auto self = shared_from_this();

    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    // Echo the caller's Origin only when it is a validated localhost origin --
    // never a wildcard, so a random web page gets no CORS grant.
    std::string origin = cors_origin();
    if (!origin.empty()) {
        ss << "Access-Control-Allow-Origin: " << origin << "\r\n";
        ss << "Vary: Origin\r\n";
    }
    ss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    ss << "Access-Control-Allow-Headers: Content-Type, Accept, Authorization, X-Mcp-Token, Mcp-Session-Id\r\n";
    ss << "Access-Control-Expose-Headers: Mcp-Session-Id\r\n";
    ss << "Connection: close\r\n";

    // Emit extra headers (e.g., Mcp-Session-Id)
    for (const auto& [key, value] : resp.extra_headers) {
        ss << key << ": " << value << "\r\n";
    }

    if (resp.is_binary) {
        ss << "Content-Type: " << resp.content_type << "\r\n";
        ss << "Content-Length: " << resp.binary_body.size() << "\r\n";
        ss << "\r\n";
        // Write header + binary body
        auto header_str = std::make_shared<std::string>(ss.str());
        auto bin_data = std::make_shared<std::vector<unsigned char>>(resp.binary_body);
        std::vector<boost::asio::const_buffer> buffers;
        buffers.push_back(boost::asio::buffer(*header_str));
        buffers.push_back(boost::asio::buffer(*bin_data));
        boost::asio::async_write(m_socket, buffers,
            [self, header_str, bin_data](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ec;
                self->m_socket.shutdown(tcp::socket::shutdown_both, ec);
            });
    } else {
        ss << "Content-Type: " << resp.content_type << "\r\n";
        ss << "Content-Length: " << resp.body.size() << "\r\n";
        ss << "\r\n";
        ss << resp.body;
        auto data = std::make_shared<std::string>(ss.str());
        boost::asio::async_write(m_socket, boost::asio::buffer(*data),
            [self, data](const boost::system::error_code&, std::size_t) {
                boost::system::error_code ec;
                self->m_socket.shutdown(tcp::socket::shutdown_both, ec);
            });
    }
}

}} // namespace Slic3r::GUI
