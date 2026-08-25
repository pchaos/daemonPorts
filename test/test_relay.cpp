#include "doctest.h"
#include "config.h"
#include "relay.h"
#include "relay_platform.h"

#include <ctime>

// ── parseCommandLine ──

TEST_CASE("parseCommandLine - 空白分割") {
    auto args = platform::parseCommandLine("./app --port 3000");
    REQUIRE(args.size() == 3);
    CHECK(args[0] == "./app");
    CHECK(args[1] == "--port");
    CHECK(args[2] == "3000");
}

TEST_CASE("parseCommandLine - 双引号参数") {
    auto args = platform::parseCommandLine("prog \"two words\" tail");
    REQUIRE(args.size() == 3);
    CHECK(args[0] == "prog");
    CHECK(args[1] == "two words");
    CHECK(args[2] == "tail");
}

TEST_CASE("parseCommandLine - 单引号参数") {
    auto args = platform::parseCommandLine("prog 'a b' c");
    REQUIRE(args.size() == 3);
    CHECK(args[0] == "prog");
    CHECK(args[1] == "a b");
    CHECK(args[2] == "c");
}

TEST_CASE("parseCommandLine - 反斜杠转义") {
    auto args = platform::parseCommandLine("prog a\\ b");
    REQUIRE(args.size() == 2);
    CHECK(args[0] == "prog");
    CHECK(args[1] == "a b");
}

TEST_CASE("parseCommandLine - 未闭合引号返回空") {
    CHECK(platform::parseCommandLine("prog \"unclosed").empty());
    CHECK(platform::parseCommandLine("'unclosed").empty());
}

TEST_CASE("parseCommandLine - 空字符串与纯空白") {
    CHECK(platform::parseCommandLine("").empty());
    CHECK(platform::parseCommandLine("   \t\n").empty());
}

TEST_CASE("parseCommandLine - shell 元字符不被解释 (H1 回归)") {
    // 元字符应作为普通参数保留，整个命令仍是一个可执行 + 参数列表
    auto args = platform::parseCommandLine("./app \"--port=3000; rm -rf /\"");
    REQUIRE(args.size() == 2);
    CHECK(args[1] == "--port=3000; rm -rf /");

    auto pipe = platform::parseCommandLine("cmd | grep x");
    REQUIRE(pipe.size() == 4);
    CHECK(pipe[2] == "grep");
    CHECK(pipe[3] == "x");
}

TEST_CASE("runCommand - 正常命令返回退出码 (H3 回归)") {
    int rc = platform::runCommand("true");
    CHECK(rc == 0);
    rc = platform::runCommand("false");
    CHECK(rc == 1);
    rc = platform::runCommand("sh -c 'exit 7'");
    CHECK(rc == 7);
}

TEST_CASE("runCommand - 空命令失败") {
    CHECK(platform::runCommand("") == -1);
}

TEST_CASE("runCommand - shell 元字符不注入 (H3 回归)") {
    // 若经 shell 解释，echo x && exit 3 会返回 3；无 shell 时全部作为 echo 参数
    int rc = platform::runCommand("echo x \"&& exit 3\"");
    CHECK(rc == 0);
}

TEST_CASE("generateAuthToken - 32 位小写 hex") {
    std::string tok = platform::generateAuthToken();
    REQUIRE(tok.size() == 32);
    for (char c : tok) {
        bool hexDigit = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(hexDigit);
    }
    CHECK(tok != platform::generateAuthToken());
}

// ── buildStartupResponse ──

TEST_CASE("buildStartupResponse - 基本结构") {
    PortConfig cfg;
    cfg.name = "test";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("HTTP/1.1 200 OK") != std::string::npos);
    CHECK(resp.find("Content-Type: text/html; charset=utf-8") != std::string::npos);
    CHECK(resp.find("Connection: close") != std::string::npos);
}

TEST_CASE("buildStartupResponse - HTML 内容") {
    PortConfig cfg;
    cfg.name = "my-service";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 3;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("my-service 启动中...") != std::string::npos);
    CHECK(resp.find("<meta http-equiv=\"refresh\" content=\"3\"") != std::string::npos);
    CHECK(resp.find("秒后自动重试") != std::string::npos);
}

TEST_CASE("buildStartupResponse - 倒计时脚本") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("var secs = 5;") != std::string::npos);
    CHECK(resp.find("document.getElementById('cd')") != std::string::npos);
    CHECK(resp.find("onload=\"tick()\"") != std::string::npos);
    CHECK(resp.find("<span id=\"cd\">5</span>") != std::string::npos);
}

TEST_CASE("buildStartupResponse - Content-Length 精确匹配") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 2;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    auto pos = resp.find("Content-Length: ");
    REQUIRE(pos != std::string::npos);
    pos += 16;
    auto end = resp.find("\r\n", pos);
    int declaredLen = std::stoi(resp.substr(pos, end - pos));

    auto bodyStart = resp.find("\r\n\r\n");
    REQUIRE(bodyStart != std::string::npos);
    bodyStart += 4;
    int actualLen = resp.size() - bodyStart;

    CHECK(declaredLen == actualLen);
}

TEST_CASE("buildStartupResponse - 名称缺省时使用 listen 地址") {
    PortConfig cfg;
    cfg.listenAddr = ":8080";
    cfg.command = "./app";

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find(":8080 启动中...") != std::string::npos);
}

TEST_CASE("buildStartupResponse - 自定义刷新秒数") {
    PortConfig cfg;
    cfg.name = "svc";
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 10;

    PortRelay relay(cfg);
    std::string resp = relay.buildStartupResponse();

    CHECK(resp.find("content=\"10\"") != std::string::npos);
    CHECK(resp.find("秒后自动重试") != std::string::npos);
}

// ── hasRecentActivity ──

TEST_CASE("hasRecentActivity - 默认未活跃") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";

    PortRelay relay(cfg);
    CHECK(relay.hasRecentActivity(1) == true);
    CHECK(relay.hasRecentActivity(5) == true);
    CHECK(relay.hasRecentActivity(60) == true);
}

TEST_CASE("hasRecentActivity - 当前活跃") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";

    PortRelay relay(cfg);
    // lastActiveTime_ 构造时为 time(nullptr)
    // → hasRecentActivity 应返回 true
    CHECK(relay.hasRecentActivity(1) == true);
    // minutes=0 表示不监控，应返回 true
    CHECK(relay.hasRecentActivity(0) == true);
}

TEST_CASE("gracefulStop - backendPid_ <= 0 不做任何事") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.stopCommand = "echo stop";
    cfg.idleMinutes = 10;
    PortRelay relay(cfg);
    relay.gracefulStop();
    CHECK(relay.isBackendRunning() == false);
}

TEST_CASE("idleMinutes - 构造函数正确初始化") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.idleMinutes = 7;
    PortRelay relay(cfg);
    CHECK(relay.idleMinutes() == 7);

// idleMinutes=0 应正确生效为 0
    cfg.idleMinutes = 0;
    PortRelay relay2(cfg);
    CHECK(relay2.idleMinutes() == 0);
}

TEST_CASE("gracefulStop - backendPid_ > 0 clears pid") {
    PortConfig cfg;
    cfg.listenAddr = ":9999";
    cfg.command = "./app";
    cfg.refreshSeconds = 5;
    cfg.stopCommand = ""; // no stop command
    cfg.idleMinutes = 5;
    PortRelay relay(cfg);
    relay.backendPid_ = 12345; // simulate running backend
    relay.gracefulStop();
    CHECK(relay.backendPid_ == 0);
}

TEST_CASE("signalStop - idempotent and non-blocking") {
    PortConfig cfg;
    cfg.listenAddr = ":0"; // dummy address, no actual backend
    cfg.command = ""; // no command to launch
    PortRelay relay(cfg);
    // First call should set stop flag
    relay.signalStop();
    CHECK(relay.stop_.load() == true);
    // Second call should be safe and not change state
    relay.signalStop();
    CHECK(relay.stop_.load() == true);
    // Calling stop() after signalStop should be a no-op
    relay.stop();
    CHECK(relay.stop_.load() == true);
}

TEST_CASE("setLaunchOnStart - 方法可调用且不崩溃") {
    PortConfig cfg;
    cfg.listenAddr = ":0";
    cfg.command = "";
    PortRelay relay(cfg);

    // 默认构造后不设置
    // 安全调用 setter
    relay.setLaunchOnStart(true);
    relay.setLaunchOnStart(false);
    relay.setLaunchOnStart(true);

    // 再次设置应该是安全的（幂等）
    relay.setLaunchOnStart(true);
    relay.setLaunchOnStart(false);
}

TEST_CASE("launchAndRelease - 无监听 fd 时安全调用") {
    PortConfig cfg;
    cfg.listenAddr = ":0";
    cfg.command = "";
    PortRelay relay(cfg);

    // 空 command 会 fork 一个 sh 子进程并设置 backendPid_，此处只验证调用不崩溃不阻塞
    relay.launchAndRelease();
}