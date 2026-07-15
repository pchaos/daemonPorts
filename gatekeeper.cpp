// gatekeeper - 多端口 TCP 接力门卫程序
//
// 通过配置文件管理多个端口，每个端口独立监听。当有人访问时，
// 自动启动对应的后端程序，透明代理所有流量。
//
// 编译: g++ -std=c++11 -O2 -o gatekeeper gatekeeper.cpp -lpthread
// 使用: ./gatekeeper config.json

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>

// ============================================================
// 简易 JSON 解析器 (处理本配置所需的子集)
// ============================================================

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;
using JsonArray = std::vector<JsonValue>;

struct JsonValue {
    enum Type { T_NULL, T_BOOL, T_NUM, T_STR, T_ARR, T_OBJ } type = T_NULL;
    bool    b = false;
    double  n = 0;
    std::string s;
    JsonArray  a;
    JsonObject o;

    bool is_obj() const { return type == T_OBJ; }
    bool is_arr() const { return type == T_ARR; }
    bool is_str() const { return type == T_STR; }

    const JsonValue* get(const std::string& key) const {
        for (auto& p : o) if (p.first == key) return &p.second;
        return nullptr;
    }
    const JsonValue* idx(size_t i) const {
        return i < a.size() ? &a[i] : nullptr;
    }
    std::string as_str() const { return s; }
    bool as_bool() const { return b; }
    double as_num() const { return n; }
};

static void skipws(const std::string& in, size_t& p) {
    while (p < in.size() && (in[p]==' '||in[p]=='\t'||in[p]=='\n'||in[p]=='\r')) p++;
}

static std::string parse_str(const std::string& in, size_t& p) {
    p++; // skip "
    std::string r;
    while (p < in.size() && in[p] != '"') {
        if (in[p] == '\\') {
            p++;
            if (p >= in.size()) break;
            switch (in[p]) {
                case '"': r += '"'; break; case '\\': r += '\\'; break;
                case '/': r += '/'; break; case 'b': r += '\b'; break;
                case 'f': r += '\f'; break; case 'n': r += '\n'; break;
                case 'r': r += '\r'; break; case 't': r += '\t'; break;
                default: r += in[p]; break;
            }
        } else r += in[p];
        p++;
    }
    if (p < in.size()) p++; // skip closing "
    return r;
}

static double parse_num(const std::string& in, size_t& p) {
    size_t start = p;
    if (in[p] == '-') p++;
    while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++;
    if (p < in.size() && in[p] == '.') { p++; while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++; }
    if (p < in.size() && (in[p]=='e'||in[p]=='E')) {
        p++; if (p < in.size() && (in[p]=='+'||in[p]=='-')) p++;
        while (p < in.size() && in[p] >= '0' && in[p] <= '9') p++;
    }
    return std::stod(in.substr(start, p-start));
}

static JsonValue parse_val(const std::string& in, size_t& p);

static JsonValue parse_obj(const std::string& in, size_t& p) {
    p++; // skip {
    JsonValue v; v.type = JsonValue::T_OBJ;
    skipws(in, p);
    if (p < in.size() && in[p] == '}') { p++; return v; }
    while (true) {
        skipws(in, p); std::string k = parse_str(in, p);
        skipws(in, p); if (p < in.size()) p++; // skip :
        v.o.push_back({k, parse_val(in, p)});
        skipws(in, p);
        if (p < in.size() && in[p] == ',') p++;
        else if (p < in.size() && in[p] == '}') { p++; break; }
    }
    return v;
}

static JsonValue parse_arr(const std::string& in, size_t& p) {
    p++; // skip [
    JsonValue v; v.type = JsonValue::T_ARR;
    skipws(in, p);
    if (p < in.size() && in[p] == ']') { p++; return v; }
    while (true) {
        v.a.push_back(parse_val(in, p));
        skipws(in, p);
        if (p < in.size() && in[p] == ',') p++;
        else if (p < in.size() && in[p] == ']') { p++; break; }
    }
    return v;
}

static JsonValue parse_val(const std::string& in, size_t& p) {
    skipws(in, p);
    if (p >= in.size()) return JsonValue();
    if (in[p] == '"') { JsonValue v; v.type = JsonValue::T_STR; v.s = parse_str(in, p); return v; }
    if (in[p] == '{') return parse_obj(in, p);
    if (in[p] == '[') return parse_arr(in, p);
    if (in.substr(p,4) == "true")  { p += 4; JsonValue v; v.type = JsonValue::T_BOOL; v.b = true; return v; }
    if (in.substr(p,5) == "false") { p += 5; JsonValue v; v.type = JsonValue::T_BOOL; v.b = false; return v; }
    if (in.substr(p,4) == "null")  { p += 4; return JsonValue(); }
    if (in[p] == '-' || (in[p] >= '0' && in[p] <= '9')) {
        JsonValue v; v.type = JsonValue::T_NUM; v.n = parse_num(in, p); return v;
    }
    return JsonValue();
}

static JsonValue parse_json(const std::string& in) {
    size_t p = 0; return parse_val(in, p);
}

// ============================================================
// 配置结构
// ============================================================

struct PortConfig {
    std::string name;
    bool        enabled = true;   // 是否启用此端口
    std::string listenAddr;       // 门卫监听地址，如 ":3000"
    std::string backendAddr;      // 后端监听地址，如 ":3001"
    std::string command;          // 启动后端的命令
    int         delayMs = 500;    // 等待后端就绪的超时(ms)
    bool        autoRestart = false; // 后端退出后自动重启
};

static std::vector<PortConfig> loadConfig(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "错误: 无法打开 " << path << std::endl;
        return {};
    }
    std::stringstream ss; ss << f.rdbuf();
    JsonValue root = parse_json(ss.str());

    if (!root.is_obj()) { std::cerr << "错误: 配置格式无效\n"; return {}; }
    auto* ports = root.get("ports");
    if (!ports || !ports->is_arr()) { std::cerr << "错误: 缺少 ports 数组\n"; return {}; }

    std::vector<PortConfig> cfgs;
    for (size_t i = 0; i < ports->a.size(); i++) {
        auto* entry = ports->idx(i);
        if (!entry || !entry->is_obj()) continue;

        auto* l = entry->get("listen");
        auto* b = entry->get("backend");
        auto* c = entry->get("command");
        if (!l || !b || !c) {
            std::cerr << "警告: 端口[" << i << "] 配置不完整，跳过\n"; continue;
        }

        PortConfig cfg;
        cfg.name = entry->get("name") ? entry->get("name")->as_str() : "";
        if (auto* e = entry->get("enabled")) cfg.enabled = e->as_bool();
        if (!cfg.enabled) {
            std::string label = cfg.name.empty() ? l->as_str() : cfg.name;
            std::cout << "  " << label << " 已禁用，跳过\n";
            continue;
        }
        cfg.listenAddr = l->as_str();
        cfg.backendAddr = b->as_str();
        cfg.command = c->as_str();
        if (auto* d = entry->get("delay")) cfg.delayMs = (int)d->as_num();
        if (auto* r = entry->get("auto_restart")) cfg.autoRestart = r->as_bool();

        cfgs.push_back(cfg);
    }
    return cfgs;
}

// ============================================================
// 工具函数
// ============================================================

static bool parseSockaddr(const std::string& addr, sockaddr_in& out) {
    auto c = addr.find(':');
    if (c == std::string::npos) return false;
    std::string host = addr.substr(0, c);
    int port = std::stoi(addr.substr(c+1));
    if (port <= 0 || port > 65535) return false;

    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") out.sin_addr.s_addr = INADDR_ANY;
    else if (host == "localhost" || host == "127.0.0.1") out.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    else inet_pton(AF_INET, host.c_str(), &out.sin_addr);
    return true;
}

// ============================================================
// PortRelay - 单个端口的接力管理
// ============================================================

class PortRelay {
    std::string name_;
    std::string listenAddr_;
    std::string backendAddr_;
    std::string command_;
    int delayMs_;
    bool autoRestart_;

    int listenFd_ = -1;
    std::atomic<bool> started_{false};
    pid_t backendPid_ = 0;
    std::atomic<bool> stop_{false};
    std::thread listenThread_;
    std::thread monitorThread_;

    int createListener() {
        sockaddr_in sa;
        if (!parseSockaddr(listenAddr_, sa)) {
            std::cerr << "  [" << name_ << "] 无效地址: " << listenAddr_ << std::endl;
            return -1;
        }
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }
        int opt = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        if (bind(fd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            perror("bind"); close(fd); return -1;
        }
        if (listen(fd, 128) < 0) { perror("listen"); close(fd); return -1; }
        return fd;
    }

    pid_t launchBackend() {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return -1; }
        if (pid == 0) {
            execl("/bin/sh", "sh", "-c", command_.c_str(), (char*)NULL);
            perror("execl");
            _exit(127);
        }
        return pid;
    }

    bool waitForBackend(int ms) {
        sockaddr_in sa;
        if (!parseSockaddr(backendAddr_, sa)) return false;
        auto start = std::chrono::steady_clock::now();
        while (true) {
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd >= 0) {
                if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) == 0) { close(fd); return true; }
                close(fd);
            }
            if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(ms)) return false;
            usleep(50000);
        }
    }

    static void pipeData(int src, int dst) {
        char buf[32768];
        ssize_t n;
        while ((n = read(src, buf, sizeof(buf))) > 0) {
            ssize_t written = 0;
            while (written < n) {
                ssize_t r = write(dst, buf+written, n-written);
                if (r <= 0) return;
                written += r;
            }
        }
    }

    void proxyConnection(int clientFd) {
        sockaddr_in sa;
        if (!parseSockaddr(backendAddr_, sa)) { close(clientFd); return; }

        int bfd = socket(AF_INET, SOCK_STREAM, 0);
        if (bfd < 0) { close(clientFd); return; }
        if (connect(bfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
            close(clientFd); close(bfd); return;
        }

        std::thread t1(pipeData, clientFd, bfd);
        std::thread t2(pipeData, bfd, clientFd);
        t1.join(); t2.join();
        close(clientFd); close(bfd);
    }

    void startBackendIfNeeded() {
        if (started_.load()) return;
        pid_t pid = launchBackend();
        if (pid < 0) return;
        backendPid_ = pid;
        started_.store(true);
        std::cout << "  [" << name_ << "] 后端已启动 (PID=" << pid << ")" << std::endl;

        if (!waitForBackend(delayMs_))
            std::cerr << "  [" << name_ << "] 警告: 后端可能未就绪" << std::endl;
    }

    void listenLoop() {
        listenFd_ = createListener();
        if (listenFd_ < 0) return;

        while (!stop_.load()) {
            sockaddr_in cli;
            socklen_t len = sizeof(cli);
            int fd = accept(listenFd_, (struct sockaddr*)&cli, &len);
            if (fd < 0) { if (errno == EINTR) continue; if (stop_.load()) break; break; }

            startBackendIfNeeded();
            std::thread(&PortRelay::proxyConnection, this, fd).detach();
        }
        close(listenFd_);
    }

    void monitorBackend() {
        while (!stop_.load()) {
            if (backendPid_ > 0) {
                int status;
                waitpid(backendPid_, &status, 0);
                std::cout << "  [" << name_ << "] 后端已退出" << std::endl;
                backendPid_ = 0;
                started_.store(false);

                if (autoRestart_) {
                    std::cout << "  [" << name_ << "] 将在下次连接时重启" << std::endl;
                } else {
                    break;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

public:
    PortRelay(const PortConfig& cfg)
        : name_(cfg.name.empty() ? cfg.listenAddr : cfg.name)
        , listenAddr_(cfg.listenAddr)
        , backendAddr_(cfg.backendAddr)
        , command_(cfg.command)
        , delayMs_(cfg.delayMs)
        , autoRestart_(cfg.autoRestart) {}

    void start() {
        listenThread_ = std::thread(&PortRelay::listenLoop, this);
        monitorThread_ = std::thread(&PortRelay::monitorBackend, this);
    }

    void stop() {
        stop_.store(true);
        if (listenFd_ >= 0) { int fd = listenFd_; listenFd_ = -1; ::close(fd); }
        if (listenThread_.joinable()) listenThread_.join();
        if (monitorThread_.joinable()) monitorThread_.join();
    }

    const std::string& name() const { return name_; }
};

// ============================================================
// 信号处理 + 主函数
// ============================================================

static std::vector<std::unique_ptr<PortRelay>> g_relays;
static std::atomic<bool> g_stop{false};

static void handleSignal(int) {
    g_stop.store(true);
    for (auto& r : g_relays) r->stop();
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " config.json" << std::endl;
        return 1;
    }

    auto cfgs = loadConfig(argv[1]);
    if (cfgs.empty()) {
        std::cerr << "错误: 没有有效的端口配置" << std::endl;
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  handleSignal);
    signal(SIGTERM, handleSignal);

    std::cout << "门卫程序启动，管理 " << cfgs.size() << " 个端口:" << std::endl;
    for (auto& c : cfgs) {
        std::cout << "  " << c.listenAddr << " -> \"" << c.command << "\""
                  << " (backend=" << c.backendAddr << ")" << std::endl;
        auto relay = std::unique_ptr<PortRelay>(new PortRelay(c));
        relay->start();
        g_relays.push_back(std::move(relay));
    }

    pause();

    std::cout << "门卫程序退出" << std::endl;
    return 0;
}
