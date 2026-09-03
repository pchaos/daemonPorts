#include "relay.h"
#include "port_group.h"

#include <iostream>
#include <cstring>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <thread>
#include <chrono>

// SOCK_CLOEXEC — defined in relay_platform.h (if not available, #define SOCK_CLOEXEC 0)

// 检测端口是否还被其他进程监听（不建立连接，不影响空闲超时）
// 检测端口是否被占用，返回占用进程的 PID（找不到返回 0）




bool PortRelay::isPortBound(uint16_t port) {
    PlatformHandle fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return true;
    int opt = 1;
    platform::set_sockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);
    int rc = platform::bind_fd(fd, &sa, sizeof(sa));
    platform::close_fd(fd);
    return (rc < 0 && platform::last_error_is(PLATFORM_EADDRINUSE));
}

void PortRelay::logBindFailed() {
    std::string msg = "  [" + name_ + "] 绑定 " + listenAddr_ + " 失败（";
    if (platform::last_error_is(PLATFORM_EACCES)) {
        msg += "权限不足，需要 CAP_NET_BIND_SERVICE";
    } else if (platform::last_error_is(PLATFORM_EADDRINUSE)) {
        msg += "端口被占用";
        int port = monitorPort();
        if (port > 0) {
    std::string occupant = platform::findProcessUsingPort(static_cast<uint16_t>(port));
    if (!occupant.empty()) {
        msg += " by " + occupant;
    }
        }
    } else {
        msg += platform::last_error_str();
    }
    msg += "），" + std::to_string(retrySeconds_) + " 秒后重试（最大 "
           + std::to_string(retrySecondsMax_) + " 秒）";
    std::cerr << msg << std::endl;
}
// 绑定失败时采纳端口外部占用者为“外部后端”：
// 仅当配置了 stop_command（回收动作可行）且端口确实被占用时采纳；
// 此后统一监控线程照常采样流量，空闲超时由 gracefulStop 执行 stop_command 回收。
bool PortRelay::tryAdoptExternalOccupant() {
    if (stopCommand_.empty() || recycleGiveUp_.load()) return false;
    int port = monitorPort();
    if (port <= 0) return false;
    if (!isPortBound((uint16_t)port)) return false;
    externalOccupied_.store(true);
    consecutiveBindFailures_ = 0;
    retrySeconds_ = retrySecondsBase_;
    std::cout << "  [" << name_ << "] 端口被外部进程占用，转入外部占用监控"
              << "（空闲 " << idleMinutes_ << " 分钟后执行 stop_command 回收）" << std::endl;
    return true;
}

static bool parseSockaddr(const std::string& addr, sockaddr_in& out) {
    auto c = addr.find(':');
    if (c == std::string::npos) return false;
    std::string host = addr.substr(0, c);
    int port = 0;
    try {
        port = std::stoi(addr.substr(c+1));
    } catch (...) { return false; }
    if (port <= 0 || port > 65535) return false;

    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") out.sin_addr.s_addr = INADDR_ANY;
    else if (host == "localhost" || host == "127.0.0.1") out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else platform::inet_pton_wrap(AF_INET, host.c_str(), &out.sin_addr);
    return true;
}

PortRelay::PortRelay(const PortConfig& cfg)
    : name_(cfg.name.empty() ? cfg.listenAddr : cfg.name)
    , listenAddr_(cfg.listenAddr)
    , command_(cfg.command)
    , delayMs_(cfg.delayMs)
    , refreshSeconds_(cfg.refreshSeconds)
    , retrySeconds_(cfg.retrySeconds)
    , retrySecondsBase_(cfg.retrySeconds)
    , retrySecondsMax_(cfg.maxRetrySeconds)
    , autoRestart_(cfg.autoRestart)
    , mode_(cfg.mode)
    , holdPort_(cfg.holdPort)
    , protocols_(cfg.protocols)
    , auth_(cfg.auth)
    , httpTarget_(cfg.httpTarget)
    , stopCommand_(cfg.stopCommand)
    , idleMinutes_(cfg.idleMinutes >= 0 ? cfg.idleMinutes : 20)
    , stackSize_(cfg.stackSize > 0 ? cfg.stackSize : 512)
    , tcpMonitorInterval_(cfg.monitor.enabled ? cfg.monitor.intervalSec : 0)
    , logDedupMode_(cfg.monitor.logDedup) {}

int PortRelay::createListener() {
    sockaddr_in sa;
    if (!parseSockaddr(listenAddr_, sa)) {
        std::cerr << "  [" << name_ << "] 无效地址: " << listenAddr_ << std::endl;
        return -1;
    }
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::cerr << "socket: " << platform::last_error_str() << std::endl; return -1; }
    int opt = 1;
    platform::set_sockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (platform::bind_fd(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            platform::close_fd(fd); return -1;
        }
    if (platform::listen_fd(fd, 128) < 0) { std::cerr << "listen: " << platform::last_error_str() << std::endl; platform::close_fd(fd); return -1; }
    return fd;
}

pid_t PortRelay::launchBackend() {
    return platform::launchProcess(command_);
}

void PortRelay::launchAndRelease() {
    pid_t pid = launchBackend();
    if (pid < 0) return;
    backendPid_ = pid;
    lastActiveTime_ = time(nullptr);
    std::cout << "  [" << name_ << "] 后端已启动 (PID=" << pid << ")" << std::endl;

    int lfd = listenFd_.exchange(-1);
    if (lfd >= 0) platform::close_fd(lfd);
    std::cout << "  [" << name_ << "] 端口已释放，等待后端就绪" << std::endl;

    if (!waitForBackend(delayMs_))
        std::cerr << "  [" << name_ << "] 警告: 后端可能未就绪" << std::endl;
    else
        std::cout << "  [" << name_ << "] 后端就绪，端口已移交" << std::endl;
}

bool PortRelay::waitForBackend(int ms) {
    sockaddr_in sa;
    if (!parseSockaddr(listenAddr_, sa)) return false;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (platform::connect_fd(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) { platform::close_fd(fd); return true; }
            platform::close_fd(fd);
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

void PortRelay::sendStartupPage(int fd) {
    std::string response = buildStartupResponse();
    platform::write_fd(fd, response.data(), response.size());
}

std::string PortRelay::buildStartupResponse() const {
    std::string html =
        "<!DOCTYPE html>\n"
        "<html>\n<head>\n"
        "  <meta charset=\"utf-8\">\n"
        "  <meta http-equiv=\"refresh\" content=\""
        + std::to_string(refreshSeconds_) + "\">\n"
        "  <title>" + name_ + " 启动中</title>\n"
        "  <script>\n"
        "    var secs = " + std::to_string(refreshSeconds_) + ";\n"
        "    function tick() {\n"
        "      document.getElementById('cd').textContent = secs;\n"
        "      if (secs > 0) { secs--; setTimeout(tick, 1000); }\n"
        "    }\n"
        "  </script>\n"
        "</head>\n<body onload=\"tick()\">\n"
        "  <h1>" + name_ + " 启动中...</h1>\n"
        "  <p><span id=\"cd\">" + std::to_string(refreshSeconds_) + "</span> 秒后自动重试</p>\n"
        "</body>\n</html>\n";

    return
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(html.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n"
        + html;
}

void PortRelay::listenLoop() {
    while (!stop_.load()) {
        // If group has already released the port, stop listening
        if (groupReleased_.load()) {
            break;
        }
        // If daemon is alive and tracked by monitorBackend, skip bind
        if (backendPid_ > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // 外部占用监控态：暂停绑定，轮询端口释放后重新接管
        if (externalOccupied_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (stop_.load()) break;
            int port = monitorPort();
            if (port > 0 && !isPortBound((uint16_t)port)) {
                std::cout << "  [" << name_ << "] 外部占用者已退出，端口释放，重新接管" << std::endl;
                externalOccupied_.store(false);
            }
            continue;
        }
        listenFd_.store(createListener());
            if (listenFd_.load() < 0) {
                if (stop_.load()) break;
                consecutiveBindFailures_++;
                if (consecutiveBindFailures_ == 1) {
                    logBindFailed();
                } else if (consecutiveBindFailures_ == LOG_SILENCE_THRESHOLD) {
                    std::cerr << "  [" << name_ << "] 连续 " << LOG_SILENCE_THRESHOLD
                              << " 次绑定失败，后续日志已静默" << std::endl;
                }
                // 端口被外部进程占用且配置了 stop_command → 采纳为外部后端，纳入空闲回收
                if (tryAdoptExternalOccupant()) continue;

                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(retrySeconds_);
                while (std::chrono::steady_clock::now() < deadline && !stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (stop_.load()) break;
                retrySeconds_ = std::min(retrySeconds_ * 2, retrySecondsMax_);
                continue;
            }

        // 绑定成功，重置为初始间隔
        retrySeconds_ = retrySecondsBase_;
        consecutiveBindFailures_ = 0;

        std::cout << "  [" << name_ << "] 监听 " << listenAddr_ << std::endl;

        // launch_on_start: 绑定成功后立即启动后端并释放端口，跳过 accept 循环
        if (launchOnStart_) {
            launchOnStart_ = false;
            launchAndRelease();
        } else {
        while (!stop_.load()) {
            sockaddr_in cli;
            int len = sizeof(cli);
            int fd = platform::accept_fd(listenFd_.load(), (struct sockaddr*)&cli, &len);
            if (fd < 0) {
                if (stop_.load() || platform::last_error() == PLATFORM_EINVAL) break;
                continue;
            }

            sendStartupPage(fd);
            platform::close_fd(fd);

            // Group coordination: notify group and let it handle backend launch
            if (group_) {
                group_->onConnection(this);
                // After notifying group, stop this listen loop for this relay
                break;
            }

            if (backendPid_ == 0) {
                launchAndRelease();
                break;
            }
        }
        }

        int fd = listenFd_.exchange(-1);
        if (fd >= 0) {
            platform::close_fd(fd);
        }

        if (!autoRestart_ || stop_.load()) break;

        // After backend exits, wait delayMs then check if port is still in use
        // by a daemonized child process (e.g., 9router's next-server)
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));

        {
            int port = monitorPort();
            if (port > 0 && isPortBound((uint16_t)port)) {
                std::cout << "  [" << name_ << "] port still in use (child alive), waiting for release" << std::endl;
                while (!stop_.load()) {
                    // If monitorBackend found the daemon process, stop waiting
                    if (backendPid_ > 0) {
                        std::cout << "  [" << name_ << "] daemon process tracked (PID=" << backendPid_ << "), stopping wait" << std::endl;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (!isPortBound((uint16_t)port)) {
                        std::cout << "  [" << name_ << "] port released" << std::endl;
                        break;
                    }
                }
                if (stop_.load()) break;
            }
        }

        // If monitorBackend is tracking a live daemon, skip re-listen.
// Wait for idle timeout -> gracefulStop -> daemon exit -> backendPid_ = 0.
if (backendPid_ > 0) {
    std::cout << "  [" << name_ << "] daemon alive (PID=" << backendPid_ << "), skipping re-listen, waiting for idle timeout" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    continue;
}
std::cout << "  [" << name_ << "] 重新监听端口" << std::endl;
    }
}

bool PortRelay::hasRecentActivity(int minutes) const {
    if (minutes == 0) return true;  // idle_minutes=0 → no idle detection

    time_t cutoff = time(nullptr) - minutes * 60;
    return lastActiveTime_ >= cutoff;
}

void PortRelay::updateActivity(bool active) {
    if (active) lastActiveTime_ = time(nullptr);
}

int PortRelay::monitorPort() const {
    auto c = listenAddr_.find(':');
    if (c == std::string::npos) return -1;
    int port = -1;
    try {
        port = std::stoi(listenAddr_.substr(c+1));
    } catch (...) { return -1; }
    if (port <= 0 || port > 65535) return -1;
    return port;
}

void PortRelay::monitorBackend() {
    while (!stop_.load()) {
        if (backendPid_ > 0) {
            pid_t ret = platform::waitChild(backendPid_);
            if (ret < 0 && !platform::isChildAlive(backendPid_)) {
                // Non-child process (daemonized), poll port
                int port = monitorPort();
                if (port > 0) {
                    std::cout << "  [" << name_ << "] 监控 daemon 进程 (PID=" << backendPid_
                              << ")，轮询端口 " << port << std::endl;
                    while (!stop_.load()) {
                        std::this_thread::sleep_for(std::chrono::seconds(5));
                        if (!platform::isProcessRunning(backendPid_) && !isPortBound((uint16_t)port)) {
                            std::cout << "  [" << name_ << "] daemon 进程已退出，端口已释放" << std::endl;
                            break;
                        }
                    }
                    if (stop_.load()) break;
                }
                backendPid_ = 0;
                if (!autoRestart_ || stop_.load()) break;
                std::cout << "  [" << name_ << "] 将在下次连接时重启" << std::endl;
                continue;
            }
            // Direct child exited, check if daemonized process holds the port
            int port = monitorPort();
            pid_t realPid = 0;
            if (port > 0) {
                int maxRetries = (int)((delayMs_ * 1.5) / 2000);
                if (maxRetries < 1) maxRetries = 1;
                for (int retry = 0; retry < maxRetries; retry++) {
                    realPid = platform::findPidUsingPort((uint16_t)port);
                    if (realPid > 0 && realPid != backendPid_ && realPid != platform::currentPid()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                }
                if (realPid > 0 && realPid != backendPid_ && realPid != platform::currentPid()) {
                    std::cout << "  [" << name_ << "] 检测到 daemon 进程 (PID=" << realPid
                              << ")，更新后端 PID" << std::endl;
                    backendPid_ = realPid;
                    lastActiveTime_ = time(nullptr);
                    continue;
                }
            }
            std::cout << "  [" << name_ << "] 后端已退出" << std::endl;
            backendPid_ = 0;
            if (!autoRestart_ || stop_.load()) break;
            std::cout << "  [" << name_ << "] 将在下次连接时重启" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

// ── mixed 模式：协议检测 ──
// 用 MSG_PEEK 读前 8 字节但不消费，根据特征字节判断协议类型。
// 这样后续如果 hold_port=true 做代理转发时，已读的 preamble 可以原样传给后端。

std::string PortRelay::detectProtocol(int fd) {
    unsigned char buf[8];
    ssize_t n = platform::recv_peek_fd(fd, buf, sizeof(buf));
    if (n <= 0) return "unknown";

    // SOCKS5: 首字节固定 0x05（协议版本号）
    if (buf[0] == 0x05) return "socks5";
    // SOCKS4: 首字节固定 0x04（协议版本号）
    if (buf[0] == 0x04) return "socks4";

    // HTTP: 请求行以方法名开头，全是可见 ASCII 大写字母
    // 首字节匹配任一方法首字母后才继续精确匹配，避免误判二进制协议
    if (buf[0] == 'G' || buf[0] == 'P' || buf[0] == 'H' ||
        buf[0] == 'D' || buf[0] == 'C' || buf[0] == 'O' ||
        buf[0] == 'T' || buf[0] == 'R') {
        std::string s((const char*)buf, n);
        const char* methods[] = {"GET ", "POST", "HEAD", "PUT ", "DELETE", "CONNECT",
                                  "OPTIONS", "PATCH", "TRACE"};
        for (auto& m : methods) {
            if (s.compare(0, strlen(m), m) == 0) return "http";
        }
    }
    return "unknown";
}

// ── mixed 模式：协议对应的引导响应 ──
// hold_port=false 时，在释放端口前给客户端一个合适的回复：
//   - HTTP:   发送启动页 HTML，浏览器自动刷新（复用现有逻辑）
//   - SOCKS5: 回复 0x05 0xFF（"无可用认证方法"），客户端会报错退出但不崩溃
//   - SOCKS4: 回复请求被拒绝状态码，客户端会收到明确失败
//   - unknown: 不做回复，直接关闭连接

void PortRelay::sendMixedResponse(int fd, const std::string& proto) {
    if (proto == "http") {
        sendStartupPage(fd);
    } else if (proto == "socks5") {
        // SOCKS5 方法选择响应：版本 5，不允许任何认证方式
        unsigned char resp[] = {0x05, 0xff};
        platform::write_fd(fd, resp, sizeof(resp));
    } else if (proto == "socks4") {
        // SOCKS4 响应：VN=0, CD=0x5B(请求被拒), DSTPORT=0, DSTIP=0
        unsigned char resp[] = {0x00, 0x5b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        platform::write_fd(fd, resp, sizeof(resp));
    }
}

// ── mixed 模式：持住端口代理转发 ──
// 连接到后端并建立双向 TCP 隧道。使用两个独立线程实现全双工转发，
// 任一方向关闭后另一方向自动终止。

static void pipeRelay(int src, int dst, std::atomic<bool>& done) {
    char buf[16384];
    ssize_t n;
    while ((n = platform::read_fd(src, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = platform::write_fd(dst, buf + off, n - off);
            if (w <= 0) { done.store(true); return; }
            off += w;
        }
    }
    // 源端关闭后，通知对端停止写
    platform::shutdown_fd(dst, SHUT_WR);
    done.store(true);
}

int PortRelay::connectToBackend(const std::string& addr) {
    sockaddr_in sa;
    if (!parseSockaddr(addr, sa)) return -1;
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (platform::connect_fd(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        platform::close_fd(fd);
        return -1;
    }
    return fd;
}

void PortRelay::proxyConnection(int clientFd, const std::string& proxyTo) {
    int backendFd = connectToBackend(proxyTo);
    if (backendFd < 0) {
        platform::close_fd(clientFd);
        return;
    }

    std::cout << "  [" << name_ << "] 隧道已建立: 客户端 ↔ " << proxyTo << std::endl;

    // 双向全双工隧道，两个线程各负责一个方向
    std::atomic<bool> done1{false}, done2{false};
    std::thread t1(pipeRelay, clientFd, backendFd, std::ref(done1));
    std::thread t2(pipeRelay, backendFd, clientFd, std::ref(done2));

    // 等待任一方向结束
    while (!done1 && !done2) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 强制关闭两端，回收线程
    platform::shutdown_fd(clientFd, SHUT_RDWR);
    platform::shutdown_fd(backendFd, SHUT_RDWR);
    if (t1.joinable()) t1.join();
    if (t2.joinable()) t2.join();
    platform::close_fd(clientFd);
    platform::close_fd(backendFd);

    std::cout << "  [" << name_ << "] 隧道已关闭" << std::endl;
}

// ── hold_port=true：按协议启动后端 ──

void PortRelay::launchProtocolBackend(BackendState& bs) {
    if (bs.pid != 0 || bs.ready->load()) return;

    // 临时覆盖 command_ 和 delayMs_ 来复用现有 launchBackend/waitForBackend
    std::string savedCmd = command_;
    int savedDelay = delayMs_;
    command_ = bs.command;
    delayMs_ = bs.delayMs;

    pid_t pid = launchBackend();
    if (pid < 0) { bs.pid = 0; return; }
    bs.pid = pid;
    std::cout << "  [" << name_ << "] " << bs.type << " 后端已启动 (PID=" << pid << ")" << std::endl;

    // 等待后端就绪（连接 backend->proxyTo 地址）
    sockaddr_in sa;
    if (parseSockaddr(bs.proxyTo, sa)) {
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int fd = platform::socket_ai(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                if (platform::connect_fd(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
                    platform::close_fd(fd);
                    bs.ready->store(true);
                    break;
                }
                platform::close_fd(fd);
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(bs.delayMs))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // 恢复原值
    command_ = savedCmd;
    delayMs_ = savedDelay;

    if (bs.ready->load()) {
        std::cout << "  [" << name_ << "] " << bs.type << " 后端就绪 (" << bs.proxyTo << ")" << std::endl;
    } else {
        std::cerr << "  [" << name_ << "] " << bs.type << " 后端未就绪" << std::endl;
    }
}

PortRelay::BackendState* PortRelay::findBackend(const std::string& type) {
    for (auto& b : backends_) {
        if (b.type == type) return &b;
    }
    return nullptr;
}

// ── hold_port=true：后端监控循环 ──

void PortRelay::proxyMonitorLoop() {
    while (!stop_.load()) {
        bool anyAlive = false;
        for (auto& b : backends_) {
            if (b.pid > 0) {
                anyAlive = true;
    if (!platform::isChildAlive(b.pid)) {
        std::cout << "  [" << name_ << "] " << b.type << " 后端已退出" << std::endl;
        b.pid = 0;
        b.ready->store(false);
    }
            }
        }
        if (!anyAlive) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

// ── proxy 模式：SOCKS5 代理主循环 ──
// 常驻端口，处理 SOCKS5 代理（NO_AUTH 或 USER/PASS 认证）
// 支持 HTTP 连接转发到 httpTarget_（如果配置了）

// 辅助：从 fd 精确读取 n 字节，返回实际读取数
static int recv_exact(int fd, unsigned char* buf, int n) {
    int total = 0;
    while (total < n) {
        ssize_t r = platform::read_fd(fd, buf + total, n - total);
        if (r <= 0) return total > 0 ? total : -1;
        total += r;
    }
    return total;
}

void PortRelay::socks5ListenLoop() {
    while (!stop_.load()) {
        // If daemon is alive and tracked by monitorBackend, skip bind
        if (backendPid_ > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // 外部占用监控态：暂停绑定，轮询端口释放后重新接管
        if (externalOccupied_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (stop_.load()) break;
            int port = monitorPort();
            if (port > 0 && !isPortBound((uint16_t)port)) {
                std::cout << "  [" << name_ << "] 外部占用者已退出，端口释放，重新接管" << std::endl;
                externalOccupied_.store(false);
            }
            continue;
        }
        listenFd_ = createListener();
            if (listenFd_ < 0) {
                if (stop_.load()) break;
                consecutiveBindFailures_++;
                if (consecutiveBindFailures_ == 1) {
                    logBindFailed();
                } else if (consecutiveBindFailures_ == LOG_SILENCE_THRESHOLD) {
                    std::cerr << "  [" << name_ << "] 连续 " << LOG_SILENCE_THRESHOLD
                              << " 次绑定失败，后续日志已静默" << std::endl;
                }
                // 端口被外部进程占用且配置了 stop_command → 采纳为外部后端，纳入空闲回收
                if (tryAdoptExternalOccupant()) continue;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(retrySeconds_);
                while (std::chrono::steady_clock::now() < deadline && !stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (stop_.load()) break;
                retrySeconds_ = std::min(retrySeconds_ * 2, retrySecondsMax_);
                continue;
            }
        retrySeconds_ = retrySecondsBase_;

        std::cout << "  [" << name_ << "] SOCKS5 代理监听 " << listenAddr_
                  << " (auth=" << auth_.type << ")" << std::endl;

        while (!stop_.load()) {
            sockaddr_in cli;
            int len = sizeof(cli);
            int fd = platform::accept_fd(listenFd_, (struct sockaddr*)&cli, &len);
            if (fd < 0) {
                if (stop_.load() || platform::last_error() == PLATFORM_EINVAL) break;
                continue;
            }

            std::string proto = detectProtocol(fd);
            std::cout << "  [" << name_ << "] 检测到 " << proto << " 连接" << std::endl;

            if (proto == "socks5") {
                // ── SOCKS5 认证协商 ──
                unsigned char buf[256];
                // 读版本 + 方法数 + 方法列表
                if (recv_exact(fd, buf, 2) != 2) { platform::close_fd(fd); continue; }
                int nmethods = buf[1];
                if (nmethods < 1 || nmethods > 255 ||
                    recv_exact(fd, buf, nmethods) != nmethods) { platform::close_fd(fd); continue; }

                bool hasNoAuth = false, hasUserPass = false;
                for (int i = 0; i < nmethods; i++) {
                    if (buf[i] == 0x00) hasNoAuth = true;
                    if (buf[i] == 0x02) hasUserPass = true;
                }

                unsigned char method = 0xff;  // 默认拒绝
                bool useUserPass = false;
                if (auth_.type == "userpass" && hasUserPass) {
                    method = 0x02;
                    useUserPass = true;
                } else if (hasNoAuth) {
                    method = 0x00;
                }
                unsigned char authResp[] = {0x05, method};
                platform::write_fd(fd, authResp, sizeof(authResp));

                if (method == 0xff) { platform::close_fd(fd); continue; }

                // ── USER/PASS 认证子协商 ──
                if (useUserPass) {
                    if (recv_exact(fd, buf, 2) != 2) { platform::close_fd(fd); continue; }
                    int ulen = buf[1];
                    if (ulen < 1 || ulen > 255 ||
                        recv_exact(fd, buf, ulen) != ulen) { platform::close_fd(fd); continue; }
                    std::string uname((const char*)buf, ulen);
                    if (recv_exact(fd, buf, 1) != 1) { platform::close_fd(fd); continue; }
                    int plen = buf[0];
                    if (plen < 1 || plen > 255 ||
                        recv_exact(fd, buf, plen) != plen) { platform::close_fd(fd); continue; }
                    std::string passwd((const char*)buf, plen);

                    bool ok = (uname == auth_.username && passwd == auth_.password);
                    int r = ok ? 0 : 1;
                    unsigned char upResp[] = {0x01, (unsigned char)r};
                    platform::write_fd(fd, upResp, sizeof(upResp));
                    if (!ok) { platform::close_fd(fd); continue; }
                }

                // ── SOCKS5 请求解析 ──
                // 格式: ver(1) + cmd(1) + rsv(1) + atyp(1) + dst.addr(可变) + dst.port(2)
                unsigned char hdr[4];
                if (recv_exact(fd, hdr, 4) != 4) { platform::close_fd(fd); continue; }
                unsigned char cmd = hdr[1];
                unsigned char atyp = hdr[3];

                // 只支持 CONNECT (0x01)
                if (cmd != 0x01) { platform::close_fd(fd); continue; }

                // 解析目标地址
                std::string targetAddr;
                int targetPort = 0;
                bool addrOk = false;

                if (atyp == 0x01) {
                    // IPv4: 4 字节
                    unsigned char addr[4];
                    if (recv_exact(fd, addr, 4) != 4) { platform::close_fd(fd); continue; }
                    char ip[32];
                    snprintf(ip, sizeof(ip), "%d.%d.%d.%d", addr[0], addr[1], addr[2], addr[3]);
                    targetAddr = ip;
                    addrOk = true;
                } else if (atyp == 0x03) {
                    // 域名: 1 字节长度 + N 字节域名
                    unsigned char dlen;
                    if (recv_exact(fd, &dlen, 1) != 1) { platform::close_fd(fd); continue; }
                    if (dlen < 1) { platform::close_fd(fd); continue; }
                    unsigned char domain[256];
                    if (recv_exact(fd, domain, dlen) != (int)dlen) { platform::close_fd(fd); continue; }
                    targetAddr = std::string((const char*)domain, dlen);
                    addrOk = true;
                } else if (atyp == 0x04) {
                    platform::close_fd(fd);
                    continue;
                }

                // 读端口 (2 字节, 网络字节序)
                unsigned char portBytes[2];
                if (recv_exact(fd, portBytes, 2) != 2) { platform::close_fd(fd); continue; }
                targetPort = (portBytes[0] << 8) | portBytes[1];
                if (!addrOk || targetPort <= 0) { platform::close_fd(fd); continue; }

                std::string targetStr = targetAddr + ":" + std::to_string(targetPort);
                std::cout << "  [" << name_ << "] SOCKS5 CONNECT " << targetStr << std::endl;

                // ── 连接目标地址 ──
                // 用 getaddrinfo 支持域名解析
                struct addrinfo hints, *res = nullptr;
                memset(&hints, 0, sizeof(hints));
                hints.ai_family = AF_UNSPEC;
                hints.ai_socktype = SOCK_STREAM;
                char portStr[16];
                snprintf(portStr, sizeof(portStr), "%d", targetPort);

                int targetFd = -1;
                if (platform::getaddrinfo_wrap(targetAddr.c_str(), portStr, &hints, &res) == 0) {
                    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
                        targetFd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
                        if (targetFd < 0) continue;
                        if (platform::connect_fd(targetFd, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) break;
                        platform::close_fd(targetFd);
                        targetFd = -1;
                    }
                    platform::freeaddrinfo_wrap(res);
                }

                if (targetFd < 0) {
                    // 连接失败 → SOCKS5 响应: 一般失败
                    unsigned char resp[] = {0x05, 0x01, 0x00, 0x01, 0,0,0,0, 0,0};
                    platform::write_fd(fd, resp, sizeof(resp));
                    platform::close_fd(fd);
                    std::cout << "  [" << name_ << "] SOCKS5 连接失败: " << targetStr << std::endl;
                    continue;
                }

                // ── SOCKS5 响应: 成功 ──
                unsigned char resp[] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
                platform::write_fd(fd, resp, sizeof(resp));
                std::cout << "  [" << name_ << "] SOCKS5 隧道建立: 客户端 ↔ " << targetStr << std::endl;

                // ── 双向隧道 ──
                std::atomic<bool> done1{false}, done2{false};
                std::thread t1(pipeRelay, fd, targetFd, std::ref(done1));
                std::thread t2(pipeRelay, targetFd, fd, std::ref(done2));

                while (!done1 && !done2) std::this_thread::sleep_for(std::chrono::milliseconds(50));

                platform::shutdown_fd(fd, SHUT_RDWR);
                platform::shutdown_fd(targetFd, SHUT_RDWR);
                if (t1.joinable()) t1.join();
                if (t2.joinable()) t2.join();
                platform::close_fd(fd);
                platform::close_fd(targetFd);
                std::cout << "  [" << name_ << "] SOCKS5 隧道关闭: " << targetStr << std::endl;

            } else if (proto == "http" && !httpTarget_.empty()) {
                // HTTP → 转发到 httpTarget_
                proxyConnection(fd, httpTarget_);
            } else {
                platform::close_fd(fd);
            }
        }

        if (listenFd_ >= 0) {
            platform::close_fd(listenFd_);
            listenFd_ = -1;
        }
        if (!autoRestart_ || stop_.load()) break;
while (!stop_.load() && backendPid_ != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (stop_.load()) break;

        // After backend exits, wait delayMs then check if port is still in use
        // by a daemonized child process (e.g., 9router's next-server)
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));

        int port = monitorPort();
        if (port > 0 && isPortBound((uint16_t)port)) {
            std::cout << "  [" << name_ << "] port still in use (child alive), waiting for release" << std::endl;
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!isPortBound((uint16_t)port)) {
                    std::cout << "  [" << name_ << "] port released" << std::endl;
                    break;
                }
            }
            if (stop_.load()) break;
        }

        // If monitorBackend is tracking a live daemon, skip re-listen.
// Wait for idle timeout -> gracefulStop -> daemon exit -> backendPid_ = 0.
if (backendPid_ > 0) {
    std::cout << "  [" << name_ << "] daemon alive (PID=" << backendPid_ << "), skipping re-listen, waiting for idle timeout" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    continue;
}
std::cout << "  [" << name_ << "] 重新监听端口" << std::endl;
    }
}

// ── mixed 模式：主循环 ──
// hold_port=false: 协议感知引导，启动后端后释放端口（同 simple 模式）
// hold_port=true:  常驻端口，检测协议并转发到对应后端

void PortRelay::mixedListenLoop() {
    // hold_port=true：初始化每个协议的后端状态
    if (holdPort_) {
        backends_.clear();
        for (auto& p : protocols_) {
            if (!p.enabled || p.command.empty() || p.proxyTo.empty()) {
                std::cout << "  [" << name_ << "] " << p.type
                          << " 协议配置不完整，跳过" << std::endl;
                continue;
            }
            BackendState bs;
            bs.type = p.type;
            bs.command = p.command;
            bs.proxyTo = p.proxyTo;
            bs.delayMs = p.delayMs > 0 ? p.delayMs : delayMs_;
            backends_.push_back(std::move(bs));
        }
        if (backends_.empty()) {
            std::cerr << "  [" << name_ << "] 错误: 没有有效的协议配置" << std::endl;
            return;
        }
        // launch_on_start: 预启动所有协议后端
        if (launchOnStart_) {
            launchOnStart_ = false;
            for (auto& bs : backends_) {
                launchProtocolBackend(bs);
            }
        }
    }

    // 主循环（hold_port=true 和 hold_port=false 共用）
    while (!stop_.load()) {
        // If daemon is alive and tracked by monitorBackend, skip bind
        if (backendPid_ > 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        // 外部占用监控态：暂停绑定，轮询端口释放后重新接管
        if (externalOccupied_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (stop_.load()) break;
            int port = monitorPort();
            if (port > 0 && !isPortBound((uint16_t)port)) {
                std::cout << "  [" << name_ << "] 外部占用者已退出，端口释放，重新接管" << std::endl;
                externalOccupied_.store(false);
            }
            continue;
        }
        listenFd_.store(createListener());
            if (listenFd_.load() < 0) {
                if (stop_.load()) break;
                consecutiveBindFailures_++;
                if (consecutiveBindFailures_ == 1) {
                    logBindFailed();
                } else if (consecutiveBindFailures_ == LOG_SILENCE_THRESHOLD) {
                    std::cerr << "  [" << name_ << "] 连续 " << LOG_SILENCE_THRESHOLD
                              << " 次绑定失败，后续日志已静默" << std::endl;
                }
                // 端口被外部进程占用且配置了 stop_command → 采纳为外部后端，纳入空闲回收
                if (tryAdoptExternalOccupant()) continue;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(retrySeconds_);
                while (std::chrono::steady_clock::now() < deadline && !stop_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (stop_.load()) break;
                retrySeconds_ = std::min(retrySeconds_ * 2, retrySecondsMax_);
                continue;
            }

        // 绑定成功，重置为初始间隔
        retrySeconds_ = retrySecondsBase_;

        std::cout << "  [" << name_ << "] 混合模式监听 " << listenAddr_
                  << (holdPort_ ? " (hold_port=true)" : "") << std::endl;

        // launch_on_start: 绑定成功后立即启动后端，跳过 accept 循环
        if (!holdPort_ && launchOnStart_) {
            launchOnStart_ = false;
            launchAndRelease();
        } else {

        while (!stop_.load()) {
            sockaddr_in cli;
            int len = sizeof(cli);
            int fd = platform::accept_fd(listenFd_, (struct sockaddr*)&cli, &len);
            if (fd < 0) {
                if (stop_.load() || platform::last_error() == PLATFORM_EINVAL) break;
                continue;
            }

            // 读前几个字节识别协议（不消费数据），MSG_PEEK 保证数据还在缓冲区
            std::string proto = detectProtocol(fd);
            if (proto == "unknown") {
                platform::close_fd(fd);
                continue;
            }
            std::cout << "  [" << name_ << "] 检测到 " << proto << " 连接" << std::endl;

            if (holdPort_) {
                // ── hold_port=true：代理模式 ──
                BackendState* bs = findBackend(proto);
                if (!bs) {
                    std::cout << "  [" << name_ << "] " << proto
                              << " 协议未配置，关闭连接" << std::endl;
                    platform::close_fd(fd);
                    continue;
                }

                // 按需启动后端
                if (bs->pid == 0) {
                    launchProtocolBackend(*bs);
                }

                if (bs->ready->load()) {
                    // 后端就绪 → 建立 TCP 隧道
                    proxyConnection(fd, bs->proxyTo);
                } else {
                    // 后端未就绪 → 发送协议对应的临时响应
                    std::cout << "  [" << name_ << "] " << proto
                              << " 后端未就绪，发送引导响应" << std::endl;
                    sendMixedResponse(fd, proto);
                    platform::close_fd(fd);
                }
                // 不释放端口，继续 accept
            } else {
                // ── hold_port=false：引导后释放模式 ──
                sendMixedResponse(fd, proto);
                platform::close_fd(fd);

                // 第一个连接触发后端启动
                if (backendPid_ == 0) {
                    launchAndRelease();
                    break;
                }
            }
        }
        }

        if (listenFd_ >= 0) {
            platform::close_fd(listenFd_);
            listenFd_ = -1;
        }

        if (holdPort_) break;  // 代理模式不重启，stop 时直接退出

        if (!autoRestart_ || stop_.load()) break;

        // After backend exits, wait delayMs then check if port is still in use
        // by a daemonized child process (e.g., 9router's next-server)
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs_));

        {
            int port = monitorPort();
            if (port > 0 && isPortBound((uint16_t)port)) {
                std::cout << "  [" << name_ << "] port still in use (child alive), waiting for release" << std::endl;
                while (!stop_.load()) {
                    // If monitorBackend found the daemon process, stop waiting
                    if (backendPid_ > 0) {
                        std::cout << "  [" << name_ << "] daemon process tracked (PID=" << backendPid_ << "), stopping wait" << std::endl;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (!isPortBound((uint16_t)port)) {
                        std::cout << "  [" << name_ << "] port released" << std::endl;
                        break;
                    }
                }
                if (stop_.load()) break;
            }
        }

        // If monitorBackend is tracking a live daemon, skip re-listen.
// Wait for idle timeout -> gracefulStop -> daemon exit -> backendPid_ = 0.
if (backendPid_ > 0) {
    std::cout << "  [" << name_ << "] daemon alive (PID=" << backendPid_ << "), skipping re-listen, waiting for idle timeout" << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    continue;
}
std::cout << "  [" << name_ << "] 重新监听端口" << std::endl;
    }
}





void PortRelay::start() {
    if (mode_ == "mixed") {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->mixedListenLoop();
            return nullptr;
        }, this, stackSize_);
        if (holdPort_) {
            platform::createThread(proxyMonitorThread_, [] (void* arg) -> void* {
                static_cast<PortRelay*>(arg)->proxyMonitorLoop();
                return nullptr;
            }, this, stackSize_);
        }
    } else if (mode_ == "proxy") {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->socks5ListenLoop();
            return nullptr;
        }, this, stackSize_);
    } else {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->listenLoop();
            return nullptr;
        }, this, stackSize_);
    }
    platform::createThread(monitorThread_, [] (void* arg) -> void* {
        static_cast<PortRelay*>(arg)->monitorBackend();
        return nullptr;
    }, this, stackSize_);
}

void PortRelay::stop() {
    // Idempotent stop: if already stopping, do nothing
    if (stop_.exchange(true)) return;
    // Close listening socket if open
    int fd = listenFd_.exchange(-1);
    if (fd >= 0) {
        platform::close_fd(fd);
    }
    // Clean up simple / mixed+hold_port=false backend
    if (backendPid_ > 0) {
if (platform::isChildAlive(backendPid_)) {
        platform::killProcess(backendPid_);
    }
    }
    // Clean up mixed+hold_port=true multiple backends
    for (auto& b : backends_) {
        if (b.pid > 0) {
if (platform::isChildAlive(b.pid)) {
            platform::killProcess(b.pid);
        }
        }
    }
if (platform::threadValid(listenThread_)) platform::joinThread(listenThread_);
    if (platform::threadValid(monitorThread_)) platform::joinThread(monitorThread_);
    if (platform::threadValid(proxyMonitorThread_)) platform::joinThread(proxyMonitorThread_);
}

void PortRelay::signalStop() {
    stop_.store(true);
    int fd = listenFd_.exchange(-1);
    if (fd >= 0) {
        platform::close_fd(fd);
    }
}

// Graceful stop implementation
void PortRelay::gracefulStop() {
    // 外部占用者回收：执行 stop_command 并验证端口释放；无法接管则本次运行放弃回收
    if (externalOccupied_.load()) {
        int port = monitorPort();
        if (stopCommand_.empty()) {
            std::cout << "  [" << name_ << "] 外部占用但未配置 stop_command，放弃回收（只监控）" << std::endl;
            recycleGiveUp_.store(true);
            externalOccupied_.store(false);
            return;
        }
        std::cout << "  [" << name_ << "] 外部占用空闲回收，执行关闭命令: " << stopCommand_ << std::endl;
        platform::runCommand(stopCommand_);
        bool released = (port <= 0);
        if (port > 0) {
            for (int i = 0; i < 30; ++i) {
                if (!isPortBound((uint16_t)port)) { released = true; break; }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
        externalOccupied_.store(false);
        if (released) {
            std::cout << "  [" << name_ << "] 端口已释放，接管成功，等待重新监听" << std::endl;
        } else {
            std::cout << "  [" << name_ << "] 端口仍被占用，放弃接管（本次运行只监控不回收）" << std::endl;
            recycleGiveUp_.store(true);
        }
        return;
    }
    if (backendPid_ <= 0) return;
    std::cout << "  [" << name_ << "] 正在关闭后端 (PID=" << backendPid_ << ")" << std::endl;
    if (!stopCommand_.empty()) {
        std::cout << "  [" << name_ << "] 执行关闭命令: " << stopCommand_ << std::endl;
        platform::runCommand(stopCommand_);
        for (int i = 0; i < 30; ++i) {
            if (backendPid_ == 0) return; // monitorBackend may have cleared it
if (!platform::isChildAlive(backendPid_)) {
        std::cout << "  [" << name_ << "] 后端已退出" << std::endl;
        backendPid_ = 0;
        return;
    }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::cout << "  [" << name_ << "] 关闭命令超时，发送 SIGTERM" << std::endl;
    }
    // Fallback SIGTERM
platform::killProcess(backendPid_);
    std::cout << "  [" << name_ << "] 已发送 SIGTERM 到后端 (PID=" << backendPid_ << ")" << std::endl;
    backendPid_ = 0;
}

// 驱逐：停机运行中的后端（simple 走 stop_command/SIGTERM，混合/代理遍历协议后端），
// 然后 signalStop 关闭监听 → 本次运行内粘性禁用（auto_restart 不会复活，/reload 或重启重新武装）。
void PortRelay::evict() {
    if (backendPid_ > 0 || externalOccupied_.load()) {
        gracefulStop();
    }
    for (auto& b : backends_) {
        if (b.pid > 0 && platform::isChildAlive(b.pid)) {
            std::cout << "  [" << name_ << "] 驱逐: 关闭协议后端 (PID=" << b.pid << ")" << std::endl;
            platform::killProcess(b.pid);
            b.pid = 0;
        }
    }
    signalStop();   // 关闭监听，条目本次运行内禁用
}

void PortRelay::resetForIdle() {
    if (platform::threadValid(listenThread_)) platform::joinThread(listenThread_);
    if (platform::threadValid(monitorThread_)) platform::joinThread(monitorThread_);
    if (platform::threadValid(proxyMonitorThread_)) platform::joinThread(proxyMonitorThread_);

    stop_.store(false);
    pendingRemoval_.store(false);
    groupReleased_.store(false);
    listenFd_.store(-1);

    if (mode_ == "mixed") {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->mixedListenLoop();
            return nullptr;
        }, this, stackSize_);
        if (holdPort_) {
            platform::createThread(proxyMonitorThread_, [] (void* arg) -> void* {
                static_cast<PortRelay*>(arg)->proxyMonitorLoop();
                return nullptr;
            }, this, stackSize_);
        }
    } else if (mode_ == "proxy") {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->socks5ListenLoop();
            return nullptr;
        }, this, stackSize_);
    } else {
        platform::createThread(listenThread_, [] (void* arg) -> void* {
            static_cast<PortRelay*>(arg)->listenLoop();
            return nullptr;
        }, this, stackSize_);
    }
    platform::createThread(monitorThread_, [] (void* arg) -> void* {
        static_cast<PortRelay*>(arg)->monitorBackend();
        return nullptr;
    }, this, stackSize_);
}

// Group coordination implementations
void PortRelay::setGroup(PortGroup* g) {
    group_ = g;
}

void PortRelay::forceReleasePort() {
    groupReleased_.store(true);
    int fd = listenFd_.exchange(-1);
    if (fd >= 0) {
        platform::close_fd(fd);
    }
}

void PortRelay::clearGroupLaunch() {
    groupReleased_.store(false);
}
