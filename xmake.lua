set_xmakever("2.8.0")
local GATEKEEPER_VERSION = "1.1.14"
set_version(GATEKEEPER_VERSION)

-- ============================================================
-- xmake.lua - daemonPorts 多端口 TCP 接力门卫
-- 支持多平台(linux/macosx/windows) + 多CPU架构
-- ============================================================

add_rules("mode.debug", "mode.release")

-- ── systemd 集成选项 ───────────────────────────
option("HAVE_SYSTEMD")
    set_default(false)
    add_defines("HAVE_SYSTEMD")
    set_description("启用 systemd sd_notify 支持（编译 -DHAVE_SYSTEMD，链接 libsystemd）")
    set_category("features")

target("gatekeeper")
    set_kind("binary")
    set_languages("c++11")
    if is_plat("windows", "mingw") then
        add_files("src/relay_win.cpp")
        add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp", "src/main.cpp", "src/control_server.cpp", "src/system_monitor.cpp")
    else
        add_files("src/relay_posix.cpp")
        add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp", "src/main.cpp", "src/control_server.cpp", "src/system_monitor.cpp")
        if is_plat("linux", "android", "cross") then
            add_files("src/relay_linux.cpp")
        end
    end
    add_defines("GATEKEEPER_VERSION=\"" .. GATEKEEPER_VERSION .. "\"")
    add_includedirs("src")

    -- ── systemd 集成（需要 -DHAVE_SYSTEMD + -lsystemd）─────
    if has_config("HAVE_SYSTEMD") then
        add_defines("HAVE_SYSTEMD")
        if is_plat("linux") then
            add_syslinks("systemd")
        end
    end

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
    end

    -- ── Windows (MinGW / MSVC) ──────────────────────────────
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt")
    end

    -- ── 编译器标志 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    end

    -- ── 交叉编译：裁剪未引用的静态库死代码（交叉工具链无共享 libstdc++）────────
    if is_plat("cross") then
        add_cxxflags("-ffunction-sections", "-fdata-sections")
        add_ldflags("-Wl,--gc-sections")
    end

    -- ── 发布模式 ────────────────────────────────────────────
    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("hidden")
        set_strip("all")
    end

    -- ── 调试模式 ────────────────────────────────────────────
    if is_mode("debug") then
        set_symbols("debug")
        set_optimize("none")
    end

    -- ── 安装到 build/<平台>/<架构>/ ─────────────────────────
    after_build(function(target)
        import("core.project.config")
        local plat  = config.get("plat") or "native"
        local arch  = config.get("arch") or os.arch()
        local output_dir = path.join(os.projectdir(), "build", plat, arch)
        os.mkdir(output_dir)
        os.cp(target:targetfile(), path.join(output_dir, target:filename()))
        print("✅ 构建完成: " .. path.join(output_dir, target:filename()))
    end)

-- ── 带 systemd 的版本（始终链接 libsystemd，输出 gatekeeper-systemd）──
target("gatekeeper-systemd")
    set_kind("binary")
    set_languages("c++11")
    if is_plat("windows", "mingw") then
        add_files("src/relay_win.cpp")
        add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp", "src/main.cpp", "src/control_server.cpp", "src/system_monitor.cpp")
    else
        add_files("src/relay_posix.cpp")
        add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp", "src/main.cpp", "src/control_server.cpp", "src/system_monitor.cpp")
        if is_plat("linux", "android", "cross") then
            add_files("src/relay_linux.cpp")
        end
    end
    add_defines("GATEKEEPER_VERSION=\"" .. GATEKEEPER_VERSION .. "\"")
    add_includedirs("src")

    -- ── 始终启用 systemd（嵌入式 sd-daemon.h，无需链接 libsystemd）──
    if not is_plat("windows", "mingw", "android") then
        add_defines("HAVE_SYSTEMD")
    end

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
    end

    -- ── Windows (MinGW / MSVC) ──────────────────────────────
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt")
    end

    -- ── 编译器标志 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
    end

    if is_plat("cross") then
        add_cxxflags("-ffunction-sections", "-fdata-sections")
        add_ldflags("-Wl,--gc-sections")
    end

    -- ── 发布模式 ────────────────────────────────────────────
    if is_mode("release") then
        set_optimize("fastest")
        set_symbols("hidden")
        set_strip("all")
    end

    -- ── 调试模式 ────────────────────────────────────────────
    if is_mode("debug") then
        set_symbols("debug")
        set_optimize("none")
    end

    -- ── 安装到 build/<平台>/<架构>/ ─────────────────────────
    after_build(function(target)
        import("core.project.config")
        local plat  = config.get("plat") or "native"
        local arch  = config.get("arch") or os.arch()
        local output_dir = path.join(os.projectdir(), "build", plat, arch)
        os.mkdir(output_dir)
        os.cp(target:targetfile(), path.join(output_dir, target:filename()))
        print("✅ 构建完成: " .. path.join(output_dir, target:filename()))
    end)

target("test-gatekeeper")
    set_kind("binary")
    set_languages("c++11")
    add_files("test/test_main.cpp", "test/test_json.cpp", "test/test_config.cpp", "test/test_relay.cpp", "test/test_retry.cpp", "test/test_tcp_monitor.cpp", "test/test_port_group.cpp", "test/test_control_server.cpp", "test/test_stubs.cpp")
    add_files("test/test_run_command.cpp")
    add_files("test/test_sysmon.cpp")
    if is_plat("windows", "mingw") then
        add_files("src/relay_win.cpp")
    else
        add_files("src/relay_posix.cpp")
        if is_plat("linux", "android") then
            add_files("src/relay_linux.cpp")
        end
    end
    add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp", "src/control_server.cpp", "src/system_monitor.cpp")
    add_includedirs("test", "src")

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
    end

    -- ── Windows (MinGW / MSVC) ──────────────────────────────
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "bcrypt")
    end

    -- ── systemd 集成 ──────────────────────────────────────────
    if has_config("HAVE_SYSTEMD") then
        add_defines("HAVE_SYSTEMD")
        if is_plat("linux") then
            add_syslinks("systemd")
        end
    end

    -- ── 仅用于调试模式 ──────────────────────────────────────
    set_default(mode == "debug")

    -- ── 运行测试 ────────────────────────────────────────────
    after_build(function(target)
        import("core.project.config")
        if config.get("mode") == "debug" then
            os.exec(target:targetfile())
        end
    end)

target("smoke-n45")
    -- N4/N5 冒烟: 流式 /run 并发互不干扰 + 超时连组杀孙进程
    set_kind("binary")
    set_languages("c++11")
    add_files("scripts/smoke-n45.cpp")
    if is_plat("windows", "mingw") then
        add_files("src/relay_win.cpp")
    else
        add_files("src/relay_posix.cpp")
    end
    add_includedirs("src")
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
    end
    -- 仅显式构建, 不进默认构建
    set_default(false)
    on_run(function(target)
        os.exec(target:targetfile())
    end)

task("smoke")
    -- 一键跑全部冒烟测试 (N1/N3/N4/N5)
    on_run(function()
        import("core.project.config")
        local plat = config.get("plat") or "native"
        local arch = config.get("arch") or os.arch()
        local mode = config.get("mode") or "release"
        local root = os.projectdir()

        -- 主二进制 (smoke-n1.py / smoke-n3.py 需要)
        local candidates = {
            -- after_build 输出格式: build/<plat>/<arch>/gatekeeper
            path.join(root, "build", plat, arch, "gatekeeper"),
            path.join(root, "build", "linux", arch, "gatekeeper"),
            path.join(root, "build", plat, arch, mode, "gatekeeper"),
        }
        local bin
        for _, c in ipairs(candidates) do
            if os.isfile(c) then bin = c; break end
        end
        if not bin then
            print("未找到 gatekeeper 二进制, 请先构建: xmake f -m release && xmake")
            os.exit(1)
        end
        local bin_mtime = os.mtime(bin)
        local stale = false
        for _, s in ipairs(os.files(path.join(root, "src", "*.cpp"))) do
            if os.mtime(s) > bin_mtime then stale = true; break end
        end
        if stale then
            print("警告: " .. bin .. " 比部分 src/*.cpp 旧, 可能是修复前的二进制!")
            print("请先重新构建本机版本: xmake f -p linux -a " .. arch .. " -m release -y && xmake build gatekeeper")
            os.exit(1)
        end
        print("使用二进制: " .. bin)

        os.exec("python3 '" .. path.join(root, "scripts", "smoke-n1.py") .. "' '" .. bin .. "'")
        os.exec("python3 '" .. path.join(root, "scripts", "smoke-n3.py") .. "' '" .. bin .. "'")

        -- smoke-n45: 用本机工具链直接编译运行, 绕过当前平台配置
        -- (cross/arm64 配置下 xmake build 会产出不可运行的 arm64 二进制)
        local n45_src = path.join(root, "scripts", "smoke-n45.cpp")
        local n45_relay = path.join(root, "src", "relay_posix.cpp")
        local n45_bin = path.join(root, "build", "linux", arch, "release", "smoke-n45")
        os.mkdir(path.directory(n45_bin))
        local cxx = os.getenv("CXX") or "g++"
        local ok = os.execv(cxx, {
            "-std=c++11", "-O1", "-pthread",
            "-I" .. path.join(root, "src"),
            n45_src, n45_relay, "-o", n45_bin,
        })
        if not ok then
            print("编译 smoke-n45 失败")
            os.exit(1)
        end
        os.exec(n45_bin)
    end)
    set_menu {
        usage = "xmake smoke",
        description = "运行全部安全修复冒烟测试 (N1/N3/N4/N5)"
    }

-- ── 自定义配置提示 ──────────────────────────────────────────
task("show-config")
    on_run(function()
        import("core.project.config")
        print(string.format("平台: %s  架构: %s  模式: %s",
            config.get("plat") or "auto",
            config.get("arch") or "auto",
            config.get("mode") or "release"))
    end)
    set_menu {
        usage = "xmake show-config",
        description = "显示当前 xmake 编译配置"
    }