// A dedicated order-status consumer per server process (order_ui/consumer.py). Every start or
// partition reassignment replays the retained topic from the beginning and rebuilds the store's
// projection; offsets are never committed and every process uses its own consumer group.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "kafka/kafka_client.h"
#include "order_ui/config.h"
#include "order_ui/store.h"

namespace ng::order_ui {

class OrderStatusConsumer {
public:
    // `client_id` distinguishes this consumer in broker logs.
    OrderStatusConsumer(OrderKafkaConfig config, OrderStore& store, std::string client_id = "ktrader-order-ui");
    ~OrderStatusConsumer();

    // Creates the Kafka client first, so invalid client settings fail startup visibly.
    void start();
    void stop();
    const std::string& group_id() const { return group_id_; }

    // Applies one raw message value to the store (exposed for tests): validation failures are
    // counted as invalid messages and never stop consumption.
    void handle_value(const std::string& raw, const std::string& topic, int partition, long long offset);

private:
    std::unique_ptr<kafka::Consumer> create_consumer();
    void run();
    void handle_message(const kafka::Message& message);
    void connected();
    void on_error(int code);

    OrderKafkaConfig config_;
    OrderStore& store_;
    std::string client_id_;
    std::string group_id_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::unique_ptr<kafka::Consumer> consumer_;
    std::mutex mutex_;
    std::set<std::pair<std::string, int>> assigned_;
    std::set<std::pair<std::string, int>> caught_up_;
};

}  // namespace ng::order_ui
