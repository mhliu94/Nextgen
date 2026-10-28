#include "web/event_hub.h"

namespace ng::web {

Subscriber::Subscriber(unsigned topics, std::string book_symbol) : topics_(topics), book_symbol_(std::move(book_symbol)) {}

unsigned Subscriber::wait(std::chrono::milliseconds timeout, bool& shutting_down) {
    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, timeout, [&] { return dirty_ != 0 || shutting_down_; });
    shutting_down = shutting_down_;
    unsigned dirty = dirty_;
    dirty_ = 0;
    return dirty;
}

void Subscriber::mark(unsigned topics, const std::string& symbol) {
    unsigned relevant = topics & topics_;
    if ((relevant & kTopicBook) && !symbol.empty() && symbol != book_symbol_) relevant &= ~kTopicBook;
    if (!relevant) return;
    {
        std::lock_guard lock(mutex_);
        dirty_ |= relevant;
    }
    cv_.notify_one();
}

void Subscriber::shutdown() {
    {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
    }
    cv_.notify_all();
}

std::shared_ptr<Subscriber> EventHub::subscribe(unsigned topics, const std::string& book_symbol) {
    auto subscriber = std::make_shared<Subscriber>(topics, book_symbol);
    std::lock_guard lock(mutex_);
    if (shutting_down_) subscriber->shutdown();
    subscribers_.insert(subscriber);
    return subscriber;
}

void EventHub::unsubscribe(const std::shared_ptr<Subscriber>& subscriber) {
    std::lock_guard lock(mutex_);
    subscribers_.erase(subscriber);
}

void EventHub::notify(unsigned topics, const std::string& symbol) {
    std::vector<std::shared_ptr<Subscriber>> targets;
    {
        std::lock_guard lock(mutex_);
        targets.assign(subscribers_.begin(), subscribers_.end());
    }
    for (auto& s : targets) s->mark(topics, symbol);
}

void EventHub::shutdown() {
    std::vector<std::shared_ptr<Subscriber>> targets;
    {
        std::lock_guard lock(mutex_);
        shutting_down_ = true;
        targets.assign(subscribers_.begin(), subscribers_.end());
    }
    for (auto& s : targets) s->shutdown();
}

std::size_t EventHub::subscriber_count() {
    std::lock_guard lock(mutex_);
    return subscribers_.size();
}

}  // namespace ng::web
