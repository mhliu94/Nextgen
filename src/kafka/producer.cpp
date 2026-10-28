#include "kafka/producer.h"

#include <chrono>

#include "core/logging.h"
#include "core/time_util.h"

namespace ng::kafka {
namespace {
constexpr const char* kLog = "trading-ui.kafka.commands";
}

TradingCommandsProducer::TradingCommandsProducer(const KafkaConfig& kafka, std::size_t history_limit)
    : topic_(kafka.trading_commands_topic), history_limit_(history_limit) {
    Config conf;
    conf.set("bootstrap.servers", kafka.bootstrap_servers)
        .set("acks", kafka.producer_acks)
        .set("enable.idempotence", kafka.producer_enable_idempotence ? "true" : "false")
        .set("linger.ms", std::to_string(kafka.producer_linger_ms))
        .set("client.id", "ktrader-nextgen-commands")
        .set("log_level", "4")
        .apply(kafka.client_config)
        .route_logs(kLog);
    producer_ = std::make_unique<Producer>(conf, [this](bool ok, const std::string& error, const std::string& key) {
        on_delivery(ok, error, key);
    });
    poll_thread_ = std::thread([this] { poll_loop(); });
    NG_LOG_INFO(kLog, "trading-commands producer ready topic={} bootstrap={}", topic_, kafka.bootstrap_servers);
}

TradingCommandsProducer::~TradingCommandsProducer() {
    stop_.store(true);
    if (poll_thread_.joinable()) poll_thread_.join();
    try {
        flush(2.0);
    } catch (...) {
    }
}

void TradingCommandsProducer::poll_loop() {
    while (!stop_.load()) {
        producer_->poll(100);
    }
}

void TradingCommandsProducer::on_delivery(bool ok, const std::string& error, const std::string& key) {
    std::lock_guard lock(mutex_);
    if (ok) ++delivered_;
    else {
        ++failed_;
        last_error_ = error;
        NG_LOG_ERROR(kLog, "trading command delivery failed key={}: {}", key, error);
    }
    // Update the most recent queued entry with this key.
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->key == key && it->status == "queued") {
            it->status = ok ? "delivered" : "failed";
            it->error = error;
            break;
        }
    }
}

void TradingCommandsProducer::publish_order(const json& command, const std::string& key) {
    std::string payload = json_dump_compact(command);
    {
        std::lock_guard lock(mutex_);
        history_.push_back(PublishedCommand{wall_now(), key, command, "queued", ""});
        while (history_.size() > history_limit_) history_.pop_front();
        ++published_;
    }
    try {
        producer_->produce(topic_, key, payload);
    } catch (const KafkaError& e) {
        std::lock_guard lock(mutex_);
        ++failed_;
        last_error_ = e.what();
        if (!history_.empty()) {
            history_.back().status = "failed";
            history_.back().error = e.what();
        }
        throw;
    }
    producer_->poll(0);
}

void TradingCommandsProducer::flush(double timeout_sec) { producer_->flush(static_cast<int>(timeout_sec * 1000)); }

std::vector<PublishedCommand> TradingCommandsProducer::history() const {
    std::lock_guard lock(mutex_);
    return std::vector<PublishedCommand>(history_.begin(), history_.end());
}

json TradingCommandsProducer::stats() const {
    std::lock_guard lock(mutex_);
    json out;
    out["topic"] = topic_;
    out["published"] = published_;
    out["delivered"] = delivered_;
    out["failed"] = failed_;
    out["outstanding"] = producer_->outstanding();
    out["last_error"] = last_error_;
    return out;
}

}  // namespace ng::kafka
