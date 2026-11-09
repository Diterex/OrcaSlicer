// src/slic3r/GUI/RemoteAPI/RemoteAPIServer.cpp
#include "RemoteAPIServer.hpp"

#include "libslic3r/Thread.hpp"
#include <boost/log/trivial.hpp>
#include <cctype>
#include <deque>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
using tcp       = net::ip::tcp;

namespace Slic3r { namespace GUI { namespace RemoteAPI {

// One WebSocket connection. All socket I/O runs on the single io thread.
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    WsSession(tcp::socket socket, Server &server)
        : m_ws(std::move(socket)), m_server(server) {}

    void run(http::request<http::string_body> req)
    {
        m_ws.set_option(beast::websocket::stream_base::timeout::suggested(beast::role_type::server));
        // beast requires the upgrade request to stay valid until async_accept
        // completes, so keep it alive in a shared_ptr captured by the handler.
        auto req_held = std::make_shared<http::request<http::string_body>>(std::move(req));
        m_ws.async_accept(*req_held, [self = shared_from_this(), req_held](beast::error_code ec) {
            if (ec) return;
            self->m_server.ws_join(self);
            self->do_read();
        });
    }

    // Called on the io thread (via net::post in Server::broadcast).
    void send(const std::string &text)
    {
        // A client that stops reading must not grow this queue without bound;
        // past the cap new events are dropped for that client only.
        static constexpr size_t k_max_queued = 256;
        if (m_queue.size() >= k_max_queued) {
            if (!m_dropping) BOOST_LOG_TRIVIAL(warning) << "remote-api: WebSocket client not reading, dropping events";
            m_dropping = true;
            return;
        }
        m_dropping = false;
        m_queue.push_back(text);
        if (m_queue.size() == 1) do_write();
    }

    // Only after the io thread has been joined (Server::stop): closes the socket
    // while its io_context still exists.
    void close_socket()
    {
        beast::error_code ec;
        beast::get_lowest_layer(m_ws).socket().close(ec);
    }

private:
    void do_read()
    { // inbound messages are ignored; the read just detects disconnect
        m_ws.async_read(m_buffer, [self = shared_from_this()](beast::error_code ec, std::size_t) {
            if (ec) { self->m_server.ws_leave(self); return; }
            self->m_buffer.consume(self->m_buffer.size());
            self->do_read();
        });
    }
    void do_write()
    {
        m_ws.text(true);
        m_ws.async_write(net::buffer(m_queue.front()),
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) { self->m_server.ws_leave(self); return; }
                self->m_queue.pop_front();
                if (!self->m_queue.empty()) self->do_write();
            });
    }

    beast::websocket::stream<beast::tcp_stream> m_ws;
    beast::flat_buffer                          m_buffer;
    std::deque<std::string>                     m_queue;
    bool                                        m_dropping { false };
    Server                                     &m_server;
};

// One HTTP connection. Reads a request, answers it, closes (Connection: close).
class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket socket, Server &server)
        : m_stream(std::move(socket)), m_server(server)
    {}

    void run() { do_read(); }

private:
    void do_read()
    {
        m_stream.expires_after(std::chrono::seconds(15));
        m_parser.body_limit(4 * 1024 * 1024); // bound request body before buffering
        // Headers first: reject a request that is not ours (browser origin,
        // foreign Host, bad token) before reading up to 4 MB of body.
        http::async_read_header(m_stream, m_buffer, m_parser,
            [self = shared_from_this()](beast::error_code ec, std::size_t) {
                if (ec) return; // closed / timeout / parse error
                if (std::optional<Response> early = self->screen_headers()) {
                    self->write_response(*early);
                    return;
                }
                http::async_read(self->m_stream, self->m_buffer, self->m_parser,
                    [self](beast::error_code ec2, std::size_t) {
                        if (ec2) return; // closed / timeout / parse error / body too large
                        self->handle();
                    });
            });
    }

    // Token from ?token= on the events WebSocket upgrade (browser WS clients
    // cannot set headers), else the X-Api-Token header.
    std::string presented_token() const
    {
        const auto &req = m_parser.get();
        if (beast::websocket::is_upgrade(req)) {
            std::string target = std::string(req.target());
            auto        tpos   = target.find("token=");
            std::string wtok   = (tpos == std::string::npos) ? std::string() : target.substr(tpos + 6);
            if (auto amp = wtok.find('&'); amp != std::string::npos) wtok.resize(amp);
            return wtok;
        }
        auto tok_sv = req["X-Api-Token"];
        return std::string(tok_sv.begin(), tok_sv.end());
    }

    // Header checks that need no body. Returns the reply for a rejected request.
    std::optional<Response> screen_headers() const
    {
        const auto &req = m_parser.get();
        // No legitimate client of this API is a web page. A browser always sends
        // Origin on cross-site and WebSocket requests, so any Origin is refused
        // (stops a page in the user's browser from driving the slicer).
        if (req.find(http::field::origin) != req.end())
            return Response{ 403, {{"error", "origin_not_allowed"}} };
        // DNS rebinding: with the loopback-only default, the Host must name the
        // loopback interface. (With LAN access on, any Host is accepted.)
        if (!m_server.config().bind_lan && req.find(http::field::host) != req.end()) {
            auto        h_sv = req[http::field::host];
            std::string host(h_sv.begin(), h_sv.end());
            if (!host.empty() && host.front() == '[') { // [::1]:port
                auto rb = host.find(']');
                host = rb == std::string::npos ? host : host.substr(0, rb + 1);
            } else if (auto colon = host.rfind(':'); colon != std::string::npos) {
                host.resize(colon);
            }
            for (auto &c : host) c = (char) std::tolower((unsigned char) c);
            if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
                return Response{ 403, {{"error", "host_not_allowed"}} };
        }
        if (!m_server.check_token(presented_token()))
            return Response{ 401, {{"error", "unauthorized"}} };
        return std::nullopt;
    }

    void handle()
    {
        auto &req = m_parser.get();
        // WebSocket upgrade path: auth via ?token= query param (browser WS
        // clients can't set request headers). Runs before token-header auth.
        if (beast::websocket::is_upgrade(req)) {
            std::string target = std::string(req.target());
            auto        tpos   = target.find("token=");
            std::string wtok   = (tpos == std::string::npos) ? std::string() : target.substr(tpos + 6);
            if (auto amp = wtok.find('&'); amp != std::string::npos) wtok.resize(amp);
            if (target.rfind("/api/v1/events", 0) == 0 && m_server.check_token(wtok)) {
                std::make_shared<WsSession>(m_stream.release_socket(), m_server)->run(req);
                return;
            }
            // otherwise fall through to the normal 401/404 JSON response
        }
        // Auth first, everything else second (already screened at the header
        // stage; checked again here so this path never depends on that order).
        Response    r;
        if (!m_server.check_token(presented_token())) {
            r = { 401, {{"error", "unauthorized"}} };
        } else if (!m_server.handler()) {
            r = { 503, {{"error", "no_handler"}} };
        } else {
            Request rq { std::string(req.method_string()),
                         std::string(req.target()),
                         req.body() };
            try {
                r = m_server.handler()(rq);
            } catch (const std::exception &e) {
                // The API must never take the slicer down.
                BOOST_LOG_TRIVIAL(error) << "remote-api handler exception: " << e.what();
                r = { 500, {{"error", "internal"}, {"detail", e.what()}} };
            }
        }
        write_response(r);
    }

    void write_response(const Response &r)
    {
        auto &req = m_parser.get();
        // Build the response defensively: json::dump() can throw on malformed
        // data (e.g. non-UTF-8 bytes), and this runs on the io thread where an
        // uncaught exception would call std::terminate() and kill the slicer.
        std::shared_ptr<http::response<http::string_body>> res;
        try {
            res = std::make_shared<http::response<http::string_body>>(
                static_cast<http::status>(r.status), req.version());
            res->set(http::field::server, "orca-remote-api");
            res->keep_alive(false);
            if (r.raw_body) {
                res->set(http::field::content_type, r.raw_content_type);
                res->body() = *r.raw_body;
            } else {
                res->set(http::field::content_type, "application/json");
                res->body() = r.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
            res->prepare_payload();
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "remote-api: response build exception: " << e.what();
            res = std::make_shared<http::response<http::string_body>>(
                http::status::internal_server_error, req.version());
            res->set(http::field::content_type, "application/json");
            res->set(http::field::server, "orca-remote-api");
            res->keep_alive(false);
            res->body() = "{\"error\":\"internal\"}";
            res->prepare_payload();
        }
        m_stream.expires_after(std::chrono::seconds(15)); // re-arm timeout for the write (slow-reader guard)
        http::async_write(m_stream, *res,
            [self = shared_from_this(), res](beast::error_code, std::size_t) {
                beast::error_code ec2;
                self->m_stream.socket().shutdown(tcp::socket::shutdown_send, ec2);
            });
    }

    beast::tcp_stream                          m_stream;
    beast::flat_buffer                         m_buffer;
    http::request_parser<http::string_body>    m_parser;
    Server                                     &m_server;
};

void Server::start(const Config &cfg)
{
    if (m_running) stop();
    m_cfg = cfg;
    { std::lock_guard<std::mutex> lk(m_ws_mutex); m_last_error.clear(); }
    try {
        m_ioc = std::make_unique<net::io_context>(1);
        auto address = m_cfg.bind_lan ? net::ip::address_v4::any()
                                      : net::ip::make_address_v4("127.0.0.1");
        tcp::endpoint endpoint(address, static_cast<unsigned short>(m_cfg.port));
        m_acceptor = std::make_unique<tcp::acceptor>(*m_ioc);
        m_acceptor->open(endpoint.protocol());
#ifdef _WIN32
        // CeramicaSlicer: on Windows SO_REUSEADDR lets a second process bind the
        // SAME port and silently steal connections. SO_EXCLUSIVEADDRUSE refuses
        // that, so two apps can never share the API port. Trade-off: a very fast
        // relaunch can fail to bind while old connections sit in TIME_WAIT; the
        // failure is logged below and the API stays off for that run.
        m_acceptor->set_option(net::detail::socket_option::boolean<SOL_SOCKET, SO_EXCLUSIVEADDRUSE>(true));
#else
        // SO_REUSEADDR: rebind while a prior socket lingers in TIME_WAIT.
        // (On POSIX this does not allow two live listeners on one port.)
        m_acceptor->set_option(net::socket_base::reuse_address(true));
#endif
        m_acceptor->bind(endpoint);
        m_acceptor->listen(net::socket_base::max_listen_connections);
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "remote-api: failed to bind port " << m_cfg.port
                                 << ": " << e.what();
        { std::lock_guard<std::mutex> lk(m_ws_mutex); m_last_error = e.what(); }
        m_acceptor.reset(); // before the io_context it was created on
        m_ioc.reset();
        return; // disabled for this run; the Preferences page shows "failed" via running()
    }
    m_running = true;
    do_accept();
    m_thread = create_thread([this] {
        set_current_thread_name("remote_api");
        this->m_ioc->run();
    });
    BOOST_LOG_TRIVIAL(info) << "remote-api: listening on "
                            << (m_cfg.bind_lan ? "0.0.0.0" : "127.0.0.1")
                            << ":" << m_cfg.port;
}

void Server::do_accept()
{
    m_acceptor->async_accept([this](beast::error_code ec, tcp::socket socket) {
        if (!m_running) return;
        if (!ec)
            std::make_shared<HttpSession>(std::move(socket), *this)->run();
        do_accept();
    });
}

void Server::stop()
{
    if (!m_running) return;
    m_running = false;
    if (m_ioc) m_ioc->stop();
    if (m_thread.joinable()) m_thread.join();
    // The io thread is gone, so nothing else touches these sockets now. Every
    // socket must be closed and released BEFORE its io_context is destroyed:
    // the WebSocket sessions used to be cleared after m_ioc.reset(), so their
    // sockets were torn down against a freed io_context (use-after-free crash
    // when Preferences closed with a client connected).
    std::set<std::shared_ptr<WsSession>> sessions;
    {
        std::lock_guard<std::mutex> lk(m_ws_mutex);
        sessions.swap(m_ws_sessions);
    }
    for (auto &s : sessions) s->close_socket();
    sessions.clear();
    if (m_acceptor) {
        beast::error_code ec;
        m_acceptor->close(ec);
        m_acceptor.reset();
    }
    if (m_ioc) {
        // Let the aborted completions run so the sessions they hold are released
        // here, then destroy the io_context last.
        m_ioc->restart();
        m_ioc->poll();
        m_ioc.reset();
    }
    BOOST_LOG_TRIVIAL(info) << "remote-api: stopped";
}

bool Server::check_token(const std::string &presented) const
{
    // Constant-time comparison: this is the authentication path, so it must not
    // leak the token's contents through how early the comparison gives up. The
    // length check is separate because the token's length is not the secret.
    if (m_cfg.token.empty() || presented.size() != m_cfg.token.size())
        return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < m_cfg.token.size(); ++i)
        diff |= static_cast<unsigned char>(presented[i] ^ m_cfg.token[i]);
    return diff == 0;
}

// WS plumbing — populated in Task 11; broadcast is already safe to call.
void Server::ws_join(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.insert(s);
}
void Server::ws_leave(const std::shared_ptr<WsSession> &s)
{
    std::lock_guard<std::mutex> lk(m_ws_mutex);
    m_ws_sessions.erase(s);
}
void Server::broadcast(const nlohmann::json &event)
{
    if (!m_running || !m_ioc) return;
    // dump() can throw on non-UTF-8 (project filenames, slice warnings carry raw
    // bytes). This runs on the GUI thread; an uncaught throw could take the
    // slicer down — same hazard the HTTP response path already guards against.
    std::shared_ptr<std::string> text;
    try {
        text = std::make_shared<std::string>(
            event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "remote-api: broadcast dump failed: " << e.what();
        return;
    }
    net::post(*m_ioc, [this, text] {
        std::lock_guard<std::mutex> lk(m_ws_mutex);
        for (auto &s : m_ws_sessions) s->send(*text);
    });
}

}}} // namespace
