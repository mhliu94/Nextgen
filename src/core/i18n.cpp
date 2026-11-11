#include "core/i18n.h"

#include <map>
#include <mutex>

namespace ng::i18n {
namespace {

struct KeyValue {
    const char* key;
    const char* value;
};

#include "core/i18n_table.inc"

// Keys added by the next-generation front end. Keep both languages in sync.
static const KeyValue k_en_extra[] = {
    {"algo_category_buying", "buying"},
    {"algo_category_selling", "selling"},
    {"algo_session_rules", "Each account can run one buying session and multiple selling sessions, all on different symbols. Stopping sessions keep their reservations until outstanding orders are confirmed."},
    {"algo_waiting_volume", "Waiting for market volume"},
    {"algo_waiting_volume_minimum", "Waiting for market volume to reach the minimum order size"},
    {"algo_waiting_volume_liquidity", "Waiting for market volume and available liquidity"},
    {"algo_waiting_feeds", "Waiting for live order and market-volume feeds"},
    {"algo_waiting_baseline", "Waiting for a fresh market-volume baseline"},
    {"algo_waiting_reconciliation", "Waiting for broker order confirmation"},
    {"algo_waiting_order", "Waiting for the current order to finish"},
    {"algo_waiting_spacing", "Waiting for the next order interval"},
    {"algo_waiting_account", "Waiting for a fresh account snapshot or trading to be enabled"},
    {"app_name", "KTrader"},
    {"app_subtitle", "Control Center"},
    {"nav_account", "Accounts"},
    {"nav_control", "Trading"},
    {"nav_market", "Market"},
    {"nav_insights", "Depth"},
    {"nav_status", "Trading status"},
    {"theme_toggle", "Toggle light / dark theme"},
    {"density_toggle", "Toggle compact rows"},
    {"feed_accounts", "Accounts feed"},
    {"feed_market", "Market data"},
    {"feed_orders", "Order status"},
    {"feed_volume", "Volume tape"},
    {"feed_live", "live"},
    {"feed_stale", "stale"},
    {"feed_waiting", "waiting"},
    {"feed_replaying", "replaying"},
    {"feed_disconnected", "disconnected"},
    {"feed_starting", "starting"},
    {"feed_stopped", "stopped"},
    {"feed_unavailable", "n/a"},
    {"last_update", "Last update"},
    {"never", "never"},
    {"seconds_ago", "{seconds}s ago"},
    {"minutes_ago", "{minutes}m ago"},
    {"connection_lost", "Connection to the server was lost. Reconnecting…"},
    {"connection_restored", "Connected."},
    {"loading", "Loading…"},
    {"search_accounts", "Filter accounts"},
    {"show_hidden_cards", "Show accounts without data"},
    {"account_count", "{count} accounts"},
    {"total_equity_hint", "Cash plus valued positions (USD)"},
    {"market_value_total", "Positions value"},
    {"usd_cash_total", "USD cash"},
    {"accounts_with_data", "with data"},
    {"accounts_trading_on", "trading on"},
    {"modal_close", "Close"},
    {"copy", "Copy"},
    {"copied", "Copied"},
    {"confirm_title", "Please confirm"},
    {"confirm_stop_all", "Stop every running algorithm on this server?"},
    {"confirm_cancel_all", "Cancel open orders for all selected accounts?"},
    {"confirm_turn_off_all", "Turn trading off for every account?"},
    {"confirm", "Confirm"},
    {"submitting", "Submitting…"},
    {"orders_submitted_for", "Commands published for {count} account(s)."},
    {"active_algo", "Active algorithm"},
    {"no_active_algo", "No algorithm session is running."},
    {"algo_running_since", "running since"},
    {"algo_cycles", "cycles"},
    {"algo_published", "published"},
    {"algo_last_reason", "last cycle"},
    {"algo_mode_label", "Mode"},
    {"book_spread", "Spread"},
    {"book_mid", "Mid"},
    {"book_imbalance", "Bid / ask size"},
    {"book_updated", "Updated"},
    {"book_total_bid", "Total bid qty"},
    {"book_total_ask", "Total ask qty"},
    {"depth_ladder", "Ladder"},
    {"depth_tables", "Tables"},
    {"md_sparkline", "Recent mid prices"},
    {"health_title", "Server health"},
    {"health_uptime", "Uptime"},
    {"health_version", "Version"},
    {"health_sse_clients", "Live connections"},
    {"health_kafka_commands", "Commands published"},
    {"account_order_shortcut", "Order"},
    {"positions_none", "No positions"},
    {"keyboard_hint", "Press / to search, Esc to close dialogs"},
    {"stale_data_hint", "Values shown may be stale."},
    {"session_expired", "Your session has expired. Please sign in again."},
    {"error_generic", "Request failed."},
    {"required", "required"},
    {"optional", "optional"},
    {"symbol_placeholder", "e.g. AAPL"},
    {"all", "All"},
    {"none_selected", "None selected"},
    {"select_all", "Select all"},
    {"clear", "Clear"},
    {"operations", "Operations"},
    {"recent_commands", "Recent commands"},
    {"recent_commands_empty", "No commands published since the server started."},
    {"time", "Time"},
    {"type", "Type"},
    {"status", "Status"},
    {"details", "Details"},
    {"published", "published"},
    {"test_mode_tag", "test mode"},
    {"login_welcome", "Sign in to continue"},
    {"login_hint", "Use the credentials from the server configuration."},
    {"smart_market_state", "Market"},
    {"smart_child_state", "Child order"},
    {"smart_progress", "Progress"},
    {"smart_credit", "Free credit"},
    {"smart_participation", "Participation"},
    {"smart_deadline_at", "Deadline"},
    {"smart_started_at", "Start"},
    {"smart_accounts_detail", "Per-account inventory"},
    {"fast_cycle_result", "Last cycle"},
    {"page_not_found", "Page not found"},
    {"nav_admin", "Admin"},
    {"page_account_details", "Account details"},
    {"smart_target", "Target"},
    {"smart_average_cost", "avg cost"},
    {"smart_child_cap", "Per-order cap"},
    {"smart_child_cap_disabled", "disabled"},
    {"smart_cash_budget", "spent / budget"},
    {"smart_minimum_price_short", "Minimum price"},
    {"smart_maximum_price_short", "Maximum price"},
    {"smart_buying_status_title", "Smart buying orders"},
    {"smart_buying_status_empty", "No Smart buying orders yet."},
    {"smart_buying_started", "Smart buying started."},
    {"smart_sessions_filter_note", "Showing sessions started today and every active session; older history is kept on the server."},
    {"fast_target_position_status", "Position {current} / target {target} shares"},
    {"fast_session_id", "Session"},
    {"feed_buying_orders", "Order status (buying)"},
    {"feed_buying_volume", "Volume tape (buying)"},
    {"feed_own_orders", "Own orders (Market Data)"},
    {"md_live_quotes", "Live quotes"},
};

static const KeyValue k_zh_extra[] = {
    {"algo_category_buying", "买入"},
    {"algo_category_selling", "卖出"},
    {"algo_session_rules", "每个账户最多运行一个买入会话，可同时运行多个卖出会话，所有会话须使用不同标的。正在停止的会话将保留预占，直至未完成订单得到确认。"},
    {"algo_waiting_volume", "等待市场成交量"},
    {"algo_waiting_volume_minimum", "等待市场成交量达到最小委托数量"},
    {"algo_waiting_volume_liquidity", "等待市场成交量和可用流动性"},
    {"algo_waiting_feeds", "等待实时订单和市场成交量数据"},
    {"algo_waiting_baseline", "等待新的市场成交量基准"},
    {"algo_waiting_reconciliation", "等待券商确认订单状态"},
    {"algo_waiting_order", "等待当前订单结束"},
    {"algo_waiting_spacing", "等待下一次下单间隔"},
    {"algo_waiting_account", "等待最新账户快照或启用交易"},
    {"app_name", "KTrader"},
    {"app_subtitle", "控制中心"},
    {"nav_account", "账户"},
    {"nav_control", "交易"},
    {"nav_market", "行情"},
    {"nav_insights", "盘口"},
    {"nav_status", "交易状态"},
    {"theme_toggle", "切换明暗主题"},
    {"density_toggle", "切换紧凑模式"},
    {"feed_accounts", "账户数据"},
    {"feed_market", "行情数据"},
    {"feed_orders", "订单状态"},
    {"feed_volume", "成交量"},
    {"feed_live", "正常"},
    {"feed_stale", "过期"},
    {"feed_waiting", "等待"},
    {"feed_replaying", "回放中"},
    {"feed_disconnected", "已断开"},
    {"feed_starting", "启动中"},
    {"feed_stopped", "已停止"},
    {"feed_unavailable", "不可用"},
    {"last_update", "最近更新"},
    {"never", "从未"},
    {"seconds_ago", "{seconds} 秒前"},
    {"minutes_ago", "{minutes} 分钟前"},
    {"connection_lost", "与服务器的连接已中断，正在重连…"},
    {"connection_restored", "已连接。"},
    {"loading", "加载中…"},
    {"search_accounts", "筛选账户"},
    {"show_hidden_cards", "显示暂无数据的账户"},
    {"account_count", "{count} 个账户"},
    {"total_equity_hint", "现金加已估值持仓（美元）"},
    {"market_value_total", "持仓市值"},
    {"usd_cash_total", "美元现金"},
    {"accounts_with_data", "有数据"},
    {"accounts_trading_on", "交易开启"},
    {"modal_close", "关闭"},
    {"copy", "复制"},
    {"copied", "已复制"},
    {"confirm_title", "请确认"},
    {"confirm_stop_all", "确定停止本服务器上所有运行中的算法？"},
    {"confirm_cancel_all", "确定撤销所选账户的全部未成交订单？"},
    {"confirm_turn_off_all", "确定关闭所有账户的交易？"},
    {"confirm", "确认"},
    {"submitting", "提交中…"},
    {"orders_submitted_for", "已为 {count} 个账户发布指令。"},
    {"active_algo", "运行中的算法"},
    {"no_active_algo", "当前没有运行中的算法任务。"},
    {"algo_running_since", "开始于"},
    {"algo_cycles", "周期数"},
    {"algo_published", "已发布"},
    {"algo_last_reason", "上一周期"},
    {"algo_mode_label", "模式"},
    {"book_spread", "价差"},
    {"book_mid", "中间价"},
    {"book_imbalance", "买 / 卖 数量"},
    {"book_updated", "更新时间"},
    {"book_total_bid", "买盘总量"},
    {"book_total_ask", "卖盘总量"},
    {"depth_ladder", "阶梯"},
    {"depth_tables", "表格"},
    {"md_sparkline", "近期中间价"},
    {"health_title", "服务器状态"},
    {"health_uptime", "运行时长"},
    {"health_version", "版本"},
    {"health_sse_clients", "实时连接数"},
    {"health_kafka_commands", "已发布指令数"},
    {"account_order_shortcut", "下单"},
    {"positions_none", "无持仓"},
    {"keyboard_hint", "按 / 搜索，按 Esc 关闭对话框"},
    {"stale_data_hint", "显示的数据可能已过期。"},
    {"session_expired", "登录已过期，请重新登录。"},
    {"error_generic", "请求失败。"},
    {"required", "必填"},
    {"optional", "选填"},
    {"symbol_placeholder", "例如 AAPL"},
    {"all", "全部"},
    {"none_selected", "未选择"},
    {"select_all", "全选"},
    {"clear", "清除"},
    {"operations", "操作记录"},
    {"recent_commands", "最近发布的指令"},
    {"recent_commands_empty", "服务器启动后尚未发布任何指令。"},
    {"time", "时间"},
    {"type", "类型"},
    {"status", "状态"},
    {"details", "详情"},
    {"published", "已发布"},
    {"test_mode_tag", "测试模式"},
    {"login_welcome", "登录以继续"},
    {"login_hint", "请使用服务器配置中的账号密码。"},
    {"smart_market_state", "市场"},
    {"smart_child_state", "子订单"},
    {"smart_progress", "进度"},
    {"smart_credit", "可用额度"},
    {"smart_participation", "参与率"},
    {"smart_deadline_at", "截止"},
    {"smart_started_at", "开始"},
    {"smart_accounts_detail", "各账户持仓"},
    {"fast_cycle_result", "上一周期"},
    {"page_not_found", "页面不存在"},
    {"nav_admin", "管理"},
    {"page_account_details", "账户信息"},
    {"smart_target", "目标"},
    {"smart_average_cost", "平均成本"},
    {"smart_child_cap", "单笔上限"},
    {"smart_child_cap_disabled", "已关闭"},
    {"smart_cash_budget", "已用 / 预算"},
    {"smart_minimum_price_short", "最低卖出价"},
    {"smart_maximum_price_short", "最高买入价"},
    {"smart_buying_status_title", "智能买入订单"},
    {"smart_buying_status_empty", "暂无智能买入订单。"},
    {"smart_buying_started", "智能买入已启动。"},
    {"smart_sessions_filter_note", "显示今日启动的会话及所有进行中的会话；更早的历史记录仍保存在服务器上。"},
    {"fast_target_position_status", "持仓 {current} / 目标 {target} 股"},
    {"fast_session_id", "会话"},
    {"feed_buying_orders", "订单状态（买入）"},
    {"feed_buying_volume", "成交量（买入）"},
    {"feed_own_orders", "本方订单（行情数据）"},
    {"md_live_quotes", "实时行情"},
};

const std::map<std::string, std::map<std::string, std::string>>& tables() {
    static const std::map<std::string, std::map<std::string, std::string>> all = [] {
        std::map<std::string, std::map<std::string, std::string>> t;
        for (auto& kv : k_en_legacy) t["en"][kv.key] = kv.value;
        for (auto& kv : k_zh_legacy) t["zh"][kv.key] = kv.value;
        for (auto& kv : k_en_extra) t["en"][kv.key] = kv.value;
        for (auto& kv : k_zh_extra) t["zh"][kv.key] = kv.value;
        return t;
    }();
    return all;
}

}  // namespace

bool is_supported(std::string_view lang) { return lang == "en" || lang == "zh"; }

std::vector<std::string> supported_langs() { return {"en", "zh"}; }

std::string resolve_lang(std::string_view query_lang, std::string_view cookie_lang) {
    if (is_supported(query_lang)) return std::string(query_lang);
    if (is_supported(cookie_lang)) return std::string(cookie_lang);
    return kDefaultLang;
}

std::string t(std::string_view lang, std::string_view key) {
    const auto& all = tables();
    auto lit = all.find(std::string(is_supported(lang) ? lang : std::string_view(kDefaultLang)));
    if (lit != all.end()) {
        auto kit = lit->second.find(std::string(key));
        if (kit != lit->second.end()) return kit->second;
    }
    auto eit = all.find("en");
    auto kit = eit->second.find(std::string(key));
    if (kit != eit->second.end()) return kit->second;
    return std::string(key);
}

std::string format_named(std::string_view text, const std::vector<std::pair<std::string, std::string>>& values) {
    std::string out(text);
    for (auto& [name, value] : values) {
        std::string needle = "{" + name + "}";
        std::size_t pos = 0;
        while ((pos = out.find(needle, pos)) != std::string::npos) {
            out.replace(pos, needle.size(), value);
            pos += value.size();
        }
    }
    return out;
}

json dictionary(std::string_view lang) {
    const auto& all = tables();
    json out = json::object();
    auto eit = all.find("en");
    for (auto& [k, v] : eit->second) out[k] = v;
    if (is_supported(lang) && lang != "en") {
        auto lit = all.find(std::string(lang));
        for (auto& [k, v] : lit->second) out[k] = v;
    }
    return out;
}

}  // namespace ng::i18n
