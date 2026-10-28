#include "smart_fixtures.h"
#include <set>
#include "algo/smart_selling/runtime.h"

using namespace ngtest;
using ng::AccountDirectory;
using ng::AccountMeta;
using ng::AccountSnapshot;
using ng::Position;
using ng::smart::SmartSellingRuntime;

namespace {

struct RuntimeFixture {
    TempDir dir{"smart-runtime"};
    double now = 0.0;
    double epoch = 1800000000.0;
    AccountDirectory metas{std::vector<AccountMeta>{meta("A", 1, "AnyBroker", "WEB")}};
    std::map<std::string, AccountSnapshot> snapshots;
    std::vector<json> published;
    std::vector<std::string> keys;
    std::string publish_error;
    std::shared_ptr<SmartSellingEngine> engine;
    std::unique_ptr<SmartSellingRuntime> runtime;
    std::string parent_id;
    bool fast_active_flag = false;
    bool fast_conflict_flag = false;

    RuntimeFixture() {
        AccountSnapshot snap;
        snap.account_id = "A";
        snap.cash = 0;
        snap.positions.push_back(Position{"US.XYZ", 1000, std::nullopt, 1000});
        snap.ts = iso();
        snap.trading_enabled = true;
        snapshots["A"] = snap;
        engine = std::make_shared<SmartSellingEngine>(dir.file("state.sqlite3"), [this] { return now; }, [this] { return epoch + now; });
        build_runtime(json::object());
    }
    ~RuntimeFixture() { runtime->close(); }
    void build_runtime(const json& config_raw) {
        if (runtime) runtime->close();
        runtime = std::make_unique<SmartSellingRuntime>(
            ng::normalize_smart_selling_config(&config_raw), [this]() -> const AccountDirectory& { return metas; },
            [this] { return snapshots; },
            [this](const json& command, const std::string& key) {
                if (!publish_error.empty()) throw std::runtime_error(publish_error);
                published.push_back(command);
                keys.push_back(key);
            },
            [this](const std::string&, const std::string&) { return fast_conflict_flag; }, [this] { return fast_active_flag; }, engine,
            [this] { return now; }, [this] { return epoch + now; });
        runtime->set_feed_states_for_test({{"orders", "live"}, {"volume", "live"}});
    }
    std::string iso(double seconds = 0) const { return ng::iso_utc(epoch + now + seconds); }
    AccountSnapshot& snap() { return snapshots["A"]; }
    json start(json changes = json::object()) {
        json settings = {{"account_id", "A"}, {"symbol", "US.XYZ"}, {"quantity", "1000"}, {"mode", "urgent"}, {"deadline", iso(300)}};
        for (auto it = changes.begin(); it != changes.end(); ++it) settings[it.key()] = it.value();
        json parent = runtime->start_parent(settings);
        parent_id = parent["parent_id"].get<std::string>();
        QuoteUpdate u;
        u.bid = 10;
        u.ask = 10.01;
        u.bid_size = 1000;
        u.source_time = epoch + now;
        engine->on_quote("XYZ", u);
        return parent;
    }
    void trade(const std::string& id = "t1", json quantity = 1000, double source = -1) {
        TradeUpdate t;
        t.quantity = quantity;
        t.event_id = id;
        t.source_time = source < 0 ? epoch : source;
        engine->on_trade("XYZ", t);
    }
    json submit() {
        start();
        trade();
        runtime->tick();
        runtime->wait_idle();
        REQUIRE(!published.empty());
        return published.back();
    }
    json snapshot(const json& command, json changes = json::object()) {
        json payload = {{"schema_version", 1}, {"account_id", "A"}, {"order_id", "broker-1"}, {"sequence", 1},
                        {"created_at", command.value("ts", ng::iso_utc(epoch))}, {"updated_at", iso()}, {"symbol", "XYZ"},
                        {"side", "SELL"}, {"order_type", "LIMIT"}, {"status", "NEW"}, {"quantity", command["qty_shares"]},
                        {"filled_quantity", 0}, {"average_fill_price", nullptr}, {"limit_price", command["limit_price"]},
                        {"command_id", command["command_id"]}};
        for (auto it = changes.begin(); it != changes.end(); ++it) payload[it.key()] = it.value();
        return payload;
    }
    void order_payload(const json& payload) { runtime->on_order_payload(payload.dump()); }
    AccountSnapshot& add_account(const std::string& account = "B", double quantity = 400) {
        std::vector<AccountMeta> all = metas.ordered();
        all.push_back(meta(account, static_cast<int>(all.size()) + 1, "AnyBroker", "WEB"));
        metas = AccountDirectory(all);
        AccountSnapshot s;
        s.account_id = account;
        s.cash = 0;
        s.positions.push_back(Position{"US.XYZ", quantity, std::nullopt, quantity});
        s.ts = iso();
        s.trading_enabled = true;
        snapshots[account] = s;
        return snapshots[account];
    }
};

}  // namespace

TEST_CASE_FIXTURE(RuntimeFixture, "multi account defaults to all inventory and retains quantity override") {
    add_account();
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"start_time", nullptr}, {"deadline", nullptr}});
    CHECK(parent["account_ids"] == json::array({"A", "B"}));
    CHECK(Dec(parent["quantity"]) == Dec("1400"));
    CHECK(parent["deadline"].is_null());
    CHECK(runtime->has_active_session());
    CHECK(runtime->owns_account("A"));
    CHECK(runtime->owns_account("B"));
    CHECK(runtime->owns("B", "US.XYZ"));
    CHECK_FALSE(runtime->owns_account("C"));
    runtime->stop_parent(S(parent["parent_id"]));
    CHECK_FALSE(runtime->has_active_session());
    parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"quantity", 125}, {"deadline", nullptr}});
    CHECK(Dec(parent["quantity"]) == Dec("125"));
}

TEST_CASE_FIXTURE(RuntimeFixture, "all inventory floors each account to its sellable quantity step") {
    snap().positions[0].qty = 1000.5;
    snap().positions[0].available_qty = 1000.5;
    add_account("B", 400.75);
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}});
    CHECK(Dec(parent["quantity"]) == Dec("1400"));
    CHECK(Dec(parent["accounts"]["A"]["quantity"]) == Dec("1000"));
    CHECK(Dec(parent["accounts"]["B"]["quantity"]) == Dec("400"));
    runtime->stop_all();
    CHECK_THROWS_WITH(runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"quantity", 1401}}),
                      "Quantity exceeds the selected accounts' available position");
}

TEST_CASE_FIXTURE(RuntimeFixture, "legacy legal explicit quantity accepts fractional inventory") {
    snap().positions[0].qty = 1000.5;
    snap().positions[0].available_qty = 1000.5;
    json parent = start({{"quantity", 100}});
    CHECK(Dec(parent["quantity"]) == Dec("100"));
    CHECK(Dec(parent["accounts"]["A"]["quantity"]) == Dec("1000"));
    runtime->stop_all();
    CHECK_THROWS_WITH(start({{"quantity", 100.5}}), "quantity must satisfy the instrument minimum and quantity step");
}

TEST_CASE_FIXTURE(RuntimeFixture, "inventory rounding honors configured step and actual holdings") {
    build_runtime(json::parse(R"({"instruments": {"XYZ": {"quantity_step": "0.1", "min_quantity": "0.1"}}})"));
    snap().positions[0].qty = 10.25;
    snap().positions[0].available_qty = 50;
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A"})}, {"symbol", "XYZ"}});
    CHECK(Dec(parent["quantity"]) == Dec("10.2"));
    CHECK(Dec(parent["accounts"]["A"]["quantity"]) == Dec("10.2"));
}

TEST_CASE_FIXTURE(RuntimeFixture, "all selected accounts require fresh enabled inventory and no open orders") {
    AccountSnapshot& second = add_account();
    json settings = {{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"deadline", nullptr}};
    second.ts = iso(-100);
    CHECK_THROWS_WITH(runtime->start_parent(settings), "Account position snapshot is stale");
    second.ts = iso();
    second.trading_enabled = false;
    CHECK_THROWS_WITH(runtime->start_parent(settings), "Trading is disabled for this account");
    second.trading_enabled = true;
    runtime->track_external_order(json{{"command_id", "manual-b"}, {"account_id", "B"}, {"symbol", "XYZ"}, {"type", "LIMIT_ORDER"}, {"qty_shares", 10}, {"limit_price", 10}});
    CHECK_THROWS_WITH(runtime->start_parent(settings), "Existing orders for this account and instrument must reach terminal status first");
}

TEST_CASE_FIXTURE(RuntimeFixture, "account selection and aggregate inventory validation") {
    add_account();
    for (json accounts : {json::array(), json("A"), json::array({"A", "A"}), json::array({"A", "missing"}), json::array({"A", nullptr})}) {
        INFO(accounts.dump());
        CHECK_THROWS_AS(runtime->start_parent(json{{"account_ids", accounts}, {"symbol", "XYZ"}}), std::invalid_argument);
    }
    CHECK_THROWS_WITH(runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"quantity", 1401}}),
                      "Quantity exceeds the selected accounts' available position");
    for (json quantity : {json("NaN"), json("Infinity"), json("not-a-number"), json(0), json(-1)}) {
        INFO(quantity.dump());
        CHECK_THROWS_AS(runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"quantity", quantity}}),
                        std::invalid_argument);
    }
}

TEST_CASE_FIXTURE(RuntimeFixture, "inventory limits ignore short positions and cap reported available") {
    add_account("B", -10);
    snap().positions[0].qty = 20;
    snap().positions[0].available_qty = 1000;
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}});
    CHECK(Dec(parent["quantity"]) == Dec("20"));
    CHECK(Dec(parent["accounts"]["B"]["quantity"]) == Dec("0"));
    runtime->stop_all();
    snap().positions[0].qty = 0;
    CHECK_THROWS_WITH(runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}}),
                      "Quantity must be positive and finite; no available position to sell");
}

TEST_CASE_FIXTURE(RuntimeFixture, "global active session and stopping reservation block other starts") {
    add_account();
    json command = submit();
    runtime->stop_all();
    runtime->wait_idle();
    CHECK(runtime->has_active_session());
    CHECK(runtime->owns_account("A"));
    CHECK_THROWS_WITH(runtime->start_parent(json{{"account_ids", json::array({"B"})}, {"symbol", "OTHER"}}),
                      "Smart selling already owns an active session; stop it before starting another");
    order_payload(snapshot(command, {{"status", "CANCELED"}, {"sequence", 2}}));
    CHECK_FALSE(runtime->has_active_session());
    CHECK_FALSE(runtime->owns_account("A"));
    runtime->start_parent(json{{"account_ids", json::array({"B"})}, {"symbol", "XYZ"}});
}

TEST_CASE_FIXTURE(RuntimeFixture, "any fast session blocks start even on different account or symbol") {
    fast_active_flag = true;
    CHECK_THROWS_WITH(start(), "Stop the existing algorithm before starting another session");
}

TEST_CASE_FIXTURE(RuntimeFixture, "no deadline publishes and does not request completion forecast") {
    start({{"deadline", nullptr}});
    trade();
    runtime->tick();
    runtime->wait_idle();
    REQUIRE(!published.empty());
    CHECK(published.back()["deadline"].is_null());
}

TEST_CASE_FIXTURE(RuntimeFixture, "dispatch uses selected child account and rechecks its inventory") {
    AccountSnapshot& second = add_account("B", 1000);
    snap().positions[0].qty = 0;
    snap().positions[0].available_qty = 0;
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"mode", "urgent"}, {"deadline", iso(300)}});
    QuoteUpdate u;
    u.bid = 10;
    u.ask = 10.01;
    u.bid_size = 1000;
    u.source_time = epoch;
    engine->on_quote("XYZ", u);
    trade();
    // Inventory disappears between the strategy decision and the final pre-publication check.
    second.positions[0].qty = 0;
    second.positions[0].available_qty = 0;
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.empty());
    json result = engine->get_parent(parent["parent_id"].get<std::string>());
    CHECK(result["state"] == "STOPPED");
    bool child_for_b = false;
    for (auto& e : engine->events(parent["parent_id"].get<std::string>(), 200)) {
        if (e["kind"] == "submit_intent" && e["data"].value("account_id", "") == "B") child_for_b = true;
    }
    CHECK(child_for_b);
}

TEST_CASE_FIXTURE(RuntimeFixture, "round robin handoffs never oversell when account snapshots lag fills") {
    snap().positions[0].qty = 60;
    snap().positions[0].available_qty = 60;
    add_account("B", 140);
    json parent = runtime->start_parent(json{{"account_ids", json::array({"A", "B"})}, {"symbol", "XYZ"}, {"mode", "urgent"}, {"deadline", iso(60)}});
    struct Step { const char* account; int quantity; };
    std::vector<json> commands;
    int index = 0;
    for (Step step : {Step{"A", 60}, Step{"B", 40}, Step{"B", 100}}) {
        now = index * 5;
        for (auto& [id, s] : snapshots) s.ts = iso();
        QuoteUpdate u;
        u.bid = 10;
        u.ask = 10.01;
        u.bid_size = 1000;
        u.source_time = epoch + now;
        engine->on_quote("XYZ", u);
        if (index != 1) trade("t" + std::to_string(index), 1000, epoch + now);
        runtime->tick();
        runtime->wait_idle();
        REQUIRE(!published.empty());
        json command = published.back();
        commands.push_back(command);
        CHECK(command["account_id"] == step.account);
        CHECK(command["qty_shares"] == step.quantity);
        CHECK(keys.back() == step.account);
        order_payload(snapshot(command, {{"account_id", step.account}, {"order_id", "broker-" + std::to_string(index)}, {"status", "FILLED"},
                                         {"filled_quantity", step.quantity}, {"average_fill_price", 10}}));
        ++index;
    }
    json result = engine->get_parent(S(parent["parent_id"]));
    CHECK(Dec(result["filled"]) == Dec("200"));
    CHECK(Dec(result["remaining"]) == Dec("0"));
    CHECK(Dec(result["accounts"]["A"]["remaining"]) == Dec("0"));
    CHECK(Dec(result["accounts"]["B"]["remaining"]) == Dec("0"));
    CHECK_FALSE(runtime->has_active_session());
    std::set<std::string> ids;
    for (auto& c : commands) ids.insert(c["client_order_id"].get<std::string>());
    CHECK(ids.size() == 3);
}

TEST_CASE_FIXTURE(RuntimeFixture, "configured percentages drive each mode and ignore http rate overrides") {
    build_runtime(json::parse(R"({"participation_percentages": {"patient": "2.5", "normal": "4", "urgent": "12.25"}})"));
    struct Row { const char* mode; const char* percentage; };
    for (Row row : {Row{"patient", "2.5"}, Row{"normal", "4"}, Row{"urgent", "12.25"}, Row{"1", "2.5"}}) {
        INFO(row.mode);
        json parent = start({{"mode", row.mode}, {"participation_percentage", 99}, {"participation", ".99"}});
        trade(row.mode, 100);
        json current = engine->get_parent(S(parent["parent_id"]));
        CHECK(Dec(current["participation"]) == Dec(row.percentage).scaleb(-2));
        CHECK(Dec(current["earned"]) == Dec(row.percentage));
        runtime->stop_all();
    }
}

TEST_CASE_FIXTURE(RuntimeFixture, "changed configuration only affects new parent rates") {
    build_runtime(json::parse(R"({"participation_percentages": {"urgent": "12.5"}})"));
    json original = start();
    runtime->mutable_config().participation_percentages["urgent"] = "20";
    trade("original", 100);
    json current = engine->get_parent(S(original["parent_id"]));
    CHECK(Dec(current["participation"]) == Dec(".125"));
    CHECK(Dec(current["earned"]) == Dec("12.5"));
    runtime->stop_all();
    json replacement = start();
    trade("replacement", 100);
    current = engine->get_parent(S(replacement["parent_id"]));
    CHECK(Dec(current["participation"]) == Dec(".2"));
    CHECK(Dec(current["earned"]) == Dec("20"));
    CHECK(Dec(engine->get_parent(S(original["parent_id"]))["participation"]) == Dec(".125"));
}

TEST_CASE_FIXTURE(RuntimeFixture, "backend event signal defaults off and explicit true retains bid pricing") {
    int index = 0;
    for (auto [settings, expected] : {std::pair{json::object(), 10.01}, std::pair{json{{"imminent_event", true}}, 10.0}}) {
        json extra = settings;
        extra["mode"] = "patient";
        json parent = start(extra);
        trade("event-" + std::to_string(index), 10000);
        runtime->tick();
        runtime->wait_idle();
        REQUIRE(!published.empty());
        json command = published.back();
        CHECK(command["limit_price"].get<double>() == expected);
        order_payload(snapshot(command, {{"order_id", "event-" + std::to_string(index)}, {"status", "CANCELED"}}));
        runtime->stop_all();
        ++index;
    }
}

TEST_CASE_FIXTURE(RuntimeFixture, "portable existing limit then symbol cancel with reserved quantity") {
    json command = submit();
    CHECK(command["type"] == "LIMIT_ORDER");
    CHECK(command["broker"] == "AnyBroker");
    CHECK(command["side"] == "SELL");
    CHECK(command["qty_shares"] == 100);
    CHECK(command["time_in_force"] == "DAY");
    CHECK(command["command_id"] == command["client_order_id"]);
    CHECK(command["trading_mode"] == "G");
    order_payload(snapshot(command));
    now = 2;
    runtime->tick();
    runtime->wait_idle();
    json cancel = published.back();
    CHECK(cancel["type"] == "CANCEL_OPEN_ORDERS");
    CHECK(cancel["account_id"] == "A");
    CHECK(cancel["symbol"] == "XYZ");
    CHECK(engine->get_parent(parent_id)["reserved"] == "100");
    order_payload(snapshot(command, {{"status", "CANCELED"}, {"sequence", 2}, {"filled_quantity", 40}, {"average_fill_price", 10}, {"created_at", ng::iso_utc(epoch)}}));
    json parent = engine->get_parent(parent_id);
    CHECK(Dec(parent["filled"]) == Dec("40"));
    CHECK(Dec(parent["reserved"]) == Dec("0"));
}

TEST_CASE_FIXTURE(RuntimeFixture, "independent volume feed drives command and halt cancellation") {
    start();
    auto book = [&] {
        runtime->on_price_book(json{{"symbol", "XYZ"}, {"update_time", static_cast<long long>((epoch + now) * 1e9)}, {"sequence_num", static_cast<long long>(now) + 1},
                                    {"volume", 999999}, {"bid_side", {{"levels", json::array({{{"prc", 10}, {"qty", 1000}}})}}},
                                    {"offer_side", {{"levels", json::array({{{"prc", 10.01}, {"qty", 1000}}})}}}});
    };
    auto volume = [&](json total, long long sequence, bool halted = false) {
        runtime->on_volume_payload(json{{"schema_version", 1}, {"type", "TRADING_VOLUME"}, {"symbol", "XYZ"}, {"feed_id", "test-feed"},
                                        {"sequence", sequence}, {"session", "2027-01-15"}, {"total_volume", total}, {"observed_at", iso()},
                                        {"interval_start", iso(-5)}, {"healthy", !halted}, {"is_halted", halted}, {"is_delayed", false}});
    };
    book();
    volume(10000, 1);
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.empty());
    CHECK(engine->get_parent(parent_id)["earned"] == "0");
    now = 5;
    book();
    volume(11000, 2);
    runtime->tick();
    runtime->wait_idle();
    REQUIRE(!published.empty());
    json command = published.back();
    CHECK(command["type"] == "LIMIT_ORDER");
    CHECK(command["qty_shares"] == 100);
    order_payload(snapshot(command));
    now = 6;
    volume(nullptr, 3, true);
    book();
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.back()["type"] == "CANCEL_OPEN_ORDERS");
    CHECK(runtime->list_parents()[0]["market_state"] == "HALTED");
    CHECK(engine->get_parent(parent_id)["reserved"] == "100");
}

TEST_CASE_FIXTURE(RuntimeFixture, "publication timeout never refunds or sends another child") {
    publish_error = "ambiguous delivery";
    start();
    trade();
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.empty());
    CHECK(engine->get_parent(parent_id)["reserved"] == "100");
    now = 20;
    QuoteUpdate u;
    u.bid = 10;
    u.ask = 10.01;
    u.bid_size = 1000;
    u.source_time = epoch + now;
    engine->on_quote("XYZ", u);
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.empty());
    CHECK(engine->get_parent(parent_id)["reserved"] == "100");
}

TEST_CASE_FIXTURE(RuntimeFixture, "wire price precision loss aborts before publication") {
    build_runtime(json::parse(R"({"instruments": {"XYZ": {"tick_size": "0.0000000000000000001"}}})"));
    start();
    QuoteUpdate u;
    u.bid = "10.0000000000000000001";
    u.ask = "10.0000000000000000002";
    u.bid_size = 1000;
    u.source_time = epoch;
    engine->on_quote("XYZ", u);
    trade();
    runtime->tick();
    runtime->wait_idle();
    CHECK(published.empty());
    CHECK(engine->get_parent(parent_id)["reserved"] == "0");
    CHECK(engine->get_parent(parent_id)["state"] == "STOPPED");
}

TEST_CASE_FIXTURE(RuntimeFixture, "replay must finish and position must be available") {
    runtime->set_feed_states_for_test({{"orders", "replaying"}});
    CHECK_THROWS_WITH(start(), "Wait for the order-status feed to finish reconciliation before starting Smart selling");
    runtime->set_feed_states_for_test({{"orders", "live"}});
    CHECK_THROWS_WITH(start({{"quantity", 1001}}), "Quantity exceeds the selected accounts' available position");
    snap().trading_enabled = false;
    CHECK_THROWS_WITH(start(), "Trading is disabled for this account");
    snap().trading_enabled = true;
    now = 100;
    CHECK_THROWS_WITH(start(), "Account position snapshot is stale");
}

TEST_CASE_FIXTURE(RuntimeFixture, "unacknowledged manual command blocks start until confirmed terminal") {
    json external = {{"command_id", "manual-1"}, {"account_id", "A"}, {"symbol", "XYZ"}, {"type", "LIMIT_ORDER"}, {"qty_shares", 10}, {"limit_price", 10}};
    runtime->track_external_order(external);
    CHECK_THROWS_WITH(start(), "Existing orders for this account and instrument must reach terminal status first");
    order_payload(snapshot(external, {{"status", "CANCELED"}}));
    start();
}

TEST_CASE_FIXTURE(RuntimeFixture, "http cannot override execution rules or choose arbitrary parent id") {
    json parent = start({{"instrument_rules", {{"tick_size", "0.00001"}, {"quantity_step", ".001"}}}, {"parent_id", "forged"}});
    CHECK(parent["parent_id"] != "forged");
    CHECK(Dec(parent["child_cap"]) == Dec("100"));
    CHECK(engine->get_parent(S(parent["parent_id"]))["minimum_price"].is_null());
}

TEST_CASE_FIXTURE(RuntimeFixture, "one account symbol and conflicting fast strategy are rejected") {
    fast_conflict_flag = true;
    CHECK_THROWS_WITH(start(), "Stop the existing algorithm before starting another session");
    fast_conflict_flag = false;
    start();
    CHECK_THROWS_WITH(start({{"symbol", "XYZ"}}), "Smart selling already owns an active session; stop it before starting another");
}
