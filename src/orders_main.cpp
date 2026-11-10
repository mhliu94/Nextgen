// nextgen-orders: KTrader order monitor (C++ rewrite of order_ui). A separate, read-only server
// that follows the order-status topic and shows today's orders, algorithm-session and tag reports
// and the Smart controllers' unreconciled records.
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "core/config.h"
#include "core/logging.h"
#include "order_ui/app.h"

namespace {

void usage() {
    std::puts(
        "Usage: nextgen-orders [options]\n"
        "\n"
        "Options:\n"
        "  --config PATH      The same configuration file used by nextgen-gui (default: $ORDER_UI_CONFIG,\n"
        "                     then $ACCOUNT_DASHBOARD_CONFIG, then ./config/config.local.json, then ./config/config.json)\n"
        "  --host HOST        Listen address (overrides order_ui.server.host, default 127.0.0.1)\n"
        "  --port PORT        Listen port (overrides order_ui.server.port, default 8030)\n"
        "  --ssl-certfile PATH --ssl-keyfile PATH\n"
        "                     Serve HTTPS with these files (overrides order_ui.server.ssl_certfile/ssl_keyfile)\n"
        "  --no-ssl           Serve plain HTTP even if the configuration names certificate files\n"
        "  --static-dir DIR   Serve web assets from DIR instead of the embedded copies\n"
        "  --without-kafka    Run without the order-status consumer (UI development / demos)\n"
        "  --check-config     Validate configuration and exit\n"
        "  --version          Print version and exit\n"
        "  -h, --help         Show this help\n");
}

}  // namespace

int main(int argc, char** argv) {
    mallopt(M_ARENA_MAX, 4);
    ng::log::configure_from_env();
    ng::order_ui::OrderAppOptions options;
    bool check_only = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--version") {
            std::printf("nextgen-orders %s\n", NG_VERSION);
            return 0;
        } else if (arg == "--config") {
            options.config_path = need_value("--config");
        } else if (arg == "--host") {
            options.host = need_value("--host");
        } else if (arg == "--port") {
            const char* text = need_value("--port");
            char* end = nullptr;
            long port = std::strtol(text, &end, 10);
            if (!*text || *end || port < 1 || port > 65535) {
                std::fprintf(stderr, "--port must be between 1 and 65535\n");
                return 2;
            }
            options.port = static_cast<int>(port);
        } else if (arg == "--ssl-certfile") {
            options.ssl_certfile = need_value("--ssl-certfile");
        } else if (arg == "--ssl-keyfile") {
            options.ssl_keyfile = need_value("--ssl-keyfile");
        } else if (arg == "--no-ssl") {
            options.no_ssl = true;
        } else if (arg == "--static-dir") {
            options.static_dir = need_value("--static-dir");
        } else if (arg == "--without-kafka") {
            options.without_kafka = true;
        } else if (arg == "--check-config") {
            check_only = true;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    if (options.ssl_certfile.has_value() != options.ssl_keyfile.has_value()) {
        std::fprintf(stderr, "--ssl-certfile and --ssl-keyfile must be given together\n");
        return 2;
    }
    if (options.config_path.empty()) {
        std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
        std::filesystem::path config_dir = std::filesystem::current_path() / "config";
        if (!std::filesystem::exists(config_dir) && std::filesystem::exists(exe_dir / ".." / "config")) {
            config_dir = exe_dir / ".." / "config";
        }
        options.config_path = ng::order_ui::default_config_path(config_dir.string());
    }
    try {
        ng::order_ui::OrderApplication app(options);
        if (check_only) {
            ng::order_ui::OrderUiConfig config = ng::order_ui::load_config(options.config_path);
            std::printf("OK: %s (%zu visible accounts, %zu users, order feed %s @ %s, reports %s)\n", config.config_path.c_str(),
                        config.accounts.size(), config.users.size(), config.kafka.topic.c_str(),
                        config.kafka.bootstrap_servers.c_str(), config.reporting_db_path.c_str());
            return 0;
        }
        app.initialise();
        return app.run();
    } catch (const std::exception& e) {
        NG_LOG_ERROR("nextgen-orders", "fatal: {}", e.what());
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
