#include "web_fixture.h"
#include "orders/commands.h"
using ng::json;
using ngtest::WebFixture;
using ngtest::body;

TEST_CASE("limit price rounds exactly to nearest tick with halfway upward") {
    using ng::orders::nearest_tick_price;
    CHECK(nearest_tick_price("39.501","0.01")==39.50);
    CHECK(nearest_tick_price("39.505","0.01")==39.51);
    CHECK(nearest_tick_price("39.504999","0.01")==39.50);
    CHECK(nearest_tick_price("1.0225","0.005")==1.025);
    CHECK(nearest_tick_price("0.123456","0.0001")==0.1235);
    CHECK(nearest_tick_price(39.5,"0.01")==39.5);
    CHECK(nearest_tick_price("0.001","0.01")==0.001);
    CHECK(nearest_tick_price("0.0911","0.01")==0.0911);
    CHECK(nearest_tick_price("0.09105","0.01")==0.0911);
    CHECK(nearest_tick_price("0.99995","0.01")==1.0);
    CHECK(nearest_tick_price("1.0001","0.01")==1.0);
    CHECK_THROWS(nearest_tick_price("0.00001","0.01"));
    CHECK_THROWS(nearest_tick_price("39.5","0"));
    CHECK_THROWS(nearest_tick_price(true,"0.01"));
}

TEST_CASE("manual and percentage limit orders are tick-rounded before publication") {
    WebFixture f;auto client=f.client();f.upsert_book("TANH",39.8,40.0);f.upsert_quote("TANH",39.9);
    auto manual=client.Post("/api/account-orders/limit",json{{"account_id","A"},{"symbol","TANH"},{"side","SELL"},{"shares","100"},{"limit_price","39.501"}}.dump(),"application/json");
    REQUIRE(manual);REQUIRE(manual->status==200);
    CHECK(body(manual)["limit_price"]==39.5);
    CHECK(f.sink->history().back().command["limit_price"]==39.5);
    auto percent=client.Post("/api/orders/limit",json{{"account_ids",json::array({"B"})},{"symbol","TANH"},{"side","SELL"},{"shares","100"},{"through_market_pct","1"}}.dump(),"application/json");
    REQUIRE(percent);REQUIRE(percent->status==200);
    CHECK(f.sink->history().back().command["limit_price"]==39.5);
}

TEST_CASE("publication honors per-symbol ticks for ordinary and FOK orders") {
    WebFixture f("",R"(,"smart_selling":{"instruments":{"TANH":{"tick_size":"0.005"}}},"smart_buying":{"instruments":{"TANH":{"tick_size":"0.005"}}})");
    for(const auto* type:{"LIMIT_ORDER","LIMIT_ORDER_FOK"})for(const auto* side:{"BUY","SELL"}) {
        json command={{"type",type},{"command_id",std::string(type)+side},{"account_id","A"},{"symbol","TANH"},{"side",side},{"qty_shares",100},{"limit_price","39.5025"}};
        f.control->publish_trading_command(command,"A");
        CHECK(f.sink->history().back().command["limit_price"]==39.505);
    }
}

TEST_CASE("percentage calculations are not rounded twice before the tick") {
    WebFixture f;auto client=f.client();f.upsert_book("TANH",39.903,39.905);f.upsert_quote("TANH",39.904);
    auto reply=client.Post("/api/orders/limit",json{{"account_ids",json::array({"A"})},{"symbol","TANH"},{"side","SELL"},{"shares","100"},{"through_market_pct","1"}}.dump(),"application/json");
    REQUIRE(reply);REQUIRE(reply->status==200);
    // 39.904 * 0.99 = 39.50496, which rounds directly to 39.50, not 39.51.
    CHECK(f.sink->history().back().command["limit_price"]==39.5);
}

TEST_CASE("invalid sub-tick prices do not leave external-order reservations") {
    WebFixture f;auto client=f.client();
    auto reply=client.Post("/api/account-orders/limit",json{{"account_id","A"},{"symbol","TANH"},{"side","SELL"},{"shares","100"},{"limit_price","0.00001"}}.dump(),"application/json");
    REQUIRE(reply);CHECK(reply->status==400);CHECK(f.sink->history().empty());
    f.upsert_account("A",10000,1000,true,"TANH");f.upsert_book("TANH",39.49,39.51);
    auto next=client.Post("/api/smart-selling",json{{"account_ids",json::array({"A"})},{"symbol","TANH"},{"mode","normal"}}.dump(),"application/json");
    REQUIRE(next);CHECK(next->status==200);
}

TEST_CASE("all algo modes retain sub-dollar precision at final publication") {
    WebFixture f;f.upsert_account("A",10000,10000,true,"BDMD");
    for(const auto* mode:{"E","F","G","H","P"}) {
        bool buying=std::string(mode)=="E" || std::string(mode)=="H";
        json command={{"type",std::string(mode)=="E" || std::string(mode)=="F" ? "LIMIT_ORDER_FOK" : "LIMIT_ORDER"},
            {"command_id",std::string("subdollar-")+mode},{"trading_mode",mode},{"account_id","A"},
            {"symbol","BDMD"},{"side",buying ? "BUY" : "SELL"},{"qty_shares",100},{"limit_price",0.0911}};
        f.control->publish_trading_command(command,"A");
        CHECK(f.sink->history().back().command["limit_price"]==0.0911);
    }
}

TEST_CASE("E and F displayed sub-dollar prices survive the planner to publication") {
    WebFixture f;f.upsert_account("A",10000,10000,true,"BDMD");
    ng::OrderBookSnapshot book;book.symbol="BDMD";
    book.bids={{0.0910,1000,std::nullopt}};book.asks={{0.0911,1000,std::nullopt}};
    for(const auto* mode:{"E","F"}) {
        ng::fast::CycleInputs inputs;inputs.trading_mode=mode;inputs.symbol="BDMD";inputs.book=book;
        inputs.price_limit=std::string(mode)=="E" ? 0.0911 : 0.0910;inputs.account_ids={"A"};
        inputs.book_levels=1;inputs.target_percentage=100;inputs.account_metas=&f.control->accounts();
        auto cycle=ng::fast::build_fast_trading_cycle_commands(inputs);REQUIRE(cycle.commands.size()==1);
        CHECK(cycle.limit_price==inputs.price_limit);
        f.control->publish_trading_command(cycle.commands[0],"A");
        CHECK(f.sink->history().back().command["limit_price"]==inputs.price_limit);
    }
}
