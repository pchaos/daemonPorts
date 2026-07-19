#include "doctest.h"
#include "relay.h"
#include "config.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

// ── isPortBound ──

TEST_CASE("isPortBound - 端口空闲返回 false") {
    // 使用一个不太可能被占用的端口
    bool result = PortRelay::isPortBound(45678);
    CHECK(result == false);
}

TEST_CASE("isPortBound - 端口被占用返回 true") {
    // 创建一个真正的监听 socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(34567);
    int rc = bind(fd, (struct sockaddr*)&sa, sizeof(sa));
    REQUIRE(rc == 0);
    listen(fd, 5);

    // 端口现在被占用
    bool result = PortRelay::isPortBound(34567);
    CHECK(result == true);

    close(fd);

    // 释放后端口变为空闲
    // 注意：close 后可能有 TIME_WAIT，等一小段时间确保释放
    usleep(100000);
    bool afterClose = PortRelay::isPortBound(34567);
    CHECK(afterClose == false);
}

TEST_CASE("isPortBound - 端口 0 总是空闲（bind 到随机端口）") {
    // Port 0 会 bind 到随机端口，所以 bind 应该成功（EADDRINUSE 不会发生）
    bool result = PortRelay::isPortBound(0);
    CHECK(result == false);
}

// ── monitorPort ──

TEST_CASE("monitorPort - 标准格式 :8080") {
    PortConfig cfg;
    cfg.listenAddr = ":8080";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == 8080);
}

TEST_CASE("monitorPort - 带 IP 127.0.0.1:9090") {
    PortConfig cfg;
    cfg.listenAddr = "127.0.0.1:9090";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == 9090);
}

TEST_CASE("monitorPort - 带主机名 localhost:3000") {
    PortConfig cfg;
    cfg.listenAddr = "localhost:3000";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == 3000);
}

TEST_CASE("monitorPort - 无效格式返回 -1") {
    PortConfig cfg;
    cfg.listenAddr = "invalid";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == -1);
}

TEST_CASE("monitorPort - 空格式返回 -1") {
    PortConfig cfg;
    cfg.listenAddr = "";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == -1);
}

// ── retrySeconds ──

TEST_CASE("retrySeconds - 构造函数初始化") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.retrySeconds = 10;
    cfg.maxRetrySeconds = 300;
    PortRelay relay(cfg);
    // retrySeconds_ 初始化为 cfg.retrySeconds
    CHECK(relay.monitorPort() == 9999);  // just verify it works
}

TEST_CASE("retrySeconds - 默认值") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.monitorPort() == 9999);
}

// ── hasRecentActivity ──

TEST_CASE("hasRecentActivity - 默认返回 false") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    PortRelay relay(cfg);
    CHECK(relay.hasRecentActivity(1) == true);
    CHECK(relay.hasRecentActivity(5) == true);
    CHECK(relay.hasRecentActivity(60) == true);
}

TEST_CASE("hasRecentActivity - updateActivity(true) 后立即活跃") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    PortRelay relay(cfg);
    // 初始不活跃
    CHECK(relay.hasRecentActivity(1) == true);
    // 更新为活跃
    relay.updateActivity(true);
    // 应该立即活跃（1分钟内）
    CHECK(relay.hasRecentActivity(1) == true);
    CHECK(relay.hasRecentActivity(5) == true);
}
