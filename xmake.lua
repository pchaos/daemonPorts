set_xmakever("2.8.0")
local GATEKEEPER_VERSION = "1.0.3"
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
    add_files("src/*.cpp")
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
    if is_plat("windows") then
        add_syslinks("ws2_32")
    end

    -- ── 编译器标志 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
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
    add_files("src/*.cpp")
    add_defines("GATEKEEPER_VERSION=\"" .. GATEKEEPER_VERSION .. "\"")
    add_includedirs("src")

    -- ── 始终启用 systemd（嵌入式 sd-daemon.h，无需链接 libsystemd）──
    add_defines("HAVE_SYSTEMD")

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
    end

    -- ── Windows (MinGW / MSVC) ──────────────────────────────
    if is_plat("windows") then
        add_syslinks("ws2_32")
    end

    -- ── 编译器标志 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_cxxflags("-Wall", "-Wextra", "-Wpedantic")
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
    add_files("test/test_main.cpp", "test/test_json.cpp", "test/test_config.cpp", "test/test_relay.cpp", "test/test_retry.cpp", "test/test_tcp_monitor.cpp", "test/test_port_group.cpp")
    add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp", "src/tcp_monitor.cpp", "src/port_group.cpp")
    add_includedirs("test", "src")

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
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