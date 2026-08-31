#ifndef GATEKEEPER_CONTROL_SERVER_H
#define GATEKEEPER_CONTROL_SERVER_H

#include "config.h"
#include "relay_platform.h"
#include <string>
#include <chrono>
#include <map>
#include <atomic>
#include <mutex>
#include <vector>
#include <iostream>

class ControlServer {
    ControlConfig config_;
    std::atomic<bool> stop_{false};
    PlatformThread thread_{};
    struct PinCooldown {
        std::mutex mtx;
        std::map<std::string, long long> lockedUntil; // epoch-ms when cooldown expires, per IP
    };
    PinCooldown pinCooldown_;
    // Track streaming threads so they stop on shutdown
    std::mutex streamThreadsMtx_;
    std::vector<PlatformThread> streamThreads_;
    int listenFd_{-1};
    struct RateLimiter {
        std::map<std::string, std::vector<long long>> window;
        std::mutex mtx_;
        int maxConnections = 20;
        long long windowMs = 60000;
        long long nowMs() {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       now.time_since_epoch())
                .count();
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
    // For test access
    bool verifyPin(const std::string& pin);

private:
    void serverLoop();
    struct HttpRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    bool parseHttpRequest(int fd, HttpRequest& req);
    // Route handlers
    // Returns true if the caller (server loop) should close the client fd;
    // false when the fd ownership was handed to a streaming thread.
    bool handleRequest(int clientFd);
    void handleReload(int fd, const HttpRequest& req);
    void handleConfig(int fd, const HttpRequest& req);
    void handleVersion(int fd, const HttpRequest& req);
    void handleHealth(int fd, const HttpRequest& req);
    void handleStatus(int fd, const HttpRequest& req);
    // Returns true when the caller (handleRequest) should close the fd;
    // false when the streaming thread owns the fd and closes it.
    bool handleRun(int fd, const HttpRequest& req);
    // Auth
    bool checkAuth(const HttpRequest& req);
    // Command execution (sync capture + chunked streaming)
    void runSync(int fd, const std::string& command);
    // Returns true when the streaming thread owns the fd (thread closes it);
    // false when the caller should close the fd.
    bool runStream(int fd, const std::string& command);
    // Resolve a /run request body into the command to execute (or an error)
    struct RunResolution { std::string command; int httpStatus = 0; std::string errMsg; bool isPreset = false; };
    RunResolution resolveRun(const HttpRequest& req);
    static std::string jsonEscape(const std::string& s);
    void sendResponse(int fd, int status, const std::string& contentType, const std::string& body);
    void sendError(int fd, int status, const std::string& message);
};

#endif // GATEKEEPER_CONTROL_SERVER_H
