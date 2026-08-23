#ifndef GATEKEEPER_CONTROL_SERVER_H
#define GATEKEEPER_CONTROL_SERVER_H

#include "config.h"
#include "relay_platform.h"
#include <string>
#include <ctime>
#include <map>
#include <atomic>
#include <mutex>
#include <iostream>

class ControlServer {
    ControlConfig config_;
    std::atomic<bool> stop_{false};
    PlatformThread thread_{};
    int listenFd_{-1};
    struct RateLimiter {
        std::map<std::string, std::vector<long long>> window;
        std::mutex mtx_;
        int maxConnections = 20;
        long long windowMs = 60000;
        long long nowMs() {
            struct timespec ts;
            ::clock_gettime(CLOCK_MONOTONIC, &ts);
            return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
        }
        bool allow(const std::string& ip) {
            std::lock_guard<std::mutex> lock(mtx_);
            long long now = nowMs();
            auto& timestamps = window[ip];
            auto it = timestamps.begin();
            while (it != timestamps.end() && (now - *it) >= windowMs) {
                it = timestamps.erase(it);
            }
            if (static_cast<int>(timestamps.size()) >= maxConnections) {
                return false;
            }
            timestamps.push_back(now);
            return true;
        }
    };
    RateLimiter rateLimiter_;
public:
    // Override rate limit parameters (useful for testing and config)
    void setRateLimit(int maxConnections, int windowSeconds) {
        rateLimiter_.maxConnections = maxConnections;
        rateLimiter_.windowMs = static_cast<long long>(windowSeconds) * 1000;
    }
    explicit ControlServer(const ControlConfig& cfg);
    void start();
    void stop();
    bool isEnabled() const { return !config_.listen.empty(); }
private:
    void serverLoop();
    void handleRequest(int clientFd);
    struct HttpRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    bool parseHttpRequest(int fd, HttpRequest& req);
    // Route handlers
    void handleReload(int fd, const HttpRequest& req);
    void handleConfig(int fd, const HttpRequest& req);
    void handleHealth(int fd, const HttpRequest& req);
    void handleStatus(int fd, const HttpRequest& req);
    // Auth
    bool checkAuth(const HttpRequest& req);
    // Response helpers
    void sendResponse(int fd, int status, const std::string& contentType, const std::string& body);
    void sendError(int fd, int status, const std::string& message);
};

#endif // GATEKEEPER_CONTROL_SERVER_H
