#include "doctest.h"
#include "config.h"

#include <fstream>

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