#ifndef GATEKEEPER_CONFIG_H
#define GATEKEEPER_CONFIG_H

#include <string>
#include <vector>

struct PortConfig {
    std::string name;
    bool        enabled = true;
    std::string listenAddr;
    std::string command;
    int         delayMs = 5000;
    int         refreshSeconds = 3;
    bool        autoRestart = false;
};

// 从 JSON 字符串解析配置
std::vector<PortConfig> parseConfig(const std::string& json);

// 从文件加载并解析配置
std::vector<PortConfig> loadConfig(const std::string& path);

#endif // GATEKEEPER_CONFIG_H