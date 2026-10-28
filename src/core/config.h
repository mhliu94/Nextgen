// Application configuration: the same config.json / trading_accounts.json files the
// Python trading UI used, with identical defaults, environment overrides and validation.
#pragma once

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "core/json.h"
#include "model/account.h"

namespace ng {

class ConfigError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct KafkaConfig {
    std::string bootstrap_servers;
    std::string market_data_bootstrap_servers;
    std::string account_details_topic;
    std::string trading_commands_topic;
    std::string market_data_topic;
    std::string stock_quotes_topic;
    std::string group_id;
    std::string market_data_group_id;
    std::string auto_offset_reset;
    std::string market_data_auto_offset_reset;
    double poll_timeout_sec = 1.0;
    std::string producer_acks = "all";
    bool producer_enable_idempotence = true;
    int producer_linger_ms = 5;
    // Optional librdkafka client settings (security.protocol, sasl.*, ssl.*) for the
    // control cluster and the market-data cluster.
    json client_config = json::object();
    json market_data_client_config = json::object();
};

struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 8000;
    bool ssl_enabled = false;
    std::string ssl_certfile;
    std::string ssl_keyfile;
};

struct MarketDataConfig {
    std::string historical_prices_csv;
    int market_insights_max_levels = 20;
};

struct FastTradingLevel {
    int book_levels = 0;
    double cycle_seconds = 0.0;
};

struct FastTradingConfig {
    std::map<int, FastTradingLevel> aggression_levels;
    std::map<std::string, double> minimum_cycle_seconds_by_medium;
};

struct SmartSellingConfig {
    std::string state_db = "./logs/smart_selling.sqlite3";
    bool enabled = true;
    std::string order_status_topic = "order-status";
    std::string trading_volume_topic = "trading-volume";
    double quote_max_age = 10;
    double trade_max_age = 15;
    double account_max_age = 60;
    // Percentage units as exact decimal strings: {"patient": "3", "normal": "6", "urgent": "10"}.
    std::map<std::string, std::string> participation_percentages;
    std::map<std::string, std::string> instrument_defaults;
    std::map<std::string, std::map<std::string, std::string>> instruments;
    std::optional<std::string> order_status_bootstrap_servers;
    std::optional<std::string> trading_volume_bootstrap_servers;
    json order_status_client_config = json::object();
    json trading_volume_client_config = json::object();
    // Effective instrument rules for a canonical symbol: defaults overlaid with per-symbol overrides.
    std::map<std::string, std::string> rules_for(const std::string& canonical_symbol) const;
};

// End-of-day order persistence (orders-<date>/orders-<date>.json + source-snapshots.json).
struct OrderArchiveConfig {
    bool enabled = true;
    std::string dir = "~/Reports";             // resolved: ~ expanded, relative to the config directory
    std::string timezone = "America/New_York"; // trading day boundary and localized timestamps
    std::optional<std::string> daily_at = "17:00";  // local time of the automatic daily run; null disables it
    std::string topic = "order-status";
    std::optional<std::string> bootstrap_servers;   // defaults to kafka.bootstrap_servers
    json client_config = json::object();
    int read_timeout_seconds = 180;
};

// Minute-by-minute account holdings snapshots (holdings-<date>.jsonl + holdings.sqlite3).
struct HoldingsRecorderConfig {
    bool enabled = true;
    std::string dir = "./snapshots";
    int interval_seconds = 60;
    bool write_jsonl = true;
    bool write_sqlite = true;
    std::string timezone = "America/New_York";
};

struct PersistenceConfig {
    OrderArchiveConfig order_archive;
    HoldingsRecorderConfig account_snapshots;
};

struct AuthUser {
    std::string password;
    std::string role;
};

struct AppConfig {
    std::string config_path;
    std::string config_dir;
    std::string accounts_file;      // resolved; empty when inline `accounts` are used
    json inline_accounts = json::array();
    std::vector<std::string> symbols;
    std::map<std::string, AuthUser> users;
    // Users allowed to use currency conversion and trading-status controls (default: lmh).
    ServerConfig server;
    KafkaConfig kafka;
    MarketDataConfig market_data;
    FastTradingConfig fast_trading;
    SmartSellingConfig smart_selling;
    json order_ui = json::object();
    PersistenceConfig persistence;
    std::string operations_log_dir = "./logs";
    json raw;  // original document for diagnostics

    // Currency conversion and trading-status controls are only available to the lmh account
    // (trading_ui/webserver.py::_has_lmh_feature_access); this is not configurable, as in the original.
    bool is_privileged(const std::string& user) const;
};

// Loads and validates the configuration (Python load_config semantics, including env overrides).
// Command-line overrides applied before the configuration is validated (so `--no-ssl` can
// start a server whose file enables TLS without the certificate files being present).
struct ConfigOverrides {
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<bool> ssl;
    std::optional<std::string> ssl_certfile;
    std::optional<std::string> ssl_keyfile;
};

AppConfig load_config(const std::string& config_path, const ConfigOverrides& overrides = {});
// Loads visible account metadata (hidden accounts skipped) from accounts_file or inline accounts.
AccountDirectory load_account_metas(const AppConfig& config);
// Normalised, de-duplicated configured symbols in file order.
std::vector<std::string> load_symbols(const AppConfig& config);
// Default config path resolution: ACCOUNT_DASHBOARD_CONFIG, else <dir>/config.local.json, else <dir>/config.json.
std::string default_config_path(const std::string& config_dir);

// Exposed for the configurator-compatible validation and tests.
FastTradingConfig normalize_fast_trading_config(const json* raw);
// `persistence` section; directories resolve against config_dir (and `~`), ORDER_ARCHIVE_DIR /
// ACCOUNT_SNAPSHOTS_DIR override the file. The order topic defaults to smart_selling.order_status_topic.
PersistenceConfig normalize_persistence_config(const json* raw, const std::string& config_dir, const SmartSellingConfig& smart);
SmartSellingConfig normalize_smart_selling_config(const json* raw);
std::string resolve_relative_path(const std::string& base_dir, const std::string& path);

}  // namespace ng
