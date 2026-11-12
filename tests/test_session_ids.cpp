#include "reporting/activity_reporting.h"
#include "core/sqlite.h"
#include "test_helpers.h"
#include <thread>
#include <set>
using ng::json;
using ng::reporting::ActivityReporting;
TEST_CASE("session numbers are global durable and duplicate starts do not consume IDs") {
    ngtest::TempDir dir("session-ids");auto path=dir.file("activity.sqlite3");
    {
        ActivityReporting first(path),second(path);
        first.start_session("one",{{"symbol","POM"},{"trading_mode","E"}},{"A"});
        first.start_session("one",{{"symbol","POM"},{"trading_mode","E"}},{"A"});
        second.start_session("two",{{"symbol","TANH"},{"trading_mode","G"}},{"A"});
        CHECK(first.session_labels().at("one")=="POM-E-1");
        CHECK(first.session_labels().at("two")=="TANH-G-2");
    }
    ActivityReporting restarted(path),other(path);
    restarted.start_session("three",{{"symbol","BDMD"},{"trading_mode","H"}},{"A"});
    CHECK(restarted.session_labels().at("three")=="BDMD-H-3");
    auto start=[](ActivityReporting& store,const std::string& prefix){
        for(int i=0;i<10;++i) store.start_session(prefix+std::to_string(i),{{"symbol","POM"},{"trading_mode","F"}},{"A"});
    };
    std::thread a(start,std::ref(restarted),"a"),b(start,std::ref(other),"b");a.join();b.join();
    auto labels=restarted.session_labels();REQUIRE(labels.size()==23);
    std::set<int> numbers;
    for(const auto& [id,label]:labels) numbers.insert(std::stoi(label.substr(label.rfind('-')+1)));
    CHECK(numbers.size()==23);CHECK(*numbers.begin()==1);CHECK(*numbers.rbegin()==23);
}
TEST_CASE("legacy sessions receive start-time-ordered public IDs without changing internal references") {
    ngtest::TempDir dir("session-backfill");auto path=dir.file("activity.sqlite3");
    {
        ng::Sqlite db(path);
        db.exec(R"SQL(CREATE TABLE activity_sessions(id TEXT PRIMARY KEY,parameters TEXT NOT NULL,account_ids TEXT NOT NULL,started_at TEXT NOT NULL,updated_at TEXT NOT NULL,ended_at TEXT,status TEXT NOT NULL,reason TEXT);
            INSERT INTO activity_sessions VALUES('later','{"symbol":"POM","trading_mode":"G"}','["A"]','2026-11-11T15:00:00Z','2026-11-11T15:00:00Z',NULL,'stopped',NULL);
            INSERT INTO activity_sessions VALUES('earlier','{"symbol":"TANH","trading_mode":"E"}','["A"]','2026-11-11T14:00:00Z','2026-11-11T14:00:00Z',NULL,'stopped',NULL);)SQL");
    }
    ActivityReporting reporting(path);
    CHECK(reporting.session_labels().at("earlier")=="TANH-E-1");
    CHECK(reporting.session_labels().at("later")=="POM-G-2");
    auto snapshot=reporting.snapshot(ngtest::directory({ngtest::meta("A",1)}));
    CHECK(snapshot["sessions"][0]["id"]=="later");
    CHECK(snapshot["sessions"][0]["display_id"]=="POM-G-2");
}
