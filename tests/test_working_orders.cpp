#include "web_fixture.h"
#include "algo/smart_selling/commands.h"
using ng::json;
using ngtest::WebFixture;
using ngtest::body;

namespace {
json ticket(const std::string& id, const std::string& account = "A", const std::string& command = "") {
    json o{{"schema_version",1},{"account_id",account},{"order_id",id},{"sequence",0},
        {"created_at","2026-01-02T14:00:00Z"},{"updated_at","2026-01-02T14:00:00Z"},
        {"symbol","TANH"},{"side","SELL"},{"order_type","LIMIT"},{"status","NEW"},
        {"quantity",100},{"filled_quantity",0},{"average_fill_price",nullptr},{"limit_price",39.5}};
    if (!command.empty()) o["command_id"] = command;
    return o;
}
void receive(WebFixture& f, const json& o) {
    f.control->market_order_store()->upsert(ng::orders::parse_order_payload(o));
}
void session(WebFixture& f, const std::string& id, const std::string& command, const std::string& mode = "G") {
    auto* r=f.control->reporting();
    r->start_session(id,{{"symbol","TANH"},{"trading_mode",mode}}, {"A"}, "2026-01-02T13:00:00Z");
    r->register_command(id,{{"account_id","A"},{"command_id",command}});
}
void live(WebFixture& f) { f.control->market_order_store()->update_feed("live", "caught up"); }
}

TEST_CASE("working orders classify durable provenance and include prior-day GTC orders") {
    WebFixture f;session(f,"session","algo-command");
    f.control->reporting()->register_manual_command({{"account_id","A"},{"command_id","manual-command"}});
    receive(f,ticket("algo","A","algo-command"));receive(f,ticket("ui","A","manual-command"));
    receive(f,ticket("external","B"));
    auto terminal=ticket("done");terminal["status"]="CANCELED";receive(f,terminal);
    auto response=f.client().Get("/api/working-orders");REQUIRE(response);CHECK(response->status==200);
    auto rows=body(response).at("orders");REQUIRE(rows.size()==3);
    std::map<std::string,json> by_id;for(auto& row:rows)by_id[row.at("order_id").get<std::string>()]=row;
    CHECK(by_id["algo"]["origin"]=="algo");CHECK(by_id["algo"]["session_label"]=="TANH-G-1");
    CHECK(by_id["ui"]["origin"]=="manual_ui");CHECK(by_id["external"]["origin"]=="manual_external");
    CHECK_FALSE(by_id["ui"]["can_cancel"].get<bool>()); // feed not yet caught up
    auto status_only=ticket("ui");status_only["sequence"]=1;receive(f,status_only);
    ng::reporting::ActivityReporting reopened(f.config.reporting.db_path);
    for(const auto& row:reopened.working_orders(f.control->accounts())) {
        if(row["order_id"]=="ui") CHECK(row["origin"]=="manual_ui");
    }
}

TEST_CASE("account and global cancel-all expand only manual orders into exact commands") {
    WebFixture f;session(f,"session","algo-command");
    receive(f,ticket("algo","A","algo-command"));receive(f,ticket("manual-a"));receive(f,ticket("manual-b","B"));live(f);
    auto c=f.client();auto a=c.Post("/api/orders/cancel",json{{"account_ids",{"A"}}}.dump(),"application/json");
    REQUIRE(a);CHECK(a->status==200);CHECK(body(a)["skipped_algo"]==1);REQUIRE(f.sink->count()==1);
    CHECK(f.sink->history()[0].command["type"]=="CANCEL_ORDER");CHECK(f.sink->history()[0].command["order_id"]=="manual-a");
    CHECK(f.sink->history()[0].command["order_created_at"]=="2026-01-02T14:00:00Z");
    auto all=c.Post("/api/orders/cancel",json{{"account_ids",{"__ALL__"}}}.dump(),"application/json");
    REQUIRE(all);CHECK(all->status==200);REQUIRE(f.sink->count()==2);
    CHECK(f.sink->history()[1].command["order_id"]=="manual-b");
    CHECK(f.control->working_orders()["orders"].size()==3); // acknowledgement is not terminal
    auto forged=c.Post("/api/orders/cancel",json{{"account_ids",{"A"}},{"order_id","algo"},{"origin","manual_ui"}}.dump(),"application/json");
    REQUIRE(forged);CHECK(forged->status==400);CHECK(f.sink->count()==2);
}

TEST_CASE("manual cancellation remains available while an algo owns the account") {
    WebFixture f;f.upsert_account("A",10000,1000,true,"TANH");f.upsert_book("TANH",39,40);
    f.control->start_fast_strategy(f.fast_start("active","E","TANH",{"A"},true));
    receive(f,ticket("manual"));live(f);
    CHECK_NOTHROW(f.control->cancel_manual_orders({"A"}));REQUIRE(f.sink->count()==1);
    CHECK_THROWS_AS(f.control->publish_control_command({{"type","MARKET_ORDER"},{"account_id","A"},{"command_id","new-manual"}},"A"),std::invalid_argument);
}

TEST_CASE("unmatched algo submissions block ambiguous broker snapshots from manual cancellation") {
    WebFixture f;session(f,"pending","not-yet-correlated");receive(f,ticket("ambiguous"));live(f);
    REQUIRE(f.control->working_orders()["orders"].size()==1);
    CHECK(f.control->working_orders()["orders"][0]["origin"]=="unknown");
    auto result=f.control->cancel_manual_orders({"A"});CHECK(result["unresolved"]==1);CHECK(f.sink->count()==0);
    auto repaired=ticket("ambiguous","A","not-yet-correlated");repaired["sequence"]=1;receive(f,repaired);
    CHECK(f.control->working_orders()["orders"][0]["origin"]=="algo");
}

TEST_CASE("ended-session cleanup handles late acknowledgements and never touches manual or other sessions") {
    WebFixture f;session(f,"stopped","late-command","E");session(f,"other","other-command");
    f.control->reporting()->end_session("stopped");live(f);
    f.control->reconcile_session_cancellations();CHECK(f.sink->count()==0);
    receive(f,ticket("late","A","late-command"));receive(f,ticket("other","A","other-command"));receive(f,ticket("manual","B"));
    f.control->reconcile_session_cancellations();REQUIRE(f.sink->count()==1);
    CHECK(f.sink->history()[0].command["order_id"]=="late");CHECK(f.sink->history()[0].command["parent_id"]=="stopped");
    f.control->reconcile_session_cancellations();CHECK(f.sink->count()==1);
}

TEST_CASE("uncertain cancellation is visible and is not duplicated by retries or reopening") {
    WebFixture f;receive(f,ticket("manual"));live(f);f.sink->fail_next("transport uncertain");
    auto result=f.control->cancel_manual_orders({"A"});CHECK(result["errors"].size()==1);
    CHECK(f.control->working_orders()["orders"][0]["cancel_state"]=="uncertain");
    ng::reporting::ActivityReporting reopened(f.config.reporting.db_path);
    CHECK(reopened.working_orders(f.control->accounts())[0]["cancel_state"]=="uncertain");
    CHECK(f.control->cancel_manual_orders({"A"})["already_pending"]==1);CHECK(f.sink->count()==0);
}

TEST_CASE("disconnected order feed and invalid accounts never publish cancellations") {
    WebFixture f;receive(f,ticket("manual"));
    CHECK_THROWS(f.control->cancel_manual_orders({"A"}));live(f);
    CHECK_THROWS(f.control->cancel_manual_orders({"A","missing"}));CHECK(f.sink->count()==0);
}

TEST_CASE("Smart stop translation requires and uses the exact owned broker ID") {
    WebFixture f;json action{{"type","cancel"},{"account_id","A"},{"symbol","TANH"},{"parent_id","session"},{"broker_order_id","owned"}};
    auto cmd=ng::smart::build_smart_selling_command(action,f.control->accounts());
    CHECK(cmd["type"]=="CANCEL_ORDER");CHECK(cmd["order_id"]=="owned");
    action["broker_order_id"]=nullptr;CHECK_THROWS(ng::smart::build_smart_selling_command(action,f.control->accounts()));
}

TEST_CASE("every algo mode rejects missing and stale selected accounts before starting") {
    for (const auto* mode:{"E","F","G","H"}) for (bool stale:{false,true}) {
        WebFixture f;f.upsert_account("A",10000,1000,true,"TANH");f.upsert_book("TANH",39,40);
        if(stale){ng::AccountSnapshot s;s.account_id="B";s.ts="2026-01-01T00:00:00Z";s.trading_enabled=true;f.control->account_store().upsert(s);}
        auto attempt=[&]{
            if(std::string(mode)=="E" || std::string(mode)=="F") f.control->start_fast_strategy(f.fast_start("offline",mode,"TANH",{"A","B"},true));
            else if(std::string(mode)=="G") f.control->start_smart_parent({{"account_ids",{"A","B"}},{"symbol","TANH"}});
            else f.control->start_smart_buying_parent({{"account_ids",{"A","B"}},{"symbol","TANH"}});
        };
        CHECK_THROWS_WITH_AS(attempt(),doctest::Contains("B"),std::invalid_argument);
        CHECK(f.control->reporting()->session_labels().empty());CHECK(f.sink->count()==0);
    }
}

TEST_CASE("algo publication rechecks account freshness and invalid timestamps are offline") {
    WebFixture f;
    for(const auto& stamp:{std::string(""),std::string("bad"),std::string("2026-01-01T00:00:00Z"),std::string("2099-01-01T00:00:00Z")}) {
        ng::AccountSnapshot s;s.account_id="A";s.ts=stamp;s.trading_enabled=true;f.control->account_store().upsert(s);
        CHECK_THROWS(f.control->publish_trading_command({{"type","LIMIT_ORDER"},{"trading_mode","G"},{"account_id","A"}},"A"));
        CHECK(f.sink->count()==0);
    }
}
