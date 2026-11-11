// Mode H control-server wiring (trading_ui/services/smart_buying_runtime.py): fixed 5/8/15% POV
// buying with available USD cash and ordinary limit orders. Everything except the resource check
// (cash instead of shares), the session sizing and the pre-dispatch checks is inherited from the
// Smart selling runtime.
#pragma once

#include "algo/smart_buying/engine.h"
#include "algo/smart_selling/runtime.h"
#include "model/market_data.h"

namespace ng::smart {

class SmartBuyingRuntime : public SmartSellingRuntime {
public:
    // `config` is the normalised `smart_buying` section (normalize_smart_buying_config).
    SmartBuyingRuntime(const SmartSellingConfig& config, MarketDataStore& market_data_store, MetasProvider metas,
                       SnapshotsProvider snapshots, PublishCommand publish, std::shared_ptr<algo::SessionCoordinator> session_coordinator = nullptr,
                       std::shared_ptr<SmartBuyingEngine> engine = nullptr, Clock clock = nullptr, Clock wall_clock = nullptr,
                       reporting::SessionReporter* reporting = nullptr);

    // Sizes each account's fixed target from its available USD cash at the current best ask.
    json start_parent(const json& settings) override;

    SmartBuyingEngine& buying_engine() { return *buying_engine_; }

protected:
    // Available USD cash (available USD, else the USD balance, else the legacy cash field); also
    // refreshes the engine's per-account cash limit.
    Decimal available_position(const std::string& account_id, const std::string& symbol) override;
    std::optional<json> prepare_submit(const json& action) override;

private:
    MarketDataStore& market_data_store_;
    SmartBuyingEngine* buying_engine_ = nullptr;
};

}  // namespace ng::smart
