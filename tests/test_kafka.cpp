// Integration tests against librdkafka's in-process mock cluster: real producer/consumer code paths.
#include <doctest.h>


#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include "core/config.h"
#include "kafka/feeds.h"
#include "kafka/kafka_client.h"
#include "kafka/producer.h"
#include "kafka/smart_selling_feeds.h"
#include "ops/order_archive.h"
#include "algo/smart_selling/runtime.h"
#include "kafka_fixture.h"
#include "test_helpers.h"

using ng::json;

using ngtest::kafka_config;
using ngtest::MockCluster;
using ngtest::produce;

namespace {
template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 15000) {
    return ngtest::kafka_wait_until(pred, timeout_ms);
}
}  // namespace

TEST_CASE("account-details and market-data feeds consume from a Kafka cluster") {
    MockCluster cluster;
    cluster.topic("account-details");
    cluster.topic("price-books");
    cluster.topic("stock-quotes");
    ng::KafkaConfig kafka = kafka_config(cluster.bootstraps);
    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("A", 1), ngtest::meta("B", 2)});
    ng::AccountStore store;
    ng::MarketDataStore market;
    std::atomic<int> smart_callbacks{0};

    produce(cluster.bootstraps, "account-details", "A", R"({"account_id": "A", "account_num_id": 1, "cash": 100, "positions": [{"symbol": "AAPL", "qty": 5}], "ts": "2026-10-23T19:59:58Z", "trading_enabled": true})");
    produce(cluster.bootstraps, "account-details", "Z", R"({"account_id": "Z", "cash": 1})");
    produce(cluster.bootstraps, "account-details", "B", R"({"account_id": "B", "account_num_id": 1, "cash": 1})");  // mismatched id
    produce(cluster.bootstraps, "price-books", "price-book", R"({"symbol": "AAPL", "update_time": 1700000000000000000, "volume": 10, "bid_side": {"levels": [{"prc": 99, "qty": 1}]}, "offer_side": {"levels": [{"prc": 101, "qty": 2}]}})");
    produce(cluster.bootstraps, "stock-quotes", "AAPL", R"({"schema_version": 1, "type": "STOCK_QUOTE", "symbol": "AAPL", "last": "125.1", "trade_time": "2026-10-23T19:59:58Z", "observed_at": "2026-10-23T20:00:00Z", "is_delayed": false, "is_halted": false, "healthy": true})");

    ng::kafka::AccountDetailsConsumer account_consumer(kafka, store, accounts);
    ng::kafka::PriceBookConsumer market_consumer(kafka, market, [&](const json&, const std::string&, int, long long) { ++smart_callbacks; }, nullptr);
    account_consumer.start();
    market_consumer.start();
    CHECK(wait_until([&] { return store.get("A").has_value(); }));
    CHECK(store.get("A")->cash == 100);
    CHECK_FALSE(store.get("B"));
    CHECK_FALSE(store.get("Z"));
    CHECK(wait_until([&] { return !market.get_book("AAPL", 5).error.has_value(); }));
    CHECK(market.get_book("AAPL", 5).bids[0].price == 99.0);
    CHECK(wait_until([&] { return market.get_stock_quotes({"AAPL"}).count("AAPL") > 0; }));
    CHECK(market.get_stock_quotes({"AAPL"}).at("AAPL").last == 125.1);
    CHECK(wait_until([&] { return smart_callbacks.load() >= 1; }));
    CHECK(wait_until([&] { return account_consumer.health().messages >= 3; }));
    CHECK(account_consumer.health().state == "live");
    CHECK(account_consumer.health().rejected == 2);
    account_consumer.stop();
    market_consumer.stop();
    CHECK(account_consumer.health().state == "stopped");
}

TEST_CASE("trading commands producer delivers and tracks history") {
    MockCluster cluster;
    cluster.topic("trading-commands");
    ng::KafkaConfig kafka = kafka_config(cluster.bootstraps);
    ng::kafka::TradingCommandsProducer producer(kafka);
    json command = {{"type", "MARKET_ORDER"}, {"command_id", "cmd-1"}, {"account_id", "A"}, {"symbol", "AAPL"}, {"side", "BUY"}, {"qty_shares", 1}};
    producer.publish_order(command, "A");
    producer.flush(5.0);
    CHECK(wait_until([&] { return producer.stats()["delivered"] == 1; }));
    auto history = producer.history();
    REQUIRE(history.size() == 1);
    CHECK(history[0].status == "delivered");
    CHECK(history[0].key == "A");

    ng::kafka::Config conf;
    conf.set("bootstrap.servers", cluster.bootstraps).set("group.id", "verify").set("auto.offset.reset", "earliest").set("enable.partition.eof", "true");
    ng::kafka::Consumer::Options options;
    ng::kafka::Consumer consumer(conf, options);
    consumer.subscribe({"trading-commands"});
    std::optional<ng::kafka::Message> received;
    wait_until([&] {
        auto m = consumer.poll(200);
        if (m && !m->eof && !m->error) received = m;
        return received.has_value();
    });
    REQUIRE(received);
    CHECK(received->key == "A");
    CHECK(json::parse(received->value)["command_id"] == "cmd-1");
    consumer.close();
}

TEST_CASE("smart selling feeds replay retained snapshots before going live") {
    MockCluster cluster;
    cluster.topic("order-status");
    cluster.topic("trading-volume");
    std::string snapshot = R"({"schema_version": 1, "account_id": "A", "order_id": "broker-1", "sequence": 1, "created_at": "2026-10-23T19:59:58Z", "updated_at": "2026-10-23T19:59:58Z", "symbol": "XYZ", "side": "SELL", "order_type": "LIMIT", "status": "NEW", "quantity": 10, "filled_quantity": 0, "average_fill_price": null, "limit_price": 10, "command_id": "manual-1"})";
    produce(cluster.bootstraps, "order-status", "[\"A\",\"broker-1\"]", snapshot);
    produce(cluster.bootstraps, "order-status", "bad", "{not json");
    produce(cluster.bootstraps, "trading-volume", "XYZ", R"({"schema_version": 1, "type": "TRADING_VOLUME", "symbol": "XYZ", "feed_id": "f", "sequence": 1, "total_volume": 100, "observed_at": "2026-10-23T19:59:58Z", "healthy": true, "is_halted": false, "is_delayed": false})");

    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("A", 1)});
    ng::SmartSellingConfig smart_cfg;
    json raw = json::object();
    smart_cfg = ng::normalize_smart_selling_config(&raw);
    ng::smart::SmartSellingRuntime runtime(
        smart_cfg, [&]() -> const ng::AccountDirectory& { return accounts; }, [] { return std::map<std::string, ng::AccountSnapshot>{}; },
        [](const json&, const std::string&) {}, nullptr, std::make_shared<ng::smart::SmartSellingEngine>(":memory:"));
    ng::AppConfig app;
    app.kafka = kafka_config(cluster.bootstraps);
    app.smart_selling = smart_cfg;
    ng::kafka::SmartSellingConsumer feeds(app, runtime);
    feeds.start();
    CHECK(wait_until([&] { return runtime.feed_states().at("orders") == "live" && runtime.feed_states().at("volume") == "live"; }, 20000));
    CHECK(runtime.feedback().has_open_orders("A", "XYZ"));
    CHECK(feeds.orders().rejected_messages() == 1);
    CHECK(feeds.orders().messages() == 2);
    CHECK(feeds.volume().messages() == 1);
    feeds.stop();
    CHECK(runtime.feed_states().at("orders") == "stopped");
    runtime.close();
}

TEST_CASE("order archive reads the retained order-status topic to its end and writes the day's files") {
    MockCluster cluster;
    cluster.topic("order-status", 2);
    auto snapshot = [](const char* account, const char* order_id, const char* created, int sequence, const char* status, int filled) {
        json p = {{"schema_version", 1}, {"account_id", account}, {"account_num_id", 1}, {"order_id", order_id}, {"sequence", sequence},
                  {"created_at", created}, {"updated_at", created}, {"symbol", "XYZ"}, {"side", "SELL"}, {"order_type", "LIMIT"},
                  {"status", status}, {"quantity", 100}, {"filled_quantity", filled}, {"average_fill_price", nullptr}, {"limit_price", 10},
                  {"command_id", std::string("cmd-") + order_id}};
        return p.dump();
    };
    produce(cluster.bootstraps, "order-status", "A", snapshot("A", "o-1", "2026-10-21T13:52:51+00:00", 0, "NEW", 0));
    produce(cluster.bootstraps, "order-status", "B", snapshot("A", "o-1", "2026-10-21T13:52:51+00:00", 1, "FILLED", 100));
    produce(cluster.bootstraps, "order-status", "C", snapshot("A", "o-2", "2026-10-22T13:00:00+00:00", 0, "FILLED", 100));
    produce(cluster.bootstraps, "order-status", "D", "{broken");

    ngtest::TempDir dir{"archive-kafka"};
    ng::AppConfig config;
    config.kafka = kafka_config(cluster.bootstraps);
    config.persistence.order_archive.dir = dir.file("reports");
    config.persistence.order_archive.read_timeout_seconds = 30;
    ng::AccountDirectory accounts = ngtest::directory({ngtest::meta("A", 1)});

    std::vector<ng::ops::RawMessage> messages = ng::ops::read_retained_topic(config.persistence.order_archive, config.kafka, 30);
    CHECK(messages.size() == 4);
    CHECK(messages[0].timestamp_ms > 0);

    json result = ng::ops::persist_day_orders(config, accounts, std::string("2026-10-21"));
    CHECK(result["messages_read"] == 4);
    CHECK(result["matching_updates"] == 2);
    CHECK(result["order_count"] == 1);
    CHECK(result["invalid"].size() == 1);
    std::filesystem::path orders_file = result["files"]["orders"].get<std::string>();
    REQUIRE(std::filesystem::exists(orders_file));
    std::ifstream in(orders_file);
    json doc = json::parse(in);
    REQUIRE(doc["orders"].size() == 1);
    CHECK(doc["orders"][0]["order_id"] == "o-1");
    CHECK(doc["orders"][0]["status"] == "FILLED");
    CHECK(doc["orders"][0]["created_at_new_york"] == "2026-10-21T09:52:51-04:00");
    CHECK(std::filesystem::exists(result["files"]["snapshots"].get<std::string>()));

    json other = ng::ops::persist_day_orders(config, accounts, std::string("2026-10-22"));
    CHECK(other["order_count"] == 1);
    CHECK(other["matching_updates"] == 1);
}

// --- bounded market-data replay (tests/test_market_data_replay.py, tests/test_market_activity.py) ---

namespace {

struct ReplayHarness {
    ng::MarketDataStore store;
    ng::KafkaConfig kafka = kafka_config("unused.invalid:9092");
    int watermark_requests = 0;
    std::vector<long long> time_requests;

    long long offset(const ng::kafka::PriceBookConsumer& consumer, const std::string& topic, int partition,
                     std::optional<std::pair<long long, long long>> watermarks, std::optional<long long> for_time = std::nullopt,
                     double now = 0) {
        return consumer.start_offset(
            topic, partition, now,
            [&](const std::string&, int) {
                ++watermark_requests;
                return watermarks;
            },
            [&](const std::string&, int, long long timestamp_ms) {
                time_requests.push_back(timestamp_ms);
                return for_time;
            });
    }
};

}  // namespace

TEST_CASE("assignment replays only recent retained records") {
    struct Case { long long low, high, expected; };
    for (Case c : {Case{0, 1000000, 999900}, Case{0, 42, 0}, Case{950, 1000, 950}, Case{0, 0, 0}, Case{500, 500, 500}}) {
        CAPTURE(c.high);
        ReplayHarness h;
        ng::kafka::PriceBookConsumer consumer(h.kafka, h.store);
        CHECK(h.offset(consumer, "price-books", 0, std::pair{c.low, c.high}) == c.expected);
        CHECK(h.watermark_requests == 1);
        CHECK(h.time_requests.empty());
    }
}

TEST_CASE("each topic and partition gets its own recent window on reassignment") {
    ReplayHarness h;
    h.kafka.market_data_replay_records = 25;
    ng::kafka::PriceBookConsumer consumer(h.kafka, h.store);
    CHECK(h.offset(consumer, "price-books", 0, std::pair{0LL, 1000LL}) == 975);
    CHECK(h.offset(consumer, "price-books", 1, std::pair{0LL, 500LL}) == 475);
    CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{800LL, 810LL}) == 800);
    CHECK(h.offset(consumer, "price-books", 0, std::pair{0LL, 2000LL}) == 1975);
    CHECK(h.offset(consumer, "price-books", 1, std::pair{0LL, 800LL}) == 775);
    CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{800LL, 1000LL}) == 975);
}

TEST_CASE("zero replay waits for live data without watermark requests") {
    ReplayHarness h;
    h.kafka.market_data_replay_records = 0;
    ng::kafka::PriceBookConsumer consumer(h.kafka, h.store);
    CHECK(h.offset(consumer, "price-books", 0, std::pair{0LL, 1000LL}) == ng::kafka::kOffsetEnd);
    CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{0LL, 1000LL}) == ng::kafka::kOffsetEnd);
    CHECK(h.watermark_requests == 0);
}

TEST_CASE("missing offsets fall back to live data without losing other partitions") {
    ReplayHarness h;
    ng::kafka::PriceBookConsumer consumer(h.kafka, h.store);
    CHECK(h.offset(consumer, "price-books", 0, std::nullopt) == ng::kafka::kOffsetEnd);
    CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{0LL, 500LL}) == 400);
}

TEST_CASE("stock quote replay uses a time window without changing book replay") {
    const double start = 1794237600.0;
    struct Case { std::optional<long long> found; long long expected; };
    for (Case c : {Case{123, 123}, Case{ng::kafka::kOffsetEnd, ng::kafka::kOffsetEnd}}) {
        ReplayHarness h;
        ng::kafka::PriceBookConsumer consumer(h.kafka, h.store, nullptr, nullptr, nullptr, 1500);
        CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{0LL, 1000LL}, c.found, start) == c.expected);
        CHECK(h.offset(consumer, "price-books", 0, std::pair{0LL, 1000LL}, c.found, start) == 900);
        REQUIRE(h.time_requests.size() == 1);  // only the quote partition is looked up by time
        CHECK(h.time_requests[0] == static_cast<long long>((start - 1500) * 1000));
        CHECK(h.watermark_requests == 1);
    }
    // A failed lookup falls back to the bounded record replay.
    ReplayHarness h;
    ng::kafka::PriceBookConsumer consumer(h.kafka, h.store, nullptr, nullptr, nullptr, 1500);
    CHECK(h.offset(consumer, "stock-quotes", 0, std::pair{0LL, 1000LL}, std::nullopt, start) == 900);
}

TEST_CASE("polling skips old history then consumes new books with their original offsets") {
    MockCluster cluster;
    cluster.topic("price-books");
    cluster.topic("stock-quotes");
    ng::KafkaConfig kafka = kafka_config(cluster.bootstraps);
    kafka.market_data_auto_offset_reset = "latest";
    auto book = [](long long sequence) {
        return json{{"symbol", "AAPL"}, {"sequence_num", sequence},
                    {"bid_side", {{"levels", json::array({{{"price", sequence}, {"quantity", 10}}})}}},
                    {"offer_side", {{"levels", json::array({{{"price", sequence + 1}, {"quantity", 10}}})}}}}
            .dump();
    };
    {
        ng::kafka::Config conf;
        conf.set("bootstrap.servers", cluster.bootstraps).set("acks", "all");
        ng::kafka::Producer producer(conf);
        for (long long offset = 0; offset < 250; ++offset) producer.produce("price-books", "price-book", book(offset));
        REQUIRE(producer.flush(15000) == 0);
    }
    ng::MarketDataStore store;
    std::mutex mutex;
    std::vector<long long> offsets;
    std::atomic<bool> healthy{false};
    ng::kafka::PriceBookConsumer consumer(
        kafka, store,
        [&](const json&, const std::string& topic, int partition, long long offset) {
            std::lock_guard lock(mutex);
            CHECK(topic == "price-books");
            CHECK(partition == 0);
            offsets.push_back(offset);
        },
        [&](bool value) { healthy = value; });
    consumer.start();
    auto count = [&] {
        std::lock_guard lock(mutex);
        return offsets.size();
    };
    REQUIRE(wait_until([&] { return count() == 100; }));
    produce(cluster.bootstraps, "price-books", "price-book", book(250));
    REQUIRE(wait_until([&] { return count() == 101; }));
    {
        std::lock_guard lock(mutex);
        std::vector<long long> expected;
        for (long long offset = 150; offset <= 250; ++offset) expected.push_back(offset);
        CHECK(offsets == expected);
    }
    CHECK(store.get_book("AAPL", 5).bids[0].price == 250.0);
    CHECK(healthy.load());
    consumer.stop();
}
