#include "reporting/activity_reporting.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "core/decimal.h"
#include "core/strings.h"
#include "core/time_util.h"

namespace ng::reporting {
namespace {

// _timestamp(): ISO-8601 text with an offset -> epoch seconds.
double timestamp(const std::string& value) {
    auto parsed = parse_iso8601(to_upper(value));
    if (!parsed) throw std::invalid_argument("Invalid isoformat string: " + value);
    if (!parsed->has_offset) throw std::invalid_argument("Reporting timestamps must have a timezone.");
    return parsed->epoch_seconds;
}

std::string iso(double epoch) { return iso_utc(epoch); }

// Python datetimes carry microseconds: the stored ISO text and the epoch columns describe the same
// instant ((seconds * 10**6 + microseconds) / 10**6, like datetime.timestamp()).
double to_microseconds(double epoch) { return static_cast<double>(std::llround(epoch * 1e6)) / 1e6; }

// Decimal(str(number)) for JSON numbers and numeric strings.
Decimal number_decimal(const json& value) {
    if (value.is_number_float()) return Decimal::from_python_float(value.get<double>());
    if (value.is_number_integer()) return Decimal(value.get<long long>());
    if (value.is_number_unsigned()) return Decimal::parse(std::to_string(value.get<unsigned long long>()));
    if (value.is_string()) return Decimal::parse(value.get<std::string>());
    throw DecimalError("not a number");
}

long long integer_field(const json& value) {
    if (value.is_number_float()) return static_cast<long long>(value.get<double>());
    return value.get<long long>();
}

struct CashTotals {
    Decimal shares_bought, shares_sold;
    std::optional<Decimal> buy_cost = Decimal::zero();
    std::optional<Decimal> sell_proceeds = Decimal::zero();
    bool complete() const { return buy_cost.has_value() && sell_proceeds.has_value(); }
};

struct Totals {
    Decimal shares_bought, shares_sold;
    std::map<std::string, CashTotals> cash_by_currency;

    void accumulate(const json& order) {
        Decimal quantity = number_decimal(order.at("filled_quantity"));
        if (quantity.is_zero()) return;
        bool buy = order.at("side").get<std::string>() == "BUY";
        (buy ? shares_bought : shares_sold) += quantity;
        std::string currency = "UNKNOWN";
        if (const json* raw = json_find(order, "currency"); raw && raw->is_string() && !raw->get<std::string>().empty()) {
            currency = raw->get<std::string>();
        }
        CashTotals& cash = cash_by_currency[currency];
        (buy ? cash.shares_bought : cash.shares_sold) += quantity;
        std::optional<Decimal>& field = buy ? cash.buy_cost : cash.sell_proceeds;
        const json* price = json_find(order, "average_fill_price");
        if (!price || price->is_null() || currency == "UNKNOWN") {
            // A missing fill price or currency makes the amount unknown; it is never replaced by a
            // limit price or assumed to be USD.
            field.reset();
        } else if (field) {
            *field += quantity * number_decimal(*price);
        }
    }

    json to_json() const {
        json cash = json::object();
        bool all_complete = true;
        for (auto& [currency, totals] : cash_by_currency) {
            json part;
            part["buy_cost"] = totals.buy_cost ? json(totals.buy_cost->to_double()) : json(nullptr);
            part["sell_proceeds"] = totals.sell_proceeds ? json(totals.sell_proceeds->to_double()) : json(nullptr);
            part["cash_change"] = totals.complete() ? json((*totals.sell_proceeds - *totals.buy_cost).to_double()) : json(nullptr);
            part["complete"] = totals.complete();
            part["shares_bought"] = totals.shares_bought.to_double();
            part["shares_sold"] = totals.shares_sold.to_double();
            part["average_buy_price"] = totals.buy_cost && !totals.shares_bought.is_zero()
                ? json(totals.buy_cost->div(totals.shares_bought, 30).to_double()) : json(nullptr);
            part["average_sell_price"] = totals.sell_proceeds && !totals.shares_sold.is_zero()
                ? json(totals.sell_proceeds->div(totals.shares_sold, 30).to_double()) : json(nullptr);
            all_complete = all_complete && totals.complete();
            cash[currency] = part;
        }
        json out;
        out["shares_bought"] = shares_bought.to_double();
        out["shares_sold"] = shares_sold.to_double();
        out["cash_by_currency"] = cash;
        out["cash_complete"] = all_complete;
        return out;
    }
};

bool is_zero_quantity(const json& value) {
    if (value.is_null()) return true;
    if (value.is_number()) return value.get<double>() == 0.0;
    return false;
}

}  // namespace

json summarize_executions(const std::vector<json>& orders, const AccountDirectory& metas,
                          const std::vector<std::string>& include_accounts) {
    std::map<std::string, Totals> accounts;
    for (const auto& account : include_accounts) accounts[account];
    Totals totals;
    for (const json& order : orders) {
        if (is_zero_quantity(order.at("filled_quantity"))) continue;
        accounts[order.at("account_id").get<std::string>()].accumulate(order);
        totals.accumulate(order);
    }
    std::vector<std::pair<const AccountMeta*, const Totals*>> rows;
    for (auto& [account_id, summary] : accounts) {
        const AccountMeta* meta = metas.find(account_id);
        if (meta) rows.emplace_back(meta, &summary);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first->num_id < b.first->num_id; });
    json out;
    out["accounts"] = json::array();
    for (auto& [meta, summary] : rows) {
        json row = summary->to_json();
        row["account_id"] = meta->id;
        row["account_num_id"] = meta->num_id;
        row["broker"] = meta->broker;
        out["accounts"].push_back(row);
    }
    out["totals"] = totals.to_json();
    return out;
}

ActivityReporting::ActivityReporting(const std::string& db_path, Clock now) {
    Clock source = now ? std::move(now) : Clock(&wall_now);
    now_ = [source = std::move(source)] { return to_microseconds(source()); };
    if (db_path != ":memory:") {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(db_path).parent_path(), ec);
    }
    db_ = std::make_unique<Sqlite>(db_path, Sqlite::Mode::ReadWriteCreate, 10000);
    db_->exec("PRAGMA journal_mode=WAL");
    // High-frequency idempotent upserts (order snapshots, heartbeats) commit without an fsync; they
    // are re-derived from Kafka replay after a crash. Session and tag mutations switch to FULL.
    db_->exec("PRAGMA synchronous=NORMAL");
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS activity_sessions (
            id TEXT PRIMARY KEY, parameters TEXT NOT NULL, account_ids TEXT NOT NULL,
            started_at TEXT NOT NULL, updated_at TEXT NOT NULL, ended_at TEXT,
            status TEXT NOT NULL, reason TEXT
        );
        CREATE TABLE IF NOT EXISTS activity_commands (
            account_id TEXT NOT NULL, command_id TEXT NOT NULL, session_id TEXT NOT NULL,
            PRIMARY KEY(account_id, command_id)
        );
        CREATE TABLE IF NOT EXISTS activity_orders (
            account_id TEXT NOT NULL, order_id TEXT NOT NULL, sequence INTEGER NOT NULL,
            created_epoch REAL NOT NULL, updated_epoch REAL NOT NULL, symbol TEXT NOT NULL,
            session_id TEXT, payload TEXT NOT NULL, PRIMARY KEY(account_id, order_id)
        );
        CREATE INDEX IF NOT EXISTS activity_orders_session ON activity_orders(session_id);
        CREATE INDEX IF NOT EXISTS activity_orders_symbol_time ON activity_orders(symbol, created_epoch);
        CREATE TABLE IF NOT EXISTS activity_tags (
            id TEXT PRIMARY KEY, name TEXT NOT NULL, symbol TEXT NOT NULL,
            started_at TEXT NOT NULL, start_epoch REAL NOT NULL,
            stopped_at TEXT, stop_epoch REAL
        );
        CREATE UNIQUE INDEX IF NOT EXISTS activity_one_active_tag
            ON activity_tags((1)) WHERE stopped_at IS NULL;
    )SQL");
}

ActivityReporting::~ActivityReporting() { close(); }

void ActivityReporting::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        db_->close();
        db_.reset();
    }
}

template <typename Fn>
auto ActivityReporting::transaction(bool durable, Fn&& fn) -> decltype(fn()) {
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("activity reporting is closed");
    struct RestoreSync {
        Sqlite* db;
        ~RestoreSync() {
            if (!db) return;
            try {
                db->exec("PRAGMA synchronous=NORMAL");
            } catch (const std::exception&) {
            }
        }
    } restore{durable ? db_.get() : nullptr};
    if (durable) db_->exec("PRAGMA synchronous=FULL");
    db_->begin_immediate();
    try {
        if constexpr (std::is_void_v<decltype(fn())>) {
            fn();
            db_->commit();
        } else {
            auto result = fn();
            db_->commit();
            return result;
        }
    } catch (...) {
        db_->rollback();
        throw;
    }
}

void ActivityReporting::start_session(const std::string& session_id, const json& parameters,
                                      const std::vector<std::string>& account_ids, const std::optional<std::string>& started_at) {
    std::string now = iso(now_());
    std::string started = started_at && !started_at->empty() ? iso(timestamp(*started_at)) : now;
    json accounts = json::array();
    std::set<std::string> seen;
    for (const auto& account : account_ids) {
        if (seen.insert(account).second) accounts.push_back(account);
    }
    transaction(true, [&] {
        Statement insert(*db_, "INSERT OR IGNORE INTO activity_sessions (id,parameters,account_ids,started_at,updated_at,status) "
                               "VALUES (?,?,?,?,?,?)");
        insert.bind(1, session_id).bind(2, json_dump_compact(parameters)).bind(3, json_dump_compact(accounts));
        insert.bind(4, started).bind(5, now).bind(6, "running");
        insert.exec();
    });
}

void ActivityReporting::heartbeat_session(const std::string& session_id, const std::string& status) {
    transaction(false, [&] {
        Statement update(*db_, "UPDATE activity_sessions SET updated_at=?,status=? WHERE id=? AND ended_at IS NULL");
        update.bind(1, iso(now_())).bind(2, status).bind(3, session_id);
        update.exec();
    });
}

void ActivityReporting::end_session(const std::string& session_id, const std::string& status,
                                    const std::optional<std::string>& reason) {
    std::string now = iso(now_());
    transaction(true, [&] {
        Statement update(*db_, "UPDATE activity_sessions SET ended_at=?,updated_at=?,status=?,reason=? WHERE id=? AND ended_at IS NULL");
        update.bind(1, now).bind(2, now).bind(3, status);
        update.bind_optional(4, reason);
        update.bind(5, session_id);
        update.exec();
    });
}

void ActivityReporting::register_command(const std::string& session_id, const json& command) {
    const json* account = json_find(command, "account_id");
    if (!json_python_truthy(account)) return;
    std::string account_id = json_to_string(account).value_or("");
    std::set<std::string> identifiers;
    for (const char* key : {"command_id", "client_order_id"}) {
        const json* value = json_find(command, key);
        if (json_python_truthy(value)) identifiers.insert(json_to_string(value).value_or(""));
    }
    transaction(true, [&] {
        for (const auto& identifier : identifiers) {
            Statement owner(*db_, "SELECT session_id FROM activity_commands WHERE account_id=? AND command_id=?");
            owner.bind(1, account_id).bind(2, identifier);
            if (owner.step() && owner.column_text(0) != session_id) {
                throw std::invalid_argument("An order command cannot belong to two algorithm sessions.");
            }
            Statement insert(*db_, "INSERT OR IGNORE INTO activity_commands VALUES (?,?,?)");
            insert.bind(1, account_id).bind(2, identifier).bind(3, session_id);
            insert.exec();
        }
        // Repair a correlation if broker feedback beat command registration.
        std::vector<std::string> repair;
        {
            Statement rows(*db_, "SELECT order_id,payload FROM activity_orders WHERE account_id=? AND session_id IS NULL");
            rows.bind(1, account_id);
            while (rows.step()) {
                json order = json::parse(rows.column_text(1), nullptr, false);
                if (!order.is_object()) continue;
                for (const char* key : {"command_id", "client_order_id"}) {
                    const json* value = json_find(order, key);
                    if (value && value->is_string() && identifiers.count(value->get<std::string>())) {
                        repair.push_back(rows.column_text(0));
                        break;
                    }
                }
            }
        }
        for (const auto& order_id : repair) {
            Statement update(*db_, "UPDATE activity_orders SET session_id=? WHERE account_id=? AND order_id=?");
            update.bind(1, session_id).bind(2, account_id).bind(3, order_id);
            update.exec();
        }
    });
}

bool ActivityReporting::record_order(const orders::OrderEvent& event) {
    const json& data = event.payload;
    std::string account_id = data.at("account_id").get<std::string>();
    std::string order_id = data.at("order_id").get<std::string>();
    long long sequence = integer_field(data.at("sequence"));
    std::string symbol = to_upper(trim(data.at("symbol").get<std::string>()));
    return transaction(false, [&]() -> bool {
        std::optional<std::string> session_id;
        bool existing = false;
        {
            Statement old(*db_, "SELECT sequence,created_epoch,updated_epoch,symbol,session_id,payload FROM activity_orders "
                                "WHERE account_id=? AND order_id=?");
            old.bind(1, account_id).bind(2, order_id);
            if (old.step()) {
                existing = true;
                if (sequence <= old.column_int(0)) return false;
                json previous = json::parse(old.column_text(5), nullptr, false);
                if (event.created_at != old.column_double(1)) throw std::invalid_argument("created_at is immutable for an order.");
                if (event.updated_at < old.column_double(2)) {
                    throw std::invalid_argument("updated_at cannot decrease when sequence increases.");
                }
                if (symbol != old.column_text(3) || !previous.is_object() || data.at("side") != previous.value("side", json(nullptr))) {
                    throw std::invalid_argument("Order symbol and side are immutable.");
                }
                session_id = old.column_optional_text(4);
            }
        }
        (void)existing;
        if (!session_id) {
            for (const char* key : {"command_id", "client_order_id"}) {
                const json* value = json_find(data, key);
                if (!value || !value->is_string() || value->get<std::string>().empty()) continue;
                Statement owner(*db_, "SELECT session_id FROM activity_commands WHERE account_id=? AND command_id=?");
                owner.bind(1, account_id).bind(2, value->get<std::string>());
                if (owner.step()) {
                    session_id = owner.column_text(0);
                    break;
                }
            }
        }
        Statement upsert(*db_, "INSERT INTO activity_orders VALUES (?,?,?,?,?,?,?,?) "
                               "ON CONFLICT(account_id,order_id) DO UPDATE SET sequence=excluded.sequence,"
                               "updated_epoch=excluded.updated_epoch,session_id=excluded.session_id,payload=excluded.payload");
        upsert.bind(1, account_id).bind(2, order_id).bind(3, sequence).bind(4, event.created_at).bind(5, event.updated_at);
        upsert.bind(6, symbol);
        upsert.bind_optional(7, session_id);
        upsert.bind(8, json_dump_compact(data));
        upsert.exec();
        return true;
    });
}

std::string ActivityReporting::start_tag(const std::string& name, const std::string& symbol_in) {
    std::string clean_name = python_strip(name);
    std::size_t name_length = utf8_length(clean_name);
    if (name_length < 1 || name_length > 100) throw std::invalid_argument("Tag name must contain 1 to 100 characters.");
    for (unsigned char c : name) {
        if (c < 32) throw std::invalid_argument("Tag name cannot contain control characters.");
    }
    std::string symbol = python_strip(symbol_in);
    std::size_t symbol_length = utf8_length(symbol);
    if (symbol_length < 1 || symbol_length > 64) throw std::invalid_argument("Symbol must contain 1 to 64 characters.");
    symbol = to_upper(symbol);
    bool control = false;
    for (unsigned char c : symbol) control = control || c < 32;
    if (control || contains_python_whitespace(symbol)) {
        throw std::invalid_argument("Symbol cannot contain whitespace or control characters.");
    }
    return transaction(true, [&]() -> std::string {
        {
            Statement active(*db_, "SELECT 1 FROM activity_tags WHERE stopped_at IS NULL");
            if (active.step()) throw std::invalid_argument("Stop the active tag before starting another.");
        }
        double now = now_();
        std::string tag_id = uuid4_hex();
        Statement insert(*db_, "INSERT INTO activity_tags (id,name,symbol,started_at,start_epoch) VALUES (?,?,?,?,?)");
        insert.bind(1, tag_id).bind(2, clean_name).bind(3, symbol).bind(4, iso(now)).bind(5, now);
        insert.exec();
        return tag_id;
    });
}

void ActivityReporting::stop_tag(const std::string& tag_id) {
    transaction(true, [&] {
        std::string started_at;
        bool stopped = false;
        {
            Statement row(*db_, "SELECT started_at,stopped_at FROM activity_tags WHERE id=?");
            row.bind(1, tag_id);
            if (!row.step()) throw KeyError(tag_id);
            started_at = row.column_text(0);
            stopped = !row.is_null(1);
        }
        if (stopped) return;
        double now = std::max(now_(), timestamp(started_at));
        Statement update(*db_, "UPDATE activity_tags SET stopped_at=?,stop_epoch=? WHERE id=?");
        update.bind(1, iso(now)).bind(2, now).bind(3, tag_id);
        update.exec();
    });
}

namespace {

struct TagRow {
    std::string id, name, symbol, started_at;
    std::optional<std::string> stopped_at;
    double start_epoch = 0;
    std::optional<double> stop_epoch;
};

std::vector<TagRow> read_tags(Sqlite& db, const char* order) {
    std::vector<TagRow> tags;
    Statement rows(db, std::string("SELECT id,name,symbol,started_at,start_epoch,stopped_at,stop_epoch FROM activity_tags ORDER BY ") + order);
    while (rows.step()) {
        TagRow tag;
        tag.id = rows.column_text(0);
        tag.name = rows.column_text(1);
        tag.symbol = rows.column_text(2);
        tag.started_at = rows.column_text(3);
        tag.start_epoch = rows.column_double(4);
        tag.stopped_at = rows.column_optional_text(5);
        if (!rows.is_null(6)) tag.stop_epoch = rows.column_double(6);
        tags.push_back(std::move(tag));
    }
    return tags;
}

}  // namespace

std::map<std::pair<std::string, std::string>, json> ActivityReporting::order_tags(const std::vector<json>& orders) {
    std::vector<TagRow> tags;
    {
        std::lock_guard lock(mutex_);
        if (!db_) throw std::runtime_error("activity reporting is closed");
        tags = read_tags(*db_, "start_epoch");
    }
    std::map<std::pair<std::string, std::string>, json> result;
    for (const json& order : orders) {
        double created = timestamp(order.at("created_at").get<std::string>());
        std::string symbol = to_upper(trim(order.at("symbol").get<std::string>()));
        json matched = json::array();
        for (const TagRow& tag : tags) {
            if (symbol == tag.symbol && created >= tag.start_epoch && (!tag.stop_epoch || created < *tag.stop_epoch)) {
                matched.push_back(json{{"id", tag.id}, {"name", tag.name}});
            }
        }
        result[{order.at("account_id").get<std::string>(), order.at("order_id").get<std::string>()}] = std::move(matched);
    }
    return result;
}

json ActivityReporting::snapshot(const AccountDirectory& accounts) {
    struct SessionRow {
        std::string id, parameters, account_ids, started_at, updated_at, status;
        std::optional<std::string> ended_at, reason;
    };
    struct OrderRow {
        std::string account_id, symbol;
        std::optional<std::string> session_id;
        double created_epoch = 0;
        json payload;
    };
    std::vector<SessionRow> sessions;
    std::vector<TagRow> tags;
    std::vector<OrderRow> visible;
    {
        // Read all report tables in one SQLite snapshot while the other server writes.
        std::lock_guard lock(mutex_);
        if (!db_) throw std::runtime_error("activity reporting is closed");
        db_->begin();
        try {
            {
                Statement rows(*db_, "SELECT id,parameters,account_ids,started_at,updated_at,ended_at,status,reason "
                                     "FROM activity_sessions ORDER BY started_at DESC");
                while (rows.step()) {
                    SessionRow row;
                    row.id = rows.column_text(0);
                    row.parameters = rows.column_text(1);
                    row.account_ids = rows.column_text(2);
                    row.started_at = rows.column_text(3);
                    row.updated_at = rows.column_text(4);
                    row.ended_at = rows.column_optional_text(5);
                    row.status = rows.column_text(6);
                    row.reason = rows.column_optional_text(7);
                    sessions.push_back(std::move(row));
                }
            }
            tags = read_tags(*db_, "start_epoch DESC");
            {
                Statement rows(*db_, "SELECT account_id,symbol,session_id,created_epoch,payload FROM activity_orders");
                while (rows.step()) {
                    std::string account_id = rows.column_text(0);
                    if (!accounts.contains(account_id)) continue;
                    OrderRow row;
                    row.account_id = std::move(account_id);
                    row.symbol = rows.column_text(1);
                    row.session_id = rows.column_optional_text(2);
                    row.created_epoch = rows.column_double(3);
                    row.payload = json::parse(rows.column_text(4), nullptr, false);
                    if (row.payload.is_object()) visible.push_back(std::move(row));
                }
            }
        } catch (...) {
            db_->rollback();
            throw;
        }
        db_->rollback();
    }
    double now = now_();
    json session_reports = json::array();
    for (const SessionRow& session : sessions) {
        json all_accounts = json::parse(session.account_ids, nullptr, false);
        std::vector<std::string> selected;
        if (all_accounts.is_array()) {
            for (const auto& account : all_accounts) {
                if (account.is_string() && accounts.contains(account.get<std::string>())) selected.push_back(account.get<std::string>());
            }
        }
        if (selected.empty()) continue;
        json report;
        report["id"] = session.id;
        json parameters = json::parse(session.parameters, nullptr, false);
        report["parameters"] = parameters.is_discarded() ? json::object() : parameters;
        report["account_ids"] = selected;
        report["started_at"] = session.started_at;
        report["updated_at"] = session.updated_at;
        report["ended_at"] = session.ended_at ? json(*session.ended_at) : json(nullptr);
        report["status"] = session.status;
        report["reason"] = session.reason ? json(*session.reason) : json(nullptr);
        const json* symbol = parameters.is_object() ? json_find(parameters, "symbol") : nullptr;
        report["symbol"] = symbol ? *symbol : json("");
        bool ended = session.ended_at && !session.ended_at->empty();
        bool stale = !ended && now - timestamp(session.updated_at) > 90;
        if (stale) report["status"] = "unknown";
        report["running"] = stale ? json(nullptr) : json(!ended);
        std::vector<json> matches;
        for (const OrderRow& order : visible) {
            if (order.session_id && *order.session_id == session.id &&
                std::find(selected.begin(), selected.end(), order.account_id) != selected.end()) {
                matches.push_back(order.payload);
            }
        }
        report.update(summarize_executions(matches, accounts, selected));
        session_reports.push_back(std::move(report));
    }
    json tag_reports = json::array();
    json active_tag_id = nullptr;
    for (const TagRow& tag : tags) {
        std::vector<json> matches;
        for (const OrderRow& order : visible) {
            if (order.symbol == tag.symbol && order.created_epoch >= tag.start_epoch &&
                (!tag.stop_epoch || order.created_epoch < *tag.stop_epoch)) {
                matches.push_back(order.payload);
            }
        }
        json report;
        report["id"] = tag.id;
        report["name"] = tag.name;
        report["symbol"] = tag.symbol;
        report["started_at"] = tag.started_at;
        report["stopped_at"] = tag.stopped_at ? json(*tag.stopped_at) : json(nullptr);
        report["active"] = !tag.stopped_at.has_value();
        report.update(summarize_executions(matches, accounts));
        if (!tag.stopped_at && active_tag_id.is_null()) active_tag_id = tag.id;
        tag_reports.push_back(std::move(report));
    }
    json out;
    out["sessions"] = std::move(session_reports);
    out["tags"] = std::move(tag_reports);
    out["active_tag_id"] = active_tag_id;
    return out;
}

}  // namespace ng::reporting
