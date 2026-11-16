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

// ── B1 回归: 截断输入不得死循环 (旧实现 p 越界推进, parse_obj 内嵌
//    while(true) 永不退出, 控制线程 CPU 100%)。现在返回部分结果即可。
TEST_CASE("JSON 解析 - 截断输入不死循环") {
    auto a = parse_json("{\"a\":");
    // 旧实现挂死; 修复后返回部分对象 (键已解析, 值缺失)
    CHECK(a.is_obj());
    auto b = parse_json("[1,2,");
    CHECK(b.is_arr());
    auto c = parse_json("{\"a\":[{\"b\":");
    // 不抛异常、不挂死即通过
    (void)c;
}

// ── N11 回归: 递归深度受限, 深嵌套不得栈溢出。
TEST_CASE("JSON 解析 - 深度上限") {
    // 100k 层数组: 修复前栈溢出崩溃, 修复后到 kMaxDepth(64) 截断安全返回
    std::string deep = std::string(100000, '[') + "1" + std::string(100000, ']');
    auto v = parse_json(deep);
    // 解析到深度上限后返回, 不崩溃即可
    (void)v;
    // 常规深度仍正常工作
    auto normal = parse_json(R"({"a":{"b":{"c":[1,2]}}})");
    REQUIRE(normal.is_obj());
    CHECK(normal.get("a") != nullptr);
}