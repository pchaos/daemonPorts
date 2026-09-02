#ifndef GATEKEEPER_SYSTEM_MONITOR_H
#define GATEKEEPER_SYSTEM_MONITOR_H

#include "config.h"
#include "relay_platform.h"
#include <atomic>
#include <string>
#include <vector>

// ── 一次采样得到的系统度量 ─────────────────────────────────────────
struct SysMetrics {
    bool   cpuValid = false;
    double cpuPercent = 0.0;        // 上一采样间隔的 CPU 占用率 0-100（首拍无效）
    long long memTotal = 0;         // 物理内存（bytes）
    long long memAvailable = 0;
    long long memUsed = 0;          // total - available
    double memPercent = 0.0;        // used/total * 100
    long long swapTotal = 0;        // 虚拟内存 / swap（bytes）
    long long swapUsed = 0;
    double swapPercent = 0.0;
    bool   swapValid = false;       // 平台/容器无 swap 时 false → 相关字段为 0
};

// ── /procs 返回的进程内存信息（按 RSS 降序）────────────────────────
struct ProcMemInfo {
    long long pid = 0;
    std::string name;
    long long rss = 0;   // bytes
    long long vms = 0;   // bytes
};

// ── 平台度量原语（system_monitor.cpp 内按平台实现）─────────────────
// 返回 false 表示该平台/环境无法读取指标 → 采样器整体不启用
bool readSystemMetrics(SysMetrics& out);
// 全进程表；调用方负责过滤/排序
std::vector<ProcMemInfo> readProcessList();
// 单进程直系 RSS（驱逐并列时 tie-break），失败返回 -1
long long processRssBytes(pid_t pid);

// ── 顶层采样器（自持线程）──────────────────────────────────────────
// 供控制服务器跨线程读取的原子快照
class SystemMonitor {
public:
    explicit SystemMonitor(const SystemMonitorConfig& cfg) : cfg_(cfg) {}

    bool enabled() const { return cfg_.enabled; }
    void start();
    void stop();

    // 共享快照（采样线程写，控制线程读）
    std::atomic<bool>       ready{false};
    std::atomic<bool>       fastMode{false};
    std::atomic<bool>       emergency{false};   // 应急态：swap 超阈值 →
    std::atomic<double>     cpuPercent{0.0};
    std::atomic<long long>  memTotal{0};
    std::atomic<long long>  memAvailable{0};
    std::atomic<long long>  memUsed{0};
    std::atomic<double>     memPercent{0.0};
    std::atomic<long long>  swapTotal{0};
    std::atomic<long long>  swapUsed{0};
    std::atomic<double>     swapPercent{0.0};

    bool inEmergency() const { return emergency.load(); }
    const SystemMonitorConfig& config() const { return cfg_; }
    bool inFastMode() const { return fastMode.load(); }

private:
    SystemMonitorConfig cfg_;
    PlatformThread thread_{};
    std::atomic<bool> stop_{false};
    void loop();
    bool maybeEvict(double memPct, double swapPct);
};

// 全局采样器指针（main 创建并赋值；控制服务器经它取快照）
extern SystemMonitor* g_sysMon;

// 应急免密判定：swap 应急态下，命令首词（剥去前导 sudo/doas）命中应急名单 → true
bool sysMonEmergencyPinBypass(const std::string& rawCommand);

#endif // GATEKEEPER_SYSTEM_MONITOR_H