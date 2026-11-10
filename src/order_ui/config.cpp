#include "order_ui/config.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <set>

#include "core/strings.h"
#include "core/time_zone.h"

namespace ng::order_ui {
namespace {

json object_of(const json* value, const std::string& name) {
    if (!value) return json::object();
    if (!value->is_object()) throw ConfigError(name + " must be an object.");
    return *value;
}

std::string string_of(const json* value, const std::string& name) {
    if (!value || !value->is_string() || trim(value->get<std::string>()).empty()) {
        throw ConfigError(name + " must be a non-empty string.");
    }
    return trim(value->get<std::string>());
}

// Python prints 0.5 as "0.5" and 60 as "60".
std::string bound(double value) { return std::floor(value) == value ? std::to_string(static_cast<long long>(value)) : format_double(value); }

double number_of(const json* value, const std::string& name, double minimum, double maximum) {
    if (!value || value->is_boolean() || !value->is_number()) {
        throw ConfigError(name + " must be a number between " + bound(minimum) + " and " + bound(maximum) + ".");
    }
    double number = value->get<double>();
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        throw ConfigError(name + " must be between " + bound(minimum) + " and " + bound(maximum) + ".");
    }
    return number;
}

std::string env(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string absolute(const std::string& path) { return std::filesystem::absolute(path).lexically_normal().string(); }

}  // namespace

std::string default_config_path(const std::string& config_dir) {
    for (const char* name : {"ORDER_UI_CONFIG", "ACCOUNT_DASHBOARD_CONFIG"}) {
        if (std::string configured = env(name); !configured.empty()) return configured;
    }
    std::filesystem::path local = std::filesystem::path(config_dir) / "config.local.json";
    if (std::filesystem::exists(local)) return local.string();
    return (std::filesystem::path(config_dir) / "config.json").string();
}

OrderKafkaConfig load_kafka_config(const json& shared, const std::string& config_path) {
    std::string config_dir = std::filesystem::path(absolute(config_path)).parent_path().string();
    json settings = object_of(json_find(shared, "order_ui"), "order_ui");
    json shared_kafka = object_of(json_find(shared, "kafka"), "kafka");
    json kafka = object_of(json_find(settings, "kafka"), "order_ui.kafka");
    static const std::set<std::string> known = {"bootstrap_servers", "topic", "group_id", "poll_timeout_sec", "client_config"};
    for (auto it = kafka.begin(); it != kafka.end(); ++it) {
        if (!known.count(it.key())) throw ConfigError("Unknown order_ui.kafka setting(s).");
    }
    OrderKafkaConfig out;
    json default_bootstrap = shared_kafka.contains("bootstrap_servers")
                                 ? shared_kafka.at("bootstrap_servers")
                                 : json(env("KAFKA_BOOTSTRAP_SERVERS").empty() ? std::string("localhost:9092") : env("KAFKA_BOOTSTRAP_SERVERS"));
    json defaults = {{"bootstrap_servers", default_bootstrap}, {"topic", "order-status"}, {"group_id", "order-dashboard"},
                     {"poll_timeout_sec", 1.0}, {"client_config", json::object()}};
    defaults.update(kafka);
    out.bootstrap_servers = string_of(json_find(defaults, "bootstrap_servers"), "order_ui.kafka.bootstrap_servers");
    out.topic = string_of(json_find(defaults, "topic"), "order_ui.kafka.topic");
    out.group_id = string_of(json_find(defaults, "group_id"), "order_ui.kafka.group_id");
    out.poll_timeout_sec = number_of(json_find(defaults, "poll_timeout_sec"), "order_ui.kafka.poll_timeout_sec", 0.1, 5);
    json client_config = object_of(json_find(defaults, "client_config"), "order_ui.kafka.client_config");
    // Only connection/security settings can be overridden; replay must remain deterministic.
    static const std::set<std::string> allowed = {"security.protocol",        "sasl.mechanism",   "sasl.mechanisms", "sasl.username",
                                                  "sasl.password",            "ssl.ca.location",  "ssl.certificate.location",
                                                  "ssl.key.location",         "ssl.key.password", "ssl.endpoint.identification.algorithm"};
    std::vector<std::string> unsupported;
    for (auto it = client_config.begin(); it != client_config.end(); ++it) {
        if (!allowed.count(it.key())) unsupported.push_back(it.key());
    }
    if (!unsupported.empty()) throw ConfigError("Unsupported order_ui.kafka.client_config key(s): " + join(unsupported, ", "));
    for (auto it = client_config.begin(); it != client_config.end(); ++it) {
        if (!it.value().is_string()) throw ConfigError("order_ui.kafka.client_config." + it.key() + " must be a string.");
        if (ends_with(it.key(), ".location")) it.value() = resolve_relative_path(config_dir, it.value().get<std::string>());
    }
    out.client_config = client_config;
    return out;
}

OrderUiConfig load_config(const std::string& path_in) {
    OrderUiConfig out;
    out.config_path = absolute(path_in);
    std::string config_dir = std::filesystem::path(out.config_path).parent_path().string();
    json shared = read_json_file(out.config_path);
    if (!shared.is_object()) throw ConfigError("Config must be an object.");

    // Shared account parser: string_id, numeric_id, broker and trading_medium keep the trading UI's
    // validation and aliases; hidden accounts are excluded.
    AppConfig account_source;
    std::string accounts_file;
    for (const char* key : {"accounts_file", "trading_accounts_file"}) {
        const json* value = json_find(shared, key);
        if (accounts_file.empty() && json_python_truthy(value)) accounts_file = string_of(value, "accounts_file");
    }
    if (accounts_file.empty() && !env("TRADING_ACCOUNTS_CONFIG").empty()) accounts_file = trim(env("TRADING_ACCOUNTS_CONFIG"));
    if (!accounts_file.empty()) {
        account_source.accounts_file = resolve_relative_path(config_dir, accounts_file);
    } else {
        const json* inline_accounts = json_find(shared, "accounts");
        if (!inline_accounts || !inline_accounts->is_array() || inline_accounts->empty()) {
            throw ConfigError("Provide accounts_file or a non-empty accounts list, as in the trading UI.");
        }
        account_source.inline_accounts = *inline_accounts;
    }
    out.accounts = load_account_metas(account_source);

    json auth = object_of(json_find(shared, "auth"), "auth");
    json users = object_of(json_find(auth, "users"), "auth.users");
    for (const char* name : {"admin", "trader"}) {
        if (!users.contains(name)) throw ConfigError(std::string("auth.users.") + name + ".password is required, as in the trading UI.");
    }
    for (auto it = users.begin(); it != users.end(); ++it) {
        json record = object_of(&it.value(), "auth.users." + it.key());
        std::string password = string_of(json_find(record, "password"), "auth.users." + it.key() + ".password");
        // Match the trading UI's normalization for its built-in users.
        bool builtin = it.key() == "admin" || it.key() == "trader";
        out.users[it.key()] = builtin ? password : record.at("password").get<std::string>();
    }

    json settings = object_of(json_find(shared, "order_ui"), "order_ui");
    static const std::set<std::string> allowed = {"timezone", "refresh_interval_seconds", "server", "kafka"};
    std::vector<std::string> unknown;
    for (auto it = settings.begin(); it != settings.end(); ++it) {
        if (!allowed.count(it.key())) unknown.push_back(it.key());
    }
    if (!unknown.empty()) throw ConfigError("Unknown order_ui setting(s): " + join(unknown, ", "));
    out.timezone = settings.contains("timezone") ? string_of(&settings.at("timezone"), "order_ui.timezone") : std::string("America/New_York");
    if (!TimeZone::find(out.timezone)) throw ConfigError("Unknown order_ui.timezone: " + out.timezone);
    json default_refresh = 2;
    out.refresh_interval_seconds =
        number_of(settings.contains("refresh_interval_seconds") ? &settings.at("refresh_interval_seconds") : &default_refresh,
                  "refresh_interval_seconds", 0.5, 60);

    json server = object_of(json_find(settings, "server"), "order_ui.server");
    static const std::set<std::string> server_keys = {"host", "port", "ssl_certfile", "ssl_keyfile"};
    for (auto it = server.begin(); it != server.end(); ++it) {
        if (!server_keys.count(it.key())) throw ConfigError("Unknown order_ui.server setting(s).");
    }
    json server_defaults = {{"host", "127.0.0.1"}, {"port", 8030}, {"ssl_certfile", ""}, {"ssl_keyfile", ""}};
    server_defaults.update(server);
    out.server.host = string_of(json_find(server_defaults, "host"), "order_ui.server.host");
    number_of(json_find(server_defaults, "port"), "order_ui.server.port", 1, 65535);
    const json& port = server_defaults.at("port");
    if (!(port.is_number_integer() || port.is_number_unsigned())) throw ConfigError("order_ui.server.port must be an integer.");
    out.server.port = static_cast<int>(port.get<long long>());
    for (auto [field, target] : {std::pair{"ssl_certfile", &out.server.ssl_certfile}, std::pair{"ssl_keyfile", &out.server.ssl_keyfile}}) {
        const json& value = server_defaults.at(field);
        if (!value.is_string()) throw ConfigError(std::string("order_ui.server.") + field + " must be a string.");
        if (!value.get<std::string>().empty()) {
            *target = resolve_relative_path(config_dir, value.get<std::string>());
            if (!std::filesystem::is_regular_file(*target)) {
                throw ConfigError(std::string("order_ui.server.") + field + " does not point to a file.");
            }
        }
    }
    if (out.server.ssl_certfile.empty() != out.server.ssl_keyfile.empty()) {
        throw ConfigError("Provide both order_ui.server.ssl_certfile and ssl_keyfile for HTTPS.");
    }

    out.kafka = load_kafka_config(shared, out.config_path);
    for (auto [mode, name] : {std::pair{"G", "smart_selling"}, std::pair{"H", "smart_buying"}}) {
        json raw = object_of(json_find(shared, name), name);
        std::string state_db;
        if (raw.contains("state_db")) {
            state_db = resolve_relative_path(config_dir, string_of(&raw.at("state_db"), std::string(name) + ".state_db"));
        } else {
            // The control server's default journal location, relative to the working directory.
            state_db = std::string("./logs/") + name + ".sqlite3";
        }
        out.reconciliation_sources.push_back({mode, absolute(state_db)});
    }
    out.reporting_db_path = normalize_reporting_config(json_find(shared, "reporting"), config_dir).db_path;
    return out;
}

}  // namespace ng::order_ui
