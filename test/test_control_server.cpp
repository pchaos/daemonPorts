#include "doctest.h"
#include "control_server.h"
#include <sstream>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include "relay_platform.h"

static int httpRequestPath(int fd, uint16_t port, const char* path) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (platform::connect_fd(fd, &addr, sizeof(addr)) < 0) {
        platform::close_fd(fd);
        return -1;
    }
    platform::set_recv_timeout(fd, 3);

    std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
    ssize_t wn = platform::write_fd(fd, req.c_str(), req.size());
    if (wn <= 0) { platform::close_fd(fd); return -1; }

    char buf[4096];
    ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
    platform::close_fd(fd);
    if (n <= 0) return -1;

    std::string resp(buf, static_cast<size_t>(n));
    if (resp.find("HTTP/1.1 200") != std::string::npos) return 200;
    if (resp.find("HTTP/1.1 429") != std::string::npos) return 429;
    return -1;
}

static int httpRequest(int fd, uint16_t port) {
    return httpRequestPath(fd, port, "/health");
}

TEST_CASE("ControlServer - rate limiter blocks after maxConnections") {
    ControlConfig cfg;
    cfg.listen = ":19997";
    cfg.auth.type = "none";  // explicit no-auth for rate-limit testing
    ControlServer server(cfg);
    server.setRateLimit(2, 60);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int r0 = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) r0 = httpRequest(fd, 19997);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int r1 = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) r1 = httpRequest(fd, 19997);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int r2 = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) r2 = httpRequest(fd, 19997);
    }

    server.stop();

    CHECK(r0 == 200);
    CHECK(r1 == 200);
    CHECK(r2 == 429);
}

TEST_CASE("ControlServer - rate limiter window expiry") {
    ControlConfig cfg;
    cfg.listen = ":19996";
    cfg.auth.type = "none";  // explicit no-auth for rate-limit testing
    ControlServer server(cfg);
    server.setRateLimit(2, 1);
    server.start();

    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // 2 allowed
    int a = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) a = httpRequest(fd, 19996);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int b = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) b = httpRequest(fd, 19996);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int c = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) c = httpRequest(fd, 19996);
    }
    CHECK(c == 429);

    // Wait for window
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));

    int d = -1;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) d = httpRequest(fd, 19996);
    }
    CHECK(d == 200);

    server.stop();
}

TEST_CASE("ControlServer - /version returns version JSON") {
    ControlConfig cfg;
    cfg.listen = ":19995";
    cfg.auth.type = "none";
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string body;
    {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        REQUIRE(fd >= 0);
        sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(19995);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(platform::connect_fd(fd, &addr, sizeof(addr)) >= 0);
        platform::set_recv_timeout(fd, 3);
        const char* req = "GET /version HTTP/1.1\r\nHost: localhost\r\n\r\n";
        platform::write_fd(fd, req, strlen(req));
        char buf[4096];
        ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
        platform::close_fd(fd);
        REQUIRE(n > 0);
        body = std::string(buf, static_cast<size_t>(n));
    }

    // HTTP 200 + version JSON with a non-empty version value.
    // The test target compiles without GATEKEEPER_VERSION, so the value is
    // "unknown"; the real gatekeeper target injects the actual version.
    CHECK(body.find("HTTP/1.1 200") != std::string::npos);
    CHECK(body.find("\"version\":\"") != std::string::npos);
    std::string val = body.substr(body.find("\"version\":\"") + 11);
    val = val.substr(0, val.find('"'));
    CHECK(!val.empty());

    server.stop();
}
