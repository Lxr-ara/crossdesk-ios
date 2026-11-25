set_project("crossdesk")
set_xmakever("3.1.1")
set_license("GPL-3.0-only")

local source_dir = os.getenv("CROSSDESK_SOURCE_DIR") or os.scriptdir()
local desktop_platform = is_plat("windows", "macosx", "linux") or
    (not is_plat("iphoneos", "android", "harmony") and is_host("windows", "macosx", "linux"))

add_rules("mode.release", "mode.debug")
set_languages("c++17")
set_encodings("utf-8")
if is_plat("windows") then
    -- Shared wire targets and third-party packages must use the same CRT as
    -- the native applications; a setting inside apps/cli does not reach them.
    set_runtimes("MT")
end
add_requires("nlohmann_json 3.11.3")

option("CROSSDESK_BUILD_DESKTOP")
    set_default(desktop_platform)
    set_showmenu(true)
    set_description("Build the desktop client")
option_end()

option("CROSSDESK_BUILD_CLI")
    set_default(false)
    set_showmenu(true)
    set_description("Build the terminal controller")
option_end()

local build_desktop = desktop_platform and not is_config("CROSSDESK_BUILD_DESKTOP", false)
local build_cli = has_config("CROSSDESK_BUILD_CLI")
if build_cli and not desktop_platform then
    raise("CrossDesk CLI supports Windows, macOS and Linux")
end

if desktop_platform then
    includes(path.join(source_dir, "apps/desktop/xmake/options.lua"))
    setup_options_and_dependencies(build_desktop)
end

if build_desktop then
    includes(path.join(source_dir, "apps/desktop/xmake/platform.lua"))
    includes(path.join(source_dir, "apps/desktop/xmake/rules/slint.lua"))
    includes(path.join(source_dir, "apps/desktop/xmake/rules/about_resources.lua"))
    includes(path.join(source_dir, "apps/desktop/xmake/rules/build_identity.lua"))
    includes(path.join(source_dir, "apps/desktop/xmake/targets.lua"))

    setup_platform_settings()
end

local wire_dir = path.join(source_dir, "libs/wire")

target("crossdesk_wire")
    set_kind("static")
    add_packages("nlohmann_json", {public = true})
    add_files(path.join(wire_dir, "src/*.cpp"))
    add_headerfiles(path.join(wire_dir,
        "include/(*.h)"))
    add_includedirs(path.join(wire_dir, "include"), {public = true})
target_end()

-- iOS packages this controller helper alongside the wire and MiniRTC archives.
-- Keep crypto headers out of the Objective-C++ bridge's public include paths.
if is_plat("iphoneos") then
    includes(path.join(source_dir, "deps/submodules/minirtc/thirdparty/openssl"))
    add_requires("openssl3 3.3.2", {system = false, configs = {shared = false}})
    target("crossdesk_mobile_trust")
    -- MiniRTC patches the TLS socket layout. Both static archives must use its recipe.
    includes(path.join(source_dir, "deps/submodules/minirtc/thirdparty/websocketpp"))
    includes(path.join(source_dir, "libs/controller/mobile_direct.lua"))
    add_requires("asio 1.32.0", "websocketpp 0.8.2", {system = false})
    target("crossdesk_mobile_trust")
        add_mobile_direct_connection(source_dir)
        set_kind("static")
        add_packages("nlohmann_json", "openssl3")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/src/mobile_device_trust.cpp"))
    target_end()
end

-- Photo album support: scanning, classification and thumbnails. stb_image and
-- stb_image_write are compiled STATIC inside this translation unit, so the
-- archive exports no third party symbols and can be linked into the desktop
-- client, the controller library and the tests alike.
target("crossdesk_photo")
    set_kind("static")
    set_languages("c++20")
    add_includedirs(path.join(source_dir, "libs/controller/include"), {public = true})
    add_includedirs(path.join(source_dir, "apps/desktop/src/thumbnail"), {public = true})
    add_files(path.join(source_dir, "libs/controller/src/photo_library.cpp"))
target_end()

target("crossdesk_file_browser")
    set_kind("static")
    set_languages("c++20")
    add_deps("crossdesk_wire", "crossdesk_photo")
    add_includedirs(path.join(source_dir, "libs/controller/include"), {public = true})
    add_files(path.join(source_dir, "libs/controller/src/file_browser.cpp"))
target_end()

-- CLI source packages omit tests; full checkouts retain these existing targets.
if os.isdir(path.join(wire_dir, "tests")) then
    target("crossdesk_wire_test")
        set_kind("binary")
        set_default(false)
        add_deps("crossdesk_wire")
        add_files(path.join(wire_dir, "tests/wire_test.cpp"))
    target_end()

    target("keyboard_state_wire_test")
        set_kind("binary")
        set_default(false)
        add_deps("crossdesk_wire")
        add_files(path.join(wire_dir,
            "tests/keyboard_state_wire_test.cpp"))
    target_end()

    -- 画质档位回归测试：三档表的具体数值与合法区间、video_quality_preset 的
    -- 取值编解码与 ini 落盘往返、档位作为一个原子变更集下发（恰好三次实时重
    -- 配置，拒绝时不留半档）、以及单向自动降级状态机（阈值、持续窗口、冷却、
    -- 不震荡、永不自动升档）。表与状态机都在 wire 的
    -- video_quality_preset.h 里，读 ini 用的是 ConfigCenter 同一个 SimpleIni
    -- 头文件，所以测试不需要 peer、runtime 或 socket。
    target("crossdesk_video_quality_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_includedirs(path.join(source_dir, "apps/desktop/src/config_center"))
        add_files(path.join(wire_dir, "tests/video_quality_test.cpp"))
    target_end()
end

-- Photo album regression test: scans generated images, encodes thumbnails and
-- checks the receiver side album classification.
if os.isdir(path.join(source_dir, "libs/controller/tests")) then
    target("crossdesk_photo_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_photo")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_includedirs(path.join(source_dir, "apps/desktop/src/thumbnail"))
        add_files(path.join(source_dir, "libs/controller/tests/photo_library_test.cpp"))
    target_end()

    -- File transfer pacing regression test: a fake transport plus an explicit
    -- clock observe the ByteTokenBucket rate limit and the pause switch without
    -- sleeping. The transfer core is header only, but it calls the wire
    -- encoders/decoders, so it depends on crossdesk_wire.
    target("crossdesk_transfer_rate_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_transfer_rate_test.cpp"))
    target_end()

    -- File transfer queue regression test: the header only outbound queue policy
    -- (bounded fan-out per peer, queue level pause/resume, per-item holds, retry
    -- under a fresh wire id, clear-all vs remove-finished, batch summary) driven
    -- next to real sessions, with no peer, no runtime and no socket. It calls the
    -- wire encoders/decoders, so it depends on crossdesk_wire.
    target("crossdesk_transfer_queue_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_transfer_queue_test.cpp"))
    target_end()

    -- Cross-session resume sidecar regression test: round trip, stale detection,
    -- malformed JSON, path escape rejection, idempotent clear and listing with
    -- one bad file among good ones. The resume layer is header only; it needs
    -- nlohmann_json, which arrives through crossdesk_wire's public package.
    target("crossdesk_transfer_resume_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_transfer_resume_test.cpp"))
    target_end()

    -- 跨会话续传数据面回归测试：发送端从 resume_offset 起发（本次会话的首块仍然
    -- 带文件名），接收端只在侧车的 total_size / received 都对得上时才放行
    -- offset != 0 的首块，其余情况维持 reject()，offset == 0 的老路径原样不动。
    -- 引擎是 header only，但要调用 wire 的编解码，侧车读写还要 nlohmann_json，
    -- 两者都通过 crossdesk_wire 的公开包到达。
    target("crossdesk_transfer_resume_handshake_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_transfer_resume_handshake_test.cpp"))
    target_end()

    -- 孤儿清理回归测试：暂存目录（.crossdesk-*）与续传侧车（*.crossdesk-resume）
    -- 的保留/删除判定，以及 CROSSDESK_ORPHAN_SWEEP_HOURS 的覆盖行为。被测代码就是
    -- 桌面端真正的接收适配器 apps/desktop/src/tools/file_transfer.cpp，所以它连同
    -- rd_log 一起编进来；rd_log 的公开依赖是 spdlog，wire 的编解码与 nlohmann_json
    -- 照旧通过 crossdesk_wire 到达。
    target("crossdesk_orphan_sweep_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_packages("spdlog")
        add_includedirs(path.join(source_dir, "libs/controller/include"),
            path.join(source_dir, "apps/desktop/src/tools"),
            path.join(source_dir, "apps/desktop/src/log"))
        add_files(path.join(source_dir, "libs/controller/tests/orphan_sweep_test.cpp"),
            path.join(source_dir, "apps/desktop/src/tools/file_transfer.cpp"),
            path.join(source_dir, "apps/desktop/src/log/rd_log.cpp"))
    target_end()

    -- 收端覆盖冲突回归测试：kAskSkip 必须把「被占用的目标 + 文件大小」问出来，
    -- 并按回答落盘；「全部」变体与基础动作逐字一致；另外两个策略一个钩子都不调；
    -- 没装钩子时保持本次改动之前的行为（留下引擎的时间戳名）。同样编真正的接收
    -- 适配器，所以依赖与上面那个测试一致。
    target("crossdesk_overwrite_conflict_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_packages("spdlog")
        add_includedirs(path.join(source_dir, "libs/controller/include"),
            path.join(source_dir, "apps/desktop/src/tools"),
            path.join(source_dir, "apps/desktop/src/log"))
        add_files(path.join(source_dir, "libs/controller/tests/overwrite_conflict_test.cpp"),
            path.join(source_dir, "apps/desktop/src/tools/file_transfer.cpp"),
            path.join(source_dir, "apps/desktop/src/log/rd_log.cpp"))
    target_end()

    -- 续传开启时收端必须保住收到的文件：产品的默认配置就是续传开启（侧车目录
    -- 就是接收目录），所以单块、多块、三种冲突回答，以及「断链后按记录的偏移续
    -- 传」这四条路径上，落盘结果都必须是请求的名字 + 完整字节，而且收尾之后不
    -- 留下任何 .crossdesk-* 暂存/前缀副本或 *.crossdesk-resume 侧车。同样编真正
    -- 的接收适配器，依赖与上面那个测试一致。
    target("crossdesk_resume_receive_e2e_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_wire")
        add_packages("spdlog")
        add_includedirs(path.join(source_dir, "libs/controller/include"),
            path.join(source_dir, "apps/desktop/src/tools"),
            path.join(source_dir, "apps/desktop/src/log"))
        add_files(path.join(source_dir, "libs/controller/tests/resume_receive_e2e_test.cpp"),
            path.join(source_dir, "apps/desktop/src/tools/file_transfer.cpp"),
            path.join(source_dir, "apps/desktop/src/log/rd_log.cpp"))
    target_end()

    -- Remote file management op regression test: drives mkdir, rename, delete,
    -- stat, open, reveal and temp_clear through FileBrowser::Handle, the same
    -- entry point the peer and GUI layers use, and checks the shared op-name
    -- table, the error vocabulary and the path-safety helper. ShellExecuteW is
    -- resolved at run time, so no Shell32 needs to appear on the link line.
    target("crossdesk_file_browser_ops_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_file_browser")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_browser_ops_test.cpp"))
    target_end()

    -- Cross-session resume control plane regression test: drives op=resume
    -- through FileBrowser::Handle next to temp_clear and pins the whole answer
    -- ladder (fresh record, size/stamp mismatch, a prefix that already reaches
    -- the end, malformed and foreign sidecars, traversal names and sidecars
    -- outside the receive directory). The sidecar layer is header only and
    -- arrives through crossdesk_file_browser's include path.
    target("crossdesk_file_browser_resume_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_file_browser")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_browser_resume_test.cpp"))
    target_end()

    -- 手机端传输面板回归测试：transfer_list / transfer_control /
    -- transfer_settings 走 FileBrowser::Handle（同一个 version 1 信封），用桩
    -- TransferControl 检查行与汇总的原样透传、动作词表闭合、每个动作只到宿主
    -- 一次、设置的类型/范围/取整，以及没有注入控件时的 unsupported。控制器
    -- 库不依赖 GUI，所以测试只碰注入点。
    target("crossdesk_file_browser_transfer_ops_test")
        set_kind("binary")
        set_default(false)
        set_languages("c++20")
        add_deps("crossdesk_file_browser")
        add_includedirs(path.join(source_dir, "libs/controller/include"))
        add_files(path.join(source_dir, "libs/controller/tests/file_browser_transfer_ops_test.cpp"))
    target_end()
end

if build_desktop then
    includes(path.join(source_dir, "deps/submodules"),
        path.join(source_dir, "deps/thirdparty"))
    setup_targets()
end

if build_cli then
    -- Desktop setup already includes MiniRTC and the shared OpenSSL recipe.
    if not build_desktop then
        add_requireconfs("**.openssl", "**.openssl3",
            {version = "3.3.2", system = false, override = true})
        includes(path.join(source_dir, "deps/thirdparty/openssl"))
        includes(path.join(source_dir, "deps/submodules/minirtc"))
    end
    includes(path.join(source_dir, "apps/cli/xmake.lua"))
end
