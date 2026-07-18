#include "doctest.h"
#include "config.h"
#include "relay.h"

#include <ctime>

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

// ── hasRecentActivity ──

TEST_CASE("hasRecentActivity - 默认未活跃") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";

    PortRelay relay(cfg);
    CHECK(relay.hasRecentActivity(1) == false);
    CHECK(relay.hasRecentActivity(5) == false);
    CHECK(relay.hasRecentActivity(60) == false);
}

TEST_CASE("hasRecentActivity - 当前活跃") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";

    PortRelay relay(cfg);
    // lastActiveTime_ 只能被 monitor 线程更新，
    // 但构造后默认 0 → hasRecentActivity 应返回 false
    CHECK(relay.hasRecentActivity(1) == false);
    // 时间戳为 0 时，任何分钟数都不应该认为活跃
    CHECK(relay.hasRecentActivity(0) == false);
}

TEST_CASE("gracefulStop - backendPid_ <= 0 不做任何事") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.stopCommand = "echo stop";
    cfg.idleMinutes = 10;
    PortRelay relay(cfg);
    relay.gracefulStop();
    CHECK(relay.isBackendRunning() == false);
}

TEST_CASE("idleMinutes - 构造函数正确初始化") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.idleMinutes = 7;
    PortRelay relay(cfg);
    CHECK(relay.idleMinutes() == 7);

    cfg.idleMinutes = 0;
    PortRelay relay2(cfg);
    CHECK(relay2.idleMinutes() == 20);
}

TEST_CASE("gracefulStop - backendPid_ > 0 clears pid") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.stopCommand = ""; // no stop command
    cfg.idleMinutes = 5;
    PortRelay relay(cfg);
    relay.backendPid_ = 12345; // simulate running backend
    relay.gracefulStop();
    CHECK(relay.backendPid_ == 0);
}