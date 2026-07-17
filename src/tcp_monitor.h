#ifndef GATEKEEPER_TCP_MONITOR_H
#define GATEKEEPER_TCP_MONITOR_H

// ─── TCP 连接监控模块 ─────────────────────────────────────────
// 通过 NETLINK_INET_DIAG 查询指定端口的 TCP 连接状态。
// 仅在 Linux 平台可用（__linux__）。
// ─────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <cstdint>
#include <cstring>   // memcpy

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>  // in6_addr, ntohs, sockaddr_in
#include <netinet/tcp.h>
#endif

#ifndef TCP_ESTABLISHED
#define TCP_ESTABLISHED 1
#endif
#ifndef TCP_SYN_SENT
#define TCP_SYN_SENT 2
#endif
#ifndef TCP_SYN_RECV
#define TCP_SYN_RECV 3
#endif
#ifndef TCP_FIN_WAIT1
#define TCP_FIN_WAIT1 4
#endif
#ifndef TCP_FIN_WAIT2
#define TCP_FIN_WAIT2 5
#endif
#ifndef TCP_TIME_WAIT
#define TCP_TIME_WAIT 6
#endif
#ifndef TCP_CLOSE
#define TCP_CLOSE 7
#endif
#ifndef TCP_CLOSE_WAIT
#define TCP_CLOSE_WAIT 8
#endif
#ifndef TCP_LAST_ACK
#define TCP_LAST_ACK 9
#endif
#ifndef TCP_LISTEN
#define TCP_LISTEN 10
#endif
#ifndef TCP_CLOSING
#define TCP_CLOSING 11
#endif

// ── 一条 TCP 连接信息 ──
struct TcpConnEntry {
    uint8_t  state;            // TCP_ESTABLISHED / TCP_LISTEN / ...
    uint16_t srcPort;          // 源端口（网络字节序 → 用 ntohs 转换）
    uint16_t dstPort;          // 目标端口
    struct in6_addr srcAddr;   // 源地址
    struct in6_addr dstAddr;   // 目标地址
    int      retrans;          // 重传次数
};

// ── 查询结果：包含时间戳 ──
struct TcpSnapshot {
    std::vector<TcpConnEntry> entries;
    long elapsedUs = 0;
};

// ── 查询指定端口的 TCP 连接 ──
// @port: 要查询的端口号（主机字节序，函数内部会转网络字节序）
// 返回: 匹配该端口的连接列表（srcPort == port || dstPort == port）
TcpSnapshot queryPortConnections(uint16_t port);

// ── TCP 状态码 → 可读字符串 ──
const char* tcpStateStr(uint8_t state);

// ── 格式化 IP:PORT ──
std::string fmtAddrPort(const struct in6_addr& addr, uint16_t port);

#endif // GATEKEEPER_TCP_MONITOR_H