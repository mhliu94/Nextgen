// HTTP/HTTPS server: static single-page app, JSON APIs, server-sent event streams and the
// optional plain-HTTP redirector. Routes mirror the Python server's paths so bookmarks and
// external API callers keep working.
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "web/control.h"
#include "web/sessions.h"

namespace httplib {
class Server;
}

namespace ng::web {

struct ServerOptions {
    std::string host = "0.0.0.0";
    int port = 8000;  // 0 = any free port (tests)
    bool ssl = false;
    std::string ssl_certfile;
    std::string ssl_keyfile;
    int threads = 0;  // 0 = automatic
    std::optional<std::string> static_dir;  // serve assets from disk instead of the embedded copies
    // Plain-HTTP -> HTTPS redirector (0 disables).
    int redirect_port = 0;
    std::string redirect_host = "0.0.0.0";
    std::string redirect_target_host;
    int redirect_target_port = 443;
    // Session cookie flags.
    bool secure_cookies = false;
};

class Server {
public:
    Server(ControlCenter& control, SessionStore& sessions, ServerOptions options);
    ~Server();
    // Binds and starts serving on background threads. Throws std::runtime_error on failure.
    void start();
    void stop();
    int port() const { return bound_port_; }
    bool running() const { return running_.load(); }
    const ServerOptions& options() const { return options_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ControlCenter& control_;
    SessionStore& sessions_;
    ServerOptions options_;
    int bound_port_ = 0;
    std::atomic<bool> running_{false};
    std::thread serve_thread_;
    std::thread redirect_thread_;
};

}  // namespace ng::web
