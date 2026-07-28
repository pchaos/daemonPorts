#include "relay_platform.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <algorithm>
#include <unistd.h>

namespace platform {

pid_t findPidUsingPort(uint16_t port) {
    // 方案1: ss -tlnp 解析 pid= 字段
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ss -tlnp 2>/dev/null | grep ':%u '", port);
    FILE* p = popen(cmd, "r");
    if (p) {
        char buf[512];
        if (fgets(buf, sizeof(buf), p)) {
            pclose(p);
            char* pidStr = strstr(buf, "pid=");
            if (pidStr) {
                pid_t pid = (pid_t)strtol(pidStr + 4, nullptr, 10);
                if (pid > 0) return pid;
            }
        } else {
            pclose(p);
        }
    }

    // 方案2: /proc/net/tcp + /proc/*/fd/
    char hexPort[16];
    snprintf(hexPort, sizeof(hexPort), "%04X", port);
    std::ifstream proc("/proc/net/tcp");
    std::string line;
    while (std::getline(proc, line)) {
        std::istringstream iss(line);
        std::string sl, localAddr;
        if (!(iss >> sl >> localAddr)) continue;
        if (localAddr.size() < 5) continue;
        std::string addrPort = localAddr.substr(localAddr.find(':') + 1);
        if (addrPort != hexPort) continue;

        // 跳过不需要的字段，找到 inode（第9列）
        std::string token;
        for (int i = 0; i < 2; i++) iss >> token; // skip rem_address, st
        for (int i = 0; i < 6; i++) iss >> token;
        std::string inodeStr = token;

        DIR* procDir = opendir("/proc");
        if (!procDir) return 0;
        struct dirent* entry;
        while ((entry = readdir(procDir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char* end;
            long pid = strtol(entry->d_name, &end, 10);
            if (*end != '\0') continue;

            char fdPath[256];
            snprintf(fdPath, sizeof(fdPath), "/proc/%ld/fd", pid);
            DIR* fdDir = opendir(fdPath);
            if (!fdDir) continue;
            struct dirent* fdEntry;
            while ((fdEntry = readdir(fdDir)) != nullptr) {
                char linkPath[256];
                snprintf(linkPath, sizeof(linkPath), "/proc/%ld/fd/%s", pid, fdEntry->d_name);
                char linkTarget[256];
                ssize_t len = readlink(linkPath, linkTarget, sizeof(linkTarget) - 1);
                if (len > 0) {
                    linkTarget[len] = '\0';
                    if (strncmp(linkTarget, "socket:[", 8) == 0) {
                        char* end2;
                        long inode = strtol(linkTarget + 8, &end2, 10);
                        if (*end2 == ']' && inode == strtol(inodeStr.c_str(), nullptr, 10)) {
                            closedir(fdDir);
                            closedir(procDir);
                            return (pid_t)pid;
                        }
                    }
                }
            }
            closedir(fdDir);
        }
        closedir(procDir);
        break;
    }
    return 0;
}

std::string findProcessUsingPort(uint16_t port) {
    // 方案1: ss -tlnp（能看到同用户进程的进程名）
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ss -tlnp 2>/dev/null | grep ':%u '", port);
    FILE* p = popen(cmd, "r");
    if (p) {
        char buf[512];
        if (fgets(buf, sizeof(buf), p)) {
            pclose(p);
            std::string result(buf);
            if (!result.empty() && result.back() == '\n') result.pop_back();
            return result.substr(0, 80);
        }
        pclose(p);
    }

    // 方案2: /proc/net/tcp + /proc/*/fd/ 匹配进程名
    char hexPort[16];
    snprintf(hexPort, sizeof(hexPort), "%04X", port);
    std::ifstream proc("/proc/net/tcp");
    std::string line;
    while (std::getline(proc, line)) {
        std::istringstream iss(line);
        std::string sl, localAddr;
        if (!(iss >> sl >> localAddr)) continue;
        if (localAddr.size() < 5) continue;
        std::string addrPort = localAddr.substr(localAddr.find(':') + 1);
        if (addrPort != hexPort) continue;

        // 跳过字段，找到 inode（第9列）
        std::string token;
        for (int i = 0; i < 2; i++) iss >> token; // skip rem_address, st
        for (int i = 0; i < 6; i++) iss >> token;
        std::string inodeStr = token;

        // 扫描 /proc/*/fd/ 查找 inode
        DIR* procDir = opendir("/proc");
        if (!procDir) return "";
        struct dirent* entry;
        while ((entry = readdir(procDir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;
            char* end;
            long pid = strtol(entry->d_name, &end, 10);
            if (*end != '\0') continue;

            char fdPath[256];
            snprintf(fdPath, sizeof(fdPath), "/proc/%ld/fd", pid);
            DIR* fdDir = opendir(fdPath);
            if (!fdDir) continue;
            struct dirent* fdEntry;
            while ((fdEntry = readdir(fdDir)) != nullptr) {
                char linkPath[256];
                snprintf(linkPath, sizeof(linkPath), "/proc/%ld/fd/%s", pid, fdEntry->d_name);
                char linkTarget[256];
                ssize_t len = readlink(linkPath, linkTarget, sizeof(linkTarget) - 1);
                if (len > 0) {
                    linkTarget[len] = '\0';
                    if (strncmp(linkTarget, "socket:[", 8) == 0) {
                        char* end2;
                        long inode = strtol(linkTarget + 8, &end2, 10);
                        if (*end2 == ']' && inode == strtol(inodeStr.c_str(), nullptr, 10)) {
                            closedir(fdDir);
                            closedir(procDir);
                            char cmdlinePath[256];
                            snprintf(cmdlinePath, sizeof(cmdlinePath), "/proc/%ld/comm", pid);
                            std::ifstream comm(cmdlinePath);
                            std::string commName;
                            std::getline(comm, commName);
                            if (!commName.empty()) {
                                return commName + " (PID=" + std::to_string(pid) + ")";
                            }
                            return "PID=" + std::to_string(pid);
                        }
                    }
                }
            }
            closedir(fdDir);
        }
        closedir(procDir);
        break;
    }
    return "";
}

} // namespace platform
