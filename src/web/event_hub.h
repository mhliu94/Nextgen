// Fan-out of change notifications to server-sent-event subscribers. Publishers mark topics
// dirty; each subscriber coalesces notifications and wakes its connection thread.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace ng::web {

enum Topic : unsigned {
    kTopicAccounts = 1u << 0,
    kTopicQuotes = 1u << 1,
    kTopicSmart = 1u << 2,
    kTopicFeeds = 1u << 3,
    kTopicAlgo = 1u << 4,
    kTopicBook = 1u << 5,
    kTopicCommands = 1u << 6,
};

class Subscriber {
public:
    Subscriber(unsigned topics, std::string book_symbol);
    // Waits until a subscribed topic is dirty, the hub shuts down, or the timeout passes.
    // Returns the dirty mask (0 on timeout) and clears it.
    unsigned wait(std::chrono::milliseconds timeout, bool& shutting_down);
    void mark(unsigned topics, const std::string& symbol);
    void shutdown();
    unsigned topics() const { return topics_; }
    const std::string& book_symbol() const { return book_symbol_; }

private:
    unsigned topics_;
    std::string book_symbol_;
    std::mutex mutex_;
    std::condition_variable cv_;
    unsigned dirty_ = 0;
    bool shutting_down_ = false;
};

class EventHub {
public:
    std::shared_ptr<Subscriber> subscribe(unsigned topics, const std::string& book_symbol = "");
    void unsubscribe(const std::shared_ptr<Subscriber>& subscriber);
    void notify(unsigned topics, const std::string& symbol = "");
    void shutdown();
    std::size_t subscriber_count();

private:
    std::mutex mutex_;
    std::set<std::shared_ptr<Subscriber>> subscribers_;
    bool shutting_down_ = false;
};

}  // namespace ng::web
