// Order UI configuration (order_ui/config.py): the shared accounts and authentication of the
// trading configuration plus the independent `order_ui` settings.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "model/account.h"

namespace ng::order_ui {

// Connection settings of the retained order-status feed, shared by every reporting consumer.
struct OrderKafkaConfig {
    std::string bootstrap_servers;
    std::string topic = "order-status";
    std::string group_id = "order-dashboard";  // prefix; every process appends a unique suffix
    double poll_timeout_sec = 1.0;
    json client_config = json::object();  // connection/security librdkafka properties only
};

struct ServerSettings {
    std::string host = "127.0.0.1";
    int port = 8030;
    std::string ssl_certfile;
    std::string ssl_keyfile;
};

// One Smart controller whose unresolved records the Unreconciled orders tab shows.
struct ReconciliationSource {
    std::string mode;      // "G" (Smart selling) or "H" (Smart buying)
    std::string state_db;  // absolute journal path; the feedback store is "<state_db>.feedback.sqlite3"
};

struct OrderUiConfig {
    std::string config_path;
    AccountDirectory accounts;                 // visible accounts only
    std::map<std::string, std::string> users;  // user name -> password
    std::string timezone = "America/New_York";
    double refresh_interval_seconds = 2;
    ServerSettings server;
    OrderKafkaConfig kafka;
    std::vector<ReconciliationSource> reconciliation_sources;
    std::string reporting_db_path;
};

// --config, then ORDER_UI_CONFIG, then ACCOUNT_DASHBOARD_CONFIG, then <dir>/config.local.json when
// present, otherwise <dir>/config.json.
std::string default_config_path(const std::string& config_dir);
// Throws ConfigError with the Python service's messages.
OrderUiConfig load_config(const std::string& path);
// `order_ui.kafka` over the shared `kafka.bootstrap_servers`; `shared` is the raw configuration document.
OrderKafkaConfig load_kafka_config(const json& shared, const std::string& config_path);

}  // namespace ng::order_ui
