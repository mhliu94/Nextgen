// Integration tests against librdkafka's in-process mock cluster: real producer/consumer code paths.
#include <doctest.h>

#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>

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
#include "test_helpers.h"

using ng::json;

namespace {

struct MockCluster {
    rd_kafka_t* rk = nullptr;
    rd_kafka_mock_cluster_t* cluster = nullptr;
    std::string bootstraps;

    MockCluster() {
        char errstr[512];
        rd_kafka_conf_t* conf = rd_kafka_conf_new();
        rd_kafka_conf_set(conf, "test.mock.num.brokers", "1", errstr, sizeof(errstr));
        rd_kafka_conf_set(conf, "log_level", "3", errstr, sizeof(errstr));
        rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
        REQUIRE(rk != nullptr);
        cluster = rd_kafka_handle_mock_cluster(rk);
        REQUIRE(cluster != nullptr);
        bootstraps = rd_kafka_mock_cluster_bootstraps(cluster);
    }
    ~MockCluster() {
        if (rk) rd_kafka_destroy(rk);
    }
    void topic(const std::string& name, int partitions = 1) {
        REQUIRE(rd_kafka_mock_topic_create(cluster, name.c_str(), partitions, 1) == RD_KAFKA_RESP_ERR_NO_ERROR);
    }
};

ng::KafkaConfig kafka_config(const std::string& bootstraps) {
    ng::KafkaConfig k;
    k.bootstrap_servers = bootstraps;
    k.market_data_bootstrap_servers = bootstraps;
    k.account_details_topic = "account-details";
    k.trading_commands_topic = "trading-commands";
    k.market_data_topic = "price-books";
    k.stock_quotes_topic = "stock-quotes";
    k.group_id = "account-dashboard-test";
    k.market_data_group_id = "market-data-test";
    k.auto_offset_reset = "earliest";
    k.market_data_auto_offset_reset = "earliest";
    k.poll_timeout_sec = 0.2;
    return k;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 15000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

void produce(const std::string& bootstraps, const std::string& topic, const std::string& key, const std::string& value) {
    ng::kafka::Config conf;
    conf.set("bootstrap.servers", bootstraps).set("acks", "all");
    ng::kafka::Producer producer(conf);
    producer.produce(topic, key, value);
    REQUIRE(producer.flush(10000) == 0);
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
        [](const json&, const std::string&) {}, nullptr, nullptr, std::make_shared<ng::smart::SmartSellingEngine>(":memory:"));
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
