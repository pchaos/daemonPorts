#include "tcp_monitor.h"

// Linux 平台下才真的编译 netlink 查询
#ifdef __linux__

#include <linux/inet_diag.h>
#include <linux/sock_diag.h>
#include <linux/netlink.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
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

// ── 本机接口地址快照 ──
// 每次查询枚举一次（而非每条连接重复 getifaddrs/freeifaddrs）。
struct LocalAddrs {
    std::vector<unsigned char> v4;   // 每 4 字节一个 IPv4（网络字节序）
    std::vector<unsigned char> v6;   // 每 16 字节一个 IPv6
    bool ok = false;                 // getifaddrs 是否成功
};

static LocalAddrs snapshotLocalAddrs() {
    LocalAddrs la;
    struct ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) != 0) return la;
    la.ok = true;
    for (struct ifaddrs* it = ifa; it != nullptr; it = it->ifa_next) {
        if (it->ifa_addr == nullptr) continue;
        if (it->ifa_addr->sa_family == AF_INET) {
            const struct sockaddr_in* sa = (const struct sockaddr_in*)it->ifa_addr;
            const unsigned char* p = (const unsigned char*)&sa->sin_addr;
            la.v4.insert(la.v4.end(), p, p + 4);
        } else if (it->ifa_addr->sa_family == AF_INET6) {
            const struct sockaddr_in6* sa = (const struct sockaddr_in6*)it->ifa_addr;
            // v4-mapped 地址已在 AF_INET 分支覆盖，跳过避免重复
            if (IN6_IS_ADDR_V4MAPPED(&sa->sin6_addr) || IN6_IS_ADDR_V4COMPAT(&sa->sin6_addr)) continue;
            la.v6.insert(la.v6.end(), sa->sin6_addr.s6_addr, sa->sin6_addr.s6_addr + 16);
        }
    }
    freeifaddrs(ifa);
    return la;
}

// ── 判断地址是否属于本机接口 ──
// 仅统计"端口=监控端口"且该端位于本机的连接，排除本机 → 远端同一端口的出站连接
// （如本机客户端连到 192.168.124.87:20128 的出站连接，不应计入本地 omniroute 的活跃度）。
// 地址布局（实测 inet_diag 返回）：
//   AF_INET 查询：IPv4 存于低 4 字节 [0..3]，高 12 字节为 0；LISTEN 的 src 为全 0 = INADDR_ANY
//   AF_INET6 查询：v4-mapped 时存于 [12..15]；原生 IPv6 为完整 16 字节
static bool isLocalAddress(const unsigned char* addr16, const LocalAddrs& la) {
    // addr16: inet_diag 的 idiag_src/idiag_dst，16 字节网络字节序
    const struct in6_addr* addr = (const struct in6_addr*)addr16;
    if (IN6_IS_ADDR_UNSPECIFIED(addr)) return true;   // 0.0.0.0 / :: → LISTEN 通配视为本机

    const unsigned char* ip4 = nullptr;
    if (IN6_IS_ADDR_V4MAPPED(addr) || IN6_IS_ADDR_V4COMPAT(addr)) {
        ip4 = &addr->s6_addr[12];                        // ::ffff:a.b.c.d
    } else {
        static const unsigned char zero[12] = {0};
        if (memcmp(&addr->s6_addr[4], zero, 12) == 0)    // [4..15] 全 0 → 紧凑 IPv4（AF_INET）
            ip4 = &addr->s6_addr[0];
    }

    if (ip4) {
        for (size_t i = 0; i + 4 <= la.v4.size(); i += 4)
            if (memcmp(&la.v4[i], ip4, 4) == 0) return true;
        return false;
    }
    for (size_t i = 0; i + 16 <= la.v6.size(); i += 16)
        if (memcmp(&la.v6[i], &addr->s6_addr, 16) == 0) return true;
    return false;
}


static void queryFamily(int fd, uint16_t port, sa_family_t family, const LocalAddrs& la, TcpSnapshot& snap) {

    struct {
        struct nlmsghdr      nlh;
        struct inet_diag_req_v2 req;
    } req{};
    req.nlh.nlmsg_len   = sizeof(req);
    req.nlh.nlmsg_type  = SOCK_DIAG_BY_FAMILY;
    req.nlh.nlmsg_flags = NLM_F_DUMP | NLM_F_REQUEST;
    req.req.sdiag_family  = family;
    req.req.sdiag_protocol = IPPROTO_TCP;
    req.req.idiag_states   = (uint32_t)-1;
    

    struct sockaddr_nl peer{};
    peer.nl_family = AF_NETLINK;

    struct iovec  iov{&req, sizeof(req)};
    struct msghdr msg{&peer, sizeof(peer), &iov, 1, nullptr, 0, 0};

    if (sendmsg(fd, &msg, 0) < 0) {
        return;
    }

    char buf[8192];
    uint16_t hostPort = port;

    while (true) {
        struct sockaddr_nl nl_peer{};
        struct iovec  riov{buf, sizeof(buf)};
        struct msghdr rmsg{&nl_peer, sizeof(nl_peer), &riov, 1, nullptr, 0, 0};

        ssize_t n = recvmsg(fd, &rmsg, 0);
        if (n <= 0) break;

        struct nlmsghdr* nlh = (struct nlmsghdr*)buf;
        if (nlh->nlmsg_type == NLMSG_DONE) break;
        if (nlh->nlmsg_type == NLMSG_ERROR) break;

        for (; NLMSG_OK(nlh, n); nlh = NLMSG_NEXT(nlh, n)) {
            if (nlh->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;
            struct inet_diag_msg* diag = (struct inet_diag_msg*)NLMSG_DATA(nlh);
            uint16_t sp = ntohs(diag->id.idiag_sport);
            uint16_t dp = ntohs(diag->id.idiag_dport);
            // 端口命中且命中端位于本机，才计入监控。
            // 排除本机 → 远端同端口连接的出站连接：
            // 例如本机 omp 客户端连 192.168.124.87:20128，dstPort=20128 但 dstAddr 不是本机，
            // 若计入会错误地让本地 omniroute 组一直显示活跃、永不进入空闲回收。
            if (sp == hostPort && isLocalAddress((const unsigned char*)diag->id.idiag_src, la)) {
                // LISTEN(sp=port, src=本机通配) / 本机发起的出站连接(sp=port 是监听的本地端口)
            } else if (dp == hostPort && isLocalAddress((const unsigned char*)diag->id.idiag_dst, la)) {
                // 入站连接：dst=本机且 dstPort=port
            } else {
                continue;
            }
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
}
// ── 查询指定端口的 TCP 连接 ──
TcpSnapshot queryPortConnections(uint16_t port, bool ipv6) {
    TcpSnapshot snap;
    auto start = std::clock();

    // 1. 打开 NETLINK_INET_DIAG socket
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_INET_DIAG);
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

    // 3. 本机接口地址快照（每条连接只需查一次）
    LocalAddrs la = snapshotLocalAddrs();

    // Query IPv4 and, if enabled, IPv6 families
    queryFamily(fd, port, AF_INET, la, snap);
    if (ipv6) queryFamily(fd, port, AF_INET6, la, snap);
    close(fd);
    snap.elapsedUs = long(double(std::clock() - start) / CLOCKS_PER_SEC * 1e6);
    return snap;

}

#else
// ── 非 Linux 平台：空实现 ──

const char* tcpStateStr(uint8_t) { return "N/A"; }

std::string fmtAddrPort(const struct in6_addr&, uint16_t) { return "N/A"; }

TcpSnapshot queryPortConnections(uint16_t, bool) {
    TcpSnapshot snap;
    snap.elapsedUs = 0;
    return snap;
}

#endif // __linux__