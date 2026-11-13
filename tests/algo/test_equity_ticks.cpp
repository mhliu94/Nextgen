#include "smart_fixtures.h"
#include "core/equity_price.h"

using namespace ngtest;

TEST_CASE("equity ticks preserve sub-dollar quotes and switch grids at one dollar") {
    const auto cent=Dec("0.01");
    CHECK(ng::equity_tick_size(Dec("0.0911"),cent)==Dec("0.0001"));
    CHECK(ng::nearest_equity_price(Dec("0.0911"),cent)==Dec("0.0911"));
    CHECK(ng::nearest_equity_price(Dec("0.09105"),cent)==Dec("0.0911"));
    CHECK(ng::nearest_equity_price(Dec("0.091049"),cent)==Dec("0.0910"));
    CHECK(ng::round_equity_price_up(Dec("0.09101"),cent)==Dec("0.0911"));
    CHECK(ng::equity_tick_size(Dec("1"),cent)==cent);
    CHECK(ng::nearest_equity_price(Dec("0.99995"),cent)==Dec("1"));
    CHECK(ng::nearest_equity_price(Dec("1.0001"),cent)==Dec("1"));
    CHECK(ng::round_equity_price_up(Dec("1.0001"),cent)==Dec("1.01"));
    CHECK(ng::previous_equity_price(Dec("1"),cent)==Dec("0.9999"));
    CHECK(ng::previous_equity_price(Dec("1.01"),cent)==Dec("1"));
    CHECK(ng::previous_equity_price(Dec("0.0911"),cent)==Dec("0.0910"));
}

TEST_CASE("equity ticks retain upper-band symbol overrides without admitting invalid prices") {
    CHECK(ng::nearest_equity_price(Dec("1.0225"),Dec("0.005"))==Dec("1.025"));
    CHECK(ng::nearest_equity_price(Dec("0.0911"),Dec("0.005"))==Dec("0.0911"));
    CHECK(ng::round_equity_price_up(Dec("0.99999"),Dec("0.03"))==Dec("1.02"));
    CHECK(ng::round_equity_price_down(Dec("1.01"),Dec("0.03"))==Dec("0.9999"));
    CHECK(ng::previous_equity_price(Dec("1.02"),Dec("0.03"))==Dec("0.9999"));
    CHECK_THROWS(ng::nearest_equity_price(Dec("0"),Dec("0.01")));
    CHECK_THROWS(ng::nearest_equity_price(Dec("0.0911"),Dec("0")));
}

TEST_CASE_FIXTURE(EngineFixture,"Smart selling uses sub-dollar ticks during planning and dispatch") {
    start({{"disable_child_cap",true}});
    quote({{"bid","0.0910"},{"ask","0.0911"}});tape();auto order=child();
    CHECK(Dec(order["limit_price"])==Dec("0.0910"));
    quote({{"bid","0.0912"},{"ask","0.0913"}});
    auto prepared=engine->prepare_dispatch(S(order["client_order_id"]));REQUIRE(prepared);
    CHECK(Dec(prepared->at("limit_price"))==Dec("0.0912"));
}

TEST_CASE_FIXTURE(EngineFixture,"Smart selling respects fractional floors on the sub-dollar grid") {
    start({{"disable_child_cap",true},{"minimum_price","0.09105"}});
    quote({{"bid","0.0910"},{"ask","0.0911"},{"ask_prices",json::array({"0.0911","0.0912"})}});
    tape();auto order=child();CHECK(Dec(order["limit_price"])==Dec("0.0911"));
}

TEST_CASE_FIXTURE(EngineFixture,"Smart selling recovered cent-configured parents select ticks from current prices") {
    start({{"disable_child_cap",true}});engine.reset();reopen();
    {
        ng::Sqlite db(path);ng::Statement row(db,"SELECT data FROM smart_parents WHERE id=?");
        row.bind(1,parent);REQUIRE(row.step());CHECK(Dec(json::parse(row.column_text(0))["tick_size"])==Dec("0.01"));
    }
    quote({{"bid","0.9999"},{"ask","1"}});tape();auto order=child();
    CHECK(Dec(order["limit_price"])==Dec("0.9999"));
    quote({{"bid","1"},{"ask","1.01"}});
    auto prepared=engine->prepare_dispatch(S(order["client_order_id"]));REQUIRE(prepared);
    CHECK(Dec(prepared->at("limit_price"))==Dec("1"));
}
