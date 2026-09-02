#include "doctest.h"
#include "config.h"
#include "system_monitor.h"

#include <string>

using namespace std;

// ── parseSystemMonitorConfig：解析与默认值 ──────────────────────────

TEST_CASE("parseSystemMonitorConfig - 完整配置") {
    parseSystemMonitorConfig(R"({
        "system_monitor": {
            "enabled": true,
            "interval_seconds": 300,
            "fast_interval_seconds": 60,
            "memory_high_threshold": 0.66,
            "swap_high_threshold": 0.5,
            "emergency_commands": ["reboot", "shutdown"],
            "eviction": {
                "enabled": true,
                "memory_critical": 0.90,
                "swap_critical": 0.85,
                "sustain_seconds": 900
            }
        },
        "ports": []
    })");
    CHECK(g_sysMonConfig.enabled == true);
    CHECK(g_sysMonConfig.intervalSeconds == 300);
    CHECK(g_sysMonConfig.fastIntervalSeconds == 60);
    // 阈值按浮点比较（宽松容差）
    CHECK(g_sysMonConfig.memoryHighThreshold == doctest::Approx(0.66));
    CHECK(g_sysMonConfig.swapHighThreshold == doctest::Approx(0.5));
    REQUIRE(g_sysMonConfig.emergencyCommands.size() == 2);
    CHECK(g_sysMonConfig.emergencyCommands[0] == "reboot");
    CHECK(g_sysMonConfig.emergencyCommands[1] == "shutdown");
    // eviction
    CHECK(g_sysMonConfig.eviction.enabled == true);
    CHECK(g_sysMonConfig.eviction.memoryCritical == doctest::Approx(0.90));
    CHECK(g_sysMonConfig.eviction.swapCritical == doctest::Approx(0.85));
    CHECK(g_sysMonConfig.eviction.sustainSeconds == 900);
}

TEST_CASE("parseSystemMonitorConfig - 缺省段用默认值") {
    parseSystemMonitorConfig(R"({"ports":[]})");
    CHECK(g_sysMonConfig.enabled == false);
    CHECK(g_sysMonConfig.intervalSeconds == 300);
    CHECK(g_sysMonConfig.fastIntervalSeconds == 60);
    CHECK(g_sysMonConfig.memoryHighThreshold == doctest::Approx(0.66));
    CHECK(g_sysMonConfig.swapHighThreshold == doctest::Approx(0.5));
    REQUIRE(g_sysMonConfig.emergencyCommands.size() == 2);
    CHECK(g_sysMonConfig.emergencyCommands[0] == "reboot");
    CHECK(g_sysMonConfig.eviction.sustainSeconds == 900);
}

TEST_CASE("parseSystemMonitorConfig - 阈值钳制") {
    parseSystemMonitorConfig(R"({
        "system_monitor": {
            "enabled": true,
            "interval_seconds": -5,
            "memory_high_threshold": 2.5,
            "swap_high_threshold": 0,
            "eviction": { "memory_critical": 2.0, "swap_critical": -1, "sustain_seconds": 0 }
        },
        "ports": []
    })");
    CHECK(g_sysMonConfig.intervalSeconds == 300);           // 非法 → 回默认
    CHECK(g_sysMonConfig.memoryHighThreshold == doctest::Approx(0.66));
    CHECK(g_sysMonConfig.swapHighThreshold == doctest::Approx(0.5));
    CHECK(g_sysMonConfig.eviction.memoryCritical == doctest::Approx(0.90));
    CHECK(g_sysMonConfig.eviction.swapCritical == doctest::Approx(0.90));
    CHECK(g_sysMonConfig.eviction.sustainSeconds == 900);
}

// ── sysMonEmergencyPinBypass：首词匹配 + sudo/doas 剥离 ──────────────

namespace {
// RAII：设置/还原全局采样器指针
struct GynMonGuard {
    SystemMonitor mon;
    explicit GynMonGuard(const SystemMonitorConfig& cfg) : mon(cfg) { g_sysMon = &mon; }
    ~GynMonGuard() { g_sysMon = nullptr; }
};
}

TEST_CASE("emergencyPinBypass - 命中规则") {
    SystemMonitorConfig cfg;
    cfg.emergencyCommands = { "reboot", "shutdown" };
    GynMonGuard g(cfg);
    g.mon.emergency = true;   // 模拟 swap 超阈值进入应急态

    CHECK(sysMonEmergencyPinBypass("reboot") == true);
    CHECK(sysMonEmergencyPinBypass("reboot --force") == true);
    CHECK(sysMonEmergencyPinBypass("shutdown -h now") == true);
    // 剥前导 sudo/doas 后命中
    CHECK(sysMonEmergencyPinBypass("sudo reboot") == true);
    CHECK(sysMonEmergencyPinBypass("sudo  reboot -f") == true);
    CHECK(sysMonEmergencyPinBypass("doas shutdown -h now") == true);
    // 非名单命令 → 不豁免
    CHECK(sysMonEmergencyPinBypass("echo reboot") == false);
    CHECK(sysMonEmergencyPinBypass("ls") == false);
    CHECK(sysMonEmergencyPinBypass("") == false);
    CHECK(sysMonEmergencyPinBypass("   ") == false);
}

TEST_CASE("emergencyPinBypass - 非应急态不豁免") {
    SystemMonitorConfig cfg;
    cfg.emergencyCommands = { "reboot" };
    GynMonGuard g(cfg);
    g.mon.emergency = false;  // swap 未超阈值
    CHECK(sysMonEmergencyPinBypass("reboot") == false);
}

TEST_CASE("emergencyPinBypass - 采样器未初始化不豁免") {
    g_sysMon = nullptr;
    CHECK(sysMonEmergencyPinBypass("reboot") == false);
    g_sysMon = nullptr;
}

// ── readSystemMetrics：本机实测不为空且范围合法 ─────────────────────
TEST_CASE("readSystemMetrics - 本机可读且范围合法") {
    SysMetrics m;
    bool ok = readSystemMetrics(m);
    CAPTURE(ok);
    CAPTURE(m.memTotal);
    if (ok) {
        CHECK(m.memTotal > 0);
        CHECK(m.memPercent >= 0.0);
        CHECK(m.memPercent <= 100.0);
    } else {
        MESSAGE("当前环境无法读取系统指标（容器/受限平台），跳过断言");
    }
}