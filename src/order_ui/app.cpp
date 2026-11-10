#include "order_ui/app.h"

#include <chrono>
#include <csignal>
#include <thread>

#include "core/logging.h"

namespace ng::order_ui {
namespace {

constexpr const char* kLog = "order-ui";
std::atomic<bool> g_signal_stop{false};
void on_signal(int) { g_signal_stop.store(true); }

}  // namespace

OrderApplication::OrderApplication(OrderAppOptions options) : options_(std::move(options)) {}

OrderApplication::~OrderApplication() { shutdown(); }

void OrderApplication::initialise() {
    config_ = load_config(options_.config_path);
    if (options_.host) config_.server.host = *options_.host;
    if (options_.port) config_.server.port = *options_.port;
    if (options_.no_ssl) {
        config_.server.ssl_certfile.clear();
        config_.server.ssl_keyfile.clear();
    } else if (options_.ssl_certfile && options_.ssl_keyfile) {
        config_.server.ssl_certfile = *options_.ssl_certfile;
        config_.server.ssl_keyfile = *options_.ssl_keyfile;
    }
    reporting_ = std::make_unique<reporting::ActivityReporting>(config_.reporting_db_path);
    reconciliation_ = std::make_unique<ReconciliationView>(config_.reconciliation_sources, config_.reporting_db_path);
    // Every accepted snapshot created today or earlier also feeds the session and tag reports.
    store_ = std::make_unique<OrderStore>(config_.accounts, config_.timezone, nullptr,
                                          [this](const orders::OrderEvent& event) { reporting_->record_order(event); });
    if (!options_.without_kafka) consumer_ = std::make_unique<OrderStatusConsumer>(config_.kafka, *store_);
    OrderServerOptions server_options;
    server_options.host = config_.server.host;
    server_options.port = config_.server.port;
    server_options.ssl_certfile = config_.server.ssl_certfile;
    server_options.ssl_keyfile = config_.server.ssl_keyfile;
    server_options.static_dir = options_.static_dir;
    server_ = std::make_unique<OrderServer>(config_, *store_, *reporting_, *reconciliation_, server_options);
}

void OrderApplication::start() {
    if (started_) return;
    if (consumer_) {
        consumer_->start();
        NG_LOG_INFO(kLog, "order-status consumer started topic={} bootstrap={} group={}", config_.kafka.topic,
                    config_.kafka.bootstrap_servers, consumer_->group_id());
    } else {
        store_->update_feed(std::string("stopped"), std::string("Order consumer stopped"));
        NG_LOG_WARN(kLog, "running without Kafka: no orders will be received");
    }
    server_->start();
    started_ = true;
    NG_LOG_INFO(kLog, "order UI listening on {}://{}:{} (accounts={}, timezone={}, reports={})",
                config_.server.ssl_certfile.empty() ? "http" : "https", config_.server.host, server_->port(),
                config_.accounts.size(), config_.timezone, config_.reporting_db_path);
}

int OrderApplication::run() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    start();
    while (!stop_requested_.load() && !g_signal_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));
    NG_LOG_INFO(kLog, "shutting down");
    shutdown();
    NG_LOG_INFO(kLog, "stopped");
    return 0;
}

void OrderApplication::shutdown() {
    if (server_) server_->stop();
    if (consumer_) consumer_->stop();
    if (reporting_) reporting_->close();
    started_ = false;
}

}  // namespace ng::order_ui
