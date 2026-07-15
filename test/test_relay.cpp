#include "doctest.h"
#include "config.h"
#include "relay.h"

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