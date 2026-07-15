#include "doctest.h"
#include "json.h"

TEST_CASE("JSON 解析 - 空对象") {
    auto v = parse_json("{}");
    CHECK(v.is_obj());
    CHECK(v.o.empty());
}

TEST_CASE("JSON 解析 - 简单对象") {
    auto v = parse_json(R"({"key": "value", "num": 42, "flag": true})");
    REQUIRE(v.is_obj());
    CHECK(v.get("key")->as_str() == "value");
    CHECK(v.get("num")->as_num() == 42);
    CHECK(v.get("flag")->as_bool() == true);
}

TEST_CASE("JSON 解析 - 嵌套对象") {
    auto v = parse_json(R"({"outer": {"inner": "deep"}})");
    REQUIRE(v.is_obj());
    auto* inner = v.get("outer");
    REQUIRE(inner != nullptr);
    REQUIRE(inner->is_obj());
    CHECK(inner->get("inner")->as_str() == "deep");
}

TEST_CASE("JSON 解析 - 数组") {
    auto v = parse_json(R"({"arr": [1, "two", true]})");
    REQUIRE(v.is_obj());
    auto* arr = v.get("arr");
    REQUIRE(arr != nullptr);
    REQUIRE(arr->is_arr());
    CHECK(arr->a.size() == 3);
    CHECK(arr->idx(0)->as_num() == 1);
    CHECK(arr->idx(1)->as_str() == "two");
    CHECK(arr->idx(2)->as_bool() == true);
}

TEST_CASE("JSON 解析 - 转义字符串") {
    auto v = parse_json(R"({"s": "hello\nworld\t\"quoted\""})");
    REQUIRE(v.is_obj());
    std::string expected = "hello\nworld\t\"quoted\"";
    CHECK(v.get("s")->as_str() == expected);
}

TEST_CASE("JSON 解析 - 空数组和空对象") {
    auto v = parse_json(R"({"empty_arr": [], "empty_obj": {}})");
    REQUIRE(v.is_obj());
    CHECK(v.get("empty_arr")->is_arr());
    CHECK(v.get("empty_arr")->a.empty());
    CHECK(v.get("empty_obj")->is_obj());
    CHECK(v.get("empty_obj")->o.empty());
}

TEST_CASE("JSON 解析 - null") {
    auto v = parse_json(R"({"n": null})");
    REQUIRE(v.is_obj());
    CHECK(v.get("n")->type == JsonValue::T_NULL);
}

TEST_CASE("JSON 解析 - 负数") {
    auto v = parse_json(R"({"n": -42})");
    REQUIRE(v.is_obj());
    CHECK(v.get("n")->as_num() == -42);
}