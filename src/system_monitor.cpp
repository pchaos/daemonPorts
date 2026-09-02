#include "system_monitor.h"
#include "relay.h"

#include <iostream>
#include <fstream>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <thread>

#ifndef _WIN32
#include <unistd.h>
#include <dirent.h>
#else
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <libproc.h>
#endif

SystemMonitor* g_sysMon = nullptr;

// ── 日志格式化小工具 ────────────────────────────────────────────────
static std::string pct(double v, bool valid) {
    char buf[16];
    if (!valid) return "n/a";
    std::snprintf(buf, sizeof buf, "%.1f", v);
    return buf;
}
static std::string bytes(long long b) {
    char buf[32];
    if (b >= 1073741824LL) std::snprintf(buf, sizeof buf, "%.1fG", (double)b / 1073741824.0);
    else if (b >= 1048576LL) std::snprintf(buf, sizeof buf, "%.1fM", (double)b / 1048576.0);
    else std::snprintf(buf, sizeof buf, "%.0fK", (double)b / 1024.0);
    return buf;
}

// ── 跨进程操作 relay 需要的外部符号 ─────────────────────────────────
extern std::mutex g_relaysMutex;
extern std::vector<std::unique_ptr<PortRelay>> g_relays;

// ══════════════════════════════════════════════════════════════════
// 平台度量：Linux
// ══════════════════════════════════════════════════════════════════
#ifndef _WIN32

static bool linuxCpu(SysMetrics& m) {
    static long long prevIdle = -1, prevTotal = 0;
    std::string line;
    {
        std::ifstream f("/proc/stat");
        if (!f) return false;
        std::getline(f, line);
    }
    if (line.compare(0, 4, "cpu ") != 0) return false;
    long long u,n,s,i,io,irq,sirq,st;
    if (std::sscanf(line.c_str(), "cpu %lld %lld %lld %lld %lld %lld %lld %lld",
                    &u,&n,&s,&i,&io,&irq,&sirq,&st) < 4) return false;
    long long idle = i + io;
    long long total = u + n + s + i + io + irq + sirq + st;
    if (prevIdle < 0) { prevIdle = idle; prevTotal = total; m.cpuValid = false; m.cpuPercent = 0; return true; }
    long long dTotal = total - prevTotal;
    long long dIdle   = idle - prevIdle;
    prevIdle = idle; prevTotal = total;
    if (dTotal <= 0) { m.cpuValid = false; m.cpuPercent = 0; return true; }
    m.cpuValid = true;
    m.cpuPercent = (double)(dTotal - dIdle) / (double)dTotal * 100.0;
    return true;
}

static void linuxMem(SysMetrics& m) {
    std::ifstream f("/proc/meminfo");
    long long mt=0, ma=0, st=0, sf=0;
    std::string k; long long v; std::string unit;
    while (f >> k >> v >> unit) {
        if (k == "MemTotal:") mt = v;
        else if (k == "MemAvailable:") ma = v;
        else if (k == "SwapTotal:") st = v;
        else if (k == "SwapFree:") sf = v;
    }
    if (mt <= 0) return;
    long long usedKb = mt - (ma > 0 ? ma : mt);
    m.memTotal = mt * 1024;
    m.memAvailable = (ma > 0 ? ma : mt) * 1024;
    m.memUsed = usedKb * 1024;
    m.memPercent = (double)usedKb / (double)mt * 100.0;
    if (st > 0) {
        long long usedSwapKb = st - sf;
        m.swapTotal = st * 1024;
        m.swapUsed = (usedSwapKb > 0 ? usedSwapKb : 0) * 1024;
        m.swapPercent = (double)(usedSwapKb > 0 ? usedSwapKb : 0) / (double)st * 100.0;
        m.swapValid = true;
    }
}

bool readSystemMetrics(SysMetrics& m) {
    SysMetrics r;
    bool ok = linuxCpu(r);
    linuxMem(r);
    // CPU 有效即可视为可采样；内存读取失败则整体不可用
    if (r.memTotal <= 0) return false;
    m = r;
    return true;
}

std::vector<ProcMemInfo> readProcessList() {
    std::vector<ProcMemInfo> out;
    pid_t self = platform::currentPid();
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    DIR* dir = opendir("/proc");
    if (!dir) return out;
    struct dirent* d;
    while ((d = readdir(dir))) {
        char c0 = d->d_name[0];
        if (c0 < '0' || c0 > '9') continue;
        long long pid = atoll(d->d_name);
        if (pid <= 0 || pid == (long long)self) continue;
        std::string base = std::string("/proc/") + d->d_name;
        std::ifstream f(base + "/statm");
        long long size = -1, res = -1;
        if (f) f >> size >> res;
        if (res < 0) continue;
        ProcMemInfo p;
        p.pid = pid;
        p.rss = res * pg;
        p.vms = (size > 0 ? size : 0) * pg;
        std::ifstream cf(base + "/comm");
        std::string nm; if (cf) std::getline(cf, nm);
        p.name = nm.empty() ? std::string(d->d_name) : nm;
        out.push_back(std::move(p));
    }
    closedir(dir);
    return out;
}

long long processRssBytes(pid_t pid) {
    long pg = sysconf(_SC_PAGESIZE); if (pg <= 0) pg = 4096;
    std::ifstream f("/proc/" + std::to_string((long long)pid) + "/statm");
    long long size, res;
    if (!(f >> size >> res)) return -1;
    return res * pg;
}

#endif // _WIN32

// ══════════════════════════════════════════════════════════════════
// 平台度量：Windows
// ══════════════════════════════════════════════════════════════════
#ifdef _WIN32

static bool winCpu(SysMetrics& m) {
    FILETIME idle, kernel, user;
    if (!GetSystemTimes(&idle, &kernel, &user)) return false;
    auto toU64 = [](const FILETIME& ft) -> unsigned long long {
        return ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    };
    static unsigned long long prevK = 0, prevU = 0, prevI = 0;
    unsigned long long k = toU64(kernel), u = toU64(user), i = toU64(idle);
    if (prevK == 0 && prevU == 0 && prevI == 0) {
        prevK = k; prevU = u; prevI = i; m.cpuValid = false; m.cpuPercent = 0; return true;
    }
    unsigned long long dBusy = (k + u - i) - (prevK + prevU - prevI);
    unsigned long long dTotal = (k + u) - (prevK + prevU);
    prevK = k; prevU = u; prevI = i;
    if (dTotal == 0) { m.cpuValid = false; m.cpuPercent = 0; return true; }
    m.cpuValid = true;
    m.cpuPercent = (double)dBusy / (double)dTotal * 100.0;
    return true;
}

static void winMem(SysMetrics& m) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return;
    m.memTotal = (long long)ms.ullTotalPhys;
    m.memAvailable = (long long)ms.ullAvailPhys;
    m.memUsed = m.memTotal - m.memAvailable;
    if (m.memTotal > 0) m.memPercent = (double)m.memUsed / (double)m.memTotal * 100.0;
    unsigned long long swapTotal = ms.ullTotalPageFile, swapAvail = ms.ullAvailPageFile;
    if (swapTotal > 0) {
        m.swapTotal = (long long)swapTotal;
        m.swapUsed = (long long)(swapTotal - swapAvail);
        m.swapPercent = (double)(swapTotal - swapAvail) / (double)swapTotal * 100.0;
        m.swapValid = true;
    }
}

bool readSystemMetrics(SysMetrics& m) {
    SysMetrics r;
    winCpu(r);
    winMem(r);
    if (r.memTotal <= 0) return false;
    m = r;
    return true;
}

static std::wstring procName(DWORD pid) {
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return name;
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleBaseNameW(h, NULL, buf, MAX_PATH);
    if (n > 0) name.assign(buf, n);
    CloseHandle(h);
    return name;
}

std::vector<ProcMemInfo> readProcessList() {
    std::vector<ProcMemInfo> out;
    DWORD self = GetCurrentProcessId();
    DWORD pids[4096]; DWORD cb = 0;
    if (!EnumProcesses(pids, sizeof(pids), &cb)) return out;
    DWORD count = cb / sizeof(DWORD);
    for (DWORD i = 0; i < count; ++i) {
        if (pids[i] == self || pids[i] == 0) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pids[i]);
        if (!h) continue;
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) {
            ProcMemInfo p;
            p.pid = (long long)pids[i];
            p.rss = (long long)pmc.WorkingSetSize;
            p.vms = (long long)pmc.PagefileUsage;
            std::wstring w = procName(pids[i]);
            p.name = std::string(w.begin(), w.end());
            out.push_back(std::move(p));
        }
        CloseHandle(h);
    }
    return out;
}

long long processRssBytes(pid_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, (DWORD)pid);
    if (!h) return -1;
    PROCESS_MEMORY_COUNTERS pmc;
    long long r = (GetProcessMemoryInfo(h, &pmc, sizeof(pmc))) ? (long long)pmc.WorkingSetSize : -1;
    CloseHandle(h);
    return r;
}

#endif // _WIN32

// ══════════════════════════════════════════════════════════════════
// 平台度量：macOS
// ══════════════════════════════════════════════════════════════════
#ifdef __APPLE__

static bool darwinCpu(SysMetrics& m) {
    host_cpu_load_info_data_t info;
    mach_msg_type_number_t cnt = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO,
                        (host_info_t)&info, &cnt) != KERN_SUCCESS) return false;
    unsigned long long user   = info.cpu_ticks[CPU_STATE_USER];
    unsigned long long system = info.cpu_ticks[CPU_STATE_SYSTEM];
    unsigned long long idle   = info.cpu_ticks[CPU_STATE_IDLE];
    unsigned long long nice   = info.cpu_ticks[CPU_STATE_NICE];
    static unsigned long long prevU=0, prevS=0, prevI=0, prevN=0;
    if (prevU==0 && prevS==0 && prevI==0 && prevN==0) {
        prevU=user; prevS=system; prevI=idle; prevN=nice;
        m.cpuValid=false; m.cpuPercent=0; return true;
    }
    unsigned long long dBusy = (user+system+nice) - (prevU+prevS+prevN);
    unsigned long long dTotal = (user+system+nice+idle) - (prevU+prevS+prevN+prevI);
    prevU=user; prevS=system; prevI=idle; prevN=nice;
    if (dTotal == 0) { m.cpuValid=false; m.cpuPercent=0; return true; }
    m.cpuValid = true;
    m.cpuPercent = (double)dBusy / (double)dTotal * 100.0;
    return true;
}

static void darwinMem(SysMetrics& m) {
    int64_t total = 0; size_t len = sizeof(total);
    if (sysctlbyname("hw.memsize", &total, &len, NULL, 0) != 0) return;
    m.memTotal = (long long)total;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
        long long page = (long long)vm_page_size;
        long long freeMem = (long long)vm.free_count * page;
        long long used = total - freeMem;
        m.memAvailable = (long long)(vm.free_count + vm.inactive_count) * page;
        if (m.memAvailable < 0 || m.memAvailable > total) m.memAvailable = freeMem;
        m.memUsed = total - m.memAvailable;
        m.memPercent = total>0 ? (double)m.memUsed/(double)total*100.0 : 0.0;
    }
    // swap via sysctl vm.swapusage
    struct xsw_usage {
        unsigned long long xsu_total, xsu_used, xsu_avail, xsu_pagesize;
        int xsu_encrypted;
    } xsw;
    size_t slen = sizeof(xsw);
    if (sysctlbyname("vm.swapusage", &xsw, &slen, NULL, 0) == 0 && xsw.xsu_total > 0) {
        m.swapTotal = (long long)xsw.xsu_total;
        m.swapUsed = (long long)xsw.xsu_used;
        m.swapPercent = (double)xsw.xsu_used / (double)xsw.xsu_total * 100.0;
        m.swapValid = true;
    }
}

bool readSystemMetrics(SysMetrics& m) {
    SysMetrics r;
    darwinCpu(r);
    darwinMem(r);
    if (r.memTotal <= 0) return false;
    m = r;
    return true;
}

std::vector<ProcMemInfo> readProcessList() {
    std::vector<ProcMemInfo> out;
    int max = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (max <= 0) return out;
    std::vector<pid_t> pids(max);
    int got = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), (int)(pids.size()*sizeof(pid_t)));
    if (got <= 0) return out;
    pid_t self = platform::currentPid();
    int n = got / (int)sizeof(pid_t);
    for (int i = 0; i < n; ++i) {
        if (pids[i] <= 0 || pids[i] == self) continue;
        proc_taskinfo ti;
        if (proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != sizeof(ti)) continue;
        ProcMemInfo p;
        p.pid = (long long)pids[i];
        p.rss = (long long)ti.pti_resident_size;
        p.vms = (long long)ti.pti_virtual_size;
        char nm[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_name(pids[i], nm, sizeof(nm)) > 0) p.name = nm;
        out.push_back(std::move(p));
    }
    return out;
}

long long processRssBytes(pid_t pid) {
    proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti)) != sizeof(ti)) return -1;
    return (long long)ti.pti_resident_size;
}

#endif // __APPLE__

// ══════════════════════════════════════════════════════════════════
// 采样循环
// ══════════════════════════════════════════════════════════════════
static void sleepSecs(int n, const std::atomic<bool>& stop) {
    for (int i = 0; i < n && !stop.load(); ++i)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

bool SystemMonitor::maybeEvict(double memPct, double swapPct) {
    std::lock_guard<std::mutex> lock(g_relaysMutex);
    PortRelay* victim = nullptr;
    long long bestIdle = -1, bestRss = -1;
    time_t now = time(nullptr);
    for (auto& r : g_relays) {
        if (!r->isBackendRunning()) continue;
        long long idle = (long long)(now - r->lastActiveTime_.load());
        if (idle < 0) idle = 0;
        long long rss = processRssBytes(r->backendPid_);
        if (idle > bestIdle || (idle == bestIdle && rss > bestRss)) {
            bestIdle = idle; bestRss = rss; victim = r.get();
        }
    }
    if (!victim) {
        std::cout << "[SysMonitor] 驱逐触发但无运行中的子配置项" << std::endl;
        return false;
    }
    std::cout << "[SysMonitor] 驱逐 [" << victim->name() << "] 无流量 " << bestIdle
              << "s (RSS=" << bytes(bestRss) << ") mem=" << pct(memPct, true)
              << "% swap=" << pct(swapPct, true) << "%" << std::endl;
    victim->evict();
    return true;
}

void SystemMonitor::loop() {
    SysMetrics m;
    if (!readSystemMetrics(m)) {
        std::cout << "[SysMonitor] 无法读取系统指标，采样器不启动" << std::endl;
        return;
    }
    time_t criticalSince = 0;
    bool prevCritical = false;
    int lastMode = -1;         // 0 normal, 1 fast
    bool lastEmergency = false;

    while (!stop_.load()) {
        readSystemMetrics(m);

        ready.store(true);
        cpuPercent.store(m.cpuValid ? m.cpuPercent : -1.0);
        memTotal.store(m.memTotal);
        memAvailable.store(m.memAvailable);
        memUsed.store(m.memUsed);
        memPercent.store(m.memPercent);
        swapTotal.store(m.swapTotal);
        swapUsed.store(m.swapUsed);
        swapPercent.store(m.swapValid ? m.swapPercent : -1.0);

        bool fast = m.memPercent > cfg_.memoryHighThreshold * 100.0;
        fastMode.store(fast);
        bool em = m.swapValid && m.swapPercent > cfg_.swapHighThreshold * 100.0;
        emergency.store(em);

        int mode = fast ? 1 : 0;
        if (mode != lastMode) {
            std::cout << "[SysMonitor] " << (fast ? "进入高负载快周期" : "恢复常规周期")
                      << " 内存=" << pct(m.memPercent, true) << "%"
                      << " (阈值 " << pct(cfg_.memoryHighThreshold*100.0, true) << "%)" << std::endl;
            lastMode = mode;
        }
        if (em != lastEmergency) {
            std::cout << "[SysMonitor] " << (em ? "进入应急态（PIN 豁免启用）" : "退出应急态")
                      << " swap=" << pct(m.swapPercent, m.swapValid) << "%"
                      << " (阈值 " << pct(cfg_.swapHighThreshold*100.0, true) << "%)" << std::endl;
            lastEmergency = em;
        }

        std::cout << "[SysMonitor] cpu=" << pct(m.cpuPercent, m.cpuValid)
                  << "% mem=" << pct(m.memPercent, true) << "% ("
                  << bytes(m.memUsed) << "/" << bytes(m.memTotal) << ")"
                  << " swap=" << pct(m.swapPercent, m.swapValid) << "% ("
                  << bytes(m.swapUsed) << "/" << bytes(m.swapTotal) << ")" << std::endl;

        // 驱逐定时：物理 ∧ 虚拟 双满载持续 sustainSeconds
        if (cfg_.eviction.enabled) {
            bool crit = m.memPercent > cfg_.eviction.memoryCritical * 100.0
                     && m.swapValid
                     && m.swapPercent > cfg_.eviction.swapCritical * 100.0;
            if (crit) {
                if (!prevCritical) {
                    criticalSince = time(nullptr);
                    prevCritical = true;
                    std::cout << "[SysMonitor] 双满载（内存>" << pct(cfg_.eviction.memoryCritical*100.0, true)
                              << "%, swap>" << pct(cfg_.eviction.swapCritical*100.0, true)
                              << "%），开始计时驱逐" << std::endl;
                } else if (time(nullptr) - criticalSince >= cfg_.eviction.sustainSeconds) {
                    maybeEvict(m.memPercent, m.swapPercent);
                    prevCritical = false;
                    criticalSince = 0;   // 无论有无目标都重置，避免每 tick 重复扫描
                }
            } else if (prevCritical) {
                std::cout << "[SysMonitor] 双满载解除，重置驱逐计时" << std::endl;
                prevCritical = false;
                criticalSince = 0;
            }
        }

        int interval = fast ? cfg_.fastIntervalSeconds : cfg_.intervalSeconds;
        if (interval < 1) interval = 1;
        sleepSecs(interval, stop_);
    }
}

void SystemMonitor::start() {
    if (!cfg_.enabled) return;
    platform::createThread(thread_, [](void* arg) -> void* {
        static_cast<SystemMonitor*>(arg)->loop();
        return nullptr;
    }, this, 256);
}

void SystemMonitor::stop() {
    if (stop_.exchange(true)) return;
    if (platform::threadValid(thread_)) platform::joinThread(thread_);
}

// ── 应急免密判定 ────────────────────────────────────────────────────
// 剥掉前导 sudo/doas 后，命令首词命中应急名单，且当前处于应急态 → 豁免
bool sysMonEmergencyPinBypass(const std::string& rawCommand) {
    if (!g_sysMon || !g_sysMon->inEmergency()) return false;
    const auto& list = g_sysMon->config().emergencyCommands;
    // 取首词（空白拆分）
    size_t start = rawCommand.find_first_not_of(" \t");
    if (start == std::string::npos) return false;
    size_t end = rawCommand.find_first_of(" \t\r\n", start);
    std::string first = rawCommand.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // 剥前导特权包装
    if (first == "sudo" || first == "doas") {
        size_t s2 = rawCommand.find_first_not_of(" \t", end);
        if (s2 == std::string::npos) return false;
        size_t e2 = rawCommand.find_first_of(" \t\r\n", s2);
        first = rawCommand.substr(s2, e2 == std::string::npos ? std::string::npos : e2 - s2);
    }
    for (auto& c : list) {
        if (c == first) return true;
    }
    return false;
}