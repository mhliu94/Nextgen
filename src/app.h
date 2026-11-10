// Process-level wiring: configuration, Kafka feeds, control center, HTTP server, signals.
#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "core/config.h"
#include "kafka/feeds.h"
#include "kafka/smart_selling_feeds.h"
#include "ops/holdings_recorder.h"
#include "ops/order_archive.h"
#include "order_ui/consumer.h"
#include "reporting/system_snapshots.h"
#include "web/control.h"
#include "web/server.h"
#include "web/sessions.h"

namespace ng {

struct AppOptions {
    std::string config_path;
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<bool> ssl;
    std::optional<std::string> ssl_certfile;
    std::optional<std::string> ssl_keyfile;
    int threads = 0;
    std::optional<std::string> static_dir;
    int redirect_port = 0;
    std::string redirect_host = "0.0.0.0";
    std::optional<std::string> redirect_target_host;
    bool without_kafka = false;  // run the UI with no brokers (development / demos)
    bool check_only = false;
    bool persist_orders = false;         // --persist-orders: archive one day's orders and exit
    std::optional<std::string> persist_date;  // --date YYYY-MM-DD (default: today in the archive zone)
};

class Application {
public:
    explicit Application(AppOptions options);
    ~Application();
    // Loads configuration and wires every component; throws on configuration errors.
    void initialise();
    // Starts feeds and the HTTP server; returns once start() completed.
    void start();
    // Blocks until stop() is requested (signal) and then tears everything down.
    int run();
    void request_stop();
    // `nextgen-gui --persist-orders`: loads the configuration, archives the day's orders, prints the
    // result as JSON and returns the process exit code.
    static int persist_orders_once(const AppOptions& options);
    web::ControlCenter& control() { return *control_; }
    web::Server& server() { return *server_; }
    const AppConfig& config() const { return config_; }

private:
    AppOptions options_;
    AppConfig config_;
    std::unique_ptr<web::ControlCenter> control_;
    std::unique_ptr<web::SessionStore> sessions_;
    std::unique_ptr<web::Server> server_;
    std::unique_ptr<kafka::AccountDetailsConsumer> account_consumer_;
    std::unique_ptr<kafka::PriceBookConsumer> market_consumer_;
    std::unique_ptr<kafka::SmartSellingConsumer> smart_consumer_;
    std::unique_ptr<kafka::SmartSellingConsumer> buying_consumer_;
    // Independent order-status consumer feeding the Market Data dashboard's own-volume figures.
    std::unique_ptr<order_ui::OrderStatusConsumer> market_order_consumer_;
    // Every configured trade account, including accounts hidden in the UI (system snapshots).
    AccountDirectory snapshot_accounts_;
    std::unique_ptr<reporting::SystemSnapshotRecorder> system_snapshots_;
    std::unique_ptr<ops::HoldingsRecorder> holdings_;
    std::unique_ptr<ops::OrderArchiveScheduler> archive_;
    std::atomic<bool> stop_requested_{false};
    bool started_ = false;
};

}  // namespace ng
