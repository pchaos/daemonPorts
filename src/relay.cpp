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
    , autoRestart_(cfg.autoRestart) {}

int PortRelay::createListener() {
    sockaddr_in sa;
    if (!parseSockaddr(listenAddr_, sa)) {
        std::cerr << "  [" << name_ << "] 无效地址: " << listenAddr_ << std::endl;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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
        if (listenFd_ < 0) break;

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

void PortRelay::start() {
    listenThread_ = std::thread(&PortRelay::listenLoop, this);
    monitorThread_ = std::thread(&PortRelay::monitorBackend, this);
}

void PortRelay::stop() {
    stop_.store(true);
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
    if (backendPid_ > 0) {
        kill(backendPid_, SIGTERM);
    }
    if (listenThread_.joinable()) listenThread_.join();
    if (monitorThread_.joinable()) monitorThread_.join();
}