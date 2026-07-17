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
    CHECK(cfgs[0].refreshSeconds == 5);
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

TEST_CASE("parseConfig - mixed 模式 (简写 protocols)") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3128",
                "command": "./hybrid",
                "mode": "mixed",
                "hold_port": false,
                "protocols": ["http", "socks5", "socks4"]
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].mode == "mixed");
    CHECK(cfgs[0].holdPort == false);
    REQUIRE(cfgs[0].protocols.size() == 3);
    CHECK(cfgs[0].protocols[0].type == "http");
    CHECK(cfgs[0].protocols[1].type == "socks5");
    CHECK(cfgs[0].protocols[2].type == "socks4");
}

TEST_CASE("parseConfig - mixed 模式 (完整 protocols)") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3128",
                "mode": "mixed",
                "hold_port": true,
                "protocols": [
                    { "type": "http", "command": "./web --port 8080", "proxy_to": "127.0.0.1:8080" },
                    { "type": "socks5", "command": "./socks --port 1080", "proxy_to": "127.0.0.1:1080", "delay": 3000 },
                    { "type": "socks4", "enabled": false }
                ]
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].mode == "mixed");
    CHECK(cfgs[0].holdPort == true);
    REQUIRE(cfgs[0].protocols.size() == 2);  // socks4 disabled, should be skipped

    CHECK(cfgs[0].protocols[0].type == "http");
    CHECK(cfgs[0].protocols[0].command == "./web --port 8080");
    CHECK(cfgs[0].protocols[0].proxyTo == "127.0.0.1:8080");
    CHECK(cfgs[0].protocols[0].delayMs == 5000);  // default

    CHECK(cfgs[0].protocols[1].type == "socks5");
    CHECK(cfgs[0].protocols[1].command == "./socks --port 1080");
    CHECK(cfgs[0].protocols[1].proxyTo == "127.0.0.1:1080");
    CHECK(cfgs[0].protocols[1].delayMs == 3000);
}

TEST_CASE("parseConfig - mixed 模式默认值") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":3128", "command": "./app", "protocols": ["http"] }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].mode == "simple");    // 未指定时默认 simple
    CHECK(cfgs[0].holdPort == false);    // 未指定时默认 false
    REQUIRE(cfgs[0].protocols.size() == 1);
    CHECK(cfgs[0].protocols[0].type == "http");
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

TEST_CASE("parseConfig - stack_size 自定义") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":3000", "command": "./a", "stack_size": 512 },
            { "listen": ":4000", "command": "./b" }
        ]
    })");

    REQUIRE(cfgs.size() == 2);
    CHECK(cfgs[0].stackSize == 512);
    CHECK(cfgs[1].stackSize == 512);  // 默认值
}

TEST_CASE("parseConfig - proxy 模式: 无认证") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "name": "socks5-proxy",
                "listen": ":1080",
                "mode": "proxy"
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].name == "socks5-proxy");
    CHECK(cfgs[0].listenAddr == ":1080");
    CHECK(cfgs[0].mode == "proxy");
    CHECK(cfgs[0].auth.type == "none");
    CHECK(cfgs[0].httpTarget.empty());
}

TEST_CASE("parseConfig - proxy 模式: userpass 认证") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":1080",
                "mode": "proxy",
                "auth": {
                    "type": "userpass",
                    "username": "admin",
                    "password": "secret123"
                }
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].mode == "proxy");
    CHECK(cfgs[0].auth.type == "userpass");
    CHECK(cfgs[0].auth.username == "admin");
    CHECK(cfgs[0].auth.password == "secret123");
}

TEST_CASE("parseConfig - group field present") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3000",
                "command": "./a",
                "group": "alpha"
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].groupName == "alpha");
}

TEST_CASE("parseConfig - group field absent") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3000",
                "command": "./a"
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].groupName.empty());
}


TEST_CASE("parseConfig - proxy 模式: http_target 配置") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3128",
                "mode": "proxy",
                "http_target": "127.0.0.1:8080"
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].httpTarget == "127.0.0.1:8080");
    CHECK(cfgs[0].mode == "proxy");
    CHECK(cfgs[0].auth.type == "none");
}

TEST_CASE("parseConfig - proxy 模式: 认证 + http_target") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":1080",
                "mode": "proxy",
                "auth": { "type": "userpass", "username": "user", "password": "pass" },
                "http_target": "127.0.0.1:9090"
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].auth.type == "userpass");
    CHECK(cfgs[0].auth.username == "user");
    CHECK(cfgs[0].httpTarget == "127.0.0.1:9090");
}

TEST_CASE("parseConfig - proxy 模式: 默认值") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":1080", "mode": "proxy" }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].mode == "proxy");
    CHECK(cfgs[0].auth.type == "none");
    CHECK(cfgs[0].auth.username.empty());
    CHECK(cfgs[0].httpTarget.empty());
    CHECK(cfgs[0].enabled == true);
    CHECK(cfgs[0].autoRestart == false);
}

TEST_CASE("parseConfig - proxy 模式: auth 对象不完整时只取已有字段") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":1080",
                "mode": "proxy",
                "auth": { "type": "userpass" }
            }
        ]
    })");
    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].auth.type == "userpass");
    CHECK(cfgs[0].auth.username.empty());
    CHECK(cfgs[0].auth.password.empty());
}
