#ifndef GATEKEEPER_CONFIG_H
#define GATEKEEPER_CONFIG_H

#include <string>
#include <vector>

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
};

struct PortConfig {
    std::string name;
    bool        enabled = true;
    std::string listenAddr;
    std::string command;            // simple 模式 / mixed+hold_port=false 模式使用
    int         delayMs = 5000;
    int         refreshSeconds = 5;
    int         retrySeconds = 10;
    int         maxRetrySeconds = 300;
    bool        autoRestart = false;
    int         stackSize = 512;    // 线程栈大小(KB)，默认 512KB

    // 混合模式字段
    std::string mode = "simple";    // "simple" | "mixed"
    bool        holdPort = false;   // true = gatekeeper 持住端口做代理转发
    std::vector<ProtocolConfig> protocols;  // mixed 模式下的多协议配置

    // TCP 连接监控（可选）
    MonitorConfig monitor;
};

// 从 JSON 字符串解析配置
std::vector<PortConfig> parseConfig(const std::string& json);

// 从文件加载并解析配置
std::vector<PortConfig> loadConfig(const std::string& path);

#endif // GATEKEEPER_CONFIG_H