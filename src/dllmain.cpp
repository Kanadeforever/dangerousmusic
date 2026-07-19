// DLL 入口与工作线程：初始化日志、配置、播放器和游戏 Hook，并轮询输入及 FMOD 更新。
// DllMain 中只创建线程/发送停止信号，避免在加载器锁内执行复杂逻辑。
#include "Common.h"
#include "AlbumControls.h"
#include "Config.h"
#include "DpiAwareness.h"
#include "GameHooks.h"
#include "LocalPlayer.h"
#include "Logger.h"
#include "Localization.h"
#include "NowPlayingOverlay.h"

namespace localmusic {
namespace {

HMODULE g_module = nullptr;
std::atomic<bool> g_stop{false};
HANDLE g_worker = nullptr;

// 读取 Win32 虚拟键的按下沿，用于可选的全局媒体键与 F5 重扫。
// 该辅助只消费低位边沿，不用于需要长按重复的布局控制。
bool Pressed(int virtual_key) {
    return (GetAsyncKeyState(virtual_key) & 1) != 0;
}

// 检测实验性菜单横向输入来源，并向播放器记录短时抑制窗口。
// 发行默认关闭该实验功能，因为游戏 UI 可能延迟到下一帧才调用换曲。
bool HorizontalNavigationInputActive() {
    // VK_GAMEPAD_* 是固定的 Windows 虚拟键值。这里使用数字常量，
    // 以兼容尚未定义这些宏的旧版 Windows SDK。
    constexpr int kKeyboardA = 0x41;
    constexpr int kKeyboardD = 0x44;
    constexpr int kNumpad4 = 0x64;
    constexpr int kNumpad6 = 0x66;
    constexpr int kGamepadDpadLeft = 0xCD;
    constexpr int kGamepadDpadRight = 0xCE;
    constexpr int kGamepadLeftThumbRight = 0xD5;
    constexpr int kGamepadLeftThumbLeft = 0xD6;
    for (const int key : {VK_LEFT, VK_RIGHT, kKeyboardA, kKeyboardD, kNumpad4, kNumpad6,
                          kGamepadDpadLeft, kGamepadDpadRight,
                          kGamepadLeftThumbLeft, kGamepadLeftThumbRight}) {
        if ((GetAsyncKeyState(key) & 0x8000) != 0) {
            return true;
        }
    }
    return false;
}

// 插件主工作线程：加载配置、初始化 DPI/通知/FM0D/Hook，并轮询更新。
// 所有复杂工作都离开 DllMain 的加载器锁执行，退出时按依赖逆序释放资源。
DWORD WINAPI WorkerThread(void*) {
    HMODULE pinned_module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&g_module), &pinned_module);
    (void)pinned_module;

    const std::filesystem::path dll_path = GetModulePath(g_module);
    const std::filesystem::path dll_directory = dll_path.parent_path();
    std::filesystem::path ini_path = dll_path;
    ini_path.replace_extension(L".ini");
    std::filesystem::path log_path = dll_path;
    log_path.replace_extension(L".log");

    // 语言层必须早于日志和首次配置模板初始化：日志标题/级别、启动诊断与
    // 自动生成 INI 的注释都需要使用同一份已解析语言。Language 只读取 ASCII
    // 配置键，因此主 INI 尚不存在时会安全按 auto 处理。
    (void)loc::Initialize(dll_path, ini_path);

    // 必须在创建、截断或打开日志文件之前直接预读总开关。
    // EnableLogging=false 时不调用 Logger::Initialize，因此本次进程不会
    // 创建、不截断、也不追加同名日志文件；已经存在的旧日志保持原样。
    const bool logging_enabled = ReadLoggingEnabled(ini_path);
    if (logging_enabled) {
        log::Initialize(log_path);
        log::Info(loc::Format(L"Log.DllMain.PluginLoaded", {dll_path.wstring()}));
        for (const auto& diagnostic : loc::StartupDiagnostics()) {
            diagnostic.warning ? log::Warn(diagnostic.message) : log::Info(diagnostic.message);
        }
    }

    const Config config = Config::Load(ini_path, dll_directory);

    // DPI 模式必须在创建任何插件窗口前设置。Windows 只允许进程默认 DPI
    // 模式设置一次；若游戏 EXE 的 manifest 已经声明 DPI 感知，这里会保持
    // 游戏原设置，并由通知线程单独请求 Per-Monitor V2。
    if (config.enable_high_dpi_awareness) {
        const ProcessDpiAwarenessReport dpi_report = EnableProcessHighDpiAwareness();
        switch (dpi_report.result) {
            case ProcessDpiAwarenessResult::EnabledPerMonitorV2:
                log::Info(loc::Text(L"Log.DllMain.DpiPerMonitorV2"));
                break;
            case ProcessDpiAwarenessResult::EnabledPerMonitor:
                log::Info(loc::Text(L"Log.DllMain.DpiPerMonitor"));
                break;
            case ProcessDpiAwarenessResult::EnabledSystemAware:
                log::Info(loc::Text(L"Log.DllMain.DpiSystem"));
                break;
            case ProcessDpiAwarenessResult::AlreadyConfigured:
                log::Info(loc::Text(L"Log.DllMain.DpiAlready"));
                break;
            case ProcessDpiAwarenessResult::Failed:
                log::Warn(loc::Format(L"Log.DllMain.DpiFailed",
                                      {std::to_wstring(dpi_report.error)}));
                break;
            case ProcessDpiAwarenessResult::Disabled:
            default:
                break;
        }
    } else {
        log::Info(loc::Text(L"Log.DllMain.DpiDisabled"));
    }

    GameHooks hooks;
    AlbumControls album_controls;

    // 通知窗口独立于播放器和游戏 Hook。即使窗口创建失败，播放后端
    // 仍会继续初始化，绝不能因为非核心 UI 功能阻止游戏启动。
    (void)NowPlayingOverlay::Instance().Initialize(config);

    const bool player_initialized = LocalPlayer::Instance().Initialize(config, dll_directory);
    (void)album_controls.Initialize(config.enable_album_controls);
    if (!player_initialized) {
        log::Error(loc::Text(L"Log.DllMain.PlayerInitFailed"));
    }

    // 安装 Hook 前先扫描曲库。这里不直接开始播放：播放时机仍由
    // 游戏的 Play/Pause/Unpause 蓝图调用或持久音量初始化信号控制。
    if (player_initialized && LocalPlayer::Instance().TrackCount() == 0) {
        log::Warn(loc::Text(L"Log.DllMain.EmptyLibrary"));
        if (config.show_empty_library_warning) {
            const std::wstring message = loc::Format(
                L"MessageBox.EmptyLibraryBody", {config.music_path.wstring()});
            const std::wstring title = loc::Text(L"MessageBox.EmptyLibraryTitle");
            MessageBoxW(nullptr, message.c_str(), title.c_str(),
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        }
    }

    if (player_initialized && config.enable_game_hooks) {
        if (!hooks.Install(config)) {
            log::Warn(loc::Text(L"Log.DllMain.HooksUnavailable"));
        }
    }

    while (!g_stop.load(std::memory_order_acquire)) {
        if (config.suppress_navigation_key_skips && HorizontalNavigationInputActive()) {
            LocalPlayer::Instance().RecordNavigationInput();
        }
        LocalPlayer::Instance().Update();
        album_controls.Update();
        if (config.enable_hotkeys) {
            if (Pressed(VK_MEDIA_PLAY_PAUSE)) {
                LocalPlayer::Instance().TogglePause();
            }
            if (Pressed(VK_MEDIA_NEXT_TRACK)) {
                LocalPlayer::Instance().Next();
            }
            if (Pressed(VK_MEDIA_PREV_TRACK)) {
                LocalPlayer::Instance().Previous();
            }
            if (Pressed(VK_F5)) {
                LocalPlayer::Instance().Rescan();
            }
        }
        Sleep(15);
    }

    hooks.Remove();
    album_controls.Shutdown();
    NowPlayingOverlay::Instance().Shutdown();
    LocalPlayer::Instance().Shutdown();
    log::Info(loc::Text(L"Log.DllMain.WorkerStopped"));
    return 0;
}

}  // namespace
}  // namespace localmusic

// Windows DLL 入口，只保存模块句柄、创建工作线程或发送停止信号。
// 禁止在这里等待线程、读写 INI、加载 FMOD 或安装 Hook，以免触发加载器锁死锁。
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    using namespace localmusic;
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_module = module;
            DisableThreadLibraryCalls(module);
            g_worker = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
            if (g_worker) {
                CloseHandle(g_worker);
                g_worker = nullptr;
            }
            break;
        case DLL_PROCESS_DETACH:
            // 正常退出进程时由操作系统回收线程；显式 FreeLibrary 时只发送
            // 停止信号，绝不能在加载器锁内等待工作线程。
            if (reserved == nullptr) {
                g_stop.store(true, std::memory_order_release);
            }
            break;
        default:
            break;
    }
    return TRUE;
}
