#include "ops/holdings_recorder.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"

namespace ng::ops {

namespace {
constexpr const char* kLog = "nextgen-gui.holdings";

const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS snapshots (
    id INTEGER PRIMARY KEY,
    taken_at TEXT NOT NULL,
    taken_at_local TEXT NOT NULL,
    date_local TEXT NOT NULL,
    account_count INTEGER NOT NULL,
    accounts_with_data INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS snapshots_taken_at ON snapshots(taken_at);
CREATE TABLE IF NOT EXISTS account_states (
    snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),
    account_id TEXT NOT NULL,
    account_num_id INTEGER,
    broker TEXT,
    trading_medium TEXT,
    has_snapshot INTEGER NOT NULL,
    trading_enabled INTEGER,
    snapshot_ts TEXT,
    age_seconds REAL,
    cash REAL,
    position_count INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS account_states_snapshot ON account_states(snapshot_id);
CREATE TABLE IF NOT EXISTS holdings (
    snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),
    account_id TEXT NOT NULL,
    symbol TEXT NOT NULL,
    qty REAL NOT NULL,
    available_qty REAL,
    avg_price REAL
);
CREATE INDEX IF NOT EXISTS holdings_snapshot ON holdings(snapshot_id);
CREATE INDEX IF NOT EXISTS holdings_account_symbol ON holdings(account_id, symbol);
CREATE TABLE IF NOT EXISTS cash (
    snapshot_id INTEGER NOT NULL REFERENCES snapshots(id),
    account_id TEXT NOT NULL,
    currency TEXT NOT NULL,
    amount REAL NOT NULL,
    available REAL
);
CREATE INDEX IF NOT EXISTS cash_snapshot ON cash(snapshot_id);
)SQL";
}  // namespace

HoldingsRecorder::HoldingsRecorder(const HoldingsRecorderConfig& config, const AccountDirectory& accounts, SnapshotsProvider snapshots)
    : config_(config),
      accounts_(accounts),
      snapshots_(std::move(snapshots)),
      zone_(TimeZone::find(config.timezone).value_or(*TimeZone::find("UTC"))) {
    std::error_code ec;
    std::filesystem::create_directories(config_.dir, ec);
    if (ec) throw std::runtime_error("cannot create account snapshot directory " + config_.dir + ": " + ec.message());
    if (config_.write_sqlite) {
        db_ = std::make_unique<Sqlite>((std::filesystem::path(config_.dir) / "holdings.sqlite3").string());
        db_->exec("PRAGMA journal_mode=WAL");
        db_->exec("PRAGMA synchronous=NORMAL");
        db_->exec(kSchema);
    }
}

HoldingsRecorder::~HoldingsRecorder() { stop(); }

json HoldingsRecorder::build_document(double wall) const {
    std::map<std::string, AccountSnapshot> snapshots = snapshots_();
    json doc;
    doc["taken_at"] = iso_utc(wall);
    doc["taken_at_local"] = zone_.iso(wall);
    doc["date_local"] = zone_.date(wall);
    doc["timezone"] = zone_.name();
    doc["accounts"] = json::array();
    int with_data = 0;
    for (const AccountMeta* meta : accounts_.by_num_id()) {
        json account;
        account["account_id"] = meta->id;
        account["account_num_id"] = meta->num_id;
        account["broker"] = meta->broker;
        account["trading_medium"] = meta->trading_medium;
        auto it = snapshots.find(meta->id);
        if (it == snapshots.end()) {
            account["has_snapshot"] = false;
            account["snapshot_ts"] = nullptr;
            account["age_seconds"] = nullptr;
            account["trading_enabled"] = nullptr;
            account["cash"] = nullptr;
            account["cash_by_currency"] = json::object();
            account["available_cash_by_currency"] = json::object();
            account["positions"] = json::array();
        } else {
            ++with_data;
            const AccountSnapshot& snap = it->second;
            account["has_snapshot"] = true;
            account["snapshot_ts"] = snap.ts ? json(*snap.ts) : json(nullptr);
            auto ts = snap.ts_epoch();
            account["age_seconds"] = ts ? json(std::max(0.0, wall - *ts)) : json(nullptr);
            account["trading_enabled"] = snap.trading_enabled;
            account["cash"] = snap.cash;
            account["cash_by_currency"] = json::object();
            for (auto& [currency, amount] : snap.cash_by_currency) account["cash_by_currency"][currency] = amount;
            account["available_cash_by_currency"] = json::object();
            for (auto& [currency, amount] : snap.available_cash_by_currency) account["available_cash_by_currency"][currency] = amount;
            account["positions"] = json::array();
            for (const Position& p : snap.positions) {
                json row;
                row["symbol"] = p.symbol;
                row["qty"] = p.qty;
                row["available_qty"] = p.available_qty ? json(*p.available_qty) : json(nullptr);
                row["avg_price"] = p.avg_price ? json(*p.avg_price) : json(nullptr);
                account["positions"].push_back(row);
            }
        }
        doc["accounts"].push_back(account);
    }
    doc["account_count"] = static_cast<long long>(doc["accounts"].size());
    doc["accounts_with_data"] = with_data;
    return doc;
}

void HoldingsRecorder::append_jsonl(const json& document, const std::string& date) {
    std::filesystem::path path = std::filesystem::path(config_.dir) / ("holdings-" + date + ".jsonl");
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) throw std::runtime_error("cannot append to " + path.string());
    out << json_dump_compact(document) << '\n';
    if (!out) throw std::runtime_error("write failed for " + path.string());
}

void HoldingsRecorder::insert_sqlite(const json& document) {
    db_->begin_immediate();
    try {
        {
            Statement insert(*db_, "INSERT INTO snapshots(taken_at, taken_at_local, date_local, account_count, accounts_with_data) VALUES(?,?,?,?,?)");
            insert.bind(1, document.at("taken_at").get<std::string>());
            insert.bind(2, document.at("taken_at_local").get<std::string>());
            insert.bind(3, document.at("date_local").get<std::string>());
            insert.bind(4, document.at("account_count").get<long long>());
            insert.bind(5, document.at("accounts_with_data").get<long long>());
            insert.exec();
        }
        long long snapshot_id = db_->last_insert_rowid();
        Statement state(*db_, "INSERT INTO account_states(snapshot_id, account_id, account_num_id, broker, trading_medium, has_snapshot, "
                              "trading_enabled, snapshot_ts, age_seconds, cash, position_count) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
        Statement holding(*db_, "INSERT INTO holdings(snapshot_id, account_id, symbol, qty, available_qty, avg_price) VALUES(?,?,?,?,?,?)");
        Statement cash(*db_, "INSERT INTO cash(snapshot_id, account_id, currency, amount, available) VALUES(?,?,?,?,?)");
        for (const json& account : document.at("accounts")) {
            std::string account_id = account.at("account_id").get<std::string>();
            state.reset();
            state.bind(1, snapshot_id);
            state.bind(2, account_id);
            state.bind(3, account.at("account_num_id").get<long long>());
            state.bind(4, account.at("broker").get<std::string>());
            state.bind(5, account.at("trading_medium").get<std::string>());
            bool has = account.at("has_snapshot").get<bool>();
            state.bind(6, static_cast<long long>(has ? 1 : 0));
            if (account.at("trading_enabled").is_null()) state.bind_null(7); else state.bind(7, static_cast<long long>(account.at("trading_enabled").get<bool>() ? 1 : 0));
            if (account.at("snapshot_ts").is_null()) state.bind_null(8); else state.bind(8, account.at("snapshot_ts").get<std::string>());
            if (account.at("age_seconds").is_null()) state.bind_null(9); else state.bind(9, account.at("age_seconds").get<double>());
            if (account.at("cash").is_null()) state.bind_null(10); else state.bind(10, account.at("cash").get<double>());
            state.bind(11, static_cast<long long>(account.at("positions").size()));
            state.exec();
            for (const json& p : account.at("positions")) {
                holding.reset();
                holding.bind(1, snapshot_id);
                holding.bind(2, account_id);
                holding.bind(3, p.at("symbol").get<std::string>());
                holding.bind(4, p.at("qty").get<double>());
                if (p.at("available_qty").is_null()) holding.bind_null(5); else holding.bind(5, p.at("available_qty").get<double>());
                if (p.at("avg_price").is_null()) holding.bind_null(6); else holding.bind(6, p.at("avg_price").get<double>());
                holding.exec();
            }
            const json& by_currency = account.at("cash_by_currency");
            const json& available = account.at("available_cash_by_currency");
            for (auto it = by_currency.begin(); it != by_currency.end(); ++it) {
                cash.reset();
                cash.bind(1, snapshot_id);
                cash.bind(2, account_id);
                cash.bind(3, it.key());
                cash.bind(4, it.value().get<double>());
                if (available.contains(it.key())) cash.bind(5, available.at(it.key()).get<double>()); else cash.bind_null(5);
                cash.exec();
            }
        }
        db_->commit();
    } catch (...) {
        db_->rollback();
        throw;
    }
}

json HoldingsRecorder::record_once(double wall) {
    json document = build_document(wall);
    std::lock_guard lock(mutex_);
    try {
        if (config_.write_jsonl) append_jsonl(document, document.at("date_local").get<std::string>());
        if (config_.write_sqlite && db_) insert_sqlite(document);
        ++recorded_;
        last_taken_ = wall;
        last_error_.clear();
    } catch (const std::exception& e) {
        last_error_ = e.what();
        throw;
    }
    return document;
}

void HoldingsRecorder::start() {
    if (thread_.joinable()) return;
    thread_ = std::thread([this] { loop(); });
}

void HoldingsRecorder::stop() {
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

json HoldingsRecorder::status() const {
    std::lock_guard lock(mutex_);
    json out;
    out["enabled"] = true;
    out["directory"] = config_.dir;
    out["interval_seconds"] = config_.interval_seconds;
    out["formats"] = json::array();
    if (config_.write_jsonl) out["formats"].push_back("jsonl");
    if (config_.write_sqlite) out["formats"].push_back("sqlite");
    out["recorded"] = recorded_;
    out["last_taken_at"] = last_taken_ > 0 ? json(iso_utc(last_taken_)) : json(nullptr);
    out["last_error"] = last_error_.empty() ? json(nullptr) : json(last_error_);
    return out;
}

void HoldingsRecorder::loop() {
    const double interval = std::max(1, config_.interval_seconds);
    for (;;) {
        double now = wall_now();
        double next = (std::floor(now / interval) + 1) * interval;  // aligned to the wall clock
        {
            std::unique_lock lock(mutex_);
            while (!stop_ && wall_now() < next) {
                double remaining = next - wall_now();
                cv_.wait_for(lock, std::chrono::milliseconds(static_cast<long long>(std::max(0.0, std::min(remaining, 5.0)) * 1000) + 1));
            }
            if (stop_) return;
        }
        try {
            record_once(next);
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "holdings snapshot failed: {}", e.what());
        }
    }
}

}  // namespace ng::ops
