#include "web/server.h"
#include "core/public_payload.h"

#include <httplib.h>
#include <functional>
#include <vector>
#include <deque>
#include <condition_variable>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

#include "core/i18n.h"
#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "orders/commands.h"
#include "web/assets.h"

namespace ng::web {
namespace {

constexpr const char* kLog = "trading-ui.http";
constexpr const char* kJson = "application/json; charset=utf-8";
constexpr const char* kAuthCookie = "auth_token";

std::map<std::string, std::string> parse_cookies(const std::string& header) {
    std::map<std::string, std::string> out;
    for (auto& part : split(header, ';')) {
        std::string item = trim(part);
        std::size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        out[trim(item.substr(0, eq))] = url_decode(trim(item.substr(eq + 1)));
    }
    return out;
}

std::string cookie_value(const httplib::Request& req, const std::string& name) {
    auto cookies = parse_cookies(req.get_header_value("Cookie"));
    auto it = cookies.find(name);
    return it == cookies.end() ? "" : it->second;
}

bool wants_json(const httplib::Request& req) {
    std::string accept = to_lower(req.get_header_value("Accept"));
    std::string content_type = to_lower(req.get_header_value("Content-Type"));
    return accept.find("application/json") != std::string::npos || content_type.find("application/json") != std::string::npos ||
           req.get_header_value("X-Requested-With") == "fetch";
}

// Unified access to JSON bodies, url-encoded forms, multipart forms and query parameters.
class RequestInput {
public:
    explicit RequestInput(const httplib::Request& req) : req_(req) {
        std::string content_type = to_lower(req.get_header_value("Content-Type"));
        if (content_type.find("application/json") != std::string::npos && !req.body.empty()) {
            json parsed = json::parse(req.body, nullptr, false);
            if (parsed.is_object()) body_ = parsed;
        }
    }
    bool has_json() const { return body_.is_object(); }
    const json& body() const { return body_; }
    const json* raw(const std::string& key) const { return has_json() ? json_find(body_, key) : nullptr; }
    bool has(const std::string& key) const {
        if (has_json()) return body_.contains(key);
        return req_.has_param(key) || (req_.is_multipart_form_data() && req_.form.has_field(key));
    }
    // String form of a field; nullopt when absent or JSON null.
    std::optional<std::string> str(const std::string& key) const {
        if (has_json()) {
            const json* v = json_find(body_, key);
            if (!v || v->is_null()) return std::nullopt;
            if (v->is_array()) return v->empty() ? std::optional<std::string>() : json_to_string(&(*v)[0]);
            return json_to_string(v);
        }
        if (req_.is_multipart_form_data() && req_.form.has_field(key)) return req_.form.get_field(key);
        if (req_.has_param(key)) return req_.get_param_value(key);
        return std::nullopt;
    }
    std::string str_or(const std::string& key, const std::string& fallback) const { return str(key).value_or(fallback); }
    std::vector<std::string> list(const std::string& key) const {
        std::vector<std::string> out;
        if (has_json()) {
            const json* v = json_find(body_, key);
            if (!v || v->is_null()) return out;
            if (v->is_array()) {
                for (const auto& item : *v) {
                    auto s = json_to_string(&item);
                    out.push_back(s.value_or(""));
                }
            } else {
                out.push_back(json_to_string(v).value_or(""));
            }
            return out;
        }
        if (req_.is_multipart_form_data()) return req_.form.get_fields(key);
        return req_.get_param_values(key);
    }
    bool truthy(const std::string& key) const {
        if (has_json()) return json_truthy(json_find(body_, key));
        auto s = str(key);
        return s && truthy_string(*s);
    }

private:
    const httplib::Request& req_;
    json body_;
};

struct StreamContext {
    std::shared_ptr<Subscriber> subscriber;
    std::string lang;
    std::string token;
    std::string book_symbol;
    int depth = 20;
    bool first = true;
    double last_auth_check = 0;
    std::map<unsigned, std::string> last_payload;
    std::map<unsigned, double> last_sent;
    long long event_id = 0;
};

const std::map<std::string, unsigned>& topic_names() {
    static const std::map<std::string, unsigned> names = {
        {"accounts", kTopicAccounts}, {"quotes", kTopicQuotes}, {"smart", kTopicSmart}, {"feeds", kTopicFeeds},
        {"algo", kTopicAlgo},         {"book", kTopicBook},     {"commands", kTopicCommands}};
    return names;
}

const char* topic_event_name(unsigned topic) {
    for (auto& [name, bit] : topic_names()) if (bit == topic) return name.c_str();
    return "message";
}

double min_interval_for(unsigned topic) {
    switch (topic) {
        case kTopicBook: return 0.1;
        case kTopicQuotes: return 0.25;
        case kTopicAccounts: return 0.1;
        default: return 0.5;
    }
}

}  // namespace

// Task queue for cpp-httplib: a core pool that grows on demand. Every server-sent-events
// connection parks a worker for its whole lifetime, so a fixed pool would cap the number of
// open browser streams and, worse, let them starve ordinary requests. Overflow workers exit
// after sitting idle for a while.
class ElasticThreadPool : public httplib::TaskQueue {
public:
    explicit ElasticThreadPool(std::size_t core, std::size_t max_threads = 4096)
        : core_(std::max<std::size_t>(1, core)), max_(std::max(max_threads, core_)) {
        for (std::size_t i = 0; i < core_; ++i) spawn(false);
    }
    ~ElasticThreadPool() override { shutdown(); }

    bool enqueue(std::function<void()> fn) override {
        std::unique_lock lock(mutex_);
        if (shutdown_) return false;
        jobs_.push_back(std::move(fn));
        if (idle_ == 0 && threads_ < max_) spawn(true);
        cond_.notify_one();
        return true;
    }

    void shutdown() override {
        std::vector<std::thread> workers;
        {
            std::unique_lock lock(mutex_);
            if (shutdown_) return;
            shutdown_ = true;
            cond_.notify_all();
            workers.swap(workers_);
        }
        for (auto& t : workers) {
            if (t.get_id() == std::this_thread::get_id()) t.detach();
            else t.join();
        }
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }

private:
    void spawn(bool overflow) {
        // mutex_ is held by the caller.
        ++threads_;
        workers_.emplace_back([this, overflow] { run(overflow); });
    }

    void run(bool overflow) {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex_);
                ++idle_;
                bool have_job = true;
                if (overflow) {
                    have_job = cond_.wait_for(lock, std::chrono::seconds(30), [this] { return shutdown_ || !jobs_.empty(); });
                } else {
                    cond_.wait(lock, [this] { return shutdown_ || !jobs_.empty(); });
                }
                --idle_;
                if (shutdown_ && jobs_.empty()) {
                    --threads_;
                    return;
                }
                if (!have_job || jobs_.empty()) {
                    // Idle overflow worker: retire (joined lazily at shutdown).
                    --threads_;
                    retire_current(lock);
                    return;
                }
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            try {
                job();
            } catch (...) {
            }
        }
    }

    void retire_current(std::unique_lock<std::mutex>& lock) {
        auto me = std::this_thread::get_id();
        for (auto it = workers_.begin(); it != workers_.end(); ++it) {
            if (it->get_id() == me) {
                it->detach();
                workers_.erase(it);
                break;
            }
        }
        (void)lock;
    }

    std::size_t core_;
    std::size_t max_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    std::deque<std::function<void()>> jobs_;
    std::vector<std::thread> workers_;
    std::size_t threads_ = 0;
    std::size_t idle_ = 0;
    bool shutdown_ = false;
};

struct Server::Impl {
    std::unique_ptr<httplib::Server> http;
    std::unique_ptr<httplib::Server> redirector;
};

Server::Server(ControlCenter& control, SessionStore& sessions, ServerOptions options)
    : impl_(std::make_unique<Impl>()), control_(control), sessions_(sessions), options_(std::move(options)) {}

Server::~Server() { stop(); }

void Server::stop() {
    if (!running_.exchange(false)) {
        if (impl_->http && serve_thread_.joinable()) {
            impl_->http->stop();
            serve_thread_.join();
        }
        if (impl_->redirector && redirect_thread_.joinable()) {
            impl_->redirector->stop();
            redirect_thread_.join();
        }
        return;
    }
    control_.hub().shutdown();
    if (impl_->http) impl_->http->stop();
    if (impl_->redirector) impl_->redirector->stop();
    if (serve_thread_.joinable()) serve_thread_.join();
    if (redirect_thread_.joinable()) redirect_thread_.join();
}

void Server::start() {
    using httplib::Request;
    using httplib::Response;

    if (options_.ssl) {
        auto* ssl = new httplib::SSLServer(options_.ssl_certfile.c_str(), options_.ssl_keyfile.c_str());
        impl_->http.reset(ssl);
        if (!ssl->is_valid()) throw std::runtime_error("failed to load TLS certificate/key");
    } else {
        impl_->http = std::make_unique<httplib::Server>();
    }
    httplib::Server& svr = *impl_->http;
    int threads = options_.threads > 0 ? options_.threads : static_cast<int>(std::max(16u, std::thread::hardware_concurrency() * 4));
    svr.new_task_queue = [threads] { return new ElasticThreadPool(static_cast<std::size_t>(threads)); };
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(60, 0);
    svr.set_payload_max_length(4 * 1024 * 1024);
    svr.set_exception_handler([](const Request& req, Response& res, std::exception_ptr ep) {
        std::string message = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
        NG_LOG_ERROR(kLog, "{} {} failed: {}", req.method, req.path, message);
        res.status = 500;
        if (starts_with(req.path, "/api/")) {
            json out{{"ok", false}, {"error", message}};
            res.set_content(json_dump_compact(out), kJson);
        } else {
            res.set_content("Internal server error", "text/plain; charset=utf-8");
        }
    });
    svr.set_logger([](const Request& req, const Response& res) {
        if (starts_with(req.path, "/api/stream")) return;
        NG_LOG_DEBUG(kLog, "{} {} -> {}", req.method, req.path, res.status);
    });

    // --- helpers -------------------------------------------------------------------------------
    auto lang_of = [](const Request& req) {
        return i18n::resolve_lang(req.has_param("lang") ? req.get_param_value("lang") : "", cookie_value(req, "lang"));
    };
    auto current_user = [this](const Request& req) -> std::optional<std::string> {
        return sessions_.user_for(cookie_value(req, kAuthCookie));
    };
    auto json_response = [this](Response& res, const json& payload, int status = 200) {
        res.status = status;
        res.set_header("Cache-Control", "no-store");
        res.set_content(json_dump_compact(control_.browser_payload(payload)), kJson);
    };
    auto text_response = [](Response& res, const std::shared_ptr<const std::string>& payload) {
        res.status = 200;
        res.set_header("Cache-Control", "no-store");
        res.set_content(*payload, kJson);
    };
    auto api_error = [=, this](Response& res, const std::string& message, int status) {
        json_response(res, json{{"ok", false}, {"error", message}}, status);
    };
    auto redirect = [](Response& res, const std::string& url, int status = 303) {
        res.status = status;
        res.set_header("Location", url);
        res.set_header("Cache-Control", "no-store");
        res.set_content("", "text/plain");
    };
    auto cookie_flags = [this]() {
        std::string flags = "; Path=/; HttpOnly; SameSite=Lax";
        if (options_.secure_cookies) flags += "; Secure";
        return flags;
    };
    auto same_origin = [this](const Request& req) {
        std::string origin = req.get_header_value("Origin");
        if (origin.empty()) return true;
        std::string host = req.get_header_value("X-Forwarded-Host");
        if (host.empty()) host = req.get_header_value("Host");
        std::string scheme = req.get_header_value("X-Forwarded-Proto");
        if (scheme.empty()) scheme = options_.ssl ? "https" : "http";
        std::string expected = scheme + "://" + host;
        while (!origin.empty() && origin.back() == '/') origin.pop_back();
        return origin == expected;
    };
    auto require_api_user = [=, this](const Request& req, Response& res) -> std::optional<std::string> {
        auto user = current_user(req);
        if (!user) {
            api_error(res, "Unauthorized", 401);
            return std::nullopt;
        }
        if (req.method == "POST" && !same_origin(req)) {
            api_error(res, "Cross-origin submission is not allowed", 403);
            return std::nullopt;
        }
        return user;
    };
    auto serve_asset = [this](const std::string& rel, Response& res, bool is_html) {
        std::string body;
        bool found = false;
        if (options_.static_dir) {
            std::ifstream in(*options_.static_dir + "/" + rel, std::ios::binary);
            if (in) {
                std::stringstream buffer;
                buffer << in.rdbuf();
                body = buffer.str();
                found = true;
            }
        }
        if (!found) {
            if (const assets::Asset* asset = assets::find(rel)) {
                body.assign(reinterpret_cast<const char*>(asset->data), asset->size);
                found = true;
            }
        }
        if (!found) {
            res.status = 404;
            res.set_content("Not found", "text/plain; charset=utf-8");
            return;
        }
        res.set_header("Cache-Control", is_html ? "no-store" : "public, max-age=120");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_content(body, assets::content_type_for(rel));
    };
    auto serve_app = [serve_asset](Response& res) { serve_asset("index.html", res, true); };

    // Order messages per language, resolved lazily.
    auto order_messages = [](const std::string& lang) {
        orders::OrderMessages m;
        m.invalid_account = i18n::t(lang, "invalid_account");
        m.invalid_symbol = i18n::t(lang, "invalid_symbol");
        m.invalid_side = i18n::t(lang, "invalid_side");
        m.both_shares_and_dollars = i18n::t(lang, "both_shares_and_dollars");
        m.neither_shares_nor_dollars = i18n::t(lang, "neither_shares_nor_dollars");
        m.shares_positive = i18n::t(lang, "shares_positive");
        m.dollars_positive = i18n::t(lang, "dollars_positive");
        return m;
    };
    auto quick_messages = [](const std::string& lang) {
        orders::QuickMessages m;
        m.invalid_account = i18n::t(lang, "invalid_account");
        m.invalid_symbol = i18n::t(lang, "invalid_symbol");
        m.invalid_side = i18n::t(lang, "invalid_side");
        m.dollars_positive = i18n::t(lang, "dollars_positive");
        m.no_last_price = i18n::t(lang, "quick_no_last_price");
        m.dollars_too_low = i18n::t(lang, "quick_dollars_too_low");
        return m;
    };
    auto limit_messages = [](const std::string& lang) {
        orders::LimitMessages m;
        m.invalid_account = i18n::t(lang, "invalid_account");
        m.invalid_symbol = i18n::t(lang, "invalid_symbol");
        m.invalid_side = i18n::t(lang, "invalid_side");
        m.shares_positive = i18n::t(lang, "shares_positive");
        m.price_positive = i18n::t(lang, "price_positive");
        m.no_last_price = i18n::t(lang, "no_last_price");
        return m;
    };
    auto algo_messages = [](const std::string& lang) {
        orders::AlgoMessages m;
        m.invalid_mode = i18n::t(lang, "invalid_mode");
        m.invalid_symbol = i18n::t(lang, "invalid_symbol");
        m.invalid_account = i18n::t(lang, "invalid_account");
        m.fast_config_required = i18n::t(lang, "fast_config_required");
        m.fast_price_limit_positive = i18n::t(lang, "fast_price_limit_positive");
        m.fast_accounts_required = i18n::t(lang, "fast_accounts_required");
        m.fast_account_duplicate = i18n::t(lang, "fast_account_duplicate");
        m.fast_book_levels_invalid = i18n::t(lang, "fast_book_levels_invalid");
        m.fast_target_percentage_invalid = i18n::t(lang, "fast_target_percentage_invalid");
        m.fast_cycle_seconds_invalid = i18n::t(lang, "fast_cycle_seconds_invalid");
        m.fast_total_target_position_invalid = i18n::t(lang, "fast_total_target_position_invalid");
        return m;
    };

    // Shared implementations: each returns {ok, message} or throws std::invalid_argument(error text).
    struct Outcome {
        bool ok;
        std::string message;
        json extra = json::object();
        int status = 400;
    };
    auto fail = [](const std::string& message, int status = 400) { return Outcome{false, message, json::object(), status}; };

    auto do_market_order = [=, this](const RequestInput& in, const std::string& lang) -> Outcome {
        std::optional<std::string> shares = in.str("shares"), dollars = in.str("dollar_amount");
        orders::Validated v = orders::validate_order_inputs(in.str_or("account_id", ""), in.str_or("symbol", ""), in.str_or("side", "BUY"),
                                                            shares, dollars, control_.accounts(), order_messages(lang));
        if (!v.ok()) return fail(v.error);
        control_.publish_control_command(*v.command, v.command->value("account_id", std::string("MARKET_ORDER")));
        Outcome out{true, i18n::t(lang, "published_cmd") + "=" + v.command->at("command_id").get<std::string>()};
        out.extra["command"] = *v.command;
        return out;
    };
    auto do_quick_order = [=, this](const RequestInput& in, const std::string& lang) -> Outcome {
        std::vector<std::string> account_ids = in.list("account_ids");
        std::string symbol = to_upper(trim(in.str_or("symbol", "")));
        std::string side = in.str_or("side", "BUY");
        std::string dollars = trim(in.str_or("dollar_amount", ""));
        if (dollars.empty()) dollars = "10000";
        if (account_ids.empty()) return fail(i18n::t(lang, "quick_pick_account"));
        std::optional<double> last = control_.market_last(symbol);
        std::vector<json> commands;
        std::set<std::string> seen_accounts;
        int published = 0;
        std::string first_id;
        for (const auto& account_id : account_ids) {
            if (!seen_accounts.insert(trim(account_id)).second) return fail("Duplicate account IDs are not allowed");
            orders::Validated v = orders::validate_quick_order_inputs(account_id, symbol, side, dollars, last, control_.accounts(), quick_messages(lang));
            if (!v.ok()) return fail(v.error);
            commands.push_back(control_.normalize_trading_command(*v.command));
            ++published;
            if (first_id.empty()) first_id = v.command->at("command_id").get<std::string>();
        }
        json queued=json::array();
        for (const auto& command:commands) {
            try { control_.publish_control_command(command,command.at("account_id").get<std::string>()); }
            catch (const std::exception&) {
                return Outcome{false,"Batch publication interrupted; inspect queued and uncertain command IDs before retrying",
                    {{"queued_commands",queued},{"uncertain_command",command},{"requested_count",commands.size()}},503};
            }
            queued.push_back(command);
        }
        return Outcome{true, i18n::t(lang, "quick_submitted") + " " + std::to_string(published) + ". " + i18n::t(lang, "published_cmd") + "=" + first_id};
    };
    auto do_limit_order = [=, this](const RequestInput& in, const std::string& lang) -> Outcome {
        std::vector<std::string> account_ids = in.list("account_ids");
        std::string symbol = to_upper(trim(in.str_or("symbol", "")));
        std::string side = in.str_or("side", "BUY");
        std::optional<std::string> shares = in.str("shares");
        std::optional<std::string> limit_price = in.str("limit_price");
        std::optional<std::string> through = in.str("through_market_pct");
        if (account_ids.empty()) return fail(i18n::t(lang, "quick_pick_account"));
        std::optional<double> last;
        if (!limit_price || trim(*limit_price).empty()) last = control_.market_last(symbol);
        std::vector<json> commands;
        std::set<std::string> seen_accounts;
        int published = 0;
        std::string first_id;
        for (const auto& account_id : account_ids) {
            if (!seen_accounts.insert(trim(account_id)).second) return fail("Duplicate account IDs are not allowed");
            orders::Validated v = orders::validate_limit_order_inputs(account_id, symbol, side, shares, limit_price, through, last,
                                                                      control_.accounts(), limit_messages(lang));
            if (!v.ok()) return fail(v.error);
            commands.push_back(control_.normalize_trading_command(*v.command));
            ++published;
            if (first_id.empty()) first_id = v.command->at("command_id").get<std::string>();
        }
        json queued=json::array();
        for (const auto& command:commands) {
            try { control_.publish_control_command(command,command.at("account_id").get<std::string>()); }
            catch (const std::exception&) {
                return Outcome{false,"Batch publication interrupted; inspect queued and uncertain command IDs before retrying",
                    {{"queued_commands",queued},{"uncertain_command",command},{"requested_count",commands.size()}},503};
            }
            queued.push_back(command);
        }
        return Outcome{true, i18n::t(lang, "limit_submitted") + " " + std::to_string(published) + ". " + i18n::t(lang, "published_cmd") + "=" + first_id};
    };
    auto do_cancel = [=, this](const RequestInput& in, const std::string& lang) -> Outcome {
        std::vector<std::string> raw = in.list("account_ids");
        std::string symbol = to_upper(trim(in.str_or("symbol", "")));
        std::vector<std::string> account_ids;
        bool all = std::find(raw.begin(), raw.end(), "__ALL__") != raw.end() || in.truthy("all_accounts");
        if (all) {
            account_ids = control_.accounts().ids_by_num_id();
        } else {
            std::set<std::string> seen;
            for (auto& id : raw) {
                std::string clean = trim(id);
                if (clean.empty() || seen.count(clean)) continue;
                seen.insert(clean);
                account_ids.push_back(clean);
            }
        }
        if (account_ids.empty()) return fail(i18n::t(lang, "quick_pick_account"));
        auto result = control_.cancel_manual_orders(account_ids, symbol, trim(in.str_or("order_id", "")));
        bool ok = result.at("errors").empty() && result.at("unresolved") == 0;
        std::string message = "Manual cancellations queued: " + std::to_string(result.at("queued_commands").size()) +
            "; algo orders excluded: " + std::to_string(result.at("skipped_algo").get<int>()) +
            "; already pending: " + std::to_string(result.at("already_pending").get<int>()) +
            "; unresolved: " + std::to_string(result.at("unresolved").get<int>()) +
            "; publication errors: " + std::to_string(result.at("errors").size()) +
            ". Orders remain open until broker confirmation.";
        return Outcome{ok, message, result, ok ? 200 : 409};
    };
    // Admin Control: one read-only, multi-account request for a day's broker orders. Only the lmh
    // account may use it; the request can be queued while algorithms own an account.
    auto do_broker_orders = [=, this](const RequestInput& in, const std::string& lang, const std::string& user) -> Outcome {
        if (!control_.config().can_access_admin_control(user)) return fail(i18n::t(lang, "admin_control_forbidden"), 403);
        orders::BrokerOrdersMessages m{i18n::t(lang, "broker_orders_invalid_date"), i18n::t(lang, "broker_orders_future_date"),
                                       i18n::t(lang, "invalid_account"), i18n::t(lang, "quick_pick_account")};
        orders::Validated v = orders::validate_broker_orders_inputs(in.str_or("trading_date", ""), in.list("account_ids"), control_.accounts(), m);
        if (!v.ok()) return fail(v.error);
        try {
            if (!control_.command_sink()) throw std::runtime_error("Trading command producer unavailable");
            control_.publish_control_command(*v.command, "GET_BROKER_ORDERS");
        } catch (const std::exception& e) {
            NG_LOG_ERROR(kLog, "Failed to queue broker order history request: {}", e.what());
            return fail(i18n::t(lang, "broker_orders_publish_failed"), 503);
        }
        std::string ok = i18n::format_named(i18n::t(lang, "broker_orders_queued"),
                                            {{"date", v.command->at("trading_date").get<std::string>()},
                                             {"count", std::to_string(v.command->at("account_ids").size())}});
        Outcome out{true, ok + " " + i18n::t(lang, "published_cmd") + "=" + v.command->at("command_id").get<std::string>()};
        out.extra["command"] = *v.command;
        return out;
    };
    auto do_trading_status = [=, this](const RequestInput& in, const std::string& lang, const std::string& user) -> Outcome {
        if (!control_.config().is_privileged(user)) return fail(i18n::t(lang, "trading_status_forbidden"), 403);
        bool disable_all = in.truthy("disable_all");
        std::string enabled_raw = in.str_or("trading_enabled", "");
        std::vector<std::string> account_ids = disable_all ? [=, this] {
            std::vector<std::string> ids;
            for (auto& m : control_.accounts().ordered()) ids.push_back(m.id);
            return ids;
        }() : std::vector<std::string>{in.str_or("account_id", "")};
        orders::TradingStatusMessages m{i18n::t(lang, "invalid_account"), i18n::t(lang, "invalid_trading_status")};
        std::vector<json> commands;
        std::set<std::string> seen_accounts;
        int published = 0;
        std::string first_id;
        for (const auto& account_id : account_ids) {
            if (!seen_accounts.insert(trim(account_id)).second) return fail("Duplicate account IDs are not allowed");
            orders::Validated v = orders::validate_trading_status_inputs(account_id, enabled_raw, control_.accounts(), m);
            if (!v.ok()) return fail(v.error);
            commands.push_back(control_.normalize_trading_command(*v.command));
            ++published;
            if (first_id.empty()) first_id = v.command->at("command_id").get<std::string>();
        }
        json queued=json::array();
        for (const auto& command:commands) {
            try { control_.publish_control_command(command,command.at("account_id").get<std::string>()); }
            catch (const std::exception&) {
                return Outcome{false,"Batch publication interrupted; inspect queued and uncertain command IDs before retrying",
                    {{"queued_commands",queued},{"uncertain_command",command},{"requested_count",commands.size()}},503};
            }
            queued.push_back(command);
        }
        return Outcome{true, i18n::t(lang, disable_all ? "trading_disabled_all" : "trading_status_submitted") + " " + std::to_string(published) + ". " +
                                 i18n::t(lang, "published_cmd") + "=" + first_id};
    };
    auto do_algo_start = [=, this](const std::string& trading_mode, const std::string& symbol, const json& fast_config, const std::string& lang) -> Outcome {
        orders::Validated v = orders::validate_algo_start_inputs(trading_mode, symbol, fast_config, control_.accounts(), algo_messages(lang));
        if (!v.ok()) return fail(v.error);
        std::string key = control_.start_fast_strategy(*v.command);
        Outcome out{true, i18n::t(lang, "algo_submitted") + " " + key + ". " + i18n::t(lang, "published_cmd") + "=" + v.command->at("command_id").get<std::string>()};
        out.extra["command"] = *v.command;
        out.extra["strategy_key"] = key;
        return out;
    };
    auto do_algo_stop = [=, this](const std::optional<std::string>& reason, const std::string& lang) -> Outcome {
        StopAllResult result = control_.stop_all_algorithms(reason);
        std::string ok = i18n::format_named(i18n::t(lang, "algo_stop_submitted"), {{"count", std::to_string(result.fast_stopped)}});
        if (result.smart_stopped) {
            ok += lang == "zh" ? " 已请求停止 " + std::to_string(result.smart_stopped) + " 个智能卖出任务，等待订单终态确认。"
                               : " Stop requested for " + std::to_string(result.smart_stopped) + " Smart selling parent(s); awaiting final order confirmations.";
        }
        if (result.buying_stopped) {
            ok += lang == "zh" ? " 已请求停止 " + std::to_string(result.buying_stopped) + " 个智能买入任务，等待订单终态确认。"
                               : " Stop requested for " + std::to_string(result.buying_stopped) + " Smart buying parent(s); awaiting final order confirmations.";
        }
        Outcome out{true, ok};
        if (result.notification_published) {
            out.message += " " + i18n::t(lang, "published_cmd") + "=" + result.notification.at("command_id").get<std::string>();
        } else {
            out.extra["warning"] = i18n::t(lang, "algo_stop_notification_failed");
        }
        out.extra["fast_stopped"] = result.fast_stopped;
        out.extra["smart_stopped"] = result.smart_stopped;
        out.extra["buying_stopped"] = result.buying_stopped;
        return out;
    };

    // Convert an Outcome into a JSON response.
    auto respond = [=, this](Response& res, const Outcome& outcome, int error_status = 400) {
        if (!outcome.ok) {
            json out=outcome.extra;
            out["ok"]=false;
            out["error"]=outcome.message;
            json_response(res,out,outcome.status != 400 ? outcome.status : error_status);
            return;
        }
        json out = outcome.extra;
        out["ok"] = true;
        out["message"] = outcome.message;
        json_response(res, out);
    };
    // Run a handler, mapping validation exceptions to 400 and anything else to 500.
    auto guarded = [=, this](Response& res, const std::function<Outcome()>& fn, int validation_status = 400) {
        try {
            respond(res, fn(), validation_status);
        } catch (const std::invalid_argument& e) {
            api_error(res, e.what(), validation_status);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    };
    // Legacy form flow: redirect back to a page with ?ok= or ?err=.
    auto legacy = [=, this](const Request& req, Response& res, const std::string& page, const std::function<Outcome()>& fn) {
        auto user = current_user(req);
        if (!user) {
            redirect(res, "/login?next=" + url_encode(req.path));
            return;
        }
        if (!same_origin(req)) {
            redirect(res, page + "?err=" + url_encode("Cross-origin submission is not allowed"));
            return;
        }
        try {
            Outcome outcome = fn();
            if (!outcome.ok) {
                redirect(res, page + "?err=" + url_encode(outcome.message));
                return;
            }
            std::string url = page + "?ok=" + url_encode(outcome.message);
            if (outcome.extra.contains("warning")) url += "&err=" + url_encode(outcome.extra.at("warning").get<std::string>());
            redirect(res, url);
        } catch (const std::exception& e) {
            redirect(res, page + "?err=" + url_encode(e.what()));
        }
    };

    // --- static & pages ----------------------------------------------------------------------
    svr.Get("/", [=, this](const Request&, Response& res) { redirect(res, "/account-details", 302); });
    for (const char* page : {"/account-details", "/control-panel", "/algo", "/market-insights", "/working-orders", "/trading-status", "/login"}) {
        svr.Get(page, [serve_app](const Request&, Response& res) { serve_app(res); });
    }
    // Admin Control and Market Data are only available to the lmh account: other signed-in users get
    // HTTP 403, anonymous visitors get the app (which shows the sign-in form).
    auto restricted_page = [=, this](const Request& req, Response& res, bool allowed, const char* message_key) {
        auto user = current_user(req);
        if (user && !allowed) {
            res.status = 403;
            res.set_header("Cache-Control", "no-store");
            res.set_content(i18n::t(lang_of(req), message_key), "text/plain; charset=utf-8");
            return;
        }
        serve_app(res);
    };
    svr.Get("/admin-control", [=, this](const Request& req, Response& res) {
        auto user = current_user(req);
        restricted_page(req, res, user && control_.config().can_access_admin_control(*user), "admin_control_forbidden");
    });
    svr.Get("/market-data", [=, this](const Request& req, Response& res) {
        auto user = current_user(req);
        restricted_page(req, res, user && control_.config().can_access_market_data(*user), "md_forbidden");
    });
    // The former Currency Conversion tab was replaced by Admin Control; its page and submission
    // routes no longer exist (404).
    svr.Get("/favicon.svg", [serve_asset](const Request&, Response& res) { serve_asset("favicon.svg", res, false); });
    svr.Get("/favicon.ico", [serve_asset](const Request&, Response& res) { serve_asset("favicon.svg", res, false); });
    svr.Get(R"(/static/(.+))", [serve_asset](const Request& req, Response& res) {
        std::string rel = req.matches[1];
        if (rel.find("..") != std::string::npos) {
            res.status = 404;
            res.set_content("Not found", "text/plain");
            return;
        }
        serve_asset(rel, res, ends_with(rel, ".html"));
    });

    // --- authentication ------------------------------------------------------------------------
    svr.Get("/set-lang", [=, this](const Request& req, Response& res) {
        std::string lang = req.get_param_value("lang");
        if (!i18n::is_supported(lang)) lang = "en";
        std::string next = req.has_param("next") ? req.get_param_value("next") : "/account-details";
        if (next.empty() || next[0] != '/' || starts_with(next, "//")) next = "/account-details";
        res.set_header("Set-Cookie", "lang=" + lang + "; Path=/; Max-Age=15552000; SameSite=Lax");
        redirect(res, next, 302);
    });
    svr.Post("/login", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        std::string username = trim(in.str_or("username", ""));
        std::string password = in.str_or("password", "");
        std::string next = in.str_or("next", "/account-details");
        if (next.empty() || next[0] != '/' || starts_with(next, "//")) next = "/account-details";
        bool as_json = wants_json(req) || in.has_json();
        if (!sessions_.verify_credentials(control_.config().users, username, password)) {
            NG_LOG_WARN(kLog, "failed login attempt user={} from={}", username, req.remote_addr);
            if (as_json) api_error(res, i18n::t(lang, "login_failed"), 401);
            else redirect(res, "/login?next=" + url_encode(next) + "&err=" + url_encode(i18n::t(lang, "login_failed")));
            return;
        }
        std::string token = sessions_.create(username);
        res.set_header("Set-Cookie", std::string(kAuthCookie) + "=" + token + "; Max-Age=43200" + cookie_flags());
        NG_LOG_INFO(kLog, "login user={} from={}", username, req.remote_addr);
        if (as_json) json_response(res, json{{"ok", true}, {"user", username}, {"next", next}});
        else redirect(res, next);
    });
    auto do_logout = [=, this](const Request& req, Response& res, bool as_json) {
        std::string token = cookie_value(req, kAuthCookie);
        if (!token.empty()) sessions_.revoke(token);
        res.set_header("Set-Cookie", std::string(kAuthCookie) + "=; Max-Age=0" + cookie_flags());
        if (as_json) json_response(res, json{{"ok", true}});
        else redirect(res, "/login", 302);
    };
    svr.Get("/logout", [=, this](const Request& req, Response& res) { do_logout(req, res, false); });
    svr.Post("/api/logout", [=, this](const Request& req, Response& res) { do_logout(req, res, true); });

    svr.Get("/api/session", [=, this](const Request& req, Response& res) {
        auto user = current_user(req);
        json out{{"ok", true}, {"authenticated", user.has_value()}, {"lang", lang_of(req)}, {"server_time", iso_utc_now()}, {"version", NG_VERSION}};
        if (user) {
            out["user"] = *user;
            out["privileged"] = control_.config().is_privileged(*user);
            out["can_access_admin_control"] = control_.config().can_access_admin_control(*user);
            out["can_access_market_data"] = control_.config().can_access_market_data(*user);
            out["can_stop_all_algorithms"] = control_.config().can_stop_all_algorithms(*user);
        }
        json_response(res, out, user ? 200 : 401);
    });
    svr.Get("/api/i18n", [=, this](const Request& req, Response& res) {
        std::string lang = lang_of(req);
        res.set_header("Cache-Control", "no-cache");
        res.set_content(json_dump_compact(i18n::dictionary(lang)), kJson);
    });

    // --- read APIs ------------------------------------------------------------------------------
    svr.Get("/api/working-orders", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        json_response(res, control_.working_orders());
    });
    svr.Get("/api/bootstrap", [=, this](const Request& req, Response& res) {
        auto user = require_api_user(req, res);
        if (!user) return;
        json_response(res, control_.bootstrap(lang_of(req), *user));
    });
    svr.Get("/api/accounts", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.accounts_payload_text(lang_of(req)));
    });
    svr.Get("/api/accounts/view", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.accounts_view_text(lang_of(req)));
    });
    // Five-minute market and own-volume windows for one publisher symbol (lmh only, never cached).
    svr.Get("/api/market-data", [=, this](const Request& req, Response& res) {
        auto user = require_api_user(req, res);
        if (!user) return;
        std::string lang = lang_of(req);
        if (!control_.config().can_access_market_data(*user)) {
            api_error(res, i18n::t(lang, "md_forbidden"), 403);
            return;
        }
        std::vector<std::string> symbols = control_.config().market_data_symbols();
        std::string symbol = to_upper(trim(req.has_param("symbol") ? req.get_param_value("symbol") : (symbols.empty() ? "" : symbols.front())));
        if (symbol.empty() || std::find(symbols.begin(), symbols.end(), symbol) == symbols.end()) {
            api_error(res, i18n::t(lang, "invalid_symbol"), 400);
            return;
        }
        if (!control_.market_activity() || !control_.market_order_store()) {
            api_error(res, i18n::t(lang, "md_unavailable"), 503);
            return;
        }
        json_response(res, control_.market_activity_payload(symbol));
    });
    // Live last-trade quotes with previous close and daily volume for trading forms and price lookups.
    svr.Get("/api/quotes", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        std::optional<std::string> symbol;
        if (req.has_param("symbol")) symbol = req.get_param_value("symbol");
        if (symbol && !trim(*symbol).empty()) json_response(res, control_.quotes_payload(symbol));
        else text_response(res, control_.quotes_payload_text());
    });
    svr.Get("/api/market-insights", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        std::string symbol = to_upper(trim(req.get_param_value("symbol")));
        if (symbol.empty()) {
            api_error(res, i18n::t(lang_of(req), "invalid_symbol"), 400);
            return;
        }
        int depth = 20;
        if (req.has_param("depth")) {
            try {
                depth = std::stoi(req.get_param_value("depth"));
            } catch (...) {
                api_error(res, "depth must be an integer", 400);
                return;
            }
        }
        text_response(res, control_.insights_payload_text(symbol, depth));
    });
    svr.Get("/api/algo", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.algo_status_text());
    });
    svr.Get("/api/health", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.health_text());
    });
    svr.Get("/api/commands", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.commands_history_text());
    });
    svr.Get("/api/operations", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        json out{{"ok", true}};
        out["document"] = control_.operations_log() ? control_.operations_log()->read_document() : json(nullptr);
        out["path"] = control_.operations_log() ? json(control_.operations_log()->path()) : json(nullptr);
        json_response(res, out);
    });
    svr.Get("/api/smart-selling", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.smart_status_text());
    });
    svr.Get(R"(/api/smart-selling/([^/]+)/events)", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.smart_runtime()) {
            api_error(res, "Smart selling is unavailable", 503);
            return;
        }
        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        try {
            json out{{"ok", true}};
            out["events"] = control_.smart_runtime()->engine().events(req.matches[1], limit);
            json_response(res, out);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });

    // --- write APIs -------------------------------------------------------------------------
    svr.Post("/api/orders/market", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_market_order(in, lang); });
    });
    svr.Post("/api/orders/quick", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_quick_order(in, lang); });
    });
    svr.Post("/api/orders/limit", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_limit_order(in, lang); });
    });
    svr.Post("/api/orders/cancel", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_cancel(in, lang); });
    });
    svr.Post("/api/broker-orders", [=, this](const Request& req, Response& res) {
        auto user = require_api_user(req, res);
        if (!user) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_broker_orders(in, lang, *user); });
    });
    svr.Post("/api/trading-status", [=, this](const Request& req, Response& res) {
        auto user = require_api_user(req, res);
        if (!user) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_trading_status(in, lang, *user); });
    });
    svr.Post("/api/algo/stop", [=, this](const Request& req, Response& res) {
        auto user = require_api_user(req, res);
        if (!user) return;
        if (!control_.config().can_stop_all_algorithms(*user)) {
            api_error(res, i18n::t(lang_of(req), "algo_stop_all_forbidden"), 403); return;
        }
        RequestInput in(req);
        std::string lang = lang_of(req);
        guarded(res, [=, this] { return do_algo_stop(in.str("reason"), lang); });
    });
    svr.Post(R"(/api/algo/fast/([^/]+)/stop)", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        try { json_response(res, control_.stop_fast_session(req.matches[1])); }
        catch (const std::out_of_range& e) { api_error(res, e.what(), 404); }
        catch (const std::exception& e) { api_error(res, e.what(), 500); }
    });

    // Legacy JSON APIs (same request/response shapes as the Python server).
    svr.Post("/api/submit-order", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        try {
            Outcome outcome = do_market_order(in, lang);
            if (!outcome.ok) {
                api_error(res, outcome.message, 400);
                return;
            }
            json_response(res, json{{"ok", true}, {"command", outcome.extra.at("command")}});
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });
    svr.Post("/api/submit-algo", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        json body = in.has_json() ? in.body() : json::object();
        if (!in.has_json()) {
            for (const char* key : {"trading_mode", "symbol", "fast_trading_config"}) {
                if (auto v = in.str(key)) body[key] = *v;
            }
        }
        std::string mode = json_to_string(json_find(body, "trading_mode")).value_or("");
        std::string symbol = json_to_string(json_find(body, "symbol")).value_or("");
        json fast_config = orders::extract_fast_trading_config(body);
        try {
            Outcome outcome = do_algo_start(mode, symbol, fast_config, lang);
            if (!outcome.ok) {
                api_error(res, outcome.message, 400);
                return;
            }
            json_response(res, json{{"ok", true}, {"command", outcome.extra.at("command")}, {"strategy_key", outcome.extra.at("strategy_key")}});
        } catch (const std::invalid_argument& e) {
            api_error(res, e.what(), 400);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });
    auto account_order = [=, this](const Request& req, Response& res, bool limit) {
        if (!require_api_user(req, res)) return;
        RequestInput in(req);
        std::string lang = lang_of(req);
        std::vector<std::string> ids = in.list("account_id");
        if (ids.size() != 1) {
            api_error(res, i18n::t(lang, "invalid_account"), 400);
            return;
        }
        std::string account_id = ids[0];
        std::string symbol = to_upper(trim(in.str_or("symbol", "")));
        std::string side = in.str_or("side", "BUY");
        std::optional<std::string> shares = trim(in.str_or("shares", ""));
        orders::Validated v;
        try {
            if (limit) {
                std::optional<std::string> limit_price = trim(in.str_or("limit_price", ""));
                std::optional<std::string> through = trim(in.str_or("through_market_pct", ""));
                std::optional<double> last;
                if (limit_price->empty()) last = control_.market_last(symbol);
                v = orders::validate_limit_order_inputs(account_id, symbol, side, shares, limit_price, through, last, control_.accounts(), limit_messages(lang));
            } else {
                orders::OrderMessages m = order_messages(lang);
                m.both_shares_and_dollars = m.shares_positive;
                m.neither_shares_nor_dollars = m.shares_positive;
                v = orders::validate_order_inputs(account_id, symbol, side, shares, std::nullopt, control_.accounts(), m);
            }
            if (v.ok()) v.command=control_.normalize_trading_command(*v.command);
        } catch (const std::exception&) {
            api_error(res, i18n::t(lang, limit ? "price_positive" : "shares_positive"), 400);
            return;
        }
        if (!v.ok()) {
            api_error(res, v.error, 400);
            return;
        }
        if (!control_.command_sink()) {
            api_error(res, i18n::t(lang, "publish_failed"), 503);
            return;
        }
        try {
            control_.publish_control_command(*v.command, account_id);
        } catch (const std::exception& e) {
            std::string message = e.what();
            api_error(res, message.empty() ? i18n::t(lang, "publish_failed") : message, 500);
            return;
        }
        std::string command_id = v.command->at("command_id").get<std::string>();
        json response{{"ok",true},{"message",i18n::t(lang,"order_submitted")+". "+i18n::t(lang,"published_cmd")+"="+command_id},{"command_id",command_id}};
        if (limit) response["limit_price"]=v.command->at("limit_price");
        json_response(res,response);
    };
    svr.Post("/api/account-orders/quick-market", [=, this](const Request& req, Response& res) { account_order(req, res, false); });
    svr.Post("/api/account-orders/limit", [=, this](const Request& req, Response& res) { account_order(req, res, true); });

    svr.Post("/api/smart-selling", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.smart_runtime()) {
            api_error(res, "Smart selling is unavailable", 503);
            return;
        }
        RequestInput in(req);
        try {
            json payload = in.has_json() ? in.body() : json::parse(req.body);
            json parent = control_.start_smart_parent(payload);
            json_response(res, json{{"ok", true}, {"parent", parent}});
        } catch (const std::invalid_argument& e) {
            api_error(res, e.what(), 400);
        } catch (const json::exception& e) {
            api_error(res, e.what(), 400);
        } catch (const DecimalError& e) {
            api_error(res, e.what(), 400);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });
    svr.Post(R"(/api/smart-selling/([^/]+)/stop)", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.smart_runtime()) {
            api_error(res, "Smart selling is unavailable", 503);
            return;
        }
        try {
            json_response(res, json{{"ok", true}, {"parent", control_.stop_smart_parent(req.matches[1])}});
        } catch (const smart::KeyError&) {
            api_error(res, "Unknown Smart selling parent", 404);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });

    svr.Get("/api/smart-buying", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        text_response(res, control_.smart_buying_status_text());
    });
    svr.Get(R"(/api/smart-buying/([^/]+)/events)", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.buying_runtime()) {
            api_error(res, "Smart buying is unavailable", 503);
            return;
        }
        int limit = 100;
        if (req.has_param("limit")) {
            try { limit = std::stoi(req.get_param_value("limit")); } catch (...) {}
        }
        try {
            json out{{"ok", true}};
            out["events"] = control_.buying_runtime()->engine().events(req.matches[1], limit);
            json_response(res, out);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });
    svr.Post("/api/smart-buying", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.buying_runtime()) {
            api_error(res, "Smart buying is unavailable", 503);
            return;
        }
        RequestInput in(req);
        try {
            json payload = in.has_json() ? in.body() : json::parse(req.body);
            json parent = control_.start_smart_buying_parent(payload);
            json_response(res, json{{"ok", true}, {"parent", parent}});
        } catch (const std::invalid_argument& e) {
            api_error(res, e.what(), 400);
        } catch (const json::exception& e) {
            api_error(res, e.what(), 400);
        } catch (const DecimalError& e) {
            api_error(res, e.what(), 400);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });
    svr.Post(R"(/api/smart-buying/([^/]+)/stop)", [=, this](const Request& req, Response& res) {
        if (!require_api_user(req, res)) return;
        if (!control_.buying_runtime()) {
            api_error(res, "Smart buying is unavailable", 503);
            return;
        }
        try {
            json_response(res, json{{"ok", true}, {"parent", control_.stop_smart_buying_parent(req.matches[1])}});
        } catch (const smart::KeyError&) {
            api_error(res, "Unknown Smart buying parent", 404);
        } catch (const std::exception& e) {
            api_error(res, e.what(), 500);
        }
    });

    // --- legacy form endpoints (redirect flow) ------------------------------------------------
    svr.Post("/submit-order", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/control-panel", [=, this] { return do_market_order(in, lang); });
    });
    svr.Post("/submit-quick-order", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/control-panel", [=, this] { return do_quick_order(in, lang); });
    });
    svr.Post("/submit-limit-order", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/control-panel", [=, this] { return do_limit_order(in, lang); });
    });
    svr.Post("/submit-cancel-open-orders", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/control-panel", [=, this] { return do_cancel(in, lang); });
    });
    svr.Post("/submit-broker-orders", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        auto user = current_user(req);
        if (user && !control_.config().can_access_admin_control(*user)) {
            res.status = 403;
            res.set_header("Cache-Control", "no-store");
            res.set_content(i18n::t(lang, "admin_control_forbidden"), "text/plain; charset=utf-8");
            return;
        }
        legacy(req, res, "/admin-control", [=, this] { return do_broker_orders(in, lang, user.value_or("")); });
    });
    svr.Post("/submit-trading-status", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        auto user = current_user(req);
        legacy(req, res, "/trading-status", [=, this] { return do_trading_status(in, lang, user.value_or("")); });
    });
    svr.Post("/submit-algo", [=, this](const Request& req, Response& res) {
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/control-panel", [=, this] {
            json fast_config = in.str("fast_trading_config") ? json(*in.str("fast_trading_config")) : json(nullptr);
            return do_algo_start(in.str_or("trading_mode", ""), in.str_or("symbol", ""), fast_config, lang);
        });
    });
    svr.Post("/submit-algo-stop", [=, this](const Request& req, Response& res) {
        auto user = current_user(req);
        if (user && !control_.config().can_stop_all_algorithms(*user)) {
            api_error(res, i18n::t(lang_of(req), "algo_stop_all_forbidden"), 403); return;
        }
        RequestInput in(req);
        std::string lang = lang_of(req);
        legacy(req, res, "/algo", [=, this] { return do_algo_stop(in.str("reason"), lang); });
    });
    for (const char* path : {"/submit-order", "/submit-quick-order", "/submit-limit-order", "/submit-cancel-open-orders", "/submit-algo",
                             "/submit-algo-stop"}) {
        svr.Get(path, [=, this](const Request&, Response& res) { redirect(res, "/control-panel", 302); });
    }
    // A history request is only ever queued by an explicit POST.
    svr.Get("/submit-broker-orders", [](const Request&, Response& res) {
        res.status = 405;
        res.set_header("Allow", "POST");
        res.set_content("Method Not Allowed", "text/plain; charset=utf-8");
    });
    svr.Get("/submit-trading-status", [=, this](const Request&, Response& res) { redirect(res, "/trading-status", 302); });

    // --- server-sent events ----------------------------------------------------------------------
    auto stream_handler = [=, this](const Request& req, Response& res, unsigned forced_topics) {
        auto user = current_user(req);
        if (!user) {
            api_error(res, "Unauthorized", 401);
            return;
        }
        unsigned topics = forced_topics;
        if (!topics) {
            std::string list = req.has_param("topics") ? req.get_param_value("topics") : "accounts,quotes,smart,algo,feeds,commands";
            for (auto& name : split(list, ',')) {
                auto it = topic_names().find(trim(name));
                if (it != topic_names().end()) topics |= it->second;
            }
        }
        auto ctx = std::make_shared<StreamContext>();
        ctx->lang = lang_of(req);
        ctx->token = cookie_value(req, kAuthCookie);
        ctx->book_symbol = to_upper(trim(req.get_param_value("book")));
        if (req.has_param("depth")) {
            try { ctx->depth = std::stoi(req.get_param_value("depth")); } catch (...) {}
        }
        if (ctx->book_symbol.empty()) topics &= ~kTopicBook;
        if (!topics) {
            api_error(res, "no topics selected", 400);
            return;
        }
        ctx->subscriber = control_.hub().subscribe(topics, ctx->book_symbol);
        ctx->last_auth_check = mono_now();
        res.set_header("Cache-Control", "no-cache, no-transform");
        res.set_header("X-Accel-Buffering", "no");
        res.set_header("Connection", "keep-alive");
        auto payload_for = [this, ctx](unsigned topic) -> std::string {
            switch (topic) {
                case kTopicAccounts: return *control_.accounts_view_text(ctx->lang);
                case kTopicQuotes: return *control_.quotes_payload_text();
                case kTopicBook: return *control_.insights_payload_text(ctx->book_symbol, ctx->depth);
                case kTopicSmart: return *control_.smart_status_text();
                case kTopicAlgo: return *control_.algo_status_text();
                case kTopicFeeds: return *control_.feeds_health_text();
                case kTopicCommands: return *control_.commands_history_text();
            }
            return "{}";
        };
        auto send_topic = [this, ctx, payload_for](httplib::DataSink& sink, unsigned topic, bool force) -> bool {
            std::string payload = payload_for(topic);
            if (!force) {
                auto it = ctx->last_payload.find(topic);
                if (it != ctx->last_payload.end() && it->second == payload) return true;
            }
            ctx->last_payload[topic] = payload;
            ctx->last_sent[topic] = mono_now();
            std::string frame = "event: " + std::string(topic_event_name(topic)) + "\nid: " + std::to_string(++ctx->event_id) + "\ndata: " + payload + "\n\n";
            return sink.write(frame.data(), frame.size());
        };
        res.set_chunked_content_provider(
            "text/event-stream",
            [this, ctx, send_topic](std::size_t, httplib::DataSink& sink) -> bool {
                if (ctx->first) {
                    ctx->first = false;
                    for (unsigned bit = 1; bit <= kTopicCommands; bit <<= 1) {
                        if ((ctx->subscriber->topics() & bit) && !send_topic(sink, bit, true)) return false;
                    }
                    return true;
                }
                bool shutting_down = false;
                unsigned dirty = ctx->subscriber->wait(std::chrono::milliseconds(15000), shutting_down);
                if (shutting_down) {
                    sink.done();
                    return true;
                }
                double now = mono_now();
                if (now - ctx->last_auth_check > 5) {
                    ctx->last_auth_check = now;
                    if (!sessions_.user_for(ctx->token)) {
                        sink.done();
                        return true;
                    }
                }
                if (!dirty) {
                    static const char keep_alive[] = ": keep-alive\n\n";
                    return sink.write(keep_alive, sizeof(keep_alive) - 1);
                }
                for (unsigned bit = 1; bit <= kTopicCommands; bit <<= 1) {
                    if (!(dirty & bit)) continue;
                    auto last = ctx->last_sent.find(bit);
                    if (last != ctx->last_sent.end()) {
                        double elapsed = mono_now() - last->second;
                        double minimum = min_interval_for(bit);
                        if (elapsed < minimum) std::this_thread::sleep_for(std::chrono::duration<double>(minimum - elapsed));
                    }
                    if (!send_topic(sink, bit, false)) return false;
                }
                return true;
            },
            [this, ctx](bool) { control_.hub().unsubscribe(ctx->subscriber); });
    };
    svr.Get("/api/stream", [=, this](const Request& req, Response& res) { stream_handler(req, res, 0); });
    svr.Get("/api/account-details/stream", [=, this](const Request& req, Response& res) { stream_handler(req, res, kTopicAccounts); });

    // --- bind & serve ---------------------------------------------------------------------------
    if (options_.port == 0) {
        bound_port_ = svr.bind_to_any_port(options_.host);
        if (bound_port_ <= 0) throw std::runtime_error("failed to bind HTTP server on " + options_.host);
    } else {
        if (!svr.bind_to_port(options_.host, options_.port)) {
            throw std::runtime_error("failed to bind HTTP server on " + options_.host + ":" + std::to_string(options_.port));
        }
        bound_port_ = options_.port;
    }
    running_.store(true);
    serve_thread_ = std::thread([this] {
        impl_->http->listen_after_bind();
        running_.store(false);
    });
    for (int i = 0; i < 200 && !impl_->http->is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (options_.redirect_port > 0) {
        impl_->redirector = std::make_unique<httplib::Server>();
        // The global default pool is disabled; the redirector needs its own workers.
        impl_->redirector->new_task_queue = [] { return new httplib::ThreadPool(4); };
        std::string target_host = options_.redirect_target_host.empty() ? "127.0.0.1" : options_.redirect_target_host;
        int target_port = options_.redirect_target_port;
        auto handler = [target_host, target_port](const Request& req, Response& res) {
            std::string location = "https://" + target_host;
            if (target_port != 443) location += ":" + std::to_string(target_port);
            location += req.target.empty() ? req.path : req.target;
            res.status = 308;
            res.set_header("Location", location);
            res.set_header("Connection", "close");
            res.set_content("Redirecting to " + location + "\n", "text/plain; charset=utf-8");
        };
        impl_->redirector->Get(R"(.*)", handler);
        impl_->redirector->Post(R"(.*)", handler);
        impl_->redirector->Put(R"(.*)", handler);
        impl_->redirector->Patch(R"(.*)", handler);
        impl_->redirector->Delete(R"(.*)", handler);
        impl_->redirector->Options(R"(.*)", handler);
        if (!impl_->redirector->bind_to_port(options_.redirect_host, options_.redirect_port)) {
            NG_LOG_ERROR(kLog, "failed to bind HTTP redirector on {}:{}", options_.redirect_host, options_.redirect_port);
            impl_->redirector.reset();
        } else {
            redirect_thread_ = std::thread([this] { impl_->redirector->listen_after_bind(); });
            NG_LOG_INFO(kLog, "redirect listening on http://{}:{} -> https://{}:{}", options_.redirect_host, options_.redirect_port, target_host,
                        target_port);
        }
    }
}

}  // namespace ng::web
