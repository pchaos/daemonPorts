#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "json.h"
#include "config.h"
#include "relay.h"

#include <fstream>

// ============================================================
// JSON 解析器测试
// ============================================================

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

// ============================================================
// 配置解析测试
// ============================================================

TEST_CASE("parseConfig - 有效配置") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "name": "web",
                "listen": ":3000",
                "command": "./web-app --port 3000",
                "delay": 5000,
                "refresh_seconds": 3,
                "auto_restart": true
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].name == "web");
    CHECK(cfgs[0].listenAddr == ":3000");
    CHECK(cfgs[0].command == "./web-app --port 3000");
    CHECK(cfgs[0].delayMs == 5000);
    CHECK(cfgs[0].refreshSeconds == 3);
    CHECK(cfgs[0].autoRestart == true);
    CHECK(cfgs[0].enabled == true);
}

TEST_CASE("parseConfig - 禁用端口") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "name": "a", "listen": ":3000", "command": "./a", "enabled": true },
            { "name": "b", "listen": ":4000", "command": "./b", "enabled": false }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].name == "a");
}

TEST_CASE("parseConfig - 配置不完整则跳过") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "name": "valid", "listen": ":3000", "command": "./app" },
            { "name": "no-cmd", "listen": ":4000" },
            { "name": "no-listen", "command": "./app" }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].name == "valid");
}

TEST_CASE("parseConfig - 默认值") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":5000", "command": "./app" }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].name == "");
    CHECK(cfgs[0].delayMs == 5000);
    CHECK(cfgs[0].refreshSeconds == 3);
    CHECK(cfgs[0].autoRestart == false);
    CHECK(cfgs[0].enabled == true);
}

TEST_CASE("parseConfig - 空配置") {
    auto cfgs = parseConfig(R"({"ports": []})");
    CHECK(cfgs.empty());
}

TEST_CASE("parseConfig - 缺少 ports 数组") {
    auto cfgs = parseConfig(R"({"something": "else"})");
    CHECK(cfgs.empty());
}

TEST_CASE("parseConfig - 多端口") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":3000", "command": "./a" },
            { "listen": ":4000", "command": "./b" },
            { "listen": ":5000", "command": "./c" }
        ]
    })");

    REQUIRE(cfgs.size() == 3);
    CHECK(cfgs[0].listenAddr == ":3000");
    CHECK(cfgs[1].listenAddr == ":4000");
    CHECK(cfgs[2].listenAddr == ":5000");
}

TEST_CASE("loadConfig - 文件不存在返回空") {
    auto cfgs = loadConfig("/tmp/nonexistent_config_deadbeef.json");
    CHECK(cfgs.empty());
}

TEST_CASE("loadConfig - 从文件加载") {
    std::string path = "/tmp/test_loadconfig.json";
    std::ofstream f(path);
    f << R"({"ports": [{"listen": ":3000", "command": "./a"}]})";
    f.close();

    auto cfgs = loadConfig(path);
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].listenAddr == ":3000");
}

// ============================================================
// HTTP 响应测试
// ============================================================

TEST_CASE("buildStartupResponse - 基本结构") {
    PortConfig cfg;
    cfg.name = "test";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(resp.find("Content-Type: text/html; charset=utf-8") != std::string::npos);
    CHECK(resp.find("Connection: close") != std::string::npos);
}

TEST_CASE("buildStartupResponse - HTML 内容") {
    PortConfig cfg;
    cfg.name = "my-service";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 3;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("my-service 启动中...") != std::string::npos);
    CHECK(resp.find("<meta http-equiv=\"refresh\" content=\"3\"") != std::string::npos);
    CHECK(resp.find("秒后自动重试") != std::string::npos);
}

TEST_CASE("buildStartupResponse - 倒计时脚本") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("var secs = 5;") != std::string::npos);
    CHECK(resp.find("document.getElementById('cd')") != std::string::npos);
    CHECK(resp.find("onload=\"tick()\"") != std::string::npos);
    CHECK(resp.find("<span id=\"cd\">5</span>") != std::string::npos);
}

TEST_CASE("buildStartupResponse - Content-Length 精确匹配") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 2;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    auto pos = resp.find("Content-Length: ");
    REQUIRE(pos != std::string::npos);
    pos += 16;
    auto end = resp.find("\r\n", pos);
    int declaredLen = std::stoi(resp.substr(pos, end - pos));

    auto bodyStart = resp.find("\r\n\r\n");
    REQUIRE(bodyStart != std::string::npos);
    bodyStart += 4;
    int actualLen = resp.size() - bodyStart;

    CHECK(declaredLen == actualLen);
}

TEST_CASE("buildStartupResponse - 名称缺省时使用 listen 地址") {
    PortConfig cfg;
    cfg.listenAddr = ":8080";
    cfg.command = "./app";

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find(":8080 启动中...") != std::string::npos);
}

TEST_CASE("buildStartupResponse - 自定义刷新秒数") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 10;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("content=\"10\"") != std::string::npos);
    CHECK(resp.find("秒后自动重试") != std::string::npos);
}