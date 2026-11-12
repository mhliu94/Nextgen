#include "order_ui/server.h"
#include "core/public_payload.h"

#include <httplib.h>

#include <chrono>
#include <fstream>
#include <sstream>

#include "core/logging.h"
#include "core/strings.h"
#include "core/time_util.h"
#include "web/assets.h"
#include "web/sessions.h"

namespace ng::order_ui {
namespace {

constexpr const char* kLog = "order-ui.http";
constexpr const char* kJson = "application/json";
constexpr const char* kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; frame-ancestors 'none'; "
    "base-uri 'none'; form-action 'self'";

std::string cookie_value(const httplib::Request& req, const std::string& name) {
    for (auto& part : split(req.get_header_value("Cookie"), ';')) {
        std::string item = trim(part);
        std::size_t eq = item.find('=');
        if (eq != std::string::npos && trim(item.substr(0, eq)) == name) return trim(item.substr(eq + 1));
    }
    return "";
}

// FastAPI's error shape: {"detail": "..."}.
void fail(httplib::Response& res, int status, const std::string& detail) {
    res.status = status;
    res.set_content(json_dump_compact(json{{"detail", detail}}), kJson);
}

void respond(httplib::Response& res, const json& payload, int status = 200) {
    res.status = status;
    res.set_content(json_dump_compact(public_payload(payload)), kJson);
}

// A JSON object body whose string fields respect the given length limits; nullopt otherwise (422).
std::optional<json> validated_body(const httplib::Request& req, std::initializer_list<std::pair<const char*, std::size_t>> fields) {
    json body = json::parse(req.body, nullptr, false);
    if (!body.is_object()) return std::nullopt;
    for (const auto& [name, max_length] : fields) {
        const json* value = json_find(body, name);
        if (!value || !value->is_string()) return std::nullopt;
        std::size_t length = utf8_length(value->get<std::string>());
        if (length < 1 || length > max_length) return std::nullopt;
    }
    return body;
}

}  // namespace

json public_algo_parameters(const json& value) {
    return public_payload(value);
}

struct OrderServer::Impl {
    std::unique_ptr<httplib::Server> http;
};

OrderServer::OrderServer(const OrderUiConfig& config, OrderStore& store, reporting::ActivityReporting& reporting,
                         ReconciliationView& reconciliation, OrderServerOptions options, Clock now)
    : config_(config),
      store_(store),
      reporting_(reporting),
      reconciliation_(reconciliation),
      options_(std::move(options)),
      now_(now ? std::move(now) : Clock(&wall_now)),
      impl_(std::make_unique<Impl>()) {}

OrderServer::~OrderServer() { stop(); }

std::optional<std::string> OrderServer::session_user(const std::string& token) {
    std::lock_guard lock(sessions_mutex_);
    auto it = sessions_.find(token);
    if (it == sessions_.end()) return std::nullopt;
    if (it->second.second > now_()) return it->second.first;
    sessions_.erase(it);
    return std::nullopt;
}

std::string OrderServer::create_session(const std::string& username, const std::string& previous_token) {
    double now = now_();
    std::lock_guard lock(sessions_mutex_);
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second.second <= now) it = sessions_.erase(it);
        else ++it;
    }
    sessions_.erase(previous_token);
    std::string token = random_token(32);
    sessions_[token] = {username, now + kSessionSeconds};
    return token;
}

void OrderServer::drop_session(const std::string& token) {
    std::lock_guard lock(sessions_mutex_);
    sessions_.erase(token);
}

json OrderServer::orders_payload(const std::string& username) {
    json snapshot = store_.snapshot();
    std::vector<json> orders(snapshot.at("orders").begin(), snapshot.at("orders").end());
    auto tags = reporting_.order_tags(orders);
    for (json& order : snapshot["orders"]) {
        auto found = tags.find({order.at("account_id").get<std::string>(), order.at("order_id").get<std::string>()});
        order["tags"] = found == tags.end() ? json::array() : found->second;
    }
    json reporting = reporting_.snapshot(config_.accounts);
    for (json& session : reporting["sessions"]) session["parameters"] = public_algo_parameters(session["parameters"]);
    snapshot["username"] = username;
    snapshot["unreconciled"] = reconciliation_.snapshot(config_.accounts);
    snapshot["reporting"] = std::move(reporting);
    snapshot["refresh_interval_seconds"] = config_.refresh_interval_seconds;
    snapshot["statuses"] = orders::order_statuses();
    snapshot["accounts"] = json::array();
    for (const AccountMeta* meta : config_.accounts.by_num_id()) {
        snapshot["accounts"].push_back(json{{"id", meta->id}, {"num_id", meta->num_id}, {"broker", meta->broker}});
    }
    label_sessions(snapshot,reporting_.session_labels());
    return snapshot;
}

void OrderServer::start() {
    using httplib::Request;
    using httplib::Response;

    bool tls = !options_.ssl_certfile.empty();
    if (tls) {
        auto* ssl = new httplib::SSLServer(options_.ssl_certfile.c_str(), options_.ssl_keyfile.c_str());
        impl_->http.reset(ssl);
        if (!ssl->is_valid()) throw std::runtime_error("failed to load the order UI TLS certificate/key");
    } else {
        impl_->http = std::make_unique<httplib::Server>();
    }
    httplib::Server& svr = *impl_->http;
    int threads = std::max(2, options_.threads);
    svr.new_task_queue = [threads] { return new httplib::ThreadPool(static_cast<std::size_t>(threads)); };
    svr.set_keep_alive_max_count(1000);
    svr.set_keep_alive_timeout(30);
    svr.set_read_timeout(30, 0);
    svr.set_write_timeout(60, 0);
    svr.set_payload_max_length(64 * 1024);
    // Every response: never cached, never framed, scripts and styles only from this server.
    svr.set_post_routing_handler([](const Request&, Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Content-Security-Policy", kContentSecurityPolicy);
    });
    svr.set_error_handler([](const Request&, Response& res) {
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("X-Frame-Options", "DENY");
        res.set_header("Content-Security-Policy", kContentSecurityPolicy);
        if (res.body.empty()) res.set_content(json_dump_compact(json{{"detail", res.status == 404 ? "Not Found" : "Error"}}), kJson);
    });
    svr.set_exception_handler([](const Request& req, Response& res, std::exception_ptr ep) {
        std::string message = "internal error";
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
        }
        NG_LOG_ERROR(kLog, "{} {} failed: {}", req.method, req.path, message);
        fail(res, 500, "Internal Server Error");
    });

    auto user_of = [this](const Request& req) { return session_user(cookie_value(req, kSessionCookie)); };
    auto require_user = [user_of](const Request& req, Response& res) -> std::optional<std::string> {
        auto user = user_of(req);
        if (!user) fail(res, 401, "Sign in with your KTrader credentials.");
        return user;
    };
    // Mutating requests must come from this UI: the browser's Origin (when sent) and a header that a
    // cross-site form cannot set.
    auto same_origin = [tls](const Request& req, Response& res) {
        std::string origin = req.get_header_value("Origin");
        if (!origin.empty()) {
            std::string host = req.get_header_value("X-Forwarded-Host");
            if (host.empty()) host = req.get_header_value("Host");
            std::string scheme = req.get_header_value("X-Forwarded-Proto");
            if (scheme.empty()) scheme = tls ? "https" : "http";
            if (origin != scheme + "://" + host) {
                fail(res, 403, "Requests must come from this order UI.");
                return false;
            }
        }
        if (req.get_header_value("X-Order-UI-Request") != "1") {
            fail(res, 403, "Missing order UI request header.");
            return false;
        }
        return true;
    };
    auto serve_asset = [this](const std::string& rel, Response& res) {
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
            fail(res, 404, "Not Found");
            return;
        }
        res.set_content(body, assets::content_type_for(rel));
    };

    svr.Get("/", [serve_asset](const Request&, Response& res) { serve_asset("orders/index.html", res); });
    svr.Get("/favicon.ico", [serve_asset](const Request&, Response& res) { serve_asset("favicon.svg", res); });
    svr.Get(R"(/static/(.+))", [serve_asset](const Request& req, Response& res) {
        std::string rel = req.matches[1];
        if (rel.find("..") != std::string::npos) {
            fail(res, 404, "Not Found");
            return;
        }
        // The order UI's own files, plus the fonts and icon it shares with the trading UI.
        if (starts_with(rel, "fonts/") || rel == "favicon.svg") serve_asset(rel, res);
        else serve_asset("orders/" + rel, res);
    });

    svr.Post("/api/login", [=, this](const Request& req, Response& res) {
        auto body = validated_body(req, {{"username", 256}, {"password", 4096}});
        if (!body) {
            fail(res, 422, "Invalid sign-in request.");
            return;
        }
        if (!same_origin(req, res)) return;
        std::string username = python_strip(body->at("username").get<std::string>());
        auto record = config_.users.find(username);
        if (record == config_.users.end() || !web::compare_digest(record->second, body->at("password").get<std::string>())) {
            fail(res, 401, "Incorrect username or password.");
            return;
        }
        std::string token = create_session(username, cookie_value(req, kSessionCookie));
        std::string cookie = std::string(kSessionCookie) + "=" + token + "; HttpOnly; Max-Age=" + std::to_string(kSessionSeconds) +
                             "; Path=/; SameSite=strict";
        if (tls || req.get_header_value("X-Forwarded-Proto") == "https") cookie += "; Secure";
        res.set_header("Set-Cookie", cookie);
        respond(res, json{{"ok", true}});
    });
    svr.Post("/api/logout", [=, this](const Request& req, Response& res) {
        if (!same_origin(req, res)) return;
        drop_session(cookie_value(req, kSessionCookie));
        res.set_header("Set-Cookie", std::string(kSessionCookie) + "=\"\"; expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0; Path=/; SameSite=lax");
        respond(res, json{{"ok", true}});
    });
    svr.Get("/api/orders", [=, this](const Request& req, Response& res) {
        auto user = require_user(req, res);
        if (!user) return;
        respond(res, orders_payload(*user));
    });
    svr.Post("/api/tags", [=, this](const Request& req, Response& res) {
        auto body = validated_body(req, {{"name", 100}, {"symbol", 64}});
        if (!body) {
            fail(res, 422, "Enter a tag name and valid symbol within the field limits.");
            return;
        }
        if (!require_user(req, res)) return;
        if (!same_origin(req, res)) return;
        try {
            std::string tag_id = reporting_.start_tag(body->at("name").get<std::string>(), body->at("symbol").get<std::string>());
            respond(res, json{{"id", tag_id}}, 201);
        } catch (const std::invalid_argument& e) {
            std::string message = e.what();
            fail(res, message.find("active tag") != std::string::npos ? 409 : 422, message);
        }
    });
    svr.Post(R"(/api/tags/([^/]+)/stop)", [=, this](const Request& req, Response& res) {
        if (!require_user(req, res)) return;
        if (!same_origin(req, res)) return;
        try {
            reporting_.stop_tag(url_decode(req.matches[1].str()));
            respond(res, json{{"ok", true}});
        } catch (const reporting::KeyError&) {
            fail(res, 404, "Tag not found.");
        }
    });

    if (options_.port == 0) {
        bound_port_ = svr.bind_to_any_port(options_.host);
        if (bound_port_ <= 0) throw std::runtime_error("failed to bind the order UI on " + options_.host);
    } else {
        if (!svr.bind_to_port(options_.host, options_.port)) {
            throw std::runtime_error("failed to bind the order UI on " + options_.host + ":" + std::to_string(options_.port));
        }
        bound_port_ = options_.port;
    }
    running_.store(true);
    serve_thread_ = std::thread([this] {
        impl_->http->listen_after_bind();
        running_.store(false);
    });
    for (int i = 0; i < 200 && !impl_->http->is_running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void OrderServer::stop() {
    if (impl_->http) impl_->http->stop();
    if (serve_thread_.joinable()) serve_thread_.join();
    running_.store(false);
    std::lock_guard lock(sessions_mutex_);
    sessions_.clear();
}

}  // namespace ng::order_ui
