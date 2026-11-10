// librdkafka's in-process mock cluster and small helpers shared by the Kafka integration tests.
#pragma once

#include <doctest.h>

#include <librdkafka/rdkafka.h>
#include <librdkafka/rdkafka_mock.h>

#include <chrono>
#include <string>
#include <thread>

#include "core/config.h"
#include "kafka/kafka_client.h"

namespace ngtest {

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

inline ng::KafkaConfig kafka_config(const std::string& bootstraps) {
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
bool kafka_wait_until(Pred pred, int timeout_ms = 15000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return pred();
}

inline void produce(const std::string& bootstraps, const std::string& topic, const std::string& key, const std::string& value) {
    ng::kafka::Config conf;
    conf.set("bootstrap.servers", bootstraps).set("acks", "all");
    ng::kafka::Producer producer(conf);
    producer.produce(topic, key, value);
    REQUIRE(producer.flush(10000) == 0);
}

}  // namespace ngtest
