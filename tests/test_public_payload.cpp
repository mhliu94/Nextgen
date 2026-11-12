#include "core/public_payload.h"
#include "web_fixture.h"
using ng::json;
TEST_CASE("browser redaction covers nested rate variants and serialized reports") {
    json original={{"participation","0.12345"},{"parameters",{{"participationRate",12.345},{"mode","normal"}}},
        {"events",json::array({{{"participation_pct",12.345},{"quantity",100}}})},
        {"document",R"({"participation_percentages":{"normal":12.345},"status":"stopped"})"}};
    auto result=ng::public_payload(original);
    CHECK(result.dump().find("participation")==std::string::npos);
    CHECK(result.dump().find("12345")==std::string::npos);
    CHECK(result["parameters"]["mode"]=="normal");
    CHECK(result["events"][0]["quantity"]==100);
    CHECK(original["participation"]=="0.12345");
}
TEST_CASE("cached HTTP and SSE algorithm documents omit internal participation values") {
    ngtest::WebFixture f;f.upsert_account("A",10000,1000);f.upsert_book("XYZ",19.99,20);
    auto client=f.client();
    auto response=client.Post("/api/smart-selling",json{{"account_ids",json::array({"A"})},{"symbol","AAPL"},{"mode","normal"}}.dump(),"application/json");
    REQUIRE(response);REQUIRE(response->status==200);
    for (const auto* route:{"/api/smart-selling","/api/algo","/api/health","/api/operations"}) {
        auto result=client.Get(route);REQUIRE(result);CHECK(result->body.find("participation")==std::string::npos);
    }
    CHECK(f.control->smart_status_text()->find("participation")==std::string::npos);
    CHECK(f.control->algo_status_text()->find("participation")==std::string::npos);
    CHECK(f.control->smart_runtime()->list_parents()[0].contains("participation"));
}
