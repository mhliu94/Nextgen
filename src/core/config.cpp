#include "core/config.h"

#include "core/time_zone.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "core/decimal.h"
#include "core/strings.h"

namespace ng {
namespace {

std::string env_or(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : fallback;
}

std::optional<std::string> env_opt(const char* name) {
    const char* v = std::getenv(name);
    if (!v) return std::nullopt;
    return std::string(v);
}

json read_json_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError("Unable to read file: " + path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    try {
        return json::parse(buffer.str());
    } catch (const json::exception& e) {
        throw ConfigError("Invalid JSON in " + path + ": " + e.what());
    }
}

json& ensure_object(json& parent, const char* key) {
    auto it = parent.find(key);
    if (it == parent.end() || it->is_null()) parent[key] = json::object();
    if (!parent[key].is_object()) throw ConfigError(std::string("Config ") + key + " must be an object.");
    return parent[key];
}

void set_default(json& object, const char* key, const json& value) {
    if (object.find(key) == object.end()) object[key] = value;
}

std::string json_string(const json& v) {
    auto s = json_to_string(&v);
    return s.value_or("");
}

double positive_finite_number(const json* value, const std::string& field) {
    if (!value || value->is_boolean() || !value->is_number()) {
        throw ConfigError("Config " + field + " must be a positive finite number.");
    }
    double d = value->get<double>();
    if (!std::isfinite(d) || d <= 0) throw ConfigError("Config " + field + " must be a positive finite number.");
    return d;
}

int positive_integer(const json* value, const std::string& field) {
    if (!value || value->is_boolean() || !value->is_number_integer()) {
        throw ConfigError("Config " + field + " must be a positive integer.");
    }
    long long v = value->get<long long>();
    if (v <= 0) throw ConfigError("Config " + field + " must be a positive integer.");
    return static_cast<int>(v);
}

int normalize_level_key(const std::string& key) {
    if (key == "1" || key == "2" || key == "3") return key[0] - '0';
    throw ConfigError("Config fast_trading.aggression_levels must define exactly levels 1, 2, and 3.");
}

const std::set<std::string>& trading_media() {
    static const std::set<std::string> media = {"EMULATOR", "WINDOWS", "WEB", "API"};
    return media;
}

}  // namespace

std::string resolve_relative_path(const std::string& base_dir, const std::string& path) {
    if (path.empty()) return path;
    std::filesystem::path p(path);
    if (p.is_absolute()) return p.lexically_normal().string();
    return (std::filesystem::path(base_dir) / p).lexically_normal().string();
}

FastTradingConfig normalize_fast_trading_config(const json* raw) {
    json empty = json::object();
    const json& source = (raw && !raw->is_null()) ? *raw : empty;
    if (!source.is_object()) throw ConfigError("Config fast_trading must be an object.");
    static const std::set<std::string> allowed = {"aggression_levels", "minimum_cycle_seconds_by_medium"};
    std::vector<std::string> unknown;
    for (auto it = source.begin(); it != source.end(); ++it) {
        if (!allowed.count(it.key())) unknown.push_back(it.key());
    }
    if (!unknown.empty()) {
        std::sort(unknown.begin(), unknown.end());
        throw ConfigError("Config fast_trading contains unknown field(s): " + join(unknown, ", "));
    }
    FastTradingConfig cfg;
    const json* raw_levels = json_find(source, "aggression_levels");
    if (!raw_levels || raw_levels->is_null()) {
        cfg.aggression_levels = {{1, {2, 30.0}}, {2, {3, 20.0}}, {3, {5, 10.0}}};
    } else {
        if (!raw_levels->is_object()) throw ConfigError("Config fast_trading.aggression_levels must be an object.");
        for (auto it = raw_levels->begin(); it != raw_levels->end(); ++it) {
            int level = normalize_level_key(it.key());
            if (cfg.aggression_levels.count(level)) {
                throw ConfigError("Config fast_trading.aggression_levels defines level " + std::to_string(level) +
                                  " more than once.");
            }
            const json& settings = it.value();
            if (!settings.is_object()) {
                throw ConfigError("Config fast_trading.aggression_levels." + std::to_string(level) + " must be an object.");
            }
            std::set<std::string> keys;
            for (auto s = settings.begin(); s != settings.end(); ++s) keys.insert(s.key());
            if (keys != std::set<std::string>{"book_levels", "cycle_seconds"}) {
                throw ConfigError("Config fast_trading.aggression_levels." + std::to_string(level) +
                                  " must contain exactly book_levels and cycle_seconds.");
            }
            std::string prefix = "fast_trading.aggression_levels." + std::to_string(level) + ".";
            int book_levels = positive_integer(json_find(settings, "book_levels"), prefix + "book_levels");
            if (book_levels > 20) throw ConfigError("Config " + prefix + "book_levels must be at most 20.");
            cfg.aggression_levels[level] = {book_levels,
                                            positive_finite_number(json_find(settings, "cycle_seconds"), prefix + "cycle_seconds")};
        }
        if (cfg.aggression_levels.size() != 3) {
            throw ConfigError("Config fast_trading.aggression_levels must define exactly levels 1, 2, and 3.");
        }
    }
    cfg.minimum_cycle_seconds_by_medium = {{"API", 3.0}, {"WEB", 30.0}, {"WINDOWS", 40.0}, {"EMULATOR", 40.0}};
    const json* raw_media = json_find(source, "minimum_cycle_seconds_by_medium");
    if (raw_media && !raw_media->is_null()) {
        if (!raw_media->is_object()) {
            throw ConfigError("Config fast_trading.minimum_cycle_seconds_by_medium must be an object.");
        }
        std::set<std::string> seen;
        for (auto it = raw_media->begin(); it != raw_media->end(); ++it) {
            std::string medium = to_upper(trim(it.key()));
            if (!trading_media().count(medium)) {
                throw ConfigError("Invalid medium in fast_trading.minimum_cycle_seconds_by_medium: '" + it.key() +
                                  "'. Allowed: API, EMULATOR, WEB, WINDOWS");
            }
            if (seen.count(medium)) {
                throw ConfigError("Config fast_trading.minimum_cycle_seconds_by_medium defines " + medium +
                                  " more than once.");
            }
            seen.insert(medium);
            cfg.minimum_cycle_seconds_by_medium[medium] =
                positive_finite_number(&it.value(), "fast_trading.minimum_cycle_seconds_by_medium." + medium);
        }
    }
    return cfg;
}

namespace {

std::map<std::string, std::string> validate_instrument_rules(const json& rules, const char* context) {
    if (!rules.is_object()) throw ConfigError("Smart selling instrument rules must be objects");
    if (rules.contains("tick_ladder")) throw ConfigError("Smart selling currently requires a constant tick_size");
    static const std::set<std::string> known = {"tick_size", "quantity_step", "min_quantity", "min_notional"};
    std::vector<std::string> unknown;
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        if (!known.count(it.key())) unknown.push_back(it.key());
    }
    if (!unknown.empty()) {
        std::sort(unknown.begin(), unknown.end());
        throw ConfigError("Unknown Smart selling instrument rules: " + join(unknown, ", "));
    }
    std::map<std::string, std::string> out;
    for (auto it = rules.begin(); it != rules.end(); ++it) {
        std::string text = json_string(it.value());
        auto value = Decimal::try_parse(text);
        if (!value || it.value().is_boolean()) throw ConfigError("invalid Smart selling instrument " + it.key());
        if (value->is_negative() || (it.key() != "min_notional" && value->is_zero())) {
            throw ConfigError("invalid Smart selling instrument " + it.key());
        }
        out[it.key()] = text;
    }
    (void)context;
    return out;
}

}  // namespace

std::map<std::string, std::string> SmartSellingConfig::rules_for(const std::string& canonical) const {
    std::map<std::string, std::string> rules = instrument_defaults;
    auto it = instruments.find(canonical);
    if (it != instruments.end()) {
        for (auto& [k, v] : it->second) rules[k] = v;
    }
    return rules;
}

SmartSellingConfig normalize_smart_selling_config(const json* raw_in) {
    json empty = json::object();
    const json& raw = (raw_in && !raw_in->is_null()) ? *raw_in : empty;
    if (!raw.is_object()) throw ConfigError("smart_selling must be an object");
    SmartSellingConfig cfg;
    static const std::map<std::string, std::string> default_percentages = {
        {"patient", "3"}, {"normal", "6"}, {"urgent", "10"}};
    static const std::map<std::string, std::string> instrument_defaults = {
        {"tick_size", "0.01"}, {"quantity_step", "1"}, {"min_quantity", "1"}, {"min_notional", "0"}};

    if (const json* v = json_find(raw, "state_db")) {
        if (!v->is_string() || trim(v->get<std::string>()).empty()) throw ConfigError("smart_selling.state_db must be nonempty");
        cfg.state_db = v->get<std::string>();
    }
    if (const json* v = json_find(raw, "enabled")) {
        if (!v->is_boolean()) throw ConfigError("smart_selling.enabled must be boolean");
        cfg.enabled = v->get<bool>();
    }
    for (auto [key, target] : {std::pair{"order_status_topic", &cfg.order_status_topic},
                               std::pair{"trading_volume_topic", &cfg.trading_volume_topic}}) {
        if (const json* v = json_find(raw, key)) {
            if (!v->is_string() || trim(v->get<std::string>()).empty()) {
                throw ConfigError(std::string("smart_selling.") + key + " must be nonempty");
            }
            *target = v->get<std::string>();
        }
    }
    for (auto [key, target] : {std::pair{"quote_max_age", &cfg.quote_max_age}, std::pair{"trade_max_age", &cfg.trade_max_age},
                               std::pair{"account_max_age", &cfg.account_max_age}}) {
        if (const json* v = json_find(raw, key)) {
            auto parsed = json_to_double(v);
            if (v->is_boolean() || !parsed || !std::isfinite(*parsed) || *parsed <= 0) {
                throw ConfigError(std::string("smart_selling.") + key + " must be positive and finite");
            }
            *target = *parsed;
        }
    }
    cfg.participation_percentages = default_percentages;
    if (const json* p = json_find(raw, "participation_percentages")) {
        if (!p->is_object()) throw ConfigError("smart_selling.participation_percentages must be an object");
        std::vector<std::string> unknown;
        for (auto it = p->begin(); it != p->end(); ++it) {
            if (!default_percentages.count(it.key())) unknown.push_back(it.key());
        }
        if (!unknown.empty()) {
            std::sort(unknown.begin(), unknown.end());
            throw ConfigError("Unknown smart_selling.participation_percentages modes: " + join(unknown, ", "));
        }
        for (auto it = p->begin(); it != p->end(); ++it) {
            std::string label = "smart_selling.participation_percentages." + it.key();
            if (it.value().is_boolean() || it.value().is_null()) throw ConfigError(label + " must be a finite number");
            auto value = Decimal::try_parse(json_string(it.value()));
            if (!value) throw ConfigError(label + " must be a finite number");
            if (!value->is_positive()) throw ConfigError("invalid " + label);
            if (value->digit_count() > 30 || value->adjusted() > 24 || value->exponent() < -24) {
                throw ConfigError(label + " exceeds supported decimal precision");
            }
            if (*value > Decimal(100)) throw ConfigError(label + " must be greater than 0 and at most 100");
            cfg.participation_percentages[it.key()] = value->str();
        }
    }
    cfg.instrument_defaults = instrument_defaults;
    if (const json* d = json_find(raw, "instrument_defaults")) {
        for (auto& [k, v] : validate_instrument_rules(*d, "instrument_defaults")) cfg.instrument_defaults[k] = v;
    }
    if (const json* instruments = json_find(raw, "instruments")) {
        if (!instruments->is_object()) throw ConfigError("Smart selling instrument settings must be objects");
        for (auto it = instruments->begin(); it != instruments->end(); ++it) {
            cfg.instruments[canonical_symbol(it.key())] = validate_instrument_rules(it.value(), "instruments");
        }
    }
    if (const json* v = json_find(raw, "order_status_bootstrap_servers"); v && v->is_string()) {
        cfg.order_status_bootstrap_servers = v->get<std::string>();
    }
    if (const json* v = json_find(raw, "trading_volume_bootstrap_servers"); v && v->is_string()) {
        cfg.trading_volume_bootstrap_servers = v->get<std::string>();
    }
    if (const json* v = json_find(raw, "order_status_client_config"); v && v->is_object()) cfg.order_status_client_config = *v;
    if (const json* v = json_find(raw, "trading_volume_client_config"); v && v->is_object()) {
        cfg.trading_volume_client_config = *v;
    }
    return cfg;
}

bool AppConfig::is_privileged(const std::string& user) const { return trim(user) == "lmh"; }

std::string default_config_path(const std::string& config_dir) {
    if (auto env = env_opt("ACCOUNT_DASHBOARD_CONFIG"); env && !trim(*env).empty()) return trim(*env);
    std::filesystem::path local = std::filesystem::path(config_dir) / "config.local.json";
    if (std::filesystem::exists(local)) return local.string();
    return (std::filesystem::path(config_dir) / "config.json").string();
}

std::string expand_user_path(const std::string& path) {
    if (path == "~" || path.rfind("~/", 0) == 0) {
        const char* home = std::getenv("HOME");
        if (home && *home) return std::string(home) + path.substr(1);
    }
    return path;
}

PersistenceConfig normalize_persistence_config(const json* raw_in, const std::string& config_dir, const SmartSellingConfig& smart) {
    json empty = json::object();
    const json& raw = (raw_in && !raw_in->is_null()) ? *raw_in : empty;
    if (!raw.is_object()) throw ConfigError("persistence must be an object");
    PersistenceConfig cfg;
    cfg.order_archive.topic = smart.order_status_topic.empty() ? "order-status" : smart.order_status_topic;

    auto read_dir = [&](const json& section, const char* key, const char* env, std::string current) {
        if (const json* v = json_find(section, key)) {
            if (!v->is_string() || trim(v->get<std::string>()).empty()) throw ConfigError(std::string("persistence.") + key + " must be a nonempty string");
            current = trim(v->get<std::string>());
        }
        if (auto override = env_opt(env); override && !trim(*override).empty()) current = trim(*override);
        return resolve_relative_path(config_dir, expand_user_path(current));
    };
    auto read_zone = [&](const json& section, const char* label, std::string current) {
        if (const json* v = json_find(section, "timezone")) {
            if (!v->is_string()) throw ConfigError(std::string(label) + ".timezone must be a string");
            current = trim(v->get<std::string>());
        }
        if (!TimeZone::find(current)) throw ConfigError(std::string(label) + ".timezone " + current + " is not supported (use America/New_York, UTC, Asia/Shanghai, Asia/Hong_Kong, Asia/Singapore or Asia/Tokyo)");
        return current;
    };

    if (const json* section = json_find(raw, "order_archive"); section && section->is_object()) {
        OrderArchiveConfig& oa = cfg.order_archive;
        if (const json* v = json_find(*section, "enabled")) oa.enabled = json_truthy(v);
        oa.dir = read_dir(*section, "dir", "ORDER_ARCHIVE_DIR", oa.dir);
        oa.timezone = read_zone(*section, "persistence.order_archive", oa.timezone);
        if (const json* v = json_find(*section, "daily_at")) {
            if (v->is_null()) oa.daily_at.reset();
            else {
                if (!v->is_string()) throw ConfigError("persistence.order_archive.daily_at must be \"HH:MM\" or null");
                std::string text = trim(v->get<std::string>());
                int hour = -1, minute = -1;
                char extra = 0;
                if (std::sscanf(text.c_str(), "%d:%d%c", &hour, &minute, &extra) != 2 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
                    throw ConfigError("persistence.order_archive.daily_at must be \"HH:MM\" (24-hour local time), got " + text);
                }
                char normalized[8];
                std::snprintf(normalized, sizeof(normalized), "%02d:%02d", hour, minute);
                oa.daily_at = normalized;
            }
        }
        if (const json* v = json_find(*section, "topic")) {
            if (!v->is_string() || trim(v->get<std::string>()).empty()) throw ConfigError("persistence.order_archive.topic must be a nonempty string");
            oa.topic = trim(v->get<std::string>());
        }
        if (const json* v = json_find(*section, "bootstrap_servers"); v && v->is_string() && !trim(v->get<std::string>()).empty()) {
            oa.bootstrap_servers = trim(v->get<std::string>());
        }
        if (const json* v = json_find(*section, "client_config"); v && v->is_object()) oa.client_config = *v;
        if (const json* v = json_find(*section, "read_timeout_seconds")) {
            auto n = json_to_int(v);
            if (!n || *n < 5) throw ConfigError("persistence.order_archive.read_timeout_seconds must be an integer >= 5");
            oa.read_timeout_seconds = static_cast<int>(*n);
        }
    } else {
        cfg.order_archive.dir = read_dir(empty, "dir", "ORDER_ARCHIVE_DIR", cfg.order_archive.dir);
    }

    if (const json* section = json_find(raw, "account_snapshots"); section && section->is_object()) {
        HoldingsRecorderConfig& hs = cfg.account_snapshots;
        if (const json* v = json_find(*section, "enabled")) hs.enabled = json_truthy(v);
        hs.dir = read_dir(*section, "dir", "ACCOUNT_SNAPSHOTS_DIR", hs.dir);
        hs.timezone = read_zone(*section, "persistence.account_snapshots", hs.timezone);
        if (const json* v = json_find(*section, "interval_seconds")) {
            auto n = json_to_int(v);
            if (!n || *n < 1 || *n > 86400) throw ConfigError("persistence.account_snapshots.interval_seconds must be an integer between 1 and 86400");
            hs.interval_seconds = static_cast<int>(*n);
        }
        if (const json* v = json_find(*section, "formats")) {
            if (!v->is_array()) throw ConfigError("persistence.account_snapshots.formats must be a list of \"jsonl\" / \"sqlite\"");
            hs.write_jsonl = false;
            hs.write_sqlite = false;
            for (const auto& item : *v) {
                std::string f = to_lower(trim(json_string(item)));
                if (f == "jsonl") hs.write_jsonl = true;
                else if (f == "sqlite") hs.write_sqlite = true;
                else throw ConfigError("persistence.account_snapshots.formats: unknown format " + f);
            }
            if (!hs.write_jsonl && !hs.write_sqlite) throw ConfigError("persistence.account_snapshots.formats must include jsonl or sqlite");
        }
    } else {
        cfg.account_snapshots.dir = read_dir(empty, "dir", "ACCOUNT_SNAPSHOTS_DIR", cfg.account_snapshots.dir);
    }
    return cfg;
}

AppConfig load_config(const std::string& config_path, const ConfigOverrides& overrides) {
    json cfg = read_json_file(config_path);
    if (!cfg.is_object()) throw ConfigError("Config root must be an object.");
    AppConfig out;
    out.raw = cfg;
    out.config_path = std::filesystem::absolute(config_path).lexically_normal().string();
    out.config_dir = std::filesystem::path(out.config_path).parent_path().string();

    std::string accounts_file;
    if (const json* v = json_find(cfg, "accounts_file"); v && !v->is_null()) accounts_file = json_string(*v);
    if (accounts_file.empty()) {
        if (const json* v = json_find(cfg, "trading_accounts_file"); v && !v->is_null()) accounts_file = json_string(*v);
    }
    if (accounts_file.empty()) accounts_file = env_or("TRADING_ACCOUNTS_CONFIG", "");
    accounts_file = trim(accounts_file);
    if (!accounts_file.empty()) {
        out.accounts_file = resolve_relative_path(out.config_dir, accounts_file);
    } else {
        const json* accounts = json_find(cfg, "accounts");
        if (!accounts || !accounts->is_array() || accounts->empty()) {
            throw ConfigError("Config missing accounts_file or non-empty 'accounts' list.");
        }
        out.inline_accounts = *accounts;
    }
    const json* symbols = json_find(cfg, "symbols");
    if (!symbols || !symbols->is_array() || symbols->empty()) throw ConfigError("Config missing non-empty 'symbols' list.");
    for (const auto& s : *symbols) out.symbols.push_back(json_string(s));

    json& kafka = ensure_object(cfg, "kafka");
    set_default(kafka, "bootstrap_servers", env_or("KAFKA_BOOTSTRAP_SERVERS", "localhost:9092"));
    set_default(kafka, "market_data_bootstrap_servers",
                env_or("KAFKA_MARKET_DATA_BOOTSTRAP_SERVERS", json_string(kafka["bootstrap_servers"])));
    set_default(kafka, "account_details_topic", env_or("KAFKA_ACCOUNT_DETAILS_TOPIC", "account-details"));
    set_default(kafka, "trading_commands_topic", env_or("KAFKA_TRADING_COMMANDS_TOPIC", "trading-commands"));
    set_default(kafka, "market_data_topic", env_or("KAFKA_MARKET_DATA_TOPIC", "price-books"));
    set_default(kafka, "stock_quotes_topic", env_or("KAFKA_STOCK_QUOTES_TOPIC", "stock-quotes"));
    set_default(kafka, "group_id", env_or("KAFKA_GROUP_ID", "account-dashboard"));
    set_default(kafka, "market_data_group_id", env_or("KAFKA_MARKET_DATA_GROUP_ID", "market-data-dashboard"));
    set_default(kafka, "auto_offset_reset", env_or("KAFKA_AUTO_OFFSET_RESET", "latest"));
    set_default(kafka, "market_data_auto_offset_reset", env_or("KAFKA_MARKET_DATA_AUTO_OFFSET_RESET", "earliest"));
    set_default(kafka, "poll_timeout_sec", 1.0);
    json& producer = ensure_object(kafka, "producer");
    set_default(producer, "acks", "all");
    set_default(producer, "enable_idempotence", true);
    set_default(producer, "linger_ms", 5);

    out.kafka.bootstrap_servers = json_string(kafka["bootstrap_servers"]);
    out.kafka.market_data_bootstrap_servers = json_string(kafka["market_data_bootstrap_servers"]);
    out.kafka.account_details_topic = json_string(kafka["account_details_topic"]);
    out.kafka.trading_commands_topic = json_string(kafka["trading_commands_topic"]);
    out.kafka.market_data_topic = json_string(kafka["market_data_topic"]);
    out.kafka.stock_quotes_topic = json_string(kafka["stock_quotes_topic"]);
    out.kafka.group_id = json_string(kafka["group_id"]);
    out.kafka.market_data_group_id = json_string(kafka["market_data_group_id"]);
    out.kafka.auto_offset_reset = json_string(kafka["auto_offset_reset"]);
    out.kafka.market_data_auto_offset_reset = json_string(kafka["market_data_auto_offset_reset"]);
    out.kafka.poll_timeout_sec = json_to_double(&kafka["poll_timeout_sec"]).value_or(1.0);
    out.kafka.producer_acks = json_string(producer["acks"]);
    out.kafka.producer_enable_idempotence = json_truthy(&producer["enable_idempotence"]);
    out.kafka.producer_linger_ms = static_cast<int>(json_to_int(&producer["linger_ms"]).value_or(5));
    if (const json* cc = json_find(kafka, "client_config"); cc && cc->is_object()) out.kafka.client_config = *cc;
    if (const json* cc = json_find(kafka, "market_data_client_config"); cc && cc->is_object()) {
        out.kafka.market_data_client_config = *cc;
    }
    if (out.kafka.market_data_topic == out.kafka.stock_quotes_topic) {
        throw ConfigError("stock_quotes_topic must differ from market_data_topic");
    }

    json& market = ensure_object(cfg, "market_data");
    set_default(market, "historical_prices_csv",
                env_or("MARKET_DATA_HISTORICAL_PRICES_CSV", "./market_data/historical_prices.csv"));
    std::string max_levels_raw = env_or("MARKET_INSIGHTS_MAX_LEVELS",
                                        json_string(market.value("market_insights_max_levels", json(20))));
    int max_levels = 20;
    {
        std::string t = trim(max_levels_raw);
        bool ok = !t.empty();
        std::size_t i = (ok && (t[0] == '-' || t[0] == '+')) ? 1 : 0;
        if (i >= t.size()) ok = false;
        for (std::size_t k = i; ok && k < t.size(); ++k) ok = std::isdigit(static_cast<unsigned char>(t[k]));
        if (!ok) throw ConfigError("Invalid MARKET_INSIGHTS_MAX_LEVELS: '" + max_levels_raw + "'");
        max_levels = std::stoi(t);
    }
    out.market_data.market_insights_max_levels = std::max(1, std::min(max_levels, 20));
    out.market_data.historical_prices_csv = resolve_relative_path(out.config_dir, json_string(market["historical_prices_csv"]));

    out.fast_trading = normalize_fast_trading_config(json_find(cfg, "fast_trading"));

    const json* smart_raw = json_find(cfg, "smart_selling");
    out.smart_selling = normalize_smart_selling_config(smart_raw);
    if (smart_raw && smart_raw->is_object() && smart_raw->contains("state_db")) {
        out.smart_selling.state_db = resolve_relative_path(out.config_dir, out.smart_selling.state_db);
    }

    out.persistence = normalize_persistence_config(json_find(cfg, "persistence"), out.config_dir, out.smart_selling);

    json& auth = ensure_object(cfg, "auth");
    json& users = ensure_object(auth, "users");
    if (!users.is_object()) throw ConfigError("Config auth.users must be an object.");
    const json* admin = json_find(users, "admin");
    const json* trader = json_find(users, "trader");
    json empty_obj = json::object();
    if (!admin) admin = &empty_obj;
    if (!trader) trader = &empty_obj;
    if (!admin->is_object() || !trader->is_object()) throw ConfigError("Config auth.users.admin/trader must be objects.");
    std::string admin_pw = trim(json_string(admin->value("password", json(""))));
    std::string trader_pw = trim(json_string(trader->value("password", json(""))));
    if (admin_pw.empty() || trader_pw.empty()) {
        throw ConfigError("Config must provide non-empty auth.users.admin.password and auth.users.trader.password.");
    }
    for (auto it = users.begin(); it != users.end(); ++it) {
        if (!it.value().is_object()) continue;
        std::string pw = trim(json_string(it.value().value("password", json(""))));
        if (pw.empty()) continue;
        std::string role = it.key() == "admin" ? "admin" : (it.key() == "trader" ? "trader" : json_string(it.value().value("role", json("trader"))));
        out.users[it.key()] = AuthUser{pw, role};
    }

    json& server = ensure_object(cfg, "server");
    set_default(server, "host", env_or("UI_HOST", "0.0.0.0"));
    set_default(server, "port", std::stoi(env_or("UI_PORT", "8000")));
    std::string ssl_raw = to_lower(trim(env_or("UI_SSL_ENABLED", json_truthy(json_find(server, "ssl_enabled")) ? "true" : "false")));
    out.server.ssl_enabled = ssl_raw == "1" || ssl_raw == "true" || ssl_raw == "yes" || ssl_raw == "on";
    set_default(server, "ssl_certfile", env_or("UI_SSL_CERTFILE", ""));
    set_default(server, "ssl_keyfile", env_or("UI_SSL_KEYFILE", ""));
    out.server.host = json_string(server["host"]);
    out.server.port = static_cast<int>(json_to_int(&server["port"]).value_or(8000));
    std::string certfile = trim(json_string(server["ssl_certfile"]));
    std::string keyfile = trim(json_string(server["ssl_keyfile"]));
    out.server.ssl_certfile = certfile.empty() ? "" : resolve_relative_path(out.config_dir, certfile);
    out.server.ssl_keyfile = keyfile.empty() ? "" : resolve_relative_path(out.config_dir, keyfile);
    if (overrides.host) out.server.host = *overrides.host;
    if (overrides.port) out.server.port = *overrides.port;
    if (overrides.ssl) out.server.ssl_enabled = *overrides.ssl;
    if (overrides.ssl_certfile) out.server.ssl_certfile = *overrides.ssl_certfile;
    if (overrides.ssl_keyfile) out.server.ssl_keyfile = *overrides.ssl_keyfile;
    if (out.server.ssl_enabled) {
        if (out.server.ssl_certfile.empty() || out.server.ssl_keyfile.empty()) {
            throw ConfigError("SSL is enabled but ssl_certfile/ssl_keyfile are missing.");
        }
        if (!std::filesystem::is_regular_file(out.server.ssl_certfile)) {
            throw ConfigError("SSL cert file not found: " + out.server.ssl_certfile);
        }
        if (!std::filesystem::is_regular_file(out.server.ssl_keyfile)) {
            throw ConfigError("SSL key file not found: " + out.server.ssl_keyfile);
        }
    }
    if (const json* order_ui = json_find(cfg, "order_ui"); order_ui && order_ui->is_object()) out.order_ui = *order_ui;
    out.operations_log_dir = env_or("OPERATIONS_LOG_DIR", "./logs");
    return out;
}

AccountDirectory load_account_metas(const AppConfig& config) {
    if (config.accounts_file.empty()) {
        return AccountDirectory(parse_account_metas(config.inline_accounts, false, false));
    }
    json data = read_json_file(config.accounts_file);
    const json* accounts = data.is_object() ? json_find(data, "accounts") : &data;
    if (!accounts || !accounts->is_array() || accounts->empty()) {
        throw ConfigError("Trading accounts file must contain a non-empty accounts list: " + config.accounts_file);
    }
    try {
        return AccountDirectory(parse_account_metas(*accounts, true, false));
    } catch (const std::invalid_argument& e) {
        throw ConfigError(e.what());
    }
}

std::vector<std::string> load_symbols(const AppConfig& config) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (auto& raw : config.symbols) {
        std::string symbol = to_upper(trim(raw));
        if (symbol.empty() || seen.count(symbol)) continue;
        out.push_back(symbol);
        seen.insert(symbol);
    }
    return out;
}

}  // namespace ng
