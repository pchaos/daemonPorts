#include "control_server.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include "relay.h"
#include <vector>

struct ReloadSummary {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> modified;
    bool success = true;
};
#include <mutex>
#include <map>
#include <cctype>
#include <cstring>
#include <arpa/inet.h>

extern ReloadSummary reloadFromFile();
extern ReloadSummary reloadFromJson(const std::string& json);
extern std::mutex g_relaysMutex;
extern std::vector<std::unique_ptr<PortRelay>> g_relays;

// Helper: trim whitespace from both ends
static std::string trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end-1]))) --end;
    return s.substr(start, end-start);
}

// Simple address parser (host:port) – same logic as relay.cpp
static bool parseSockaddr(const std::string& addr, sockaddr_in& out) {
    auto pos = addr.find(':');
    if (pos == std::string::npos) return false;
    std::string host = addr.substr(0, pos);
    int port = std::stoi(addr.substr(pos+1));
    if (port <= 0 || port > 65535) return false;
    std::memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") out.sin_addr.s_addr = INADDR_ANY;
    else if (host == "localhost" || host == "127.0.0.1") out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else platform::inet_pton_wrap(AF_INET, host.c_str(), &out.sin_addr);
    return true;
}

ControlServer::ControlServer(const ControlConfig& cfg) : config_(cfg) {}

void ControlServer::start() {
    if (!isEnabled()) return;
    platform::createThread(thread_, [] (void* arg) -> void* {
        static_cast<ControlServer*>(arg)->serverLoop();
        return nullptr;
    }, this, 256); // small stack is fine for control server
}

void ControlServer::stop() {
    if (stop_.exchange(true)) return;
    int fd = listenFd_;
    if (fd >= 0) platform::close_fd(fd);
    if (platform::threadValid(thread_)) platform::joinThread(thread_);
}

void ControlServer::serverLoop() {
    // create listener
    sockaddr_in sa;
    if (!parseSockaddr(config_.listen, sa)) {
        std::cerr << "[ControlServer] invalid listen address: " << config_.listen << std::endl;
        return;
    }
    int fd = platform::socket_ai(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { std::cerr << "[ControlServer] socket error: " << platform::last_error_str() << std::endl; return; }
    int opt = 1;
    platform::set_sockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (platform::bind_fd(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { platform::close_fd(fd); return; }
    if (platform::listen_fd(fd, 128) < 0) { platform::close_fd(fd); return; }
    // Non-blocking listener so accept() returns immediately when no connection
    if (platform::set_nonblock(fd, 1) < 0) { platform::close_fd(fd); return; }
    listenFd_ = fd;
    std::cout << "[ControlServer] listening on " << config_.listen << std::endl;
    while (!stop_.load()) {
        sockaddr_in cli; int len = sizeof(cli);
        int client = platform::accept_fd(listenFd_, (struct sockaddr*)&cli, &len);
        if (client < 0) {
            if (stop_.load() || platform::last_error() == PLATFORM_EINVAL || platform::last_error() == EBADF || platform::last_error() == EAGAIN || platform::last_error() == EWOULDBLOCK) continue;
            continue;
        }
        // Get client IP for rate limiting
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli.sin_addr, clientIp, sizeof(clientIp));

        // Per-IP sliding-window rate limiter
        if (!rateLimiter_.allow(clientIp)) {
            sendError(client, 429, "Too Many Requests");
            platform::close_fd(client);
            continue;
        }

        // Set recv timeout so slow/idle clients don't hold the connection
        platform::set_recv_timeout(client, 15);

        handleRequest(client);
        platform::close_fd(client);
    }
    // cleanup
    int lfd = listenFd_; listenFd_ = -1;
    if (lfd >= 0) platform::close_fd(lfd);
}

bool ControlServer::parseHttpRequest(int fd, HttpRequest& req) {
    // read up to 8KB
    char buf[4096];
    std::string data;
    while (true) {
        ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
        if (n <= 0) break;
        data.append(buf, static_cast<size_t>(n));
        if (data.find("\r\n\r\n") != std::string::npos) break;
        if (data.size() > 8192) break; // safety
    }
    // split header/body
    size_t pos = data.find("\r\n\r\n");
    std::string header = (pos == std::string::npos) ? data : data.substr(0, pos);
    std::string body = (pos == std::string::npos) ? "" : data.substr(pos+4);
    std::istringstream hs(header);
    std::string line;
    if (!std::getline(hs, line)) return false;
    // request line e.g. "GET /path HTTP/1.1"
    std::istringstream rl(line);
    rl >> req.method >> req.path; // ignore version
    // headers
    while (std::getline(hs, line)) {
        if (line.empty() || line == "\r") break;
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon+1));
        // lower-case header name for lookup
        std::transform(name.begin(), name.end(), name.begin(), ::tolower);
        req.headers[name] = value;
    }
    // handle Content-Length if present and body incomplete
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        int cl = std::stoi(it->second);
        while ((int)body.size() < cl) {
            ssize_t n = platform::read_fd(fd, buf, sizeof(buf));
            if (n <= 0) break;
            body.append(buf, static_cast<size_t>(n));
        }
    }
    req.body = body;
    return true;
}

bool ControlServer::checkAuth(const HttpRequest& req) {
    if (config_.auth.type != "token") return true; // no auth needed
    auto it = req.headers.find("x-auth-token");
    if (it == req.headers.end()) return false;
    return it->second == config_.auth.token;
}

void ControlServer::handleRequest(int clientFd) {
    HttpRequest req;
    if (!parseHttpRequest(clientFd, req)) {
        sendError(clientFd, 400, "Bad Request");
        return;
    }
    if (!checkAuth(req)) {
        sendError(clientFd, 401, "Unauthorized");
        return;
    }
    // route dispatch
    if (req.method == "GET" && req.path == "/health") {
        handleHealth(clientFd, req);
    } else if (req.method == "GET" && req.path == "/status") {
        handleStatus(clientFd, req);
    } else if (req.method == "POST" && req.path == "/reload") {
        handleReload(clientFd, req);
    } else if (req.method == "POST" && req.path == "/config") {
        handleConfig(clientFd, req);
    } else {
        sendError(clientFd, 404, "Not Found");
    }
}

void ControlServer::sendResponse(int fd, int status, const std::string& contentType, const std::string& body) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status << " OK\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    std::string resp = oss.str();
    platform::write_fd(fd, resp.data(), resp.size());
}

void ControlServer::sendError(int fd, int status, const std::string& message) {
    std::string body = "{\"error\":\"" + message + "\"}";
    sendResponse(fd, status, "application/json", body);
}

void ControlServer::handleHealth(int fd, const HttpRequest&) {
    sendResponse(fd, 200, "application/json", "{\"status\":\"ok\"}");
}

void ControlServer::handleStatus(int fd, const HttpRequest&) {
    std::string body = "{\"ports\":[";
    bool first = true;
    {
        std::lock_guard<std::mutex> lock(g_relaysMutex);
        for (auto& r : g_relays) {
            if (!first) body += ",";
            first = false;
            body += "{\"name\":\"" + r->name() + "\",\"running\":" + (r->isBackendRunning() ? "true" : "false") + "}";
        }
    }
    body += "]}";
    sendResponse(fd, 200, "application/json", body);
}

void ControlServer::handleReload(int fd, const HttpRequest&) {
    auto result = reloadFromFile();
    if (result.success) {
        std::string body = "{\"result\":\"reload completed\",\"added\":" + std::to_string(result.added.size()) + ",\"removed\":" + std::to_string(result.removed.size()) + ",\"modified\":" + std::to_string(result.modified.size()) + "}";
        sendResponse(fd, 200, "application/json", body);
    } else {
        sendError(fd, 400, "Reload failed");
    }
}

void ControlServer::handleConfig(int fd, const HttpRequest& req) {
    auto result = reloadFromJson(req.body);
    if (result.success) {
        std::string body = "{\"result\":\"config accepted\",\"added\":" + std::to_string(result.added.size()) + ",\"removed\":" + std::to_string(result.removed.size()) + ",\"modified\":" + std::to_string(result.modified.size()) + "}";
        sendResponse(fd, 200, "application/json", body);
    } else {
        sendError(fd, 400, "Invalid config");
    }
}
