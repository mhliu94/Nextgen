// Trading-commands producer (producer_trading_commands.py) with a background delivery-report
// thread, a bounded in-memory history of published commands for the UI, and counters.
#pragma once

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "kafka/kafka_client.h"

namespace ng::kafka {

struct PublishedCommand {
    double wall;
    std::string key;
    json command;
    std::string status;  // queued | delivered | failed
    std::string error;
};

class TradingCommandsProducer {
public:
    explicit TradingCommandsProducer(const KafkaConfig& kafka, std::size_t history_limit = 200);
    ~TradingCommandsProducer();
    // Enqueues the command without waiting for delivery (the publisher contract). Throws KafkaError
    // when the local queue is full or the topic is invalid.
    void publish_order(const json& command, const std::string& key);
    void flush(double timeout_sec = 2.0);
    std::vector<PublishedCommand> history() const;
    json stats() const;
    const std::string& topic() const { return topic_; }

private:
    void poll_loop();
    void on_delivery(bool ok, const std::string& error, const std::string& key);

    std::string topic_;
    std::unique_ptr<Producer> producer_;
    std::thread poll_thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::deque<PublishedCommand> history_;
    std::size_t history_limit_;
    std::uint64_t published_ = 0;
    std::uint64_t delivered_ = 0;
    std::uint64_t failed_ = 0;
    std::string last_error_;
};

}  // namespace ng::kafka
