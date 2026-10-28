#include "algo/smart_selling/feedback_store.h"

#include <filesystem>
#include <set>

#include "core/strings.h"

namespace ng::smart {

std::string canonical_order_symbol(const std::string& value) {
    std::string symbol = to_upper(trim(value));
    for (const char* prefix : {"US.", "HK.", "CN.", "SH.", "SZ."}) {
        if (starts_with(symbol, prefix)) {
            symbol = symbol.substr(std::string(prefix).size());
            break;
        }
    }
    std::size_t slash = symbol.find('/');
    if (slash != std::string::npos) symbol.erase(slash);
    return symbol;
}

OrderFeedbackStore::OrderFeedbackStore(const std::string& state_path) {
    if (state_path != ":memory:") {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(state_path).parent_path(), ec);
    }
    db_ = std::make_unique<Sqlite>(state_path);
    db_->exec("PRAGMA journal_mode=WAL");
    db_->exec("PRAGMA synchronous=FULL");
    db_->exec(R"SQL(
        CREATE TABLE IF NOT EXISTS feedback_orders (
            account_id TEXT NOT NULL,
            order_id TEXT NOT NULL,
            symbol TEXT NOT NULL,
            side TEXT NOT NULL,
            status TEXT NOT NULL,
            sequence INTEGER NOT NULL,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL,
            PRIMARY KEY(account_id, order_id)
        );
        CREATE INDEX IF NOT EXISTS feedback_order_instrument
            ON feedback_orders(account_id, symbol, status);
        CREATE TABLE IF NOT EXISTS feedback_pending_commands (
            command_id TEXT PRIMARY KEY,
            account_id TEXT NOT NULL,
            symbol TEXT NOT NULL,
            pending INTEGER NOT NULL DEFAULT 1
        );
        CREATE INDEX IF NOT EXISTS feedback_pending_instrument
            ON feedback_pending_commands(account_id, symbol, pending);
    )SQL");
}

OrderFeedbackStore::~OrderFeedbackStore() { close(); }

void OrderFeedbackStore::close() {
    std::lock_guard lock(mutex_);
    if (db_) {
        db_->close();
        db_.reset();
    }
}

void OrderFeedbackStore::validate_external_identifiers(const json& payload) {
    std::set<std::string> pending_ids;
    for (const char* name : {"command_id", "client_order_id"}) {
        const json* identity = json_find(payload, name);
        if (!identity || !identity->is_string() || identity->get<std::string>().empty()) continue;
        Statement row(*db_, "SELECT 1 FROM feedback_pending_commands WHERE command_id=?");
        row.bind(1, identity->get<std::string>());
        if (row.step()) pending_ids.insert(identity->get<std::string>());
    }
    if (pending_ids.size() > 1) throw std::invalid_argument("Snapshot identifiers refer to different external commands");
}

void OrderFeedbackStore::validate_immutable_identity(const FeedbackRow& snapshot, const std::optional<FeedbackRow>& previous) {
    if (!previous) return;
    if (previous->created_at != snapshot.created_at || previous->symbol != snapshot.symbol || previous->side != snapshot.side) {
        throw std::invalid_argument("Order snapshot changed immutable order identity");
    }
    if (snapshot.sequence > previous->sequence && snapshot.updated_at < previous->updated_at) {
        throw std::invalid_argument("Order snapshot source time decreased as sequence increased");
    }
}

bool OrderFeedbackStore::is_current_terminal(const FeedbackRow& snapshot, const std::optional<FeedbackRow>& previous) {
    if (!is_feedback_terminal(snapshot.status)) return false;
    if (!previous || snapshot.sequence > previous->sequence) return true;
    return snapshot.sequence == previous->sequence && snapshot.status == previous->status;
}

void OrderFeedbackStore::complete_external_commands(const json& payload, const FeedbackRow& snapshot) {
    for (const char* name : {"command_id", "client_order_id"}) {
        const json* identity = json_find(payload, name);
        if (!identity || !identity->is_string() || identity->get<std::string>().empty()) continue;
        Statement update(*db_,
                         "UPDATE feedback_pending_commands SET pending=0 WHERE command_id=? AND account_id=? AND symbol=?");
        update.bind(1, identity->get<std::string>());
        update.bind(2, snapshot.account_id);
        update.bind(3, snapshot.symbol);
        update.exec();
    }
}

std::optional<FeedbackRow> OrderFeedbackStore::record_snapshot(const OrderEvent& event) {
    const json& payload = event.payload;
    FeedbackRow snapshot;
    snapshot.account_id = payload.at("account_id").get<std::string>();
    snapshot.order_id = payload.at("order_id").get<std::string>();
    snapshot.symbol = canonical_order_symbol(payload.at("symbol").get<std::string>());
    snapshot.side = payload.at("side").get<std::string>();
    snapshot.status = payload.at("status").get<std::string>();
    snapshot.sequence = payload.at("sequence").is_number_float() ? static_cast<long long>(payload.at("sequence").get<double>())
                                                                 : payload.at("sequence").get<long long>();
    // Python stores datetime.isoformat() with "+00:00"; keep an equivalent sortable form.
    snapshot.created_at = event.created_at_iso;
    snapshot.updated_at = event.updated_at_iso;
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("feedback store is closed");
    db_->begin_immediate();
    try {
        validate_external_identifiers(payload);
        std::optional<FeedbackRow> previous;
        {
            Statement row(*db_, "SELECT * FROM feedback_orders WHERE account_id=? AND order_id=?");
            row.bind(1, snapshot.account_id);
            row.bind(2, snapshot.order_id);
            if (row.step()) {
                FeedbackRow p;
                p.account_id = row.column_text(0);
                p.order_id = row.column_text(1);
                p.symbol = row.column_text(2);
                p.side = row.column_text(3);
                p.status = row.column_text(4);
                p.sequence = row.column_int(5);
                p.created_at = row.column_text(6);
                p.updated_at = row.column_text(7);
                previous = p;
            }
        }
        validate_immutable_identity(snapshot, previous);
        if (!previous || snapshot.sequence > previous->sequence) {
            Statement insert(*db_, "INSERT OR REPLACE INTO feedback_orders VALUES (?,?,?,?,?,?,?,?)");
            insert.bind(1, snapshot.account_id);
            insert.bind(2, snapshot.order_id);
            insert.bind(3, snapshot.symbol);
            insert.bind(4, snapshot.side);
            insert.bind(5, snapshot.status);
            insert.bind(6, snapshot.sequence);
            insert.bind(7, snapshot.created_at);
            insert.bind(8, snapshot.updated_at);
            insert.exec();
        }
        if (is_current_terminal(snapshot, previous)) complete_external_commands(payload, snapshot);
        db_->commit();
        return previous;
    } catch (...) {
        db_->rollback();
        throw;
    }
}

void OrderFeedbackStore::track_external_order(const json& command) {
    std::string identity = trim(json_to_string(json_find(command, "command_id")).value_or(""));
    std::string account = trim(json_to_string(json_find(command, "account_id")).value_or(""));
    std::string symbol = canonical_order_symbol(json_to_string(json_find(command, "symbol")).value_or(""));
    if (identity.empty() || account.empty() || symbol.empty()) {
        throw std::invalid_argument("External order tracking requires command_id, account_id and symbol");
    }
    std::lock_guard lock(mutex_);
    if (!db_) throw std::runtime_error("feedback store is closed");
    db_->begin_immediate();
    try {
        Statement previous(*db_, "SELECT account_id, symbol FROM feedback_pending_commands WHERE command_id=?");
        previous.bind(1, identity);
        if (previous.step() && (previous.column_text(0) != account || previous.column_text(1) != symbol)) {
            throw std::invalid_argument("External command ID was reused for another account/instrument");
        }
        Statement insert(*db_, "INSERT OR IGNORE INTO feedback_pending_commands(command_id, account_id, symbol) VALUES (?,?,?)");
        insert.bind(1, identity);
        insert.bind(2, account);
        insert.bind(3, symbol);
        insert.exec();
        db_->commit();
    } catch (...) {
        db_->rollback();
        throw;
    }
}

bool OrderFeedbackStore::has_open_orders(const std::string& account_id, const std::string& symbol) {
    std::string instrument = canonical_order_symbol(symbol);
    std::lock_guard lock(mutex_);
    if (!db_) return false;
    {
        Statement row(*db_,
                      "SELECT 1 FROM feedback_orders WHERE account_id=? AND symbol=? "
                      "AND status NOT IN ('FILLED','CANCELED','REJECTED','EXPIRED') LIMIT 1");
        row.bind(1, account_id);
        row.bind(2, instrument);
        if (row.step()) return true;
    }
    Statement row(*db_, "SELECT 1 FROM feedback_pending_commands WHERE account_id=? AND symbol=? AND pending=1 LIMIT 1");
    row.bind(1, account_id);
    row.bind(2, instrument);
    return row.step();
}

}  // namespace ng::smart
