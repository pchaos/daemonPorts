#include "doctest.h"
#include "tcp_monitor.h"
#include "config.h"

#include <netinet/tcp.h>
#include <netinet/in.h>

// ── tcpStateStr ──

TEST_CASE("tcpStateStr - 所有已知状态") {
    CHECK(std::string(tcpStateStr(TCP_ESTABLISHED)) == "ESTABLISHED");
    CHECK(std::string(tcpStateStr(TCP_SYN_SENT))    == "SYN_SENT");
    CHECK(std::string(tcpStateStr(TCP_SYN_RECV))    == "SYN_RECV");
    CHECK(std::string(tcpStateStr(TCP_FIN_WAIT1))   == "FIN_WAIT1");
    CHECK(std::string(tcpStateStr(TCP_FIN_WAIT2))   == "FIN_WAIT2");
    CHECK(std::string(tcpStateStr(TCP_TIME_WAIT))   == "TIME_WAIT");
    CHECK(std::string(tcpStateStr(TCP_CLOSE))       == "CLOSE");
    CHECK(std::string(tcpStateStr(TCP_CLOSE_WAIT))  == "CLOSE_WAIT");
    CHECK(std::string(tcpStateStr(TCP_LAST_ACK))    == "LAST_ACK");
    CHECK(std::string(tcpStateStr(TCP_LISTEN))      == "LISTEN");
    CHECK(std::string(tcpStateStr(TCP_CLOSING))     == "CLOSING");
}

TEST_CASE("tcpStateStr - 未知状态返回 UNKNOWN") {
    CHECK(std::string(tcpStateStr(99)) == "UNKNOWN");
    CHECK(std::string(tcpStateStr(0xFF)) == "UNKNOWN");
}

// ── fmtAddrPort ──

TEST_CASE("fmtAddrPort - IPv4 mapped IPv6 (::ffff:x.x.x.x)") {
    struct in6_addr addr{};
    // ::ffff:127.0.0.1
    addr.s6_addr[10] = 0xff;
    addr.s6_addr[11] = 0xff;
    addr.s6_addr[12] = 127;
    addr.s6_addr[13] = 0;
    addr.s6_addr[14] = 0;
    addr.s6_addr[15] = 1;

    std::string s = fmtAddrPort(addr, htons(8080));
    CHECK(s.find("127.0.0.1") != std::string::npos);
    CHECK(s.find("8080") != std::string::npos);
}

TEST_CASE("fmtAddrPort - IPv4 mapped 任意地址") {
    struct in6_addr addr{};
    addr.s6_addr[10] = 0xff;
    addr.s6_addr[11] = 0xff;
    addr.s6_addr[12] = 192;
    addr.s6_addr[13] = 168;
    addr.s6_addr[14] = 1;
    addr.s6_addr[15] = 100;

    std::string s = fmtAddrPort(addr, htons(1095));
    CHECK(s.find("192.168.1.100") != std::string::npos);
    CHECK(s.find("1095") != std::string::npos);
}

TEST_CASE("fmtAddrPort - 纯 IPv6 地址") {
    struct in6_addr addr{};
    // ::1 (loopback)
    addr.s6_addr[15] = 1;

    std::string s = fmtAddrPort(addr, htons(53));
    CHECK(s.find("[") != std::string::npos);
    CHECK(s.find("]") != std::string::npos);
    CHECK(s.find(":53") != std::string::npos);
}

// ── TcpConnEntry 结构 ──

TEST_CASE("TcpConnEntry - 默认初始化") {
    TcpConnEntry e{};
    CHECK(e.state == 0);
    CHECK(e.srcPort == 0);
    CHECK(e.dstPort == 0);
    CHECK(e.retrans == 0);
}

TEST_CASE("TcpConnEntry - 赋值") {
    TcpConnEntry e;
    e.state = TCP_ESTABLISHED;
    e.srcPort = htons(12345);
    e.dstPort = htons(80);
    e.retrans = 3;

    CHECK(e.state == TCP_ESTABLISHED);
    CHECK(ntohs(e.srcPort) == 12345);
    CHECK(ntohs(e.dstPort) == 80);
    CHECK(e.retrans == 3);
}

// ── TcpSnapshot ──

TEST_CASE("TcpSnapshot - 默认初始化") {
    TcpSnapshot snap;
    CHECK(snap.entries.empty());
    CHECK(snap.elapsedUs == 0);
}

// ── config: monitor 配置解析 ──

TEST_CASE("parseConfig - monitor 默认关闭") {
    PortConfig cfg;
    CHECK(cfg.monitor.enabled == false);
    CHECK(cfg.monitor.intervalSec == 60);
}

TEST_CASE("parseConfig - monitor 启用") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3099",
                "command": "./app",
                "monitor": {
                    "enabled": true,
                    "interval_seconds": 3
                }
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].monitor.enabled == true);
    CHECK(cfgs[0].monitor.intervalSec == 3);
}

TEST_CASE("parseConfig - monitor 仅启用不指定间隔（默认 60 秒）") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3099",
                "command": "./app",
                "monitor": { "enabled": true }
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].monitor.enabled == true);
    CHECK(cfgs[0].monitor.intervalSec == 60);  // 默认值
}

TEST_CASE("parseConfig - 无 monitor 字段") {
    auto cfgs = parseConfig(R"({
        "ports": [
            { "listen": ":3099", "command": "./app" }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].monitor.enabled == false);  // 默认关闭
    CHECK(cfgs[0].monitor.intervalSec == 60);  // 默认间隔
}

TEST_CASE("parseConfig - monitor 禁用") {
    auto cfgs = parseConfig(R"({
        "ports": [
            {
                "listen": ":3099",
                "command": "./app",
                "monitor": { "enabled": false }
            }
        ]
    })");

    REQUIRE(cfgs.size() == 1);
    CHECK(cfgs[0].monitor.enabled == false);
}