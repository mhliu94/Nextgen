// Process-level wiring of the order UI: configuration, the shared activity reports, the order-status
// consumer and the HTTP server (order_ui/webserver.py: create_app + run).
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "order_ui/config.h"
#include "order_ui/consumer.h"
#include "order_ui/reconciliation.h"
#include "order_ui/server.h"
#include "order_ui/store.h"
#include "reporting/activity_reporting.h"

namespace ng::order_ui {

struct OrderAppOptions {
    std::string config_path;
    std::optional<std::string> host;  // overrides order_ui.server.host
    std::optional<int> port;          // overrides order_ui.server.port
    std::optional<std::string> static_dir;
    std::optional<std::string> ssl_certfile;  // with ssl_keyfile: overrides order_ui.server TLS files
    std::optional<std::string> ssl_keyfile;
    bool no_ssl = false;         // plain HTTP even when the configuration names certificate files
    bool without_kafka = false;  // serve the UI without an order-status consumer (development / demos)
};

class OrderApplication {
public:
    explicit OrderApplication(OrderAppOptions options);
    ~OrderApplication();
    // Loads the configuration and opens the reports; throws ConfigError on invalid settings.
    void initialise();
    // Starts the consumer and the HTTP server.
    void start();
    // Blocks until SIGINT/SIGTERM (or request_stop) and shuts everything down.
    int run();
    void request_stop() { stop_requested_.store(true); }
    void shutdown();

    const OrderUiConfig& config() const { return config_; }
    OrderStore& store() { return *store_; }
    reporting::ActivityReporting& reporting() { return *reporting_; }
    OrderServer& server() { return *server_; }

private:
    OrderAppOptions options_;
    OrderUiConfig config_;
    std::unique_ptr<reporting::ActivityReporting> reporting_;
    std::unique_ptr<ReconciliationView> reconciliation_;
    std::unique_ptr<OrderStore> store_;
    std::unique_ptr<OrderStatusConsumer> consumer_;
    std::unique_ptr<OrderServer> server_;
    std::atomic<bool> stop_requested_{false};
    bool started_ = false;
};

}  // namespace ng::order_ui
