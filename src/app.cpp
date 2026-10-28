#include "app.h"

#include <chrono>
#include <csignal>
#include <filesystem>
#include <thread>

#include "core/logging.h"
#include "core/time_util.h"
#include "kafka/kafka_client.h"

namespace ng {
namespace {

constexpr const char* kLog = "nextgen-gui";
std::atomic<bool> g_signal_stop{false};

void on_signal(int) { g_signal_stop.store(true); }

}  // namespace

Application::Application(AppOptions options) : options_(std::move(options)) {}

Application::~Application() {
    try {
        request_stop();
        if (server_) server_->stop();
        if (smart_consumer_) smart_consumer_->stop();
        if (market_consumer_) market_consumer_->stop();
        if (account_consumer_) account_consumer_->stop();
        if (control_) control_->shutdown();
    } catch (...) {
    }
}

void Application::initialise() {
    ConfigOverrides overrides;
    overrides.host = options_.host;
    overrides.port = options_.port;
    overrides.ssl = options_.ssl;
    overrides.ssl_certfile = options_.ssl_certfile;
    overrides.ssl_keyfile = options_.ssl_keyfile;
    config_ = load_config(options_.config_path, overrides);
    AccountDirectory accounts = load_account_metas(config_);
    std::vector<std::string> symbols = load_symbols(config_);
    NG_LOG_INFO(kLog, "configuration loaded path={} accounts={} symbols={} librdkafka={}", config_.config_path, accounts.size(),
                symbols.size(), kafka::library_version());
    NG_LOG_INFO(kLog, "market-data historical_prices_csv={}", config_.market_data.historical_prices_csv);

    control_ = std::make_unique<web::ControlCenter>(config_, std::move(accounts), std::move(symbols));
    sessions_ = std::make_unique<web::SessionStore>();

    double started_at = wall_now();
    control_->set_operations_log(std::make_unique<OperationsLog>(
        config_.operations_log_dir, started_at, [this] { return control_->served_snapshots(); }));
    NG_LOG_INFO(kLog, "operations log path={}", control_->operations_log()->path());

    if (config_.persistence.account_snapshots.enabled) {
        holdings_ = std::make_unique<ops::HoldingsRecorder>(config_.persistence.account_snapshots, control_->accounts(),
                                                            [this] { return control_->served_snapshots(); });
        NG_LOG_INFO(kLog, "account holdings snapshots every {}s in {}", config_.persistence.account_snapshots.interval_seconds,
                    config_.persistence.account_snapshots.dir);
    }
    if (config_.persistence.order_archive.enabled && config_.persistence.order_archive.daily_at && !options_.without_kafka) {
        archive_ = std::make_unique<ops::OrderArchiveScheduler>(config_, control_->accounts());
        NG_LOG_INFO(kLog, "daily order archive at {} {} into {}", *config_.persistence.order_archive.daily_at,
                    config_.persistence.order_archive.timezone, config_.persistence.order_archive.dir);
    }
    control_->set_persistence_status([this] {
        json out;
        out["order_archive"] = archive_ ? archive_->status() : json{{"enabled", false}};
        out["account_snapshots"] = holdings_ ? holdings_->status() : json{{"enabled", false}};
        return out;
    });

    if (options_.without_kafka) {
        NG_LOG_WARN(kLog, "running WITHOUT Kafka: commands are recorded in memory only and no feeds are consumed");
        control_->set_command_sink(std::make_unique<web::RecordingCommandSink>());
    } else {
        control_->set_command_sink(std::make_unique<web::KafkaCommandSink>(config_.kafka));
    }
    control_->create_fast_trading_manager();
    if (config_.smart_selling.enabled) {
        control_->create_smart_selling_runtime();
        NG_LOG_INFO(kLog, "smart selling journal={}", config_.smart_selling.state_db);
    } else {
        NG_LOG_INFO(kLog, "smart selling disabled by configuration");
    }

    if (!options_.without_kafka) {
        account_consumer_ = std::make_unique<kafka::AccountDetailsConsumer>(config_.kafka, control_->account_store(),
                                                                            control_->accounts());
        smart::SmartSellingRuntime* smart = control_->smart_runtime();
        market_consumer_ = std::make_unique<kafka::PriceBookConsumer>(
            config_.kafka, control_->market_store(),
            smart ? kafka::PriceBookConsumer::PayloadCallback(
                        [smart](const json& payload, const std::string& topic, int partition, long long offset) {
                            smart->on_price_book(payload, topic, partition, offset);
                        })
                  : nullptr,
            smart ? kafka::PriceBookConsumer::HealthCallback([smart](bool healthy) { smart->on_price_book_health(healthy); })
                  : nullptr);
        if (smart) smart_consumer_ = std::make_unique<kafka::SmartSellingConsumer>(config_, *smart);
    }
    web::FeedHealthProviders health;
    health.accounts = [this]() -> json { return account_consumer_ ? account_consumer_->health().to_json() : json(nullptr); };
    health.market = [this]() -> json { return market_consumer_ ? market_consumer_->health().to_json() : json(nullptr); };
    health.orders = [this]() -> json {
        if (!smart_consumer_) return json(nullptr);
        json out;
        out["state"] = smart_consumer_->orders().state();
        out["messages"] = smart_consumer_->orders().messages();
        out["rejected"] = smart_consumer_->orders().rejected_messages();
        out["topic"] = smart_consumer_->orders().topic();
        return out;
    };
    health.volume = [this]() -> json {
        if (!smart_consumer_) return json(nullptr);
        json out;
        out["state"] = smart_consumer_->volume().state();
        out["messages"] = smart_consumer_->volume().messages();
        out["rejected"] = smart_consumer_->volume().rejected_messages();
        out["topic"] = smart_consumer_->volume().topic();
        return out;
    };
    control_->set_feed_health_providers(std::move(health));

    web::ServerOptions server_options;
    server_options.host = options_.host.value_or(config_.server.host);
    server_options.port = options_.port.value_or(config_.server.port);
    server_options.ssl = options_.ssl.value_or(config_.server.ssl_enabled);
    server_options.ssl_certfile = options_.ssl_certfile.value_or(config_.server.ssl_certfile);
    server_options.ssl_keyfile = options_.ssl_keyfile.value_or(config_.server.ssl_keyfile);
    if (server_options.ssl) {
        if (server_options.ssl_certfile.empty() || server_options.ssl_keyfile.empty()) {
            throw ConfigError("SSL is enabled but ssl_certfile/ssl_keyfile are missing.");
        }
        if (!std::filesystem::is_regular_file(server_options.ssl_certfile)) {
            throw ConfigError("SSL cert file not found: " + server_options.ssl_certfile);
        }
        if (!std::filesystem::is_regular_file(server_options.ssl_keyfile)) {
            throw ConfigError("SSL key file not found: " + server_options.ssl_keyfile);
        }
    }
    server_options.threads = options_.threads;
    server_options.static_dir = options_.static_dir;
    server_options.redirect_port = options_.redirect_port;
    server_options.redirect_host = options_.redirect_host;
    server_options.redirect_target_host = options_.redirect_target_host.value_or(server_options.host == "0.0.0.0" ? "127.0.0.1" : server_options.host);
    server_options.redirect_target_port = server_options.port;
    server_options.secure_cookies = server_options.ssl;
    server_ = std::make_unique<web::Server>(*control_, *sessions_, server_options);
}

void Application::start() {
    if (started_) return;
    started_ = true;
    if (account_consumer_) account_consumer_->start();
    if (smart_consumer_) smart_consumer_->start();
    if (market_consumer_) market_consumer_->start();
    control_->start_background();
    if (holdings_) holdings_->start();
    if (archive_) archive_->start();
    server_->start();
    NG_LOG_INFO(kLog, "{} listening on {}://{}:{}", options_.ssl.value_or(config_.server.ssl_enabled) ? "https" : "http",
                server_->options().ssl ? "https" : "http", server_->options().host, server_->port());
}

void Application::request_stop() { stop_requested_.store(true); }

int Application::persist_orders_once(const AppOptions& options) {
    AppConfig config = load_config(options.config_path);
    AccountDirectory accounts = load_account_metas(config);
    try {
        json result = ops::persist_day_orders(config, accounts, options.persist_date);
        std::printf("%s\n", result.dump(2).c_str());
        return 0;
    } catch (const std::exception& e) {
        NG_LOG_ERROR(kLog, "persist-orders failed: {}", e.what());
        std::fprintf(stderr, "persist-orders failed: %s\n", e.what());
        return 1;
    }
}

int Application::run() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);
    start();
    while (!stop_requested_.load() && !g_signal_stop.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    NG_LOG_INFO(kLog, "shutting down");
    server_->stop();
    if (archive_) archive_->stop();
    if (holdings_) holdings_->stop();
    if (smart_consumer_) smart_consumer_->stop();
    if (market_consumer_) market_consumer_->stop();
    if (account_consumer_) account_consumer_->stop();
    control_->shutdown();
    if (auto* sink = dynamic_cast<web::KafkaCommandSink*>(control_->command_sink())) sink->producer().flush(2.0);
    NG_LOG_INFO(kLog, "stopped");
    return 0;
}

}  // namespace ng
