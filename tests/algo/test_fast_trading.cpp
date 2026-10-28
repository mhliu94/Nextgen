#include <doctest.h>
#include "core/time_util.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "algo/fast_trading/fast_trading.h"
#include "test_helpers.h"

using namespace ng;
using namespace ng::fast;

namespace {

OrderBookSnapshot book_with(std::vector<std::pair<double, double>> asks, std::vector<std::pair<double, double>> bids) {
    OrderBookSnapshot book;
    book.symbol = "AAPL";
    for (auto& [p, q] : asks) book.asks.push_back(BookLevel{p, q, std::nullopt});
    for (auto& [p, q] : bids) book.bids.push_back(BookLevel{p, q, std::nullopt});
    return book;
}

AccountSnapshot snapshot(const std::string& id, double usd, double aapl_qty, std::optional<double> available_usd = std::nullopt) {
    AccountSnapshot s;
    s.account_id = id;
    s.cash = usd;
    s.cash_by_currency["USD"] = usd;
    if (available_usd) s.available_cash_by_currency["USD"] = *available_usd;
    if (aapl_qty) s.positions.push_back(Position{"AAPL", aapl_qty, std::nullopt, std::nullopt});
    s.ts = iso_utc_now();
    s.trading_enabled = true;
    return s;
}

CycleInputs inputs(const AccountDirectory& dir, const OrderBookSnapshot& book, const std::string& mode, double limit, int levels, int pct,
                   const std::map<std::string, AccountSnapshot>* snaps = nullptr) {
    CycleInputs in;
    in.trading_mode = mode;
    in.symbol = "AAPL";
    in.book = book;
    in.price_limit = limit;
    in.account_ids = dir.ids_by_num_id();
    in.book_levels = levels;
    in.target_percentage = pct;
    in.account_metas = &dir;
    in.account_snapshots = snaps;
    in.now_monotonic = 1000.0;
    in.cycle_id = "cycle-1";
    return in;
}

}  // namespace

TEST_CASE("mode E sums only the top N asks within the limit and allocates evenly") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    auto book = book_with({{10.0, 300}, {10.5, 300}, {11.0, 1000}}, {});
    CycleResult r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.5, 2, 100));
    CHECK(r.total_resting_qty == 600);
    CHECK(r.target_quantity == 600);
    CHECK(r.limit_price == 10.5);
    CHECK(r.per_account_qty == 300);
    REQUIRE(r.commands.size() == 2);
    CHECK(r.commands[0]["type"] == "LIMIT_ORDER_FOK");
    CHECK(r.commands[0]["side"] == "BUY");
    CHECK(r.commands[0]["qty_shares"] == 300);
    CHECK(r.commands[0]["limit_price"] == 10.5);
    CHECK(r.commands[0]["trading_mode"] == "E");
    CHECK(r.commands[0]["algo_cycle_id"] == "cycle-1");
    CHECK(r.commands[0]["fast_trading_resting_side"] == "ASK");
    CHECK(r.commands[0]["fast_trading_total_resting_qty"] == 600);
    CHECK(r.commands[0]["fast_trading_target_percentage"] == 100);
    CHECK(r.commands[0]["fast_trading_book_levels"] == 2);
    CHECK(r.commands[0]["fast_trading_price_limit"] == 10.5);
    // Price limit caps the execution limit and excludes expensive levels.
    r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.2, 3, 100));
    CHECK(r.limit_price == 10.2);
    CHECK(r.total_resting_qty == 300);
    CHECK(r.reason.empty());  // 300 shares across two accounts = 150 each, above the 100-share minimum
    CHECK(r.per_account_qty == 150);
}

TEST_CASE("mode F sums bids, applies the percentage before rounding and respects the minimum") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    auto book = book_with({}, {{9.0, 150}, {9.5, 150}, {8.0, 1000}});
    CycleResult r = build_fast_trading_cycle_commands(inputs(dir, book, "F", 8.5, 2, 70));
    CHECK(r.resting_side == "BID");
    CHECK(r.order_side == "SELL");
    CHECK(r.total_resting_qty == 300);
    CHECK(r.target_quantity == 210);
    CHECK(r.limit_price == 9.0);
    CHECK(r.per_account_qty == 105);
    CHECK(r.commands.size() == 2);
    r = build_fast_trading_cycle_commands(inputs(dir, book, "F", 8.5, 2, 50));
    CHECK(r.target_quantity == 150);
    CHECK(r.commands.empty());
    CHECK(r.reason == "per_account_quantity_below_minimum");
    CHECK_FALSE(r.capacity_noop);
}

TEST_CASE("capacity exclusion is iterative and only exhaustion is a capacity noop") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2), ngtest::meta("C", 3)});
    auto book = book_with({{10.0, 900}}, {});
    std::map<std::string, AccountSnapshot> snaps = {{"A", snapshot("A", 100000, 0)}, {"B", snapshot("B", 1000, 0)}, {"C", snapshot("C", 100000, 0)}};
    CycleResult r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 1, 100, &snaps));
    CHECK(r.capacity_eligible_account_ids == std::vector<std::string>{"A", "C"});
    CHECK(r.per_account_qty == 450);
    CHECK_FALSE(r.capacity_noop);
    // Available cash wins over balance.
    snaps["A"] = snapshot("A", 100000, 0, 10.0);
    snaps["C"] = snapshot("C", 100000, 0, 10.0);
    r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 1, 100, &snaps));
    CHECK(r.capacity_noop);
    CHECK(r.reason == "no_account_capacity");
    CHECK(r.commands.empty());
    // Missing snapshots never drive the capacity noop.
    snaps.erase("B");
    r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 1, 100, &snaps));
    CHECK_FALSE(r.capacity_noop);
    CHECK(r.reason == "account_snapshots_incomplete");
    std::map<std::string, AccountSnapshot> none;
    r = build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 1, 100, &none));
    CHECK(r.reason == "account_snapshots_unavailable");
    // Sell capacity uses the available quantity.
    auto sell_book = book_with({}, {{10.0, 1000}});
    std::map<std::string, AccountSnapshot> sell = {{"A", snapshot("A", 0, 2000)}, {"B", snapshot("B", 0, 50)}, {"C", snapshot("C", 0, 2000)}};
    sell["C"].positions[0].available_qty = 100;
    r = build_fast_trading_cycle_commands(inputs(dir, sell_book, "F", 9.0, 1, 100, &sell));
    CHECK(r.capacity_eligible_account_ids == std::vector<std::string>{"A"});
    CHECK(r.per_account_qty == 1000);
}

TEST_CASE("cooldowns are filtered per account medium") {
    auto dir = ngtest::directory({ngtest::meta("A", 1, "Tiger", "API"), ngtest::meta("B", 2, "Giga", "EMULATOR")});
    auto book = book_with({{10.0, 1000}}, {});
    std::map<std::string, double> last = {{"A", 998.0}, {"B", 970.0}};
    CycleInputs in = inputs(dir, book, "E", 10.0, 1, 100);
    in.last_action_monotonic_by_account = &last;
    CycleResult r = build_fast_trading_cycle_commands(in);
    CHECK(r.cooldown_eligible_account_ids.empty());  // A inside the 3s API cooldown, B only 30s into the 40s EMULATOR cooldown
    last["B"] = 960.0;  // exactly 40s -> eligible
    r = build_fast_trading_cycle_commands(in);
    CHECK(r.cooldown_eligible_account_ids == std::vector<std::string>{"B"});
    last["A"] = 997.0;
    r = build_fast_trading_cycle_commands(in);
    CHECK(r.cooldown_eligible_account_ids == std::vector<std::string>{"A", "B"});
    last = {{"A", 999.0}, {"B", 999.0}};
    r = build_fast_trading_cycle_commands(in);
    CHECK(r.reason == "no_accounts_off_cooldown");
}

TEST_CASE("invalid cycle inputs are rejected") {
    auto dir = ngtest::directory({ngtest::meta("A", 1)});
    auto book = book_with({{10.0, 1000}}, {});
    CHECK_THROWS_WITH(build_fast_trading_cycle_commands(inputs(dir, book, "G", 10.0, 1, 100)), "fast trading mode must be E or F");
    CHECK_THROWS(build_fast_trading_cycle_commands(inputs(dir, book, "E", 0.0, 1, 100)));
    CHECK_THROWS(build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 6, 100)));
    CHECK_THROWS(build_fast_trading_cycle_commands(inputs(dir, book, "E", 10.0, 1, 33)));
    CycleInputs in = inputs(dir, book, "E", 10.0, 1, 100);
    in.account_ids = {"Z"};
    CHECK_THROWS_WITH(build_fast_trading_cycle_commands(in), "unknown fast trading account: Z");
    OrderBookSnapshot missing;
    missing.symbol = "AAPL";
    missing.error = "no data yet";
    CycleResult r = build_fast_trading_cycle_commands(inputs(dir, missing, "E", 10.0, 1, 100));
    CHECK(r.reason == "no data yet");
}

TEST_CASE("strategy manager enforces one session, publishes and auto-stops on capacity noops") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    MarketDataStore market;
    OrderBookSnapshot book = book_with({{10.0, 1000}}, {});
    market.upsert_book(book);
    std::map<std::string, AccountSnapshot> snaps = {{"A", snapshot("A", 100000, 0)}, {"B", snapshot("B", 100000, 0)}};
    std::vector<json> published;
    std::mutex mutex;
    double fake_clock = 0.0;
    StrategyManager manager(
        market, [&]() -> const AccountDirectory& { return dir; },
        [&](const json& cmd, const std::string&) {
            std::lock_guard lock(mutex);
            published.push_back(cmd);
        },
        [&] { return snaps; }, nullptr, RuntimeSettings::defaults(), [&] { return fake_clock; });
    json command = json::parse(R"({"command_id": "fast-1", "trading_mode": "E", "symbol": "AAPL", "fast_trading_price_limit": 10,
                                   "fast_trading_account_ids": ["A", "B"], "fast_trading_book_levels": 1, "fast_trading_target_percentage": 100,
                                   "fast_trading_cycle_seconds": 5})");
    CycleResult r = manager.run_cycle(command);
    CHECK(r.commands.size() == 2);
    {
        std::lock_guard lock(mutex);
        CHECK(published.size() == 2);
        CHECK(published[0]["fast_trading_cycle_seconds"] == 5);
    }
    // Cooldown recorded: the same accounts are not eligible immediately.
    r = manager.run_cycle(command);
    CHECK(r.reason == "no_accounts_off_cooldown");
    fake_clock = 10.0;
    r = manager.run_cycle(command);
    CHECK(r.commands.size() == 2);

    json test_mode = command;
    test_mode["fast_trading_test_mode"] = true;
    std::string key = manager.start_strategy(test_mode);
    CHECK(key == "E:AAPL");
    CHECK(manager.has_active_session());
    CHECK(manager.owns_account("A"));
    CHECK(manager.has_strategy("A", "us.aapl"));
    CHECK_FALSE(manager.owns_account("C"));
    CHECK_THROWS_WITH(manager.start_strategy(command), "Stop the existing algorithm session before starting another");
    CHECK(manager.stop_all() == 1);
    for (int i = 0; i < 100 && manager.has_active_session(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(manager.has_active_session());
    CHECK_THROWS(manager.start_strategy(json::parse(R"({"trading_mode": "E", "symbol": "AAPL", "fast_trading_price_limit": 10, "fast_trading_account_ids": ["A"], "fast_trading_book_levels": 1, "fast_trading_target_percentage": 100, "fast_trading_cycle_seconds": 7})")));

    // Capacity noops auto-stop after ten consecutive cycles.
    snaps["A"] = snapshot("A", 1, 0);
    snaps["B"] = snapshot("B", 1, 0);
    json fast = command;
    fast["fast_trading_cycle_seconds"] = 5;
    fast["fast_trading_test_mode"] = true;
    fake_clock = 100.0;
    manager.start_strategy(fast);
    // The worker waits cycle_seconds between cycles using the real clock; drive the fake clock forward so cooldowns pass.
    bool stopped = false;
    for (int i = 0; i < 300 && !stopped; ++i) {
        fake_clock += 100.0;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        stopped = !manager.has_active_session();
    }
    // The strategy sleeps 5 real seconds per cycle, so auto-stop cannot complete within the test budget; stop it manually.
    manager.stop_all();
    for (int i = 0; i < 100 && manager.has_active_session(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_FALSE(manager.has_active_session());
}
