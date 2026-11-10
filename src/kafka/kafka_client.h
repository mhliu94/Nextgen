// Thin RAII wrappers over the librdkafka C API: configuration, consumer and producer.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"

struct rd_kafka_s;
struct rd_kafka_conf_s;
struct rd_kafka_topic_partition_list_s;

namespace ng::kafka {

class KafkaError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Logical offsets understood by librdkafka.
inline constexpr long long kOffsetBeginning = -2;  // RD_KAFKA_OFFSET_BEGINNING
inline constexpr long long kOffsetEnd = -1;        // RD_KAFKA_OFFSET_END
inline constexpr long long kOffsetInvalid = -1001; // RD_KAFKA_OFFSET_INVALID

struct TopicPartition {
    std::string topic;
    int partition = 0;
    long long offset = kOffsetInvalid;
};

struct Message {
    std::string topic;
    int partition = 0;
    long long offset = 0;
    long long timestamp_ms = -1;  // message timestamp (create/log-append time), -1 when unavailable
    std::string key;
    std::string value;
    bool eof = false;    // RD_KAFKA_RESP_ERR__PARTITION_EOF
    bool error = false;  // any other error; see error_code/error_text
    int error_code = 0;
    std::string error_text;
};

// Builds a librdkafka configuration. `client_config` may carry extra librdkafka properties
// (security.protocol, sasl.*, ssl.*) from the application configuration.
class Config {
public:
    Config();
    ~Config();
    Config(const Config&) = delete;
    Config& operator=(const Config&) = delete;
    Config& set(const std::string& key, const std::string& value);
    Config& apply(const json& client_config);
    // Logs librdkafka messages through ng::log.
    Config& route_logs(const std::string& logger_name);
    rd_kafka_conf_s* release();
    rd_kafka_conf_s* get() const { return conf_; }

private:
    rd_kafka_conf_s* conf_;
};

class Consumer {
public:
    using AssignCallback = std::function<void(std::vector<TopicPartition>& partitions)>;
    using RevokeCallback = std::function<void(const std::vector<TopicPartition>& partitions)>;
    using ErrorCallback = std::function<void(int code, const std::string& reason)>;
    using StatsCallback = std::function<void(const std::string& stats_json)>;

    struct Options {
        bool seek_to_beginning_on_assign = false;  // set every assigned partition to OFFSET_BEGINNING
        AssignCallback on_assign;
        RevokeCallback on_revoke;
        ErrorCallback on_error;
        StatsCallback on_stats;
    };

    Consumer(Config& config, Options options);
    ~Consumer();
    void subscribe(const std::vector<std::string>& topics);
    // Returns nullopt on timeout; EOF and errors are reported as messages with flags set.
    std::optional<Message> poll(int timeout_ms);
    std::vector<TopicPartition> assignment() const;
    // Low and high watermark of a partition, asked from the broker ("high" is the offset after the
    // latest record). nullopt when the broker did not answer in time. Usable from on_assign.
    std::optional<std::pair<long long, long long>> query_watermark_offsets(const std::string& topic, int partition,
                                                                          int timeout_ms);
    // Earliest offset whose timestamp is >= `timestamp_ms` (kOffsetEnd when the partition has no such
    // record). nullopt when the lookup failed. Usable from on_assign.
    std::optional<long long> offset_for_time(const std::string& topic, int partition, long long timestamp_ms, int timeout_ms);
    void close();
    std::string name() const;

private:
    static void rebalance_cb(rd_kafka_s* rk, int err, rd_kafka_topic_partition_list_s* partitions, void* opaque);
    static void error_cb(rd_kafka_s* rk, int err, const char* reason, void* opaque);
    static int stats_cb(rd_kafka_s* rk, char* json_text, size_t len, void* opaque);

    rd_kafka_s* rk_ = nullptr;
    Options options_;
    bool closed_ = false;
};

class Producer {
public:
    using DeliveryCallback = std::function<void(bool ok, const std::string& error, const std::string& key)>;

    Producer(Config& config, DeliveryCallback on_delivery = nullptr);
    ~Producer();
    // Non-blocking enqueue. Throws KafkaError when the local queue is full or the topic is invalid.
    void produce(const std::string& topic, const std::string& key, const std::string& value);
    // Serves delivery reports; returns the number of events served.
    int poll(int timeout_ms);
    // Waits for outstanding deliveries; returns the number still outstanding.
    int flush(int timeout_ms);
    int outstanding() const;

private:
    static void dr_cb(rd_kafka_s* rk, const void* message, void* opaque);
    rd_kafka_s* rk_ = nullptr;
    DeliveryCallback on_delivery_;
};

std::string library_version();

}  // namespace ng::kafka
