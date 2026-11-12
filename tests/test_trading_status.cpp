#include "web_fixture.h"
using ngtest::WebFixture;
using ngtest::body;

TEST_CASE("Trading status page and commands require the exact lmh account") {
    WebFixture f;
    for (const auto* user : {"trader", "admin", "LMH", " lmh "}) {
        auto client=f.client_for(user);
        auto page=client.Get("/trading-status");REQUIRE(page);CHECK(page->status==403);
        CHECK(body(client.Get("/api/bootstrap"))["can_manage_trading"]==false);
        CHECK(client.Post("/api/trading-status",R"({"account_id":"A","trading_enabled":true})","application/json")->status==403);
        auto legacy=client.Post("/submit-trading-status","account_id=A&trading_enabled=true","application/x-www-form-urlencoded");
        REQUIRE(legacy);CHECK(legacy->get_header_value("Location").find("err=")!=std::string::npos);
        CHECK(f.sink->count()==0);
    }
    auto owner=f.client_for("lmh");CHECK(owner.Get("/trading-status")->status==200);
    CHECK(body(owner.Get("/api/bootstrap"))["can_manage_trading"]==true);
    CHECK(f.client("").Post("/api/trading-status","{}","application/json")->status==401);
}
