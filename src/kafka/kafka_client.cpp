#include "kafka/kafka_client.h"

#include <librdkafka/rdkafka.h>

#include <cstring>

#include "core/logging.h"

namespace ng::kafka {

namespace {

void log_cb(const rd_kafka_t* rk, int level, const char* fac, const char* buf) {
    const char* name = rk ? rd_kafka_opaque(rk) ? static_cast<const char*>(rd_kafka_opaque(rk)) : "kafka" : "kafka";
    (void)name;
    std::string logger = "kafka";
    if (rk) logger = std::string("kafka.") + rd_kafka_name(rk);
    if (level <= 3) NG_LOG_ERROR(logger, "{}: {}", fac, buf);
    else if (level == 4) NG_LOG_WARN(logger, "{}: {}", fac, buf);
    else if (level <= 6) NG_LOG_INFO(logger, "{}: {}", fac, buf);
    else NG_LOG_DEBUG(logger, "{}: {}", fac, buf);
}

std::vector<TopicPartition> to_vector(const rd_kafka_topic_partition_list_t* list) {
    std::vector<TopicPartition> out;
    if (!list) return out;
    for (int i = 0; i < list->cnt; ++i) {
        out.push_back({list->elems[i].topic, list->elems[i].partition, list->elems[i].offset});
    }
    return out;
}

}  // namespace

std::string library_version() { return rd_kafka_version_str(); }

Config::Config() : conf_(rd_kafka_conf_new()) {}

Config::~Config() {
    if (conf_) rd_kafka_conf_destroy(conf_);
}

Config& Config::set(const std::string& key, const std::string& value) {
    char errstr[512];
    if (rd_kafka_conf_set(conf_, key.c_str(), value.c_str(), errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        throw KafkaError("invalid Kafka configuration " + key + "=" + value + ": " + errstr);
    }
    return *this;
}

Config& Config::apply(const json& client_config) {
    if (!client_config.is_object()) return *this;
    for (auto it = client_config.begin(); it != client_config.end(); ++it) {
        auto text = json_to_string(&it.value());
        if (!text) continue;
        set(it.key(), *text);
    }
    return *this;
}

Config& Config::route_logs(const std::string& logger_name) {
    (void)logger_name;
    rd_kafka_conf_set_log_cb(conf_, log_cb);
    return *this;
}

rd_kafka_conf_t* Config::release() {
    rd_kafka_conf_t* c = conf_;
    conf_ = nullptr;
    return c;
}

// ---------------------------------------------------------------------------
// Consumer
// ---------------------------------------------------------------------------

void Consumer::rebalance_cb(rd_kafka_s* rk, int err_code, rd_kafka_topic_partition_list_s* partitions, void* opaque) {
    auto err = static_cast<rd_kafka_resp_err_t>(err_code);
    auto* self = static_cast<Consumer*>(opaque);
    if (err == RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS) {
        std::vector<TopicPartition> assigned = to_vector(partitions);
        if (self->options_.seek_to_beginning_on_assign) {
            for (int i = 0; i < partitions->cnt; ++i) partitions->elems[i].offset = RD_KAFKA_OFFSET_BEGINNING;
            for (auto& tp : assigned) tp.offset = RD_KAFKA_OFFSET_BEGINNING;
        }
        if (self->options_.on_assign) {
            try {
                self->options_.on_assign(assigned);
                for (int i = 0; i < partitions->cnt && i < static_cast<int>(assigned.size()); ++i) {
                    partitions->elems[i].offset = assigned[static_cast<std::size_t>(i)].offset;
                }
            } catch (const std::exception& e) {
                NG_LOG_ERROR("kafka", "assign callback failed: {}", e.what());
            }
        }
        rd_kafka_assign(rk, partitions);
    } else if (err == RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS) {
        if (self->options_.on_revoke) {
            try {
                self->options_.on_revoke(to_vector(partitions));
            } catch (const std::exception& e) {
                NG_LOG_ERROR("kafka", "revoke callback failed: {}", e.what());
            }
        }
        rd_kafka_assign(rk, nullptr);
    } else {
        NG_LOG_WARN("kafka", "rebalance error: {}", rd_kafka_err2str(err));
        rd_kafka_assign(rk, nullptr);
    }
}

void Consumer::error_cb(rd_kafka_s* rk, int err, const char* reason, void* opaque) {
    auto* self = static_cast<Consumer*>(opaque);
    (void)rk;
    if (self && self->options_.on_error) {
        try {
            self->options_.on_error(err, reason ? reason : "");
        } catch (...) {
        }
    }
}

int Consumer::stats_cb(rd_kafka_s* rk, char* json_text, size_t len, void* opaque) {
    auto* self = static_cast<Consumer*>(opaque);
    (void)rk;
    if (self && self->options_.on_stats) {
        try {
            self->options_.on_stats(std::string(json_text, len));
        } catch (...) {
        }
    }
    return 0;  // librdkafka frees the buffer
}

Consumer::Consumer(Config& config, Options options) : options_(std::move(options)) {
    rd_kafka_conf_t* conf = config.get();
    rd_kafka_conf_set_opaque(conf, this);
    rd_kafka_conf_set_rebalance_cb(conf, [](rd_kafka_t* rk, rd_kafka_resp_err_t err, rd_kafka_topic_partition_list_t* parts,
                                            void* opaque) { Consumer::rebalance_cb(rk, static_cast<int>(err), parts, opaque); });
    if (options_.on_error) {
        rd_kafka_conf_set_error_cb(conf, [](rd_kafka_t* rk, int err, const char* reason, void* opaque) {
            Consumer::error_cb(rk, err, reason, opaque);
        });
    }
    if (options_.on_stats) {
        rd_kafka_conf_set_stats_cb(conf, [](rd_kafka_t* rk, char* json_text, size_t len, void* opaque) {
            return Consumer::stats_cb(rk, json_text, len, opaque);
        });
    }
    char errstr[512];
    rk_ = rd_kafka_new(RD_KAFKA_CONSUMER, config.release(), errstr, sizeof(errstr));
    if (!rk_) throw KafkaError(std::string("failed to create Kafka consumer: ") + errstr);
    rd_kafka_poll_set_consumer(rk_);
}

Consumer::~Consumer() {
    close();
    if (rk_) rd_kafka_destroy(rk_);
}

void Consumer::subscribe(const std::vector<std::string>& topics) {
    rd_kafka_topic_partition_list_t* list = rd_kafka_topic_partition_list_new(static_cast<int>(topics.size()));
    for (const auto& topic : topics) rd_kafka_topic_partition_list_add(list, topic.c_str(), RD_KAFKA_PARTITION_UA);
    rd_kafka_resp_err_t err = rd_kafka_subscribe(rk_, list);
    rd_kafka_topic_partition_list_destroy(list);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) throw KafkaError(std::string("subscribe failed: ") + rd_kafka_err2str(err));
}

std::optional<Message> Consumer::poll(int timeout_ms) {
    if (closed_) return std::nullopt;
    rd_kafka_message_t* msg = rd_kafka_consumer_poll(rk_, timeout_ms);
    if (!msg) return std::nullopt;
    Message out;
    out.topic = msg->rkt ? rd_kafka_topic_name(msg->rkt) : "";
    out.partition = msg->partition;
    out.offset = msg->offset;
    if (msg->err) {
        if (msg->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
            out.eof = true;
        } else {
            out.error = true;
            out.error_code = msg->err;
            out.error_text = rd_kafka_message_errstr(msg);
        }
    } else {
        rd_kafka_timestamp_type_t tstype = RD_KAFKA_TIMESTAMP_NOT_AVAILABLE;
        out.timestamp_ms = rd_kafka_message_timestamp(msg, &tstype);
        if (tstype == RD_KAFKA_TIMESTAMP_NOT_AVAILABLE) out.timestamp_ms = -1;
        if (msg->key && msg->key_len) out.key.assign(static_cast<const char*>(msg->key), msg->key_len);
        if (msg->payload && msg->len) out.value.assign(static_cast<const char*>(msg->payload), msg->len);
    }
    rd_kafka_message_destroy(msg);
    return out;
}

std::vector<TopicPartition> Consumer::assignment() const {
    rd_kafka_topic_partition_list_t* list = nullptr;
    if (rd_kafka_assignment(rk_, &list) != RD_KAFKA_RESP_ERR_NO_ERROR || !list) return {};
    auto out = to_vector(list);
    rd_kafka_topic_partition_list_destroy(list);
    return out;
}

std::optional<std::pair<long long, long long>> Consumer::query_watermark_offsets(const std::string& topic, int partition,
                                                                                 int timeout_ms) {
    if (!rk_ || closed_) return std::nullopt;
    int64_t low = 0, high = 0;
    rd_kafka_resp_err_t err = rd_kafka_query_watermark_offsets(rk_, topic.c_str(), partition, &low, &high, timeout_ms);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) return std::nullopt;
    return std::make_pair(static_cast<long long>(low), static_cast<long long>(high));
}

std::optional<long long> Consumer::offset_for_time(const std::string& topic, int partition, long long timestamp_ms, int timeout_ms) {
    if (!rk_ || closed_) return std::nullopt;
    rd_kafka_topic_partition_list_t* list = rd_kafka_topic_partition_list_new(1);
    rd_kafka_topic_partition_list_add(list, topic.c_str(), partition)->offset = timestamp_ms;
    rd_kafka_resp_err_t err = rd_kafka_offsets_for_times(rk_, list, timeout_ms);
    std::optional<long long> result;
    if (err == RD_KAFKA_RESP_ERR_NO_ERROR && list->cnt == 1 && list->elems[0].err == RD_KAFKA_RESP_ERR_NO_ERROR) {
        long long offset = list->elems[0].offset;
        result = offset >= 0 ? offset : kOffsetEnd;
    }
    rd_kafka_topic_partition_list_destroy(list);
    return result;
}

void Consumer::close() {
    if (closed_ || !rk_) return;
    closed_ = true;
    rd_kafka_consumer_close(rk_);
}

std::string Consumer::name() const { return rk_ ? rd_kafka_name(rk_) : ""; }

// ---------------------------------------------------------------------------
// Producer
// ---------------------------------------------------------------------------

void Producer::dr_cb(rd_kafka_s* rk, const void* message, void* opaque) {
    (void)rk;
    auto* self = static_cast<Producer*>(opaque);
    const auto* msg = static_cast<const rd_kafka_message_t*>(message);
    if (!self || !self->on_delivery_) return;
    std::string key;
    if (msg->key && msg->key_len) key.assign(static_cast<const char*>(msg->key), msg->key_len);
    try {
        if (msg->err) self->on_delivery_(false, rd_kafka_err2str(msg->err), key);
        else self->on_delivery_(true, "", key);
    } catch (...) {
    }
}

Producer::Producer(Config& config, DeliveryCallback on_delivery) : on_delivery_(std::move(on_delivery)) {
    rd_kafka_conf_t* conf = config.get();
    rd_kafka_conf_set_opaque(conf, this);
    rd_kafka_conf_set_dr_msg_cb(conf, [](rd_kafka_t* rk, const rd_kafka_message_t* msg, void* opaque) {
        Producer::dr_cb(rk, msg, opaque);
    });
    char errstr[512];
    rk_ = rd_kafka_new(RD_KAFKA_PRODUCER, config.release(), errstr, sizeof(errstr));
    if (!rk_) throw KafkaError(std::string("failed to create Kafka producer: ") + errstr);
}

Producer::~Producer() {
    if (rk_) {
        rd_kafka_flush(rk_, 2000);
        rd_kafka_destroy(rk_);
    }
}

void Producer::produce(const std::string& topic, const std::string& key, const std::string& value) {
    rd_kafka_resp_err_t err = rd_kafka_producev(
        rk_, RD_KAFKA_V_TOPIC(topic.c_str()), RD_KAFKA_V_MSGFLAGS(RD_KAFKA_MSG_F_COPY),
        RD_KAFKA_V_KEY(key.empty() ? nullptr : key.data(), key.size()),
        RD_KAFKA_V_VALUE(const_cast<char*>(value.data()), value.size()), RD_KAFKA_V_END);
    if (err != RD_KAFKA_RESP_ERR_NO_ERROR) throw KafkaError(std::string("produce failed: ") + rd_kafka_err2str(err));
}

int Producer::poll(int timeout_ms) { return rd_kafka_poll(rk_, timeout_ms); }

int Producer::flush(int timeout_ms) {
    rd_kafka_flush(rk_, timeout_ms);
    return rd_kafka_outq_len(rk_);
}

int Producer::outstanding() const { return rd_kafka_outq_len(rk_); }

}  // namespace ng::kafka
