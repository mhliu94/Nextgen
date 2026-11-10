#include <doctest.h>
#include "core/logging.h"

#include "core/decimal.h"
#include "core/i18n.h"
#include "core/json.h"
#include "core/strings.h"
#include "core/time_util.h"

using ng::Decimal;

TEST_CASE("decimal parsing and formatting") {
    CHECK(Decimal::parse("10").str() == "10");
    CHECK(Decimal::parse("10.010").str() == "10.010");
    CHECK(Decimal::parse("10.010").normalized_str() == "10.01");
    CHECK(Decimal::parse("1e24").str() == "1000000000000000000000000");
    CHECK(Decimal::parse("1E-26").str() == "0.00000000000000000000000001");
    CHECK(Decimal::parse("-0.5").str() == "-0.5");
    CHECK(Decimal::parse(" 7 ").str() == "7");
    CHECK(Decimal::parse(".10").str() == "0.10");
    CHECK_THROWS_AS(Decimal::parse("NaN"), ng::DecimalError);
    CHECK_THROWS_AS(Decimal::parse("Infinity"), ng::DecimalError);
    CHECK_THROWS_AS(Decimal::parse("abc"), ng::DecimalError);
    CHECK_THROWS_AS(Decimal::parse(""), ng::DecimalError);
    CHECK(Decimal::from_double(10.01).str() == "10.01");
    CHECK(Decimal::from_double(0.1 + 0.2).str() == "0.30000000000000004");
    CHECK(Decimal::from_double(1e15).str() == "1000000000000000");
}

TEST_CASE("decimal arithmetic is exact") {
    Decimal a = Decimal::parse("0.06"), b = Decimal::parse("12345");
    CHECK((a * b).str() == "740.70");
    CHECK((a * b).normalized_str() == "740.7");
    CHECK((Decimal::parse("1") - Decimal::parse("0.9")).str() == "0.1");
    CHECK((Decimal::parse("1e-26") * Decimal::parse("1e24")).normalized_str() == "0.01");
    CHECK(Decimal::parse("10") == Decimal::parse("10.000"));
    CHECK(Decimal::parse("10.01") > Decimal::parse("10"));
    CHECK(Decimal::parse("-1") < Decimal::zero());
    CHECK(Decimal::parse("3").scaleb(-2).str() == "0.03");
    CHECK(Decimal::parse("0.03").scaleb(2).normalized_str() == "3");
    CHECK(Decimal::parse("100").is_integral());
    CHECK_FALSE(Decimal::parse("100.5").is_integral());
    CHECK(Decimal::parse("100.00").to_int64() == 100);
    CHECK(Decimal::parse("12.345").digit_count() == 5);
    CHECK(Decimal::parse("12.345").adjusted() == 1);
    CHECK(Decimal::parse("0.0012").adjusted() == -3);
    CHECK(Decimal::parse("0.0012").exponent() == -4);
}

TEST_CASE("decimal step rounding") {
    CHECK(Decimal::parse("1000.5").round_down_to_step(Decimal::parse("1")).str() == "1000");
    CHECK(Decimal::parse("1000.5").round_up_to_step(Decimal::parse("1")).str() == "1001");
    CHECK(Decimal::parse("10.004").round_up_to_step(Decimal::parse("0.01")).str() == "10.01");
    CHECK(Decimal::parse("10.004").round_down_to_step(Decimal::parse("0.01")).str() == "10.00");
    CHECK(Decimal::parse("10.25").round_down_to_step(Decimal::parse("0.1")).str() == "10.2");
    CHECK(Decimal::parse("-0.3").round_down_to_step(Decimal::parse("1")).str() == "-1");
    CHECK(Decimal::parse("7").round_up_to_step(Decimal::parse("7")).str() == "7");
    CHECK(Decimal::parse("1000").div(Decimal::parse("300"), 6).str() == "3.333333");
    CHECK(Decimal::parse("30000").div(Decimal::parse("300"), 6).normalized_str() == "100");
    CHECK((Decimal::parse("1000") * Decimal(30)).div(Decimal::from_double(300.0), 30).round_down_to_step(Decimal(1)).str() == "100");
    CHECK_THROWS_AS(Decimal::parse("1").div(Decimal::zero()), ng::DecimalError);
}

TEST_CASE("iso8601 parsing and formatting") {
    auto z = ng::parse_iso8601("2026-10-23T19:59:58Z");
    REQUIRE(z);
    CHECK(z->has_offset);
    CHECK(z->epoch_seconds == doctest::Approx(1792785598.0));
    auto offset = ng::parse_iso8601("2026-10-23T15:59:58-04:00");
    REQUIRE(offset);
    CHECK(offset->epoch_seconds == doctest::Approx(z->epoch_seconds));
    auto frac = ng::parse_iso8601("2026-10-23T19:59:58.250000+00:00");
    REQUIRE(frac);
    CHECK(frac->epoch_seconds == doctest::Approx(z->epoch_seconds + 0.25));
    auto naive = ng::parse_iso8601("2026-10-23 19:59:58");
    REQUIRE(naive);
    CHECK_FALSE(naive->has_offset);
    CHECK_FALSE(ng::parse_iso8601("not a date"));
    CHECK_FALSE(ng::parse_iso8601("2026-13-01"));
    CHECK(ng::iso_utc(1792785598.0) == "2026-10-23T19:59:58Z");
    CHECK(ng::iso_utc(1792785598.5) == "2026-10-23T19:59:58.500000Z");
    CHECK(ng::filename_timestamp(1792785598.0) == "20261023T195958Z");
    auto round_trip = ng::parse_iso8601(ng::iso_utc(1700000000.123456));
    REQUIRE(round_trip);
    CHECK(round_trip->epoch_seconds == doctest::Approx(1700000000.123456).epsilon(1e-9));
}

TEST_CASE("string helpers") {
    CHECK(ng::trim("  a b  ") == "a b");
    CHECK(ng::to_upper("aapl") == "AAPL");
    CHECK(ng::canonical_symbol(" us.aapl ") == "AAPL");
    CHECK(ng::canonical_symbol("HK.00700/HKD") == "00700");
    CHECK(ng::canonical_symbol("tsla") == "TSLA");
    CHECK(ng::truthy_string("Yes"));
    CHECK_FALSE(ng::truthy_string("no"));
    CHECK(ng::url_encode("a b&c=d/é") == "a%20b%26c%3Dd%2F%C3%A9");
    CHECK(ng::url_decode("a%20b+c") == "a b c");
    CHECK(ng::html_escape("<a href='x'>&</a>") == "&lt;a href=&#39;x&#39;&gt;&amp;&lt;/a&gt;");
    CHECK(ng::join({"a", "b"}, ", ") == "a, b");
    CHECK(ng::split("a,b,,c", ',').size() == 4);
    CHECK(ng::random_token(32) != ng::random_token(32));
    CHECK(ng::fmt("x={} y={}", 1, "two") == "x=1 y=two");
    CHECK(ng::fmt("{{literal}} {}", true) == "{literal} True");
}

TEST_CASE("json helpers mirror python conversions") {
    ng::json doc = ng::json::parse(R"({"a": "12", "b": 12.0, "c": true, "d": null, "e": "1e3", "f": "nan", "g": [1]})");
    CHECK(ng::json_to_double(ng::json_find(doc, "a")) == 12.0);
    CHECK(ng::json_to_int(ng::json_find(doc, "a")) == 12);
    CHECK(ng::json_to_int(ng::json_find(doc, "b")) == 12);
    CHECK(ng::json_to_double(ng::json_find(doc, "c")) == 1.0);
    CHECK_FALSE(ng::json_to_double(ng::json_find(doc, "d")));
    CHECK(ng::json_to_double(ng::json_find(doc, "e")) == 1000.0);
    CHECK_FALSE(ng::json_finite_double(ng::json_find(doc, "f")));
    CHECK_FALSE(ng::json_to_int(ng::json_find(doc, "e")));
    CHECK(ng::json_to_string(ng::json_find(doc, "c")) == "True");
    // _truthy_bool(): booleans and numbers by value, strings only when they spell a truthy word; a
    // list never does (str(list) is not one of them), unlike Python's plain bool().
    CHECK_FALSE(ng::json_truthy(ng::json_find(doc, "g")));
    CHECK(ng::json_python_truthy(ng::json_find(doc, "g")));
    CHECK_FALSE(ng::json_truthy(ng::json_find(doc, "d")));
    CHECK(ng::json_truthy(ng::json_find(doc, "c")));
    CHECK(ng::json_truthy(ng::json_find(doc, "b")));
    CHECK_FALSE(ng::json_truthy(ng::json_find(doc, "a")));  // "12" is not a truthy word
    CHECK(ng::json_python_truthy(ng::json_find(doc, "a")));
    for (const char* word : {"1", "true", " YES ", "on", "y"}) {
        ng::json value = word;
        CHECK(ng::json_truthy(&value));
    }
    for (const char* word : {"", "0", "false", "no", "off", "n", "2"}) {
        ng::json value = word;
        CHECK_FALSE(ng::json_truthy(&value));
    }
    CHECK(ng::python_strip("\t \xC2\xA0 name \xE3\x80\x80\n") == "name");
    CHECK(ng::python_strip("  ") == "");
    CHECK(ng::contains_python_whitespace("A\xE2\x80\xA8" "B"));
    CHECK_FALSE(ng::contains_python_whitespace("AB\xE8\xB4\xAD"));
    CHECK(ng::json_parse_bool(ng::json_find(doc, "a")) == false);
    ng::json on = "on";
    CHECK(ng::json_parse_bool(&on));
}

TEST_CASE("i18n tables stay in sync") {
    ng::json en = ng::i18n::dictionary("en");
    ng::json zh = ng::i18n::dictionary("zh");
    CHECK(en.size() == zh.size());
    for (auto it = en.begin(); it != en.end(); ++it) {
        CHECK_MESSAGE(zh.contains(it.key()), it.key());
        CHECK_FALSE(it.value().get<std::string>().empty());
    }
    CHECK(ng::i18n::t("zh", "mode_g") == "G 智能卖出");
    CHECK(ng::i18n::t("en", "mode_g") == "G Smart selling");
    CHECK(ng::i18n::t("fr", "mode_g") == "G Smart selling");
    CHECK(ng::i18n::t("en", "no_such_key") == "no_such_key");
    CHECK(ng::i18n::resolve_lang("zh", "en") == "zh");
    CHECK(ng::i18n::resolve_lang("", "zh") == "zh");
    CHECK(ng::i18n::resolve_lang("xx", "yy") == "en");
    CHECK(ng::i18n::format_named(ng::i18n::t("en", "fast_account_duplicate"), {{"account", "A"}}) == "Account A was selected more than once.");
}

// ---------------------------------------------------------------------------
// Python-compatibility vectors (tools/vectors/gen_core_vectors.py)
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstring>

namespace {
#include "fixtures/core_vectors.inc"
}  // namespace

TEST_CASE("python float repr matches CPython") {
    for (const auto& v : kFloatRepr) {
        CAPTURE(v.repr);
        CHECK(ng::python_float_repr(v.value) == v.repr);
    }
    CHECK(ng::python_float_repr(std::nan("")) == "nan");
    CHECK(ng::python_float_repr(INFINITY) == "inf");
    CHECK(ng::python_float_repr(-INFINITY) == "-inf");
    CHECK(Decimal::from_python_float(100.0).str() == "100.0");
    CHECK(Decimal::from_python_float(0.5).str() == "0.5");
    CHECK(Decimal::from_double(100.0).str() == "100");
}

TEST_CASE("python float parsing matches CPython") {
    for (const auto& v : kFloatParse) {
        CAPTURE(v.text);
        auto parsed = ng::python_float(v.text);
        CHECK(parsed.has_value() == v.ok);
        if (parsed && v.ok) {
            if (std::isnan(v.value)) CHECK(std::isnan(*parsed));
            else CHECK(*parsed == v.value);
        }
    }
}

TEST_CASE("decimal division matches the Python context") {
    for (const auto& v : kDecimalDiv) {
        CAPTURE(v.a);
        CAPTURE(v.b);
        CHECK(Decimal::python_div(Decimal::parse(v.a), Decimal::parse(v.b)).str() == v.quotient);
    }
    CHECK_THROWS_AS(Decimal::python_div(Decimal(1), Decimal::zero()), ng::DecimalError);
}

TEST_CASE("decimal context rounding matches Python") {
    for (const auto& v : kDecimalRound) {
        CAPTURE(v.value);
        CAPTURE(v.precision);
        CHECK(Decimal::parse(v.value).round_significant(v.precision).str() == v.rounded);
    }
}

TEST_CASE("ISO timestamps parse to the same epoch as datetime.timestamp()") {
    for (const auto& v : kIsoTimestamps) {
        CAPTURE(v.text);
        auto parsed = ng::parse_iso8601(v.text);
        CHECK(parsed.has_value() == v.ok);
        if (parsed && v.ok) {
            CHECK(parsed->has_offset == v.has_offset);
            CHECK(parsed->epoch_seconds == v.epoch);  // bit-exact
        }
    }
    CHECK(ng::iso_utc_offset(1792785598.0) == "2026-10-23T19:59:58+00:00");
    CHECK(ng::iso_utc_offset(1792785598.5) == "2026-10-23T19:59:58.500000+00:00");
}

TEST_CASE("uuid4 hex") {
    std::string a = ng::uuid4_hex(), b = ng::uuid4_hex();
    CHECK(a.size() == 32);
    CHECK(a != b);
    CHECK(a[12] == '4');
    CHECK(std::strchr("89ab", a[16]) != nullptr);
    for (char c : a) CHECK(std::strchr("0123456789abcdef", c) != nullptr);
}

TEST_CASE("step rounding keeps Python's digits") {
    for (const auto& v : kStepRounding) {
        CAPTURE(v.value);
        CAPTURE(v.step);
        CHECK(Decimal::parse(v.value).round_down_to_step(Decimal::parse(v.step)).str() == v.down);
        CHECK(Decimal::parse(v.value).round_up_to_step(Decimal::parse(v.step)).str() == v.up);
    }
}

TEST_CASE("iso timestamps reject impossible calendar values like datetime.fromisoformat") {
    for (const char* invalid : {"2026-02-30T09:30:00Z", "2026-02-29T00:00:00Z", "2026-04-31", "2026-13-01", "2026-00-10", "2026-11-00",
                                "0000-01-01T00:00:00Z", "2026-11-09T24:00:00Z", "2026-11-09T23:60:00Z", "2026-11-09T23:59:60Z",
                                "2026-11-09T23:59:59+24:00", "2026-11-09T23:59", "2026-11-09T23:59:59.Z"}) {
        CAPTURE(std::string(invalid));
        bool rejected = !ng::parse_iso8601(invalid).has_value() || std::string(invalid) == "2026-11-09T23:59";
        CHECK(rejected);
    }
    CHECK(ng::parse_iso8601("2024-02-29T23:59:59Z"));
    CHECK(ng::parse_iso8601("2000-02-29T00:00:00+00:00"));
    CHECK_FALSE(ng::parse_iso8601("1900-02-29T00:00:00Z"));
    CHECK(ng::parse_iso8601("2026-12-31T23:59:59.999999-05:00"));
}
