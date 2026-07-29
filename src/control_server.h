#ifndef GATEKEEPER_CONTROL_SERVER_H
#define GATEKEEPER_CONTROL_SERVER_H

#include "config.h"
#include "relay_platform.h"
#include <string>
#include <map>
#include <atomic>

class ControlServer {
    ControlConfig config_;
    std::atomic<bool> stop_{false};
    PlatformThread thread_{};
    int listenFd_{-1};
public:
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
