#include "web_fixture.h"
using ng::json;
using ngtest::WebFixture;
using ngtest::body;

TEST_CASE("individual E and F stops target one session despite shared mode and ticker") {
    for(const auto* mode:{"E","F"}) {
        WebFixture f;for(const auto* id:{"A","B"})f.upsert_account(id,10000,1000,true,"AAPL");
        f.upsert_book("AAPL",10,10.01);
        f.control->start_fast_strategy(f.fast_start("first",mode,"AAPL",{"A"},true));
        f.control->start_fast_strategy(f.fast_start("second",mode,"AAPL",{"B"},true));
        auto c=f.client_for("trader");
        CHECK(c.Post("/api/algo/fast/missing/stop","{}","application/json")->status==404);
        CHECK(f.control->fast_manager()->statuses().size()==2);
        auto response=c.Post("/api/algo/fast/first/stop","{}","application/json");REQUIRE(response);CHECK(response->status==200);
        auto status=f.control->fast_manager()->statuses();REQUIRE(status.size()==1);CHECK(status[0].session_id=="second");
        CHECK(f.sink->count()==0); // no global stop notification
    }
}

TEST_CASE("active Algo API omits concluded Smart sessions while retaining their order reports") {
    WebFixture f;f.upsert_account("A",10000,1000,true,"AAPL");f.upsert_book("AAPL",10,10.01);
    auto parent=f.control->start_smart_parent({{"account_ids",{"A"}},{"symbol","AAPL"}});
    auto id=parent.at("parent_id").get<std::string>();auto c=f.client_for("trader");
    CHECK(body(c.Get("/api/algo"))["smart"]["parents"].size()==1);
    CHECK(c.Post("/api/smart-selling/"+id+"/stop","{}","application/json")->status==200);
    CHECK(body(c.Get("/api/algo"))["smart"]["parents"].empty());
    CHECK(body(c.Get("/api/smart-selling"))["parents"].empty());
    CHECK(f.control->reporting()->snapshot(f.control->accounts())["sessions"].size()==1);
}
