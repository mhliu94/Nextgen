// The order UI's HTTP server (order_ui/webserver.py): a read-only order monitor with session and
// tag reports and the unreconciled-orders view. It shares the trading configuration (accounts and
// users) but runs as its own process with its own session cookie.
#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "core/json.h"
#include "order_ui/config.h"
#include "order_ui/reconciliation.h"
#include "order_ui/store.h"
#include "reporting/activity_reporting.h"

namespace httplib {
class Server;
}

namespace ng::order_ui {

inline constexpr const char* kSessionCookie = "ktrader_order_ui_session";
inline constexpr int kSessionSeconds = 12 * 3600;

// Keeps proprietary participation settings out of browser responses (recursively drops the keys
// participation, participation_percentage and participation_percentages).
json public_algo_parameters(const json& value);

struct OrderServerOptions {
    std::string host = "127.0.0.1";
    int port = 8030;  // 0 = any free port (tests)
    std::string ssl_certfile;
    std::string ssl_keyfile;
    std::optional<std::string> static_dir;  // serve the web assets from disk instead of the embedded copies
    int threads = 8;
};

class OrderServer {
public:
    using Clock = std::function<double()>;  // wall-clock epoch seconds

    OrderServer(const OrderUiConfig& config, OrderStore& store, reporting::ActivityReporting& reporting,
                ReconciliationView& reconciliation, OrderServerOptions options, Clock now = nullptr);
    ~OrderServer();

    // Binds and serves on a background thread. Throws std::runtime_error on failure.
    void start();
    void stop();
    int port() const { return bound_port_; }

    // The /api/orders document for a signed-in user (exposed for tests).
    json orders_payload(const std::string& username);

private:
    struct Impl;
    std::optional<std::string> session_user(const std::string& token);
    std::string create_session(const std::string& username, const std::string& previous_token);
    void drop_session(const std::string& token);

    const OrderUiConfig& config_;
    OrderStore& store_;
    reporting::ActivityReporting& reporting_;
    ReconciliationView& reconciliation_;
    OrderServerOptions options_;
    Clock now_;
    std::unique_ptr<Impl> impl_;
    std::mutex sessions_mutex_;
    std::map<std::string, std::pair<std::string, double>> sessions_;  // token -> (user, expiry)
    int bound_port_ = 0;
    std::thread serve_thread_;
    std::atomic<bool> running_{false};
};

}  // namespace ng::order_ui
