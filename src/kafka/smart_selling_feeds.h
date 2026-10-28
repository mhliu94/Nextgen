// Independent retained order-status and trading-volume feeds for Smart selling
// (consumer_smart_selling.py): each feed replays every partition from the beginning and
// reports starting/replaying/live/disconnected/stopped to the runtime.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/json.h"
#include "kafka/kafka_client.h"

namespace ng::smart {
class SmartSellingRuntime;
}

namespace ng::kafka {

class SmartSellingFeed {
public:
    SmartSellingFeed(std::string name, std::string topic, std::string bootstrap_servers, smart::SmartSellingRuntime& runtime,
                     json client_config = json::object());
    ~SmartSellingFeed();
    void start();
    void stop();
    std::string state() const;
    std::uint64_t rejected_messages() const { return rejected_.load(); }
    std::uint64_t messages() const { return messages_.load(); }
    const std::string& topic() const { return topic_; }
    const std::string& group_id() const { return group_id_; }

private:
    void set_state(const std::string& state);
    void run();
    void handle_message(const Message& message);
    void deliver_message(const Message& message);
    void connected();
    void warn(const std::string& reason);

    std::string name_;
    std::string topic_;
    std::string bootstrap_servers_;
    smart::SmartSellingRuntime& runtime_;
    json client_config_;
    std::string group_id_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> reconnect_requested_{false};
    mutable std::mutex mutex_;
    std::set<std::pair<std::string, int>> assigned_;
    std::set<std::pair<std::string, int>> caught_up_;
    double last_warning_ = -1e300;
    std::string state_ = "";
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> messages_{0};
};

class SmartSellingConsumer {
public:
    SmartSellingConsumer(const AppConfig& config, smart::SmartSellingRuntime& runtime);
    void start();
    void stop();
    SmartSellingFeed& orders() { return *orders_; }
    SmartSellingFeed& volume() { return *volume_; }

private:
    std::unique_ptr<SmartSellingFeed> orders_;
    std::unique_ptr<SmartSellingFeed> volume_;
};

}  // namespace ng::kafka
