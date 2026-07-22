#!/usr/bin/env python3
"""离线核对专辑、会话随机、输入映射与配置文档的一致性。"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    """断言一个离线检查条件，并用统一中文格式输出结果。

    参数：
        condition: 条件为真表示检查通过。
        message: 面向开发者的检查项目说明。

    失败时抛出 AssertionError，由文件末尾的统一入口转换为退出码 1。
    """
    if not condition:
        raise AssertionError(message)
    print(f"[通过] {message}")


def main() -> int:
    """读取源码与文档并执行不依赖 Windows SDK 的发行一致性检查。

    本方法只做静态文本断言，不模拟游戏、FMOD 或 Win32 窗口。返回 0 表示
    所有发行约束均满足；任一 require 失败会提前中止并由入口返回 1。
    """
    config_cpp = (ROOT / "src/Config.cpp").read_text(encoding="utf-8")
    config_h = (ROOT / "src/Config.h").read_text(encoding="utf-8")
    local_cpp = (ROOT / "src/LocalPlayer.cpp").read_text(encoding="utf-8")
    controls_cpp = (ROOT / "src/AlbumControls.cpp").read_text(encoding="utf-8")
    game_hooks_cpp = (ROOT / "src/GameHooks.cpp").read_text(encoding="utf-8")
    metadata_cpp = (ROOT / "src/Metadata.cpp").read_text(encoding="utf-8")
    overlay_cpp = (ROOT / "src/NowPlayingOverlay.cpp").read_text(encoding="utf-8")
    controls_h = (ROOT / "src/AlbumControls.h").read_text(encoding="utf-8")
    overlay_h = (ROOT / "src/NowPlayingOverlay.h").read_text(encoding="utf-8")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    readme = (ROOT / "README.md").read_text(encoding="utf-8")
    ini_guide = (ROOT / "INI配置说明.md").read_text(encoding="utf-8")

    external = (ROOT / "dsound.ini").read_text(encoding="utf-8-sig")
    require("BuildDefaultConfigText" in (ROOT / "src/Localization.cpp").read_text(encoding="utf-8-sig"),
            "首次运行配置由本地化模板生成器提供")

    visible_keys = [
        "Language", "EnableLogging", "MusicPath", "PlayMode", "Volume",
        "ShowNowPlayingNotification", "ShowPauseNotification", "ShowResumeNotification",
        "NotificationDurationMs", "NotificationScalePercent", "NotificationOpacityPercent",
        "NotificationPosition", "NotificationMarginX", "NotificationMarginY",
    ]
    keys_by_section: dict[str, list[str]] = {}
    section = ""
    for line in external.splitlines():
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            section = stripped[1:-1]
            keys_by_section.setdefault(section, [])
        elif section and stripped and not stripped.startswith(";") and "=" in stripped:
            keys_by_section[section].append(stripped.split("=", 1)[0])
    require(keys_by_section.get("LocalMusic") == visible_keys,
            "发行版普通 INI 仅包含 14 个用户设置且顺序稳定")
    require("Compatibility" not in keys_by_section,
            "普通 INI 不再暴露兼容性和 Hook 开发参数")
    for key in visible_keys:
        require(f"`{key}`" in ini_guide, f"中文 INI 说明包含 {key}")

    advanced = (ROOT / "开发者/高级配置参考.ini").read_text(encoding="utf-8")
    for key in ("EnableFolderAlbums", "EnableSidecarMetadata", "AutoAdvance",
                "PreloadNextTrack", "BypassSpotifyAuthentication", "StrictVersion"):
        require(f"{key}=" in advanced, f"开发者高级参考保留 {key}")
    require("SuppressNavigationKeySkips=false" in advanced,
            "高级参考将不可靠的菜单导航抑制默认关闭")
    require("suppress_navigation_key_skips = false" in config_h and
            'L"SuppressNavigationKeySkips", false' in config_cpp,
            "源码发行默认关闭实验性菜单导航抑制")
    require("package-release.bat" in readme and (ROOT / "package-release.bat").exists(),
            "README 与源码包含自动发行打包脚本")
    require("bool enable_logging = true" in config_h and
            'L"EnableLogging", true' in config_cpp,
            "源码包含默认开启的日志总开关")
    dllmain = (ROOT / "src/dllmain.cpp").read_text(encoding="utf-8")
    require("ReadLoggingEnabled(ini_path)" in dllmain and
            dllmain.index("ReadLoggingEnabled(ini_path)") < dllmain.index("log::Initialize(log_path)"),
            "日志总开关在 Logger::Initialize 前预读")
    require("if (logging_enabled)" in dllmain,
            "关闭日志时跳过日志文件初始化")

    require("std::shuffle(play_queue_.begin(), play_queue_.end(), random_)" in local_cpp,
            "随机模式生成固定会话乱序队列")
    require("return play_queue_[(queue_position_ + 1) % play_queue_.size()]" in local_cpp,
            "下一曲沿队列游标移动")
    require("queue_position_ == 0 ? play_queue_.size() - 1" in local_cpp,
            "上一曲可沿固定队列真实回退")
    require("WriteIniString(config_.ini_path, L\"LocalMusic\", L\"PlayMode\"" in local_cpp,
            "播放模式立即写回同名 dsound.ini")
    require("left.path.stem().wstring()" in local_cpp and
            "right.path.stem().wstring()" in local_cpp,
            "专辑内曲目只按主文件名自然排序")
    require("SortableNumber" not in local_cpp and
            "left.metadata.disc_number" not in local_cpp and
            "left.metadata.track_number" not in local_cpp,
            "碟号与曲号元数据不再参与播放顺序")
    require("std::filesystem::relative(left_dir, config_.music_path" in local_cpp and
            "NaturalTextLess(left_name, right_name)" in local_cpp,
            "专辑按相对文件夹路径自然排序")

    for key in ("VK_INSERT", "VK_PRIOR", "VK_HOME"):
        require(key in controls_cpp, f"固定键盘映射包含 {key}")
    require("kLeftShoulder" in controls_cpp and "kDpadDown" in controls_cpp,
            "固定手柄映射使用 LB+十字键")
    require("enum class DirectionMode" in controls_h and
            "DirectionMode::Plain" in controls_cpp and
            "DirectionMode::LeftShoulder" in controls_cpp and
            "DirectionMode::BackLeftShoulder" in controls_cpp and
            "previous_normal_combo" not in controls_cpp,
            "方向按住周期锁定为 Plain/LB/BACK/BACK+LB 单一模式")
    require("ShouldBlockGameMediaCommand" in controls_cpp and
            "kMediaReleaseGuardMs = 750" in controls_cpp and
            "raw_combo" in controls_cpp and
            "media_capture_mask_" in controls_h,
            "组合键媒体硬门不依赖游戏 XInput 槽位扫描")
    require(game_hooks_cpp.count("AlbumControls::ShouldBlockGameMediaCommand()") == 5 and
            all(name in game_hooks_cpp for name in ("HookNativeNextTrack", "HookNativePreviousTrack",
                                                    "HookNativePause", "HookNativePlay",
                                                    "HookNativeUnpause")),
            "上一曲、下一曲、暂停、播放和恢复统一经过组合键硬门")
    require("current_directions & ~previous_directions" in controls_cpp and
            "state.direction_modes[index] = DirectionMode::Plain" in controls_cpp,
            "插件组合只在方向键按下沿决定，后按修饰键不会补触发")
    for token in ("kBack", "kRightThumb", "VK_BACK", "VK_OEM_MINUS", "VK_OEM_PLUS"):
        require(token in controls_cpp, f"布局输入实现包含 {token}")
    require("AdjustLayout(0, 0, 0, -kOpacityStepPercent)" in controls_cpp and
            "AdjustLayout(0, 0, kScaleStepPercent, 0)" in controls_cpp,
            "手柄 BACK+LB 分别调整透明度和缩放")
    require("ResetLayout()" in controls_cpp and "SaveLayout()" in controls_cpp,
            "输入层支持布局重置和持久化")
    require("kKeyboardSaveDelayMs = 1000" in controls_cpp,
            "键盘布局调整停止 1 秒后才保存")
    for token in ("InstallGameInputCapture", "RefreshGameInputCapture",
                  "HookedXInputGetState", "HookedXInputGetStateEx",
                  "HookedXInputGetKeystroke", "FilterGameInput", "FilterGameKeystroke",
                  "kLayoutCaptureMask", "IMAGE_DIRECTORY_ENTRY_IMPORT",
                  "CreateToolhelp32Snapshot", "PatchModuleCachedPointers"):
        require(token in controls_cpp, f"游戏侧 XInput 多路径捕获包含 {token}")
    require("ordinal == 100" in controls_cpp and "ordinal == 7" in controls_cpp,
            "输入捕获覆盖 XInputGetStateEx 与 XInputGetKeystroke 序号入口")
    require("kLayoutCaptureMask = kDirectionMask | kLeftShoulder | kRightThumb" in controls_cpp,
            "BACK 布局模式完整屏蔽游戏侧十字键、LB 和 R3")
    require("if (!back_down && (raw_buttons & kLayoutCaptureMask) == 0)" in controls_cpp,
            "BACK 松开后残留捕获键继续屏蔽到全部释放")
    require("keyboard_last_adjust_ms_" in controls_h and "layout_dirty" in controls_h,
            "输入状态保留键盘延迟保存与手柄未保存标记")
    require("void AdjustLayout" in overlay_h and "void SaveLayout" in overlay_h and
            "void ResetLayout" in overlay_h,
            "通知层公开布局调整、保存和重置接口")
    require("kMinimumVisiblePixels = 48" in overlay_cpp and
            "notification_opacity_percent" in overlay_cpp,
            "通知绘制包含可见区域保护和实时透明度")
    require("WritePrivateProfileStringW" in overlay_cpp and
            'L"NotificationMarginX"' in overlay_cpp and
            'L"NotificationOpacityPercent"' in overlay_cpp,
            "布局保存只更新相关 INI 键")

    require("path.stem().wstring() + L\".ini\"" in metadata_cpp,
            "曲目侧挂 INI 与音频使用相同主文件名")
    require("FindImageWithStem(path.parent_path(), path.stem().wstring())" in metadata_cpp,
            "曲目外部封面与音频使用相同主文件名")
    require("directory / L\"album.ini\"" in metadata_cpp,
            "文件夹专辑读取 album.ini")
    require("ReadEmbeddedArtwork(path)" in metadata_cpp and
            "metadata.track_cover_path" in metadata_cpp and
            "metadata.album_cover_path" in metadata_cpp,
            "封面优先级包含内嵌、同名图片与专辑封面")

    require("PlaybackNoticeKind::AlbumChanged" in overlay_cpp and
            "PlaybackNoticeKind::PlayModeChanged" in overlay_cpp,
            "通知层支持专辑切换和播放模式提示")
    require("src/AlbumControls.cpp" in cmake, "CMake 已编入固定输入模块")
    require(("folders as albums" in readme.lower() or "folders are albums" in readme.lower() or "folders are treated as albums" in readme.lower()) and "不读取 ZIP" in readme,
            "中英双语 README 明确文件夹专辑并排除 ZIP")
    require("## 简体中文" in readme and "## English" in readme,
            "README 采用完整中文后追加英文结构")
    require("## 简体中文" in ini_guide and "## English" in ini_guide,
            "INI 说明采用完整中文后追加英文结构")
    require("R3`" in readme and "Backspace" in readme and
            "plain `-`" in readme.lower() and "透明度" in readme,
            "README 记录手柄/键盘重置与透明度调整")
    require("PlayMode play_mode = PlayMode::Sequential" in config_h,
            "结构体默认播放模式为顺序播放")

    print("\n全部专辑与会话随机一致性检查通过。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        raise SystemExit(1)
