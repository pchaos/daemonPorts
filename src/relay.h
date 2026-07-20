#ifndef GATEKEEPER_RELAY_H
#define GATEKEEPER_RELAY_H

#include "config.h"

#include <string>
#include <atomic>
#include <thread>
#include <memory>
#include <vector>
#include <chrono>
#include <ctime>
#include <pthread.h>
#include <mutex>

class PortGroup; // forward declaration for grouping

class PortRelay {
    friend class PortGroup;
    std::string name_;
    std::string listenAddr_;
    std::string command_;
    int delayMs_;
    int refreshSeconds_;
    int retrySeconds_;          // 当前重试间隔（会被惩罚机制递增）
    int retrySecondsBase_;      // 初始重试间隔（成功绑定后重置）
    int retrySecondsMax_;       // 最大重试间隔上限
    bool autoRestart_;

    // 混合模式字段
    std::string mode_;
    bool holdPort_;
    std::string stopCommand_;    // 优雅关闭命令
    int idleMinutes_ = 20;       // 空闲超时分钟数
    std::vector<ProtocolConfig> protocols_;

    // proxy 模式字段
    AuthConfig  auth_;
    std::string httpTarget_;

    std::atomic<int> listenFd_{-1};
    // Group coordination members
    PortGroup* group_{nullptr};
    std::atomic<bool> groupReleased_{false};
    public:
    pid_t backendPid_ = 0;
    int stackSize_ = 256;  // KB
    std::atomic<bool> stop_{false};
    pthread_t listenThread_ = 0;
    pthread_t monitorThread_ = 0;

    // hold_port=true 时：每个协议的后端状态
    struct BackendState {
        std::string type;
        std::string command;
        std::string proxyTo;
        int delayMs = 5000;

        pid_t pid = 0;
        // 用 unique_ptr 包装 atomic，使 struct 可 move（vector 要求）
        std::unique_ptr<std::atomic<bool>> ready{new std::atomic<bool>(false)};
        std::unique_ptr<std::atomic<bool>> stopping{new std::atomic<bool>(false)};

        BackendState() = default;
        BackendState(BackendState&&) = default;
        BackendState& operator=(BackendState&&) = default;
        BackendState(const BackendState&) = delete;
        BackendState& operator=(const BackendState&) = delete;
    };
    std::vector<BackendState> backends_;
    pthread_t proxyMonitorThread_ = 0;

    // TCP 连接监控配置（0=禁用, >0=采样间隔秒）
    int tcpMonitorInterval_ = 0;

    // 日志去重模式: "skip" | "throttle" | "off"
    std::string logDedupMode_ = "skip";
    int logDedupCounter_ = 0;  // throttle 模式计数器
    int consecutiveBindFailures_ = 0; // consecutive bind failures counter
    static const int LOG_SILENCE_THRESHOLD = 10;
    void logBindFailed();

    // 活跃状态跟踪（由统一监控线程更新）
    time_t lastActiveTime_ = time(nullptr);

    int lastNonListen_ = -1;
    time_t lastSampleTime_ = 0;  // 上次采样时间（按各自 interval 控制频率）

    // 线程创建封装：用 pthread_attr_setstacksize 控制栈大小
    void createThread(pthread_t& thread, void* (*func)(void*), void* arg);

    // 通用的
    int createListener();
    pid_t launchBackend();
    bool waitForBackend(int ms);
    void sendStartupPage(int fd);
    void monitorBackend();

    // simple 模式
    void listenLoop();

    // mixed 模式
    void mixedListenLoop();
    std::string detectProtocol(int fd);
    void sendMixedResponse(int fd, const std::string& proto);

    // proxy 模式
    void socks5ListenLoop();

    // hold_port=true 代理模式
    int  connectToBackend(const std::string& addr);
    void proxyConnection(int clientFd, const std::string& proxyTo);
    void launchProtocolBackend(BackendState& bs);
    BackendState* findBackend(const std::string& type);
    void proxyMonitorLoop();

public:
    // 检测端口是否被其他进程监听（不建立连接，不影响空闲超时）
    static bool isPortBound(uint16_t port);
    PortRelay(const PortConfig& cfg);
    // Group coordination methods
    void setGroup(PortGroup* g);
    void forceReleasePort();
    void clearGroupLaunch();

    std::string buildStartupResponse() const;

    void start();
    void signalStop();
    void stop();

    void gracefulStop();
    bool isBackendRunning() const { return backendPid_ > 0; }
    PortGroup* group() const { return group_; }
    int idleMinutes() const { return idleMinutes_; }

    const std::string& name() const { return name_; }

    // 查询端口在最近 minutes 分钟内是否有过活跃连接
    bool hasRecentActivity(int minutes) const;

    // 供统一监控线程调用
    bool monitorEnabled() const { return tcpMonitorInterval_ > 0; }
    int  monitorIntervalSec() const { return tcpMonitorInterval_; }
    int  monitorPort() const;
    const std::string& logDedupMode() const { return logDedupMode_; }
    int& logDedupCounter() { return logDedupCounter_; }
    void updateActivity(bool active);
};

#endif // GATEKEEPER_RELAY_H