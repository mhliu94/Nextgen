// nextgen-gui: KTrader trading control server (C++ rewrite of trading_ui).
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <fstream>
#include <map>
#include <sstream>

#include "app.h"
#include "core/config.h"
#include "core/logging.h"
#include "ops/reconcile.h"
#include "order_ui/config.h"

namespace {

void usage() {
    std::puts(
        "Usage: nextgen-gui [options]\n"
        "\n"
        "Options:\n"
        "  --config PATH            Configuration file (default: $ACCOUNT_DASHBOARD_CONFIG,\n"
        "                           then ./config/config.local.json, then ./config/config.json)\n"
        "  --host HOST              Listen address (overrides server.host / UI_HOST)\n"
        "  --port PORT              Listen port (overrides server.port / UI_PORT)\n"
        "  --ssl | --no-ssl         Force HTTPS on/off (overrides server.ssl_enabled / UI_SSL_ENABLED)\n"
        "  --ssl-certfile PATH      TLS certificate (PEM)\n"
        "  --ssl-keyfile PATH       TLS private key (PEM)\n"
        "  --redirect-port PORT     Also run a plain-HTTP -> HTTPS redirector on PORT (0 = off)\n"
        "  --redirect-host HOST     Redirector bind address (default 0.0.0.0)\n"
        "  --redirect-target HOST   Host name used in redirect Location headers\n"
        "  --threads N              HTTP worker threads (default: automatic)\n"
        "  --static-dir DIR         Serve web assets from DIR instead of the embedded copies\n"
        "  --without-kafka          Run without any Kafka connection (UI development / demos)\n"
        "  --check-config           Validate configuration and exit\n"
        "  --persist-orders         Archive one trading day's orders from the retained order-status topic and exit\n"
        "  --date YYYY-MM-DD        Day to archive with --persist-orders (default: today in persistence.order_archive.timezone)\n"
        "  --version                Print version and exit\n"
        "  -h, --help               Show this help\n"
        "\n"
        "Operator utilities (preview by default; nothing is published, cancelled or restarted):\n"
        "  nextgen-gui reconcile-prior-day --config PATH --reason TEXT [--apply --backup-dir DIR]\n"
        "      Mark Smart selling/buying external orders and commands dated before today's trading day\n"
        "      as reconciled. --apply backs the stores up into DIR (which must not exist) first.\n"
        "  nextgen-gui reconcile-stopped-child --state-db PATH --feedback-db PATH --client-order-id ID\n"
        "                                      --snapshot-file FILE --source REF [--apply]\n"
        "      Resolve the outstanding child of a stopped Smart session against a reviewed final\n"
        "      order-status snapshot (JSON file) that is already the latest durable broker feedback.\n");
}

// Parses "--name value" pairs and bare flags for the operator utilities.
struct UtilityArgs {
    std::map<std::string, std::string> values;
    bool apply = false;
};

UtilityArgs parse_utility_args(int argc, char** argv, const std::vector<std::string>& names) {
    UtilityArgs args;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--apply") {
            args.apply = true;
            continue;
        }
        bool known = false;
        for (const auto& name : names) known = known || arg == "--" + name;
        if (!known) {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            std::exit(2);
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "%s requires a value\n", arg.c_str());
            std::exit(2);
        }
        args.values[arg.substr(2)] = argv[++i];
    }
    return args;
}

std::string required(const UtilityArgs& args, const char* name) {
    auto it = args.values.find(name);
    if (it == args.values.end() || it->second.empty()) {
        std::fprintf(stderr, "--%s is required\n", name);
        std::exit(2);
    }
    return it->second;
}

int reconcile_prior_day_command(int argc, char** argv) {
    UtilityArgs args = parse_utility_args(argc, argv, {"config", "reason", "backup-dir"});
    // The same configuration as the running servers: journal locations and the trading-day zone.
    ng::order_ui::OrderUiConfig config = ng::order_ui::load_config(required(args, "config"));
    std::vector<ng::ops::ReconcileSource> sources;
    for (const auto& source : config.reconciliation_sources) sources.push_back({source.mode, source.state_db});
    std::optional<std::string> backup_dir;
    if (auto it = args.values.find("backup-dir"); it != args.values.end()) backup_dir = it->second;
    ng::json result = ng::ops::reconcile_prior_day(sources, config.timezone, required(args, "reason"), backup_dir, args.apply);
    std::printf("%s\n", result.dump(2).c_str());
    return 0;
}

int reconcile_stopped_child_command(int argc, char** argv) {
    UtilityArgs args = parse_utility_args(argc, argv, {"state-db", "feedback-db", "client-order-id", "snapshot-file", "source"});
    std::ifstream in(required(args, "snapshot-file"), std::ios::binary);
    if (!in) throw std::runtime_error("cannot read the snapshot file");
    std::stringstream buffer;
    buffer << in.rdbuf();
    ng::json snapshot = ng::json::parse(buffer.str());
    ng::json result = ng::ops::reconcile_stopped_child(required(args, "state-db"), required(args, "feedback-db"),
                                                      required(args, "client-order-id"), snapshot, required(args, "source"),
                                                      args.apply);
    std::printf("%s\n", result.dump(2).c_str());
    return 0;
}

bool parse_int(const char* text, int& out) {
    try {
        std::size_t idx = 0;
        int value = std::stoi(text, &idx);
        if (idx != std::strlen(text)) return false;
        out = value;
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Bound glibc malloc arenas: hundreds of short-lived HTTP/SSE worker threads would otherwise
    // each grow their own arena and inflate resident memory for the lifetime of the process.
    mallopt(M_ARENA_MAX, 4);
    ng::log::configure_from_env();
    if (argc > 1 && (std::strcmp(argv[1], "reconcile-prior-day") == 0 || std::strcmp(argv[1], "reconcile-stopped-child") == 0)) {
        try {
            return std::strcmp(argv[1], "reconcile-prior-day") == 0 ? reconcile_prior_day_command(argc, argv)
                                                                    : reconcile_stopped_child_command(argc, argv);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
    }
    ng::AppOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s requires a value\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        int value = 0;
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--version") {
            std::printf("nextgen-gui %s\n", NG_VERSION);
            return 0;
        } else if (arg == "--config") {
            options.config_path = need_value("--config");
        } else if (arg == "--host") {
            options.host = need_value("--host");
        } else if (arg == "--port") {
            if (!parse_int(need_value("--port"), value)) {
                std::fprintf(stderr, "--port must be an integer\n");
                return 2;
            }
            options.port = value;
        } else if (arg == "--ssl") {
            options.ssl = true;
        } else if (arg == "--no-ssl") {
            options.ssl = false;
        } else if (arg == "--ssl-certfile") {
            options.ssl_certfile = need_value("--ssl-certfile");
        } else if (arg == "--ssl-keyfile") {
            options.ssl_keyfile = need_value("--ssl-keyfile");
        } else if (arg == "--redirect-port") {
            if (!parse_int(need_value("--redirect-port"), value)) {
                std::fprintf(stderr, "--redirect-port must be an integer\n");
                return 2;
            }
            options.redirect_port = value;
        } else if (arg == "--redirect-host") {
            options.redirect_host = need_value("--redirect-host");
        } else if (arg == "--redirect-target") {
            options.redirect_target_host = need_value("--redirect-target");
        } else if (arg == "--threads") {
            if (!parse_int(need_value("--threads"), value)) {
                std::fprintf(stderr, "--threads must be an integer\n");
                return 2;
            }
            options.threads = value;
        } else if (arg == "--static-dir") {
            options.static_dir = need_value("--static-dir");
        } else if (arg == "--without-kafka") {
            options.without_kafka = true;
        } else if (arg == "--check-config") {
            options.check_only = true;
        } else if (arg == "--persist-orders") {
            options.persist_orders = true;
        } else if (arg == "--date") {
            options.persist_date = need_value("--date");
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage();
            return 2;
        }
    }
    if (options.config_path.empty()) {
        std::filesystem::path exe_dir = std::filesystem::path(argv[0]).parent_path();
        std::filesystem::path config_dir = std::filesystem::current_path() / "config";
        if (!std::filesystem::exists(config_dir) && std::filesystem::exists(exe_dir / ".." / "config")) {
            config_dir = exe_dir / ".." / "config";
        }
        options.config_path = ng::default_config_path(config_dir.string());
    }
    try {
        if (options.persist_orders) return ng::Application::persist_orders_once(options);
        if (options.check_only) {
            ng::AppConfig config = ng::load_config(options.config_path);
            ng::AccountDirectory accounts = ng::load_account_metas(config);
            std::printf("OK: %s (%zu visible accounts, %zu symbols, smart selling %s)\n", config.config_path.c_str(),
                        accounts.size(), ng::load_symbols(config).size(), config.smart_selling.enabled ? "enabled" : "disabled");
            return 0;
        }
        ng::Application app(options);
        app.initialise();
        return app.run();
    } catch (const std::exception& e) {
        NG_LOG_ERROR("nextgen-gui", "fatal: {}", e.what());
        std::fprintf(stderr, "fatal: %s\n", e.what());
        return 1;
    }
}
