#include "tcp_monitor.h"

// Linux 平台下才真的编译 netlink 查询
#ifdef __linux__

#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <linux/netlink.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>

// ── TCP 状态码 → 可读字符串 ──
const char* tcpStateStr(uint8_t state) {
    switch (state) {
        case TCP_ESTABLISHED: return "ESTABLISHED";
        case TCP_SYN_SENT:    return "SYN_SENT";
        case TCP_SYN_RECV:    return "SYN_RECV";
        case TCP_FIN_WAIT1:   return "FIN_WAIT1";
        case TCP_FIN_WAIT2:   return "FIN_WAIT2";
        case TCP_TIME_WAIT:   return "TIME_WAIT";
        case TCP_CLOSE:       return "CLOSE";
        case TCP_CLOSE_WAIT:  return "CLOSE_WAIT";
        case TCP_LAST_ACK:    return "LAST_ACK";
        case TCP_LISTEN:      return "LISTEN";
        case TCP_CLOSING:     return "CLOSING";
        default:              return "UNKNOWN";
    }
}

// ── 格式化 IP:PORT ──
std::string fmtAddrPort(const struct in6_addr& addr, uint16_t port) {
    char buf[64];
    if (IN6_IS_ADDR_V4MAPPED(&addr) || IN6_IS_ADDR_V4COMPAT(&addr)) {
        // IPv4-mapped IPv6 → 显示为 IPv4:port
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr.s6_addr[12], ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "%s:%d", ip, ntohs(port));
    } else {
        char ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &addr, ip, sizeof(ip));
        snprintf(buf, sizeof(buf), "[%s]:%d", ip, ntohs(port));
    }
    return buf;
}

// ── 查询指定端口的 TCP 连接 ──
TcpSnapshot queryPortConnections(uint16_t port) {
    TcpSnapshot snap;
    auto start = std::clock();

    // 1. 打开 NETLINK_INET_DIAG socket
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_INET_DIAG);
    if (fd < 0) {
        snap.elapsedUs = 0;
        return snap;
    }

    // 2. 绑定本地地址
    struct sockaddr_nl local{};
    local.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr*)&local, sizeof(local)) < 0) {
        close(fd);
        return snap;
    }

    // 3. 构造请求（查所有 IPv6 连接，含 IPv4-mapped）
    struct {
        struct nlmsghdr      nlh;
        struct inet_diag_req_v2 req;
    } req{};

    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    req.req.sdiag_family  = AF_INET6;
    req.req.sdiag_protocol = IPPROTO_TCP;
    req.req.idiag_states   = (uint32_t)-1;  // 所有状态

    // 按端口过滤（内核会根据 idiag_sport 和 idiag_dport 做匹配）
    // 注意：内核只按 idiag_dport 过滤，所以我们在用户态再做二次过滤
    req.req.id.idiag_dport = htons(port);

    struct sockaddr_nl peer{};
    peer.nl_family = AF_NETLINK;

    struct iovec  iov{&req, sizeof(req)};
    struct msghdr msg{&peer, sizeof(peer), &iov, 1, nullptr, 0, 0};

    if (sendmsg(fd, &msg, 0) < 0) {
        close(fd);
        return snap;
    }

    // 4. 接收响应
    char buf[8192];
    uint16_t hostPort = port;  // 主机字节序，用于二次过滤

    while (true) {
        struct sockaddr_nl nl_peer{};
        struct iovec  riov{buf, sizeof(buf)};
        struct msghdr rmsg{&nl_peer, sizeof(nl_peer), &riov, 1, nullptr, 0, 0};

        ssize_t n = recvmsg(fd, &rmsg, 0);
        if (n <= 0) break;

        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == NLMSG_ERROR) break;

        // 遍历同批次多个 NLMSG
        for (; NLMSG_OK(nlh, n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;

            struct inet_diag_msg* diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
            uint16_t sp = ntohs(diag->id.idiag_sport);
            uint16_t dp = ntohs(diag->id.idiag_dport);

            // 二次过滤：只保留 src 或 dst 匹配目标端口的连接
            if (sp != hostPort && dp != hostPort) continue;

            TcpConnEntry e;
            e.state   = diag->idiag_state;
            e.srcPort = diag->id.idiag_sport;
            e.dstPort = diag->id.idiag_dport;
            std::memcpy(&e.srcAddr, diag->id.idiag_src, sizeof(e.srcAddr));
            std::memcpy(&e.dstAddr, diag->id.idiag_dst, sizeof(e.dstAddr));
            e.retrans = diag->idiag_retrans;
            snap.entries.push_back(e);
        }
    }

    close(fd);
    snap.elapsedUs = long(double(std::clock() - start) / CLOCKS_PER_SEC * 1e6);
    return snap;
}

#else
// ── 非 Linux 平台：空实现 ──

const char* tcpStateStr(uint8_t) { return "N/A"; }

std::string fmtAddrPort(const struct in6_addr&, uint16_t) { return "N/A"; }

TcpSnapshot queryPortConnections(uint16_t) {
    TcpSnapshot snap;
    snap.elapsedUs = 0;
    return snap;
}

#endif // __linux__