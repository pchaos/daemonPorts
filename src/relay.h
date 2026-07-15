#ifndef GATEKEEPER_RELAY_H
#define GATEKEEPER_RELAY_H

#include "config.h"

#include <string>
#include <atomic>
#include <thread>
#include <memory>

class PortRelay {
    std::string name_;
    std::string listenAddr_;
    std::string command_;
    int delayMs_;
    int refreshSeconds_;
    bool autoRestart_;

    int listenFd_ = -1;
    pid_t backendPid_ = 0;
    std::atomic<bool> stop_{false};
    std::thread listenThread_;
    std::thread monitorThread_;

    int createListener();
    pid_t launchBackend();
    bool waitForBackend(int ms);
    void sendStartupPage(int fd);
    void listenLoop();
    void monitorBackend();

public:
    PortRelay(const PortConfig& cfg);

    std::string buildStartupResponse() const;

    void start();
    void stop();

    const std::string& name() const { return name_; }
};

#endif // GATEKEEPER_RELAY_H