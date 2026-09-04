#ifndef GATEKEEPER_CONFIG_H
#define GATEKEEPER_CONFIG_H

#include <string>
#include <vector>

// SOCKS5 认证配置（proxy mode 使用）
struct AuthConfig {
    std::string type = "none";      // "none" | "userpass"
    std::string username;
    std::string password;
};

// 单个协议的后端配置（mixed mode 使用）
struct ProtocolConfig {
    std::string type;      // "http" | "socks5" | "socks4"
    std::string command;   // 启动命令（hold_port=true 时必填）
    std::string proxyTo;   // 后端地址，如 "127.0.0.1:8080"（hold_port=true 时必填）
    int         delayMs = 5000;
    bool        enabled = true;
};

// TCP 监控配置
struct MonitorConfig {
    bool        enabled = false;     // 是否启用端口连接监控
    int         intervalSec = 60;    // 采样间隔（秒）
    std::string logDedup = "skip";   // 日志去重: "skip"(不变跳过) | "throttle"(降频) | "off"(始终打印)
};

struct PortConfig {
    std::string name;
    std::string groupName; // optional group identifier, default empty
    bool        enabled = true;
    std::string listenAddr;
    std::string command;            // simple 模式 / mixed+hold_port=false 模式使用
    std::string stopCommand;    // 优雅关闭命令（可选，空字符串时不使用）
    int idleMinutes = 20;       // 空闲超时分钟数（默认 20 分钟）
    int         delayMs = 5000;
    int         refreshSeconds = 5;
    int         retrySeconds = 10;
    int         maxRetrySeconds = 300;
    bool        autoRestart = false;
    int         stackSize = 512;    // 线程栈大小(KB)，默认 512KB

    // 混合模式字段
    std::string mode = "simple";    // "simple" | "mixed" | "proxy"
    bool        holdPort = false;   // true = gatekeeper 持住端口做代理转发
    std::vector<ProtocolConfig> protocols;  // mixed 模式下的多协议配置

    // proxy 模式字段
    AuthConfig  auth;               // SOCKS5 认证配置（proxy mode）
    std::string httpTarget;         // HTTP 转发目标地址（proxy mode），如 "127.0.0.1:8080"

    // 启动时自动启动后端（首启一次性，热加载 / autoRestart 不触发）
    bool launchOnStart = false;

    // TCP 连接监控（可选）
    MonitorConfig monitor;
};

// 从 JSON 字符串解析配置
std::vector<PortConfig> parseConfig(const std::string& json);

// 从文件加载并解析配置
std::vector<PortConfig> loadConfig(const std::string& path);
// Control configuration structs
struct CommandConfig {
    std::string name;
    std::string command;
};
struct ControlAuth {
    std::string type = "token"; // "none" | "token" (default: token for security)
    std::string token;
};
struct ControlConfig {
    std::string listen;                // e.g. ":19999", empty = disabled
    ControlAuth auth;
    std::string pin;                  // PIN for ad-hoc command execution; empty = disabled
    int maxConnections = 20;           // rate limit: max connections per window
    int rateLimitSeconds = 60;         // rate limit: window size in seconds
    std::vector<CommandConfig> commands; // whitelisted named command presets
};
extern ControlConfig g_controlConfig;

// ── System monitor (系统资源监控) 配置 ──────────────────────────────
struct EvictionConfig {
    bool    enabled = false;
    double  memoryCritical = 0.90;  // 物理内存近满载阈值
    double  swapCritical = 0.90;    // 虚拟(swap)近满载阈值
    int     sustainSeconds = 900;   // 双满载持续秒数后触发驱逐
    double  relaunchMemoryBelow = 0.60; // 驱逐后重新拉起后端的内存占用上限（低于此值才允许）
    double  relaunchSwapBelow = 0.60;   // 驱逐后重新拉起后端的 swap 占用上限（低于此值才允许）
};
struct SystemMonitorConfig {
    bool    enabled = false;
    int     intervalSeconds = 300;      // 常规采样周期
    int     fastIntervalSeconds = 60;   // 高负载采样周期
    double  memoryHighThreshold = 0.66; // 物理内存 → 快周期阈值
    double  swapHighThreshold = 0.5;    // swap → 应急态阈值
    std::vector<std::string> emergencyCommands = {"reboot", "shutdown"};
    EvictionConfig eviction;
};
extern SystemMonitorConfig g_sysMonConfig;
// 启动时解析一次（与 control 段一致，热加载不触碰）
// json 传入完整配置文本，仅提取顶层 system_monitor 段
void parseSystemMonitorConfig(const std::string& json);

#endif // GATEKEEPER_CONFIG_H