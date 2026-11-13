#include <doctest.h>
#include "core/time_util.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <cmath>
#include <limits>

#include "algo/fast_trading/fast_trading.h"
#include "orders/commands.h"
#include "reporting_fakes.h"
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

TEST_CASE("E and F sub-dollar ticks respect fractional user limits before allocation") {
    auto dir=ngtest::directory({ngtest::meta("A",1)});
    auto book=book_with({{0.0910,500},{0.0912,500}},{{0.0913,500},{0.0911,500}});
    auto buy=build_fast_trading_cycle_commands(inputs(dir,book,"E",0.09115,2,100));
    REQUIRE(buy.commands.size()==1);CHECK(buy.limit_price==0.0911);CHECK(buy.total_resting_qty==500);
    auto sell=build_fast_trading_cycle_commands(inputs(dir,book,"F",0.09115,2,100));
    REQUIRE(sell.commands.size()==1);CHECK(sell.limit_price==0.0912);CHECK(sell.total_resting_qty==500);
}

TEST_CASE("E sizes buying capacity from its actual tick-rounded execution price") {
    auto dir=ngtest::directory({ngtest::meta("A",1)});
    auto book=book_with({{0.09115,1000}},{});
    std::map<std::string,AccountSnapshot> snapshots{{"A",snapshot("A",9.114,0)}};
    auto result=build_fast_trading_cycle_commands(inputs(dir,book,"E",0.10,1,100,&snapshots));
    CHECK(result.limit_price==0.0912);CHECK(result.commands.empty());CHECK(result.capacity_noop);
}

TEST_CASE("E and F honor configured upper-band ticks and the dollar threshold") {
    auto dir=ngtest::directory({ngtest::meta("A",1)});
    auto book=book_with({{1.023,1000}},{{1.023,1000}});
    auto buy=inputs(dir,book,"E",1.03,1,100);buy.tick_size="0.005";
    CHECK(build_fast_trading_cycle_commands(buy).limit_price==1.025);
    auto sell=inputs(dir,book,"F",1.02,1,100);sell.tick_size="0.005";
    CHECK(build_fast_trading_cycle_commands(sell).limit_price==1.02);
    buy.book=book_with({{0.9999,1000}},{});buy.price_limit=0.99995;
    CHECK(build_fast_trading_cycle_commands(buy).limit_price==0.9999);
}

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

TEST_CASE("strategy manager publishes cycles, reserves its accounts and stops") {
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
    CHECK(manager.has_running_strategy("A", "us.aapl"));
    CHECK_FALSE(manager.owns_account("C"));
    CHECK_THROWS_WITH(manager.start_strategy(command), "An algorithm session with this command id is already active");
    json retry = command;
    retry["command_id"] = "fast-2";
    CHECK_THROWS_WITH(manager.start_strategy(retry),
                      doctest::Contains("must use distinct symbols"));
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

namespace {

// tests/test_fast_trading.py helpers: positions are {symbol, qty[, available]} triples.
struct Holding {
    std::string symbol;
    double qty;
    std::optional<double> available;
};

AccountSnapshot holdings_snapshot(const std::string& id, double cash, std::vector<Holding> holdings,
                                  std::optional<double> available_usd = std::nullopt) {
    AccountSnapshot s;
    s.account_id = id;
    s.cash = cash;
    if (available_usd) s.available_cash_by_currency["USD"] = *available_usd;
    for (auto& h : holdings) s.positions.push_back(Position{h.symbol, h.qty, std::nullopt, h.available});
    return s;
}

CycleInputs target_inputs(const AccountDirectory& dir, const OrderBookSnapshot& book, const std::string& mode,
                          std::vector<std::string> account_ids, const std::map<std::string, AccountSnapshot>* snaps,
                          std::optional<long long> target, int levels = 1, int pct = 100) {
    static const std::map<std::string, double> minimums = {{"API", 3.0}, {"WEB", 30.0}, {"WINDOWS", 40.0}, {"EMULATOR", 40.0}};
    CycleInputs in;
    in.trading_mode = mode;
    in.symbol = "AAPL";
    in.book = book;
    in.price_limit = 10;
    in.account_ids = std::move(account_ids);
    in.book_levels = levels;
    in.target_percentage = pct;
    in.account_metas = &dir;
    in.account_snapshots = snaps;
    in.minimum_cycle_seconds_by_medium = &minimums;
    in.now_monotonic = 100.0;
    in.cycle_id = "test-cycle";
    in.total_target_position = target;
    return in;
}

json strategy_command(int book_levels = 2, int target_percentage = 100, int cycle_seconds = 5) {
    return json{{"command_id", "fast-session-test"}, {"trading_mode", "E"}, {"symbol", "AAPL"}, {"fast_trading_price_limit", 10},
                {"fast_trading_account_ids", json::array({"A"})}, {"fast_trading_book_levels", book_levels},
                {"fast_trading_target_percentage", target_percentage}, {"fast_trading_cycle_seconds", cycle_seconds}};
}

json with(json base, std::initializer_list<std::pair<const char*, json>> overrides) {
    for (const auto& [key, value] : overrides) base[key] = value;
    return base;
}

bool wait_until(const std::function<bool()>& done, int millis = 5000) {
    for (int i = 0; i < millis / 5 && !done(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return done();
}

// The Python SimulatedCycleClock: waits advance a fake monotonic clock and stop after max_cycles.
struct SimulatedCycleClock {
    std::mutex mutex;
    double now = 0.0;
    std::vector<double> waits;
    std::size_t max_cycles = 25;

    double monotonic() {
        std::lock_guard lock(mutex);
        return now;
    }
    bool wait(double seconds) {
        std::lock_guard lock(mutex);
        waits.push_back(seconds);
        now += seconds;
        return waits.size() >= max_cycles;
    }
    std::vector<double> recorded() {
        std::lock_guard lock(mutex);
        return waits;
    }
};

}  // namespace

TEST_CASE("position target uses all participants actual holdings and mode boundaries") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    OrderBookSnapshot book = book_with({{10.0, 200}}, {{10.0, 200}});
    std::map<std::string, double> last = {{"B", 100.0}};
    for (const char* mode : {"E", "F"}) {
        for (long long target : {599, 600, 601}) {
            INFO(std::string(mode) << " target=" << target);
            std::map<std::string, AccountSnapshot> snaps = {
                {"A", holdings_snapshot("A", 10000, {{"US.AAPL", 400.25, 200.0}, {"MSFT", 999999, std::nullopt}})},
                {"B", holdings_snapshot("B", 0, {{" aapl/USD ", 199.75, 0.0}})},
                {"OUTSIDE", holdings_snapshot("OUTSIDE", 999999, {{"AAPL", 999999, std::nullopt}})},
            };
            CycleInputs in = target_inputs(dir, book, mode, {"A", "B"}, &snaps, target);
            in.last_action_monotonic_by_account = &last;
            CycleResult result = build_fast_trading_cycle_commands(in);
            bool should_stop = std::string(mode) == "E" ? 600 >= target : 600 < target;
            REQUIRE(result.total_position);
            CHECK(*result.total_position == 600);
            CHECK(result.target_position_reached == should_stop);
            CHECK_FALSE(result.capacity_noop);
            if (should_stop) {
                CHECK(result.commands.empty());
                CHECK(result.reason == kStopReasonTargetPosition);
            } else {
                REQUIRE(result.commands.size() == 1);
                CHECK(result.commands[0]["account_id"] == "A");
                CHECK(result.commands[0]["fast_trading_total_target_position"] == target);
            }
        }
    }
}

TEST_CASE("target uses signed holdings without rounding and deduplicates accounts") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    OrderBookSnapshot empty;
    empty.symbol = "AAPL";
    empty.error = "no data yet";
    struct Case { const char* mode; long long target; };
    for (Case c : {Case{"E", 300}, Case{"F", 301}}) {
        INFO(std::string(c.mode));
        std::map<std::string, AccountSnapshot> snaps = {
            {"A", holdings_snapshot("A", 0, {{"AAPL", 200.25, std::nullopt}, {"AAPL", 200.25, std::nullopt}})},
            {"B", holdings_snapshot("B", 0, {{"AAPL", -100, std::nullopt}})},
        };
        CycleResult result = build_fast_trading_cycle_commands(target_inputs(dir, empty, c.mode, {"A", "A", "B"}, &snaps, c.target));
        REQUIRE(result.total_position);
        CHECK(*result.total_position == 300.5);
        CHECK(result.target_position_reached);
        CHECK(result.commands.empty());
        CHECK(result.reason == kStopReasonTargetPosition);
    }
}

TEST_CASE("position target waits for complete valid participant snapshots") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    OrderBookSnapshot book = book_with({{10.0, 200}}, {{10.0, 200}});
    AccountSnapshot a = holdings_snapshot("A", 10000, {{"AAPL", 600, std::nullopt}});
    std::vector<std::optional<std::map<std::string, AccountSnapshot>>> cases;
    cases.push_back(std::nullopt);  // no snapshot source at all
    cases.push_back(std::map<std::string, AccountSnapshot>{});
    cases.push_back(std::map<std::string, AccountSnapshot>{{"A", a}});
    for (double qty : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        cases.push_back(std::map<std::string, AccountSnapshot>{{"A", a}, {"B", holdings_snapshot("B", 0, {{"AAPL", qty, std::nullopt}})}});
    }
    for (const char* mode : {"E", "F"}) {
        for (auto& data : cases) {
            INFO(std::string(mode));
            CycleResult result = build_fast_trading_cycle_commands(target_inputs(dir, book, mode, {"A", "B"}, data ? &*data : nullptr, 600));
            CHECK(result.commands.empty());
            CHECK_FALSE(result.target_position_reached);
            CHECK_FALSE(result.capacity_noop);
            CHECK(result.reason == "total_position_unavailable");
        }
    }
}

TEST_CASE("zero holdings are known and a zero target is enabled") {
    auto dir = ngtest::directory({ngtest::meta("A", 1)});
    OrderBookSnapshot empty;
    empty.symbol = "AAPL";
    struct Case { const char* mode; long long target; bool should_stop; };
    for (Case c : {Case{"E", 0, true}, Case{"F", 1, true}, Case{"F", 0, false}}) {
        INFO(std::string(c.mode) << " target=" << c.target);
        std::map<std::string, AccountSnapshot> snaps = {{"A", holdings_snapshot("A", 0, {})}};
        CycleResult result = build_fast_trading_cycle_commands(target_inputs(dir, empty, c.mode, {"A"}, &snaps, c.target));
        REQUIRE(result.total_position);
        CHECK(*result.total_position == 0);
        CHECK(result.target_position_reached == c.should_stop);
    }
    CycleInputs bad = target_inputs(dir, empty, "E", {"A"}, nullptr, -1);
    CHECK_THROWS_WITH(build_fast_trading_cycle_commands(bad), ng::orders::kFastTotalTargetPositionError);
    bad.total_target_position = 9007199254740992LL;
    CHECK_THROWS_WITH(build_fast_trading_cycle_commands(bad), ng::orders::kFastTotalTargetPositionError);
}

TEST_CASE("oversized targets preserve depth, price limits, rounding and allocation") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    struct Mode { const char* mode; std::vector<double> prices; };
    struct Pct { int percentage; long long target_qty; long long per_account_qty; };
    for (const Mode& m : {Mode{"E", {8, 9, 11, 12}}, Mode{"F", {12, 11, 9, 8}}}) {
        for (Pct pct : {Pct{120, 1211, 605}, Pct{150, 1514, 757}}) {
            INFO(std::string(m.mode) << " pct=" << pct.percentage);
            std::vector<std::pair<double, double>> levels;
            const double quantities[] = {501.4, 508.5, 1000, 2000};
            for (std::size_t i = 0; i < 4; ++i) levels.push_back({m.prices[i], quantities[i]});
            bool buy = std::string(m.mode) == "E";
            OrderBookSnapshot book = buy ? book_with(levels, {}) : book_with({}, levels);
            std::map<std::string, AccountSnapshot> snaps = {
                {"A", holdings_snapshot("A", 20000, {{"AAPL", 2000, std::nullopt}})},
                {"B", holdings_snapshot("B", 20000, {{"AAPL", 2000, std::nullopt}})},
            };
            CycleResult result =
                build_fast_trading_cycle_commands(target_inputs(dir, book, m.mode, {"A", "B"}, &snaps, std::nullopt, 3, pct.percentage));
            CHECK(result.total_resting_qty == 1009);
            CHECK(result.target_quantity == pct.target_qty);
            CHECK(result.limit_price == 10.0);
            CHECK(result.allocated_quantity == pct.per_account_qty * 2);
            REQUIRE(result.commands.size() == 2);
            for (const auto& command : result.commands) {
                CHECK(command["qty_shares"] == pct.per_account_qty);
                CHECK(command["side"] == (buy ? "BUY" : "SELL"));
                CHECK(command["fast_trading_target_percentage"] == pct.percentage);
                CHECK(command["fast_trading_target_qty"] == pct.target_qty);
                CHECK(command["type"] == "LIMIT_ORDER_FOK");
                CHECK(command["limit_price"] == 10);
            }
        }
    }
}

TEST_CASE("oversized targets require capacity for the full scaled quantity") {
    auto dir = ngtest::directory({ngtest::meta("A", 1)});
    OrderBookSnapshot book = book_with({{10.0, 1000}}, {{10.0, 1000}});
    for (const char* mode : {"E", "F"}) {
        for (int percentage : {120, 150}) {
            std::map<std::string, AccountSnapshot> snaps = {
                {"A", holdings_snapshot("A", 20000, {{"AAPL", 2000, 1000.0}}, 10000.0)},
            };
            CycleResult result =
                build_fast_trading_cycle_commands(target_inputs(dir, book, mode, {"A"}, &snaps, std::nullopt, 1, percentage));
            INFO(std::string(mode) << " pct=" << percentage << " result=" << result.to_json().dump());
            CHECK(result.target_quantity == percentage * 10);
            CHECK(result.commands.empty());
            CHECK(result.reason == "no_account_capacity");
            CHECK(result.capacity_noop);
        }
    }
}

TEST_CASE("strategy auto-stops using fresh participant positions and reports the reason") {
    for (const char* mode : {"E", "F"}) {
        for (bool initially_met : {false, true}) {
            INFO(std::string(mode) << " initially_met=" << initially_met);
            bool buy = std::string(mode) == "E";
            double initial_qty = buy ? (initially_met ? 600 : 400) : (initially_met ? 400 : 600);
            auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2), ngtest::meta("OUTSIDE", 3)});
            std::mutex mutex;
            std::map<std::string, AccountSnapshot> accounts = {
                {"A", holdings_snapshot("A", 10000, {{"AAPL", initial_qty / 2, std::nullopt}})},
                {"B", holdings_snapshot("B", 10000, {{"AAPL", initial_qty / 2, std::nullopt}})},
                {"OUTSIDE", holdings_snapshot("OUTSIDE", 0, {{"AAPL", 999999, std::nullopt}})},
            };
            std::vector<json> published;
            MarketDataStore store;
            store.upsert_book(book_with({{10.0, 200}}, {{10.0, 200}}));
            SimulatedCycleClock clock;
            ngtest::RecordingReporter reporting;
            ngtest::TempDir dir_ops("fast-ops");
            OperationsLog operations_log(dir_ops.path.string(), 1800000000.0, [&] {
                std::lock_guard lock(mutex);
                return accounts;
            });
            StrategyManager manager(
                store, [&]() -> const AccountDirectory& { return dir; },
                [&](const json& command, const std::string& account_id) {
                    std::lock_guard lock(mutex);
                    published.push_back(command);
                    accounts.at(account_id).positions[0].qty += command.at("qty_shares").get<double>() * (buy ? 1 : -1);
                },
                [&] {
                    std::lock_guard lock(mutex);
                    return accounts;
                },
                &operations_log, RuntimeSettings::defaults(), [&] { return clock.monotonic(); }, &reporting);
            manager.set_cycle_waiter_for_test([&](double seconds) { return clock.wait(seconds); });
            json command = with(strategy_command(), {{"trading_mode", mode},
                                                    {"fast_trading_account_ids", json::array({"A", "B"})},
                                                    {"fast_trading_total_target_position", 600}});
            manager.start_strategy(command);
            REQUIRE(wait_until([&] { return !manager.has_active_session(); }));
            REQUIRE(wait_until([&] { return !reporting.calls("end_session").empty(); }));

            {
                std::lock_guard lock(mutex);
                CHECK(published.size() == (initially_met ? 0u : 2u));
            }
            CHECK(clock.recorded() == (initially_met ? std::vector<double>{} : std::vector<double>{5.0}));
            auto ended = reporting.calls("end_session");
            REQUIRE(ended.size() == 1);
            CHECK(ended[0]["session_id"] == command["command_id"]);
            CHECK(ended[0]["status"] == "stopped");
            CHECK(ended[0]["reason"] == kStopReasonTargetPosition);
            json document = operations_log.read_document();
            const json& last_event = document.at("events").back();
            CHECK(last_event.at("event") == "algo_trading_ended");
            CHECK(last_event.at("details").at("reason") == kStopReasonTargetPosition);
            CHECK(last_event.at("details").at("cycles") == (initially_met ? 1 : 2));
            CHECK(last_event.at("details").at("published_commands") == (initially_met ? 0 : 2));
        }
    }
}

TEST_CASE("missing target position data neither publishes nor auto-stops") {
    for (const char* mode : {"E", "F"}) {
        INFO(std::string(mode));
        auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
        MarketDataStore store;
        store.upsert_book(book_with({{10.0, 200}}, {{10.0, 200}}));
        SimulatedCycleClock clock;
        clock.max_cycles = 12;
        std::atomic<int> publishes{0};
        ngtest::TempDir dir_ops("fast-ops");
        OperationsLog operations_log(dir_ops.path.string(), 1800000000.0, [] { return std::map<std::string, AccountSnapshot>{}; });
        StrategyManager manager(
            store, [&]() -> const AccountDirectory& { return dir; }, [&](const json&, const std::string&) { ++publishes; },
            [] { return std::map<std::string, AccountSnapshot>{{"A", holdings_snapshot("A", 10000, {})}}; }, &operations_log,
            RuntimeSettings::defaults(), [&] { return clock.monotonic(); });
        manager.set_cycle_waiter_for_test([&](double seconds) { return clock.wait(seconds); });
        manager.start_strategy(with(strategy_command(), {{"trading_mode", mode},
                                                         {"fast_trading_account_ids", json::array({"A", "B"})},
                                                         {"fast_trading_total_target_position", 600}}));
        REQUIRE(wait_until([&] { return !manager.has_active_session(); }));
        REQUIRE(wait_until([&] {
            json events = operations_log.read_document().at("events");
            return !events.empty() && events.back().at("event") == "algo_trading_ended";
        }));
        const json last_event = operations_log.read_document().at("events").back();
        CHECK(last_event.at("details").at("cycles") == 12);
        CHECK(last_event.at("details").at("reason") == "total_position_unavailable");
        CHECK(publishes.load() == 0);
    }
}

TEST_CASE("stop all stops both modes, all symbols and accounts outside the current configuration") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2), ngtest::meta("REMOVED", 3)});
    MarketDataStore store;
    std::atomic<int> publishes{0};
    StrategyManager manager(store, [&]() -> const AccountDirectory& { return dir; }, [&](const json&, const std::string&) { ++publishes; });
    struct Session { const char* mode; const char* symbol; const char* account; };
    for (Session s : {Session{"E", "AAPL", "A"}, Session{"F", "AAPL", "B"}, Session{"E", "MSFT", "REMOVED"}}) {
        manager.start_strategy(with(strategy_command(), {{"command_id", std::string(s.mode) + "-" + s.symbol},
                                                         {"trading_mode", s.mode},
                                                         {"symbol", s.symbol},
                                                         {"fast_trading_account_ids", json::array({s.account})}}));
    }
    CHECK(manager.statuses().size() == 3);
    CHECK(manager.stop_all() == 3);
    CHECK(manager.statuses().empty());
    CHECK(wait_until([&] { return !manager.has_active_session(); }));
    CHECK(manager.stop_all() == 0);
    CHECK(publishes.load() == 0);
}

TEST_CASE("sessions share accounts only on distinct symbols with one buyer") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2), ngtest::meta("C", 3)});
    MarketDataStore store;
    StrategyManager manager(store, [&]() -> const AccountDirectory& { return dir; }, [](const json&, const std::string&) {});
    manager.start_strategy(with(strategy_command(), {{"fast_trading_account_ids", json::array({"A", "B"})}}));
    struct Attempt { const char* mode; const char* symbol; json accounts; doctest::Contains message; };
    for (const Attempt& attempt :
         {Attempt{"F", "AAPL", json::array({"B"}),
                  doctest::Contains("must use distinct symbols")},
          Attempt{"E", "AAPL", json::array({"C", "A"}),
                  doctest::Contains("must use distinct symbols")}}) {
        INFO(std::string(attempt.mode) << " " << attempt.symbol);
        CHECK_THROWS_WITH_AS(manager.start_strategy(with(strategy_command(), {{"command_id", "second"},
                                                                              {"trading_mode", attempt.mode},
                                                                              {"symbol", attempt.symbol},
                                                                              {"fast_trading_account_ids", attempt.accounts}})),
                             attempt.message, std::invalid_argument);
    }
    CHECK_FALSE(manager.owns_account("C"));
    // Free accounts may start in any mode and symbol, including the same mode and symbol.
    manager.start_strategy(with(strategy_command(), {{"command_id", "second"}, {"fast_trading_account_ids", json::array({"C"})}}));
    CHECK(manager.statuses().size() == 2);
    for (const char* account : {"A", "B", "C"}) CHECK(manager.owns_account(account));
    CHECK(manager.stop_strategy("E", "AAPL") == 2);
    REQUIRE(wait_until([&] { return !manager.has_active_session(); }));
    manager.start_strategy(with(strategy_command(), {{"trading_mode", "F"}, {"symbol", "MSFT"},
                                                     {"fast_trading_account_ids", json::array({"A", "C"})}}));
    CHECK(manager.stop_all() == 1);
}

TEST_CASE("duplicate start command id is rejected while its session is active") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    MarketDataStore store;
    StrategyManager manager(store, [&]() -> const AccountDirectory& { return dir; }, [](const json&, const std::string&) {});
    manager.start_strategy(strategy_command());
    CHECK_THROWS_WITH_AS(manager.start_strategy(with(strategy_command(), {{"fast_trading_account_ids", json::array({"B"})}})),
                         "An algorithm session with this command id is already active", std::invalid_argument);
    CHECK_FALSE(manager.owns_account("B"));
    manager.stop_all();
    REQUIRE(wait_until([&] { return !manager.has_active_session(); }));
    manager.start_strategy(with(strategy_command(), {{"fast_trading_account_ids", json::array({"B"})}}));
    CHECK(manager.owns_account("B"));
}

TEST_CASE("smart sessions reserve their account and symbol against fast starts") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    MarketDataStore store;
    std::mutex mutex;
    std::set<std::string> smart_owned = {"B"};
    auto coordinator = std::make_shared<ng::algo::SessionCoordinator>();
    coordinator->register_provider(&smart_owned, [&] {
        std::lock_guard lock(mutex);
        return std::vector<ng::algo::Session>{{"smart", "G", "AAPL", {smart_owned.begin(), smart_owned.end()}}};
    });
    StrategyManager manager(
        store, [&]() -> const AccountDirectory& { return dir; }, [](const json&, const std::string&) {}, nullptr, nullptr,
        RuntimeSettings::defaults(), nullptr, nullptr, coordinator);
    json both = with(strategy_command(), {{"fast_trading_account_ids", json::array({"A", "B"})}});
    CHECK_THROWS_WITH_AS(manager.start_strategy(both),
                         doctest::Contains("must use distinct symbols"),
                         std::invalid_argument);
    CHECK_FALSE(manager.has_active_session());
    {
        std::lock_guard lock(mutex);
        smart_owned.clear();
    }
    manager.start_strategy(both);
    CHECK(manager.has_active_session());
}

TEST_CASE("stopping worker reserves only its accounts until the in-flight cycle exits") {
    auto dir = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    MarketDataStore store;
    store.upsert_book(book_with({{10.0, 1000}}, {}));
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, release = false;
    StrategyManager manager(
        store, [&]() -> const AccountDirectory& { return dir; },
        [&](const json&, const std::string&) {
            // Block inside the publish handoff of the first cycle.
            std::unique_lock lock(mutex);
            entered = true;
            cv.notify_all();
            cv.wait_for(lock, std::chrono::seconds(10), [&] { return release; });
        });
    manager.start_strategy(with(strategy_command(1), {}));
    {
        std::unique_lock lock(mutex);
        REQUIRE(cv.wait_for(lock, std::chrono::seconds(2), [&] { return entered; }));
    }
    CHECK(manager.stop_all() == 1);
    CHECK(manager.has_active_session());
    CHECK(manager.owns_account("A"));
    CHECK_FALSE(manager.has_running_strategy("A", "AAPL"));
    CHECK_THROWS_WITH_AS(manager.start_strategy(with(strategy_command(1), {{"command_id", "retry"}})),
                         doctest::Contains("must use distinct symbols"),
                         std::invalid_argument);
    // The stopping session reserves only its own accounts.
    manager.start_strategy(with(strategy_command(1), {{"command_id", "other"}, {"fast_trading_account_ids", json::array({"B"})}}));
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    cv.notify_all();
    CHECK(manager.stop_all() == 1);
    CHECK(wait_until([&] { return !manager.has_active_session(); }));
    CHECK_FALSE(manager.owns_account("A"));
}

TEST_CASE("every percentage choice sizes one cycle in both modes") {
    for (const char* mode : {"E", "F"}) {
        for (int percentage : {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 120, 150}) {
            INFO(std::string(mode) << " pct=" << percentage);
            auto dir = ngtest::directory({ngtest::meta("A", 1)});
            MarketDataStore store;
            store.upsert_book(book_with({{10.0, 1000}}, {{10.0, 1000}}));
            std::vector<std::pair<json, std::string>> published;
            StrategyManager manager(
                store, [&]() -> const AccountDirectory& { return dir; },
                [&](const json& command, const std::string& key) { published.push_back({command, key}); },
                [] { return std::map<std::string, AccountSnapshot>{{"A", holdings_snapshot("A", 20000, {{"AAPL", 2000, std::nullopt}})}}; });
            CycleResult result = manager.run_cycle(with(strategy_command(2, percentage), {{"trading_mode", mode}}));
            REQUIRE(result.commands.size() == 1);
            CHECK(result.commands[0]["qty_shares"] == percentage * 10);
            CHECK(result.commands[0]["fast_trading_target_percentage"] == percentage);
            CHECK(result.commands[0]["side"] == (std::string(mode) == "E" ? "BUY" : "SELL"));
            REQUIRE(published.size() == 1);
            CHECK(published[0].first == result.commands[0]);
            CHECK(published[0].second == "A");
        }
    }
}

TEST_CASE("manager rejects missing or invalid controls before starting or publishing") {
    auto dir = ngtest::directory({ngtest::meta("A", 1)});
    MarketDataStore store;
    store.upsert_book(book_with({{10.0, 1000}}, {}));
    std::atomic<int> publishes{0};
    StrategyManager manager(store, [&]() -> const AccountDirectory& { return dir; }, [&](const json&, const std::string&) { ++publishes; });
    const double two_pow_53 = 9007199254740992.0;
    std::map<std::string, std::vector<json>> invalid_values = {
        {"fast_trading_book_levels", {nullptr, "", 0, 6, 1.5, true}},
        {"fast_trading_target_percentage", {nullptr, "", 0, 15, 110, 130, 140, 151, 120.5, 150.5, true}},
        {"fast_trading_cycle_seconds", {nullptr, "", 0, 1, 20, 120, true}},
        {"fast_trading_total_target_position",
         {-1, 1.5, true, false, "bad", "NaN", "Infinity", two_pow_53, json::array(), json::object()}},
    };
    for (const auto& [field, values] : invalid_values) {
        for (const json& value : values) {
            INFO(field << "=" << value.dump());
            json command = strategy_command();
            command[field] = value;
            CHECK_THROWS_AS(manager.run_cycle(command), std::invalid_argument);
            CHECK_THROWS_AS(manager.start_strategy(command), std::invalid_argument);
            if (field != "fast_trading_total_target_position") {
                command.erase(field);
                CHECK_THROWS_AS(manager.run_cycle(command), std::invalid_argument);
                CHECK_THROWS_AS(manager.start_strategy(command), std::invalid_argument);
            }
        }
    }
    CHECK_FALSE(manager.has_active_session());
    CHECK(publishes.load() == 0);
}
