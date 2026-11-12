#include "web_fixture.h"
using ng::json;
using ngtest::WebFixture;
using ngtest::body;
TEST_CASE("manual batches validate every account before queuing any command") {
    WebFixture f;auto c=f.client();
    f.upsert_quote("AAPL",10);
    for (const auto* path:{"/api/orders/quick","/api/orders/limit","/api/orders/cancel"}) {
        json request={{"account_ids",json::array({"A","UNKNOWN"})},{"symbol","AAPL"},
            {"side","BUY"},{"shares","10"},{"limit_price","10"},{"dollar_amount","100"}};
        auto result=c.Post(path,request.dump(),"application/json");
        REQUIRE(result);
        CHECK(result->status==400);
        CHECK(f.sink->history().empty());
    }
}
TEST_CASE("quick and limit requests reject duplicate account IDs") {
    WebFixture f;auto c=f.client();f.upsert_quote("AAPL",10);
    for (const auto* path:{"/api/orders/quick","/api/orders/limit"}) {
        json request={{"account_ids",json::array({"A","A"})},{"symbol","AAPL"},
            {"side","BUY"},{"shares","10"},{"limit_price","10"},{"dollar_amount","100"}};
        auto result=c.Post(path,request.dump(),"application/json");
        REQUIRE(result);CHECK(result->status==400);CHECK(f.sink->history().empty());
    }
}
TEST_CASE("interrupted batch publication reports uncertain command IDs") {
    WebFixture f;auto c=f.client();f.upsert_quote("AAPL",10);
    f.sink->fail_next("test unavailable");
    json request={{"account_ids",json::array({"A","B"})},{"symbol","AAPL"},
        {"side","BUY"},{"shares","10"},{"limit_price","10"}};
    auto result=c.Post("/api/orders/limit",request.dump(),"application/json");
    REQUIRE(result);CHECK(result->status==503);
    auto reply=body(result);CHECK(reply["queued_commands"].empty());
    CHECK(reply["uncertain_command"]["account_id"]=="A");
    CHECK(reply["requested_count"]==2);
}
