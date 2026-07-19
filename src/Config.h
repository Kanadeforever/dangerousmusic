// 配置数据结构：定义播放、认证、音量与 EXE 兼容策略。
// 字段名保持英文以对应 INI 键和现有代码接口，注释统一使用简体中文。
#pragma once
#include "Common.h"

namespace localmusic {

enum class PlayMode {
    Sequential,
    Random,
    SingleLoop,
};

enum class AuthenticationCallbackMode {
    Immediate,
    Deferred,
};

enum class VolumeSource {
    GameSpotify,
    Plugin,
};

enum class NotificationPosition {
    TopLeft,
    TopRight,
    BottomLeft,
    BottomRight,
};

struct Config {
    std::filesystem::path ini_path;
    std::filesystem::path music_path;
    // 最终生效的规范化语言标签，例如 zh-hans、en-us 或用户自定义标签。
    std::wstring language = L"zh-hans";
    // 发行版日志总开关。false 时不创建、不截断、也不追加 dsound.log。
    // 旧配置缺少该键时默认开启，便于出现兼容性问题时保留诊断信息。
    bool enable_logging = true;
    bool scan_subdirs = true;
    // 每个实际包含音乐文件的目录都视为一张专辑；音乐根目录中的散装曲目
    // 自动归入“未分类曲目”。ZIP 不属于支持的专辑容器。
    bool enable_folder_albums = true;
    // 读取与音频同主文件名的 INI，例如 Song.wav 对应 Song.ini。
    bool enable_sidecar_metadata = true;
    // false 时侧挂 INI 只补充缺失字段；true 时允许全局覆盖有效内嵌标签。
    bool sidecar_metadata_override = false;
    std::unordered_set<std::wstring> extensions{L".mp3", L".wav", L".ogg"};
    PlayMode play_mode = PlayMode::Sequential;
    // 插件侧永久音量倍率。使用 GameSpotify 模式时，FMOD 最终输出为
    // 本值乘以游戏内持久 Spotify 音量。
    float volume = 0.5F;
    bool start_paused = false;  // 旧版兼容回退；启用游戏集成后仍会从空闲状态开始。
    bool enable_hotkeys = false;
    // 固定专辑控制：键盘 Insert/PageUp/Home；手柄 LB+十字键。
    // 按键映射有意不可配置，避免与游戏已有操作产生不可预测冲突。
    bool enable_album_controls = true;
    bool allow_pause = true;
    bool allow_skip = true;
    bool enable_game_hooks = true;
    bool bypass_spotify_authentication = true;
    // 是否向游戏暴露LocalMusic 本地后端。此状态只保存在 dsound.ini；
    // 插件绝不会写入游戏存档或 SpotifyRefreshToken。默认开启时，
    // 插件提供本地播放后端，并跟随游戏原有的 Spotify 播放命令。
    bool local_spotify_replacement_enabled = true;
    bool remember_spotify_mode_in_ini = true;
    AuthenticationCallbackMode authentication_callback_mode = AuthenticationCallbackMode::Immediate;
    bool verbose_auth_callbacks = false;
    bool hook_game_volume = true;
    VolumeSource volume_source = VolumeSource::GameSpotify;
    bool show_empty_library_warning = true;

    // 自绘曲目信息通知。通知层是独立的透明 Win32 窗口，既不 Hook
    // DirectX Present，也不接管鼠标和键盘输入。
    bool show_now_playing_notification = true;
    bool show_pause_notification = true;
    bool show_resume_notification = true;
    bool show_embedded_cover = true;
    // 是否允许使用同名图片、曲目 INI 指定图片和文件夹专辑封面。
    bool show_external_cover = true;
    // 是否在“正在播放/暂停/继续”标题行附加当前播放模式。
    bool show_play_mode_in_notification = true;
    bool notification_only_when_game_foreground = true;
    uint32_t notification_duration_ms = 3000;
    uint32_t notification_status_duration_ms = 1800;
    uint32_t notification_fade_ms = 250;
    uint32_t notification_scale_percent = 100;
    uint32_t notification_opacity_percent = 92;
    // 是否在插件启动早期为游戏进程请求高 DPI 感知，并为通知线程设置
    // Per-Monitor V2。启用后，通知使用物理像素，不再被 Windows DPI
    // 虚拟化或由代码按 DPI 自动放大。
    bool enable_high_dpi_awareness = true;
    // 从所选角落向游戏客户区内部移动的独立水平/垂直距离。
    // X 和 Y 使用物理像素，不随 Windows DPI 自动缩放。
    uint32_t notification_margin_x = 32;
    uint32_t notification_margin_y = 32;
    NotificationPosition notification_position = NotificationPosition::TopRight;
    bool start_local_music_on_play = true;
    bool start_local_music_on_unpause = true;
    bool pause_toggles_playback = true;
    bool respond_to_skip_only_when_active = true;
    // 实验性菜单横向导航抑制。实机证明部分 UI 会延迟到另一帧调用，
    // 因而发行默认关闭；仅为旧配置和开发诊断保留读取能力。
    bool suppress_navigation_key_skips = false;
    uint32_t navigation_skip_suppression_ms = 400;
    bool auto_advance = true;
    // 当前曲目播放期间预先打开下一首 FMOD 流，避免上一首结束后
    // 才执行文件冷打开和解码器初始化，从而减少切歌间隔。
    bool preload_next_track = true;
    // 游戏音量降低立即生效；向上增加必须短暂保持稳定，
    // 防止设置界面初始化时短暂上报 100% 而造成突然爆音。
    uint32_t game_volume_increase_debounce_ms = 250;
    // 游戏第一次发布持久 Spotify 音量时启动本地播放。
    // 对当前游戏版本而言，这是较可靠的会话激活信号。
    bool start_local_music_on_game_volume = true;

    // 兼容策略：函数名注册表仍是首选定位方式；以下字段用于
    // 版本校验以及未来 EXE 的特征码/RVA 回退。
    bool strict_version = false;
    bool require_all_hooks = true;
    bool dry_run_hooks = false;
    bool calculate_sha256 = false;
    uint32_t expected_timestamp = 0x5E569211U;
    uint32_t expected_image_size = 0x3383000U;
    std::wstring expected_sha256 = L"847c4db36468531a1f527f14dbee36d464a36bc6cf7bcc02827f64b26c1a2014";
    int frame_code_offset = -1;  // -1 表示从已定位的包装函数自动推断。

    // UE4 动态多播委托兼容参数。默认值匹配用户提供的 2020 版 EXE，
    // 未来 EXE 可在 INI 的兼容配置中覆盖。
    std::wstring delegate_broadcast_pattern = L"4C 8B DC 53 41 54 41 56 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4";
    uintptr_t delegate_broadcast_rva = 0x258250;
    int enabled_dispatcher_offset = 0x28;
    int activate_code_dispatcher_offset = 0x38;
    int check_auth_code_dispatcher_offset = 0x48;
    int refresh_access_token_dispatcher_offset = 0x58;
    int switch_active_device_dispatcher_offset = 0x68;
    int service_enabled_offset = 0x9C;

    // 加载完整配置、创建缺失模板并把相对音乐路径解析到游戏目录。
    // 所有外部数值都在这里限制范围，后续模块只接收可用的 Config 快照。
    static Config Load(const std::filesystem::path& ini_path, const std::filesystem::path& dll_directory);
};

// 封装 GetPrivateProfileStringW，动态扩大缓冲区以读取较长配置值。
// 返回结果保持宽字符，路径和中日韩文本不会经过本地代码页损失。
std::wstring ReadIniString(const std::filesystem::path& path, const wchar_t* section, const std::wstring& key, const wchar_t* fallback = L"");
// 在日志文件初始化前预读总开关，保证 EnableLogging=false 时完全不触碰 dsound.log。
bool ReadLoggingEnabled(const std::filesystem::path& path);
// 读取十进制或 0x 前缀十六进制整数，用于 RVA、偏移和毫秒数。
// 解析失败返回空 optional，使调用方可以区分零值与缺失/错误。
std::optional<uintptr_t> ReadIniInteger(const std::filesystem::path& path, const wchar_t* section, const std::wstring& key);
// 只更新指定 INI 键并刷新 Windows INI 缓存。
// 不会重建整个文件，因此用户注释和未识别的高级键可以继续保留。
bool WriteIniString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const std::wstring& value);
// 把布尔值规范化为 true/false 后交给 WriteIniString 持久化。
// 统一格式便于用户手工阅读，也避免 0/1 与 yes/no 混用。
bool WriteIniBool(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, bool value);
// 以稳定的小数格式写回浮点配置。
// 写入前由业务层负责范围限制，本函数只负责序列化和持久化。
bool WriteIniFloat(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, float value);

}  // namespace localmusic
