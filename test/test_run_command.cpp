#include "doctest.h"
#include "control_server.h"
#include "config.h"
#include <sstream>
#include <iostream>
#include <cstring>
#include <thread>
#include <chrono>
#include "relay_platform.h"

static int runPost(int fd, uint16_t port, const std::string& body, const char* token) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (platform::connect_fd(fd, &addr, sizeof(addr)) < 0) {
        platform::close_fd(fd);
        return -1;
    }
    platform::set_recv_timeout(fd, 8);
    std::ostringstream req;
    req << "POST /run HTTP/1.1\r\nHost: localhost\r\n";
    req << "Content-Type: application/json\r\n";
    req << "Content-Length: " << body.size() << "\r\n";
    if (token && token[0]) req << "x-auth-token: " << token << "\r\n";
    req << "\r\n";
    req << body;
    ssize_t wn = platform::write_fd(fd, req.str().c_str(), req.str().size());
    if (wn <= 0) { platform::close_fd(fd); return -1; }
    char buf[8192];
    ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
    platform::close_fd(fd);
    if (n <= 0) return -1;
    std::string resp(buf, static_cast<size_t>(n));
    if (resp.find("HTTP/1.1 200") != std::string::npos) return 200;
    if (resp.find("HTTP/1.1 401") != std::string::npos) return 401;
    if (resp.find("HTTP/1.1 404") != std::string::npos) return 404;
    return -1;
}

TEST_CASE("ControlServer - /run whitelist sync command") {
    ControlConfig cfg;
    cfg.listen = ":29990";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.pin = "1234";
    cfg.commands.push_back({"test-cmd", "echo hello world"});
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29990, "{\"name\":\"test-cmd\"}", "test-token") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 200);
    server.stop();
}

TEST_CASE("ControlServer - /run ad-hoc command with PIN") {
    ControlConfig cfg;
    cfg.listen = ":29991";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.pin = "1234";
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29991, "{\"command\":\"echo test123\",\"pin\":\"1234\"}", "test-token") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 200);
    server.stop();
}

TEST_CASE("ControlServer - /run ad-hoc wrong PIN rejected") {
    ControlConfig cfg;
    cfg.listen = ":29992";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.pin = "1234";
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29992, "{\"command\":\"echo test\",\"pin\":\"wrong\"}", "test-token") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 401);
    server.stop();
}

TEST_CASE("ControlServer - /run ad-hoc without PIN rejected") {
    ControlConfig cfg;
    cfg.listen = ":29993";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.pin = "1234";
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29993, "{\"command\":\"echo test\"}", "test-token") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 401);
    server.stop();
}

TEST_CASE("ControlServer - /run unknown name returns 404") {
    ControlConfig cfg;
    cfg.listen = ":29994";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.commands.push_back({"existing", "echo ok"});
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29994, "{\"name\":\"nonexistent\"}", "test-token") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 404);
    server.stop();
}

TEST_CASE("ControlServer - /run missing token returns 401") {
    ControlConfig cfg;
    cfg.listen = ":29995";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.pin = "1234";
    cfg.commands.push_back({"test", "echo ok"});
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    int r = fd >= 0 ? runPost(fd, 29995, "{\"name\":\"test\"}", "") : -1;
    if (fd >= 0) platform::close_fd(fd);
    CHECK(r == 401);
    server.stop();
}

TEST_CASE("ControlServer - /run stream preset returns chunked") {
    ControlConfig cfg;
    cfg.listen = ":29996";
    cfg.auth.type = "token";
    cfg.auth.token = "test-token";
    cfg.commands.push_back({"stream-echo", "echo line1 && sleep 0.1 && echo line2"});
    ControlServer server(cfg);
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(29996);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(platform::connect_fd(fd, &addr, sizeof(addr)) >= 0);
    platform::set_recv_timeout(fd, 8);
    std::string body = "{\"name\":\"stream-echo\",\"stream\":true}";
    std::ostringstream req;
    req << "POST /run HTTP/1.1\r\nHost: localhost\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "x-auth-token: test-token\r\n\r\n" << body;
    std::string reqStr = req.str();
    REQUIRE(platform::write_fd(fd, reqStr.data(), reqStr.size()) > 0);
    // Read headers + chunked frames until the [exit=N] terminator arrives.
    std::string all;
    while (all.find("[exit=") == std::string::npos) {
        char buf[4096];
        ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
        if (n <= 0) break;  // closed or read timeout
        all.append(buf, static_cast<size_t>(n));
    }
    platform::close_fd(fd);
    bool gotChunked = all.find("Transfer-Encoding: chunked") != std::string::npos;
    bool gotLine1 = all.find("line1") != std::string::npos;
    bool gotLine2 = all.find("line2") != std::string::npos;
    bool gotExit = all.find("[exit=0]") != std::string::npos;
    CHECK(gotChunked);
    CHECK(gotLine1);
    CHECK(gotLine2);
    CHECK(gotExit);
    server.stop();
}

TEST_CASE("ControlServer - runShellCommand sync capture") {
    std::string output;
    int ec = platform::runShellCommand("echo hello", 5000, 65536, output);
    CHECK(ec == 0);
    CHECK(output.find("hello") != std::string::npos);
}

TEST_CASE("ControlServer - runShellCommand timeout returns nonzero") {
    std::string output;
    int ec = platform::runShellCommand("sleep 10", 100, 65536, output);
    CHECK(ec != 0);
}

TEST_CASE("ControlServer - verifyPin returns true for correct PIN") {
    ControlConfig cfg;
    cfg.pin = "1234";
    ControlServer server(cfg);
    bool ok = server.verifyPin("1234");
    CHECK(ok);
    ok = server.verifyPin("wrong");
    CHECK(!ok);
    ok = server.verifyPin("123");
    CHECK(!ok);
    ok = server.verifyPin("12345");
    CHECK(!ok);
    cfg.pin = "";
    ControlServer server2(cfg);
    ok = server2.verifyPin("anything");
    CHECK(!ok);
}

