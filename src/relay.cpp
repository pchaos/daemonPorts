#include "relay.h"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

static bool parseSockaddr(const std::string& addr, sockaddr_in& out) {
    auto c = addr.find(':');
    if (c == std::string::npos) return false;
    std::string host = addr.substr(0, c);
    int port = std::stoi(addr.substr(c+1));
    if (port <= 0 || port > 65535) return false;

    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") out.sin_addr.s_addr = INADDR_ANY;
    else if (host == "localhost" || host == "127.0.0.1") out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else inet_pton(AF_INET, host.c_str(), &out.sin_addr);
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
    , stackSize_(cfg.stackSize > 0 ? cfg.stackSize : 512) {}

int PortRelay::createListener() {
    sockaddr_in sa;
    if (!parseSockaddr(listenAddr_, sa)) {
        std::cerr << "  [" << name_ << "] 无效地址: " << listenAddr_ << std::endl;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 128) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

pid_t PortRelay::launchBackend() {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", command_.c_str(), (char*)NULL);
        perror("execl");
        _exit(127);
    }
    return pid;
}

bool PortRelay::waitForBackend(int ms) {
    sockaddr_in sa;
    if (!parseSockaddr(listenAddr_, sa)) return false;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0) {
            if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) { close(fd); return true; }
            close(fd);
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return false;
        usleep(50000);
    }
}

void PortRelay::sendStartupPage(int fd) {
    std::string response = buildStartupResponse();
    write(fd, response.data(), response.size());
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
        listenFd_ = createListener();
        if (listenFd_ < 0) {
            if (!autoRestart_ || stop_.load()) break;
            std::cerr << "  [" << name_ << "] 绑定失败，" << retrySeconds_
                      << " 秒后重试（最大 " << retrySecondsMax_ << " 秒）" << std::endl;
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(retrySeconds_));
            }
            // 惩罚：当前重试间隔乘以 2，但不超过最大上限
            retrySeconds_ = std::min(retrySeconds_ * 2, retrySecondsMax_);
            continue;
        }

        // 绑定成功，重置为初始间隔
        retrySeconds_ = retrySecondsBase_;

        std::cout << "  [" << name_ << "] 监听 " << listenAddr_ << std::endl;

        while (!stop_.load()) {
            sockaddr_in cli;
            socklen_t len = sizeof(cli);
            int fd = accept(listenFd_, (struct sockaddr*)&cli, &len);
            if (fd < 0) {
                if (stop_.load() || errno == EINVAL) break;
                continue;
            }

            sendStartupPage(fd);
            close(fd);

            if (backendPid_ == 0) {
                pid_t pid = launchBackend();
                if (pid < 0) break;
                backendPid_ = pid;
                std::cout << "  [" << name_ << "] 后端已启动 (PID=" << pid << ")" << std::endl;

                close(listenFd_);
                listenFd_ = -1;
                std::cout << "  [" << name_ << "] 端口已释放，等待后端就绪" << std::endl;

                if (!waitForBackend(delayMs_))
                    std::cerr << "  [" << name_ << "] 警告: 后端可能未就绪" << std::endl;
                else
                    std::cout << "  [" << name_ << "] 后端就绪，端口已移交" << std::endl;

                break;
            }
        }

        if (listenFd_ >= 0) {
            close(listenFd_);
            listenFd_ = -1;
        }

        if (!autoRestart_ || stop_.load()) break;

        while (!stop_.load() && backendPid_ != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (stop_.load()) break;

        std::cout << "  [" << name_ << "] 重新监听端口" << std::endl;
    }
}

void PortRelay::monitorBackend() {
    while (!stop_.load()) {
        if (backendPid_ > 0) {
            int status;
            waitpid(backendPid_, &status, 0);
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
    ssize_t n = recv(fd, buf, sizeof(buf), MSG_PEEK);
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
        write(fd, resp, sizeof(resp));
    } else if (proto == "socks4") {
        // SOCKS4 响应：VN=0, CD=0x5B(请求被拒), DSTPORT=0, DSTIP=0
        unsigned char resp[] = {0x00, 0x5b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        write(fd, resp, sizeof(resp));
    }
}

// ── mixed 模式：持住端口代理转发 ──
// 连接到后端并建立双向 TCP 隧道。使用两个独立线程实现全双工转发，
// 任一方向关闭后另一方向自动终止。

static void pipeRelay(int src, int dst, std::atomic<bool>& done) {
    char buf[16384];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        size_t off = 0;
        while (off < (size_t)n) {
            ssize_t w = write(dst, buf + off, n - off);
            if (w <= 0) { done.store(true); return; }
            off += w;
        }
    }
    // 源端关闭后，通知对端停止写
    shutdown(dst, SHUT_WR);
    done.store(true);
}

int PortRelay::connectToBackend(const std::string& addr) {
    sockaddr_in sa;
    if (!parseSockaddr(addr, sa)) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void PortRelay::proxyConnection(int clientFd, const std::string& proxyTo) {
    int backendFd = connectToBackend(proxyTo);
    if (backendFd < 0) {
        close(clientFd);
        return;
    }

    std::cout << "  [" << name_ << "] 隧道已建立: 客户端 ↔ " << proxyTo << std::endl;

    // 双向全双工隧道，两个线程各负责一个方向
    std::atomic<bool> done1{false}, done2{false};
    std::thread t1(pipeRelay, clientFd, backendFd, std::ref(done1));
    std::thread t2(pipeRelay, backendFd, clientFd, std::ref(done2));

    // 等待任一方向结束
    while (!done1 && !done2) {
        usleep(50000);
    }

    // 强制关闭两端，回收线程
    shutdown(clientFd, SHUT_RDWR);
    shutdown(backendFd, SHUT_RDWR);
    if (t1.joinable()) t1.join();
    if (t2.joinable()) t2.join();
    close(clientFd);
    close(backendFd);

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
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) {
                    close(fd);
                    bs.ready->store(true);
                    break;
                }
                close(fd);
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(bs.delayMs))
                break;
            usleep(50000);
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
                int status;
                pid_t result = waitpid(b.pid, &status, WNOHANG);
                if (result == b.pid) {
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
    }

    // 主循环（hold_port=true 和 hold_port=false 共用）
    while (!stop_.load()) {
        listenFd_ = createListener();
        if (listenFd_ < 0) {
            if (!autoRestart_ || stop_.load()) break;
            std::cerr << "  [" << name_ << "] 绑定失败，" << retrySeconds_
                      << " 秒后重试（最大 " << retrySecondsMax_ << " 秒）" << std::endl;
            while (!stop_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(retrySeconds_));
            }
            // 惩罚：当前重试间隔乘以 2，但不超过最大上限
            retrySeconds_ = std::min(retrySeconds_ * 2, retrySecondsMax_);
            continue;
        }

        // 绑定成功，重置为初始间隔
        retrySeconds_ = retrySecondsBase_;

        std::cout << "  [" << name_ << "] 混合模式监听 " << listenAddr_
                  << (holdPort_ ? " (hold_port=true)" : "") << std::endl;

        while (!stop_.load()) {
            sockaddr_in cli;
            socklen_t len = sizeof(cli);
            int fd = accept(listenFd_, (struct sockaddr*)&cli, &len);
            if (fd < 0) {
                if (stop_.load() || errno == EINVAL) break;
                continue;
            }

            // 读前几个字节识别协议（不消费数据），MSG_PEEK 保证数据还在缓冲区
            std::string proto = detectProtocol(fd);
            if (proto == "unknown") {
                close(fd);
                continue;
            }
            std::cout << "  [" << name_ << "] 检测到 " << proto << " 连接" << std::endl;

            if (holdPort_) {
                // ── hold_port=true：代理模式 ──
                BackendState* bs = findBackend(proto);
                if (!bs) {
                    std::cout << "  [" << name_ << "] " << proto
                              << " 协议未配置，关闭连接" << std::endl;
                    close(fd);
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
                    close(fd);
                }
                // 不释放端口，继续 accept
            } else {
                // ── hold_port=false：引导后释放模式 ──
                sendMixedResponse(fd, proto);
                close(fd);

                // 第一个连接触发后端启动
                if (backendPid_ == 0) {
                    pid_t pid = launchBackend();
                    if (pid < 0) break;
                    backendPid_ = pid;
                    std::cout << "  [" << name_ << "] 后端已启动 (PID=" << pid << ")"
                              << std::endl;

                    close(listenFd_);
                    listenFd_ = -1;
                    std::cout << "  [" << name_ << "] 端口已释放，等待后端就绪" << std::endl;

                    if (!waitForBackend(delayMs_))
                        std::cerr << "  [" << name_ << "] 警告: 后端可能未就绪" << std::endl;
                    else
                        std::cout << "  [" << name_ << "] 后端就绪，端口已移交" << std::endl;

                    break;
                }
            }
        }

        if (listenFd_ >= 0) {
            close(listenFd_);
            listenFd_ = -1;
        }

        if (holdPort_) break;  // 代理模式不重启，stop 时直接退出

        if (!autoRestart_ || stop_.load()) break;

        while (!stop_.load() && backendPid_ != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (stop_.load()) break;

        std::cout << "  [" << name_ << "] 重新监听端口" << std::endl;
    }
}

void PortRelay::createThread(pthread_t& thread, void* (*func)(void*), void* arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stackSize_ * 1024);
    pthread_create(&thread, &attr, func, arg);
    pthread_attr_destroy(&attr);
}

void PortRelay::start() {
    // 根据 mode 选择对应的监听循环：
    //   "mixed" → 协议感知混合模式，支持多种协议引导响应
    //   其他     → 兼容旧的 simple 模式，只发 HTTP 启动页（默认行为）
    if (mode_ == "mixed") {
        createThread(listenThread_, [](void* arg) -> void* {
            static_cast<PortRelay*>(arg)->mixedListenLoop();
            return nullptr;
        }, this);
        if (holdPort_) {
            createThread(proxyMonitorThread_, [](void* arg) -> void* {
                static_cast<PortRelay*>(arg)->proxyMonitorLoop();
                return nullptr;
            }, this);
        }
    } else {
        createThread(listenThread_, [](void* arg) -> void* {
            static_cast<PortRelay*>(arg)->listenLoop();
            return nullptr;
        }, this);
    }
    createThread(monitorThread_, [](void* arg) -> void* {
        static_cast<PortRelay*>(arg)->monitorBackend();
        return nullptr;
    }, this);
}

void PortRelay::stop() {
    stop_.store(true);
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    // 清理 simple / mixed+hold_port=false 的后端
    if (backendPid_ > 0) {
        kill(backendPid_, SIGTERM);
    }
    // 清理 mixed+hold_port=true 的多个后端
    for (auto& b : backends_) {
        if (b.pid > 0) {
            kill(b.pid, SIGTERM);
        }
    }
    if (listenThread_) pthread_join(listenThread_, nullptr);
    if (monitorThread_) pthread_join(monitorThread_, nullptr);
    if (proxyMonitorThread_) pthread_join(proxyMonitorThread_, nullptr);
}