set_xmakever("2.8.0")

-- ============================================================
-- xmake.lua - daemonPorts 多端口 TCP 接力门卫
-- 支持多平台(linux/macosx/windows) + 多CPU架构
-- ============================================================

add_rules("mode.debug", "mode.release")

target("gatekeeper")
    set_kind("binary")
    set_languages("c++11")
    add_files("src/*.cpp")
    add_includedirs("src")

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
    add_files("test/test_main.cpp", "test/test_json.cpp", "test/test_config.cpp", "test/test_relay.cpp")
    add_files("src/json.cpp", "src/config.cpp", "src/relay.cpp")
    add_includedirs("test", "src")

    -- ── POSIX 平台 ──────────────────────────────────────────
    if is_plat("linux", "macosx", "bsd") then
        add_syslinks("pthread")
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