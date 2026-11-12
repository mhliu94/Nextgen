#include "test_helpers.h"
#include "kafka/feeds.h"
using ng::json;

TEST_CASE("account deltas preserve unchanged holdings and recover only from checkpoints") {
    auto accounts=ngtest::directory({ngtest::meta("A",1)});
    ng::AccountStore store;
    ng::KafkaConfig cfg;
    ng::kafka::AccountDetailsConsumer consumer(cfg,store,accounts);
    json full={{"account_id","A"},{"account_num_id",1},{"account_update_version",1},
        {"message_type","account_snapshot"},{"stream_id","one"},{"sequence",1},
        {"ts","2026-11-11T12:00:00Z"},{"cash",100},{"trading_enabled",true},
        {"cash_by_currency",{{"USD",100},{"HKD",20}}},
        {"positions",json::array({{{"symbol","X"},{"qty",10}},{{"symbol","Y"},{"qty",20}}})}};
    auto apply=[&](const json& event){return consumer.handle_payload(event,"account-details",0,0);};
    REQUIRE(apply(full));
    json delta={{"account_id","A"},{"account_num_id",1},{"account_update_version",1},
        {"message_type","account_delta"},{"stream_id","one"},{"sequence",2},{"base_sequence",1},
        {"ts","2026-11-11T12:00:01Z"},{"cash",130},{"cash_by_currency",{{"USD",130},{"HKD",nullptr}}},
        {"positions",json::array({{{"symbol","X"},{"qty",7}}})},{"removed_positions",json::array()}};
    REQUIRE(apply(delta));
    CHECK(store.get("A")->cash==130);
    CHECK(store.get("A")->positions.size()==2);
    CHECK(store.get("A")->symbol_qty("X")==7);
    CHECK_FALSE(store.get("A")->cash_by_currency.count("HKD"));
    CHECK_FALSE(apply(delta));
    delta["sequence"]=4;delta["base_sequence"]=3;
    CHECK_FALSE(apply(delta));
    CHECK_FALSE(store.get("A")->trading_enabled);
    CHECK_FALSE(store.get("A")->ts);
    full["sequence"]=5;full["ts"]="2026-11-11T12:00:02Z";
    REQUIRE(apply(full));
    CHECK(store.get("A")->trading_enabled);
    delta["sequence"]=6;delta["base_sequence"]=5;delta["ts"]="2026-11-11T12:00:03Z";
    delta["positions"]=json::array();delta["removed_positions"]=json::array({"X"});
    REQUIRE(apply(delta));
    CHECK(store.get("A")->positions.size()==1);
    full["stream_id"]="two";full["sequence"]=1;full["ts"]="2026-11-11T12:00:04Z";
    REQUIRE(apply(full));
    delta["sequence"]=7;delta["base_sequence"]=6;delta["ts"]="2026-11-11T12:00:05Z";
    CHECK_FALSE(apply(delta));
    CHECK(store.get("A")->positions.size()==2);
}

TEST_CASE("malformed and initial account deltas never become full snapshots") {
    auto accounts=ngtest::directory({ngtest::meta("A",1)});
    ng::AccountStore store;ng::KafkaConfig cfg;
    ng::kafka::AccountDetailsConsumer consumer(cfg,store,accounts);
    json delta={{"account_id","A"},{"account_update_version",1},{"message_type","account_delta"},
        {"stream_id","s"},{"sequence",2},{"base_sequence",1},{"ts","2026-11-11T12:00:00Z"},
        {"positions",json::array()},{"removed_positions",json::array()}};
    CHECK_FALSE(consumer.handle_payload(delta,"account-details",0,0));
    CHECK_FALSE(store.get("A"));
    delta.erase("account_update_version");
    CHECK_FALSE(consumer.handle_payload(delta,"account-details",0,0));
    CHECK_FALSE(store.get("A"));
}
