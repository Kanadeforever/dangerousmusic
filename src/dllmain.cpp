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
HANDLE g_instance_mutex = nullptr;

// 取得游戏主 EXE 所在的 Binaries\Win64 目录。
// 资源根目录必须以主 EXE 为锚点，不能以插件模块目录为锚点：ASI 通常位于
// Binaries\Win64\scripts，若继续按模块目录解析默认相对路径，就会错误落到
// Binaries\Content\Music，并且无法找到 Plugins\FMODStudio 下的 fmod64.dll。
std::filesystem::path ResolveGameBinaryDirectory(const std::filesystem::path& module_directory) {
    std::filesystem::path game_binary_directory = GetModuleDirectory(GetModuleHandleW(nullptr));
    if (game_binary_directory.empty()) {
        // 极端情况下主 EXE 路径读取失败，退回插件目录，至少保持 DLL 代理旧行为。
        game_binary_directory = module_directory;
    }
    std::error_code ec;
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(game_binary_directory, ec);
    return ec ? game_binary_directory.lexically_normal() : canonical;
}

// 判断游戏目录内的 dsound.dll 代理是否已经作为独立模块载入。
// 仅 ASI 工作线程使用：如果同源 dsound.dll 同时存在，短暂让出调度机会，
// 使启动更早、承担 DirectSound 代理职责的 DLL 实例优先取得插件运行权。
bool GameLocalDsoundLoaded(const std::filesystem::path& game_binary_directory) {
    HMODULE dsound_module = GetModuleHandleW(L"dsound.dll");
    if (!dsound_module || dsound_module == g_module) {
        return false;
    }
    const std::filesystem::path dsound_path = GetModulePath(dsound_module);
    if (dsound_path.empty()) {
        return false;
    }
    std::error_code parent_ec;
    std::error_code expected_ec;
    const std::filesystem::path parent =
        std::filesystem::weakly_canonical(dsound_path.parent_path(), parent_ec);
    const std::filesystem::path expected =
        std::filesystem::weakly_canonical(game_binary_directory, expected_ec);
    return !parent_ec && !expected_ec &&
           ToLower(parent.wstring()) == ToLower(expected.wstring());
}

// 取得进程级唯一运行权，防止同一二进制以 dsound.dll 与 LocalMusic.asi
// 两个文件名同时载入时重复创建播放器、通知窗口和游戏 Hook。
// DirectSound 导出属于各模块自身，不受该互斥影响；未取得运行权的副本只保留导出。
bool AcquireProcessInstance(const std::filesystem::path& module_path,
                            const std::filesystem::path& game_binary_directory) {
    const std::wstring extension = ToLower(module_path.extension().wstring());
    if (extension == L".asi" && GameLocalDsoundLoaded(game_binary_directory)) {
        // ASI Loader 通常稍晚于代理 DLL 工作。短暂延迟可稳定让 dsound.dll 成为
        // 主实例；只有 ASI 单独安装时不会命中此分支，因此不会增加额外等待。
        Sleep(250);
    }

    wchar_t mutex_name[160]{};
    std::swprintf(mutex_name, std::size(mutex_name),
                  L"Local\\DangerousDriving.LocalMusic.Instance.%lu",
                  static_cast<unsigned long>(GetCurrentProcessId()));
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutex_name);
    if (!mutex) {
        // 创建互斥量失败时采用 fail-open：单文件安装仍应继续工作。
        // 这种系统级失败极少见；代价仅是同时放置 DLL/ASI 时可能重复初始化。
        return true;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return false;
    }
    g_instance_mutex = mutex;
    return true;
}

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

    const std::filesystem::path module_path = GetModulePath(g_module);
    const std::filesystem::path module_directory = module_path.parent_path();
    const std::filesystem::path game_binary_directory =
        ResolveGameBinaryDirectory(module_directory);

    // DLL 与 ASI 可以由同一二进制直接改名得到，也允许同时放置。只有取得
    // 进程级运行权的实例执行本地播放器和 Hook；另一实例立即结束工作线程。
    if (!AcquireProcessInstance(module_path, game_binary_directory)) {
        return 0;
    }

    std::filesystem::path ini_path = module_path;
    ini_path.replace_extension(L".ini");
    std::filesystem::path log_path = module_path;
    log_path.replace_extension(L".log");

    // 语言层必须早于日志和首次配置模板初始化：日志标题/级别、启动诊断与
    // 自动生成 INI 的注释都需要使用同一份已解析语言。Language 只读取 ASCII
    // 配置键，因此主 INI 尚不存在时会安全按 auto 处理。
    (void)loc::Initialize(module_path, ini_path);

    // 必须在创建、截断或打开日志文件之前直接预读总开关。
    // EnableLogging=false 时不调用 Logger::Initialize，因此本次进程不会
    // 创建、不截断、也不追加同名日志文件；已经存在的旧日志保持原样。
    const bool logging_enabled = ReadLoggingEnabled(ini_path);
    if (logging_enabled) {
        log::Initialize(log_path);
        log::Info(loc::Format(L"Log.DllMain.PluginLoaded", {module_path.wstring()}));
        for (const auto& diagnostic : loc::StartupDiagnostics()) {
            diagnostic.warning ? log::Warn(diagnostic.message) : log::Info(diagnostic.message);
        }
    }

    const Config config = Config::Load(ini_path, game_binary_directory);

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

    const bool player_initialized = LocalPlayer::Instance().Initialize(config, game_binary_directory);
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
    if (g_instance_mutex) {
        CloseHandle(g_instance_mutex);
        g_instance_mutex = nullptr;
    }
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
