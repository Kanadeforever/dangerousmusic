// 配置实现：生成默认 INI、解析用户配置并执行范围校验。
// 已存在的 INI 永不自动覆盖，写入失败时安全回退到内置默认值。
#include "Config.h"
#include "Logger.h"
#include "Localization.h"

namespace localmusic {
namespace {

constexpr uint32_t kBuiltInExpectedTimestamp = 0x5E569211U;
constexpr uint32_t kBuiltInExpectedImageSize = 0x3383000U;
constexpr wchar_t kBuiltInExpectedSha256[] =
    L"847c4db36468531a1f527f14dbee36d464a36bc6cf7bcc02827f64b26c1a2014";

// LocalMusic 是发行版使用的主配置节。LocalSpotify 仅作为旧版本兼容回退：
// 新生成的 INI 与所有运行时写回都使用 LocalMusic；当主节缺少某个键时，
// 才读取旧节中的同名键，避免升级用户丢失既有设置。
constexpr wchar_t kPrimarySettingsSection[] = L"LocalMusic";
constexpr wchar_t kLegacySettingsSection[] = L"LocalSpotify";
constexpr wchar_t kMissingIniValue[] = L"{LOCALMUSIC_MISSING_VALUE}";

// 优先读取 [LocalMusic]，键不存在时回退 [LocalSpotify]。
// 使用不可与正常配置混淆的哨兵值来区分“缺失”和“显式空字符串”。
std::wstring ReadLocalString(const std::filesystem::path& path,
                             const std::wstring& key,
                             const wchar_t* fallback = L"") {
    const std::wstring primary = ReadIniString(path, kPrimarySettingsSection, key, kMissingIniValue);
    if (primary != kMissingIniValue) return primary;
    return ReadIniString(path, kLegacySettingsSection, key, fallback);
}

// 读取 LocalMusic 布尔值，并兼容旧 LocalSpotify 节及常见真假拼写。
bool ReadLocalBool(const std::filesystem::path& path,
                   const wchar_t* key,
                   bool fallback) {
    const std::wstring raw = ToLower(Trim(ReadLocalString(path, key, fallback ? L"true" : L"false")));
    return raw == L"1" || raw == L"true" || raw == L"yes" || raw == L"on";
}

// 读取 LocalMusic 整数。主节缺失时回退旧节；解析规则与公共整数读取函数一致。
std::optional<uintptr_t> ReadLocalInteger(const std::filesystem::path& path,
                                          const wchar_t* key) {
    const std::wstring raw = Trim(ReadLocalString(path, key));
    if (raw.empty() || ToLower(raw) == L"auto") return std::nullopt;
    try {
        size_t consumed = 0;
        const uintptr_t value = static_cast<uintptr_t>(std::stoull(raw, &consumed, 0));
        return consumed == raw.size() ? std::optional<uintptr_t>(value) : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

// 读取 LocalMusic 浮点值。无效文本安全回退，不允许配置错误阻止游戏启动。
float ReadLocalFloat(const std::filesystem::path& path,
                     const wchar_t* key,
                     float fallback) {
    const std::wstring raw = Trim(ReadLocalString(path, key));
    if (raw.empty()) return fallback;
    try {
        return std::stof(raw);
    } catch (...) {
        return fallback;
    }
}



enum class IniCreationResult {
    AlreadyExists,
    Created,
    Failed,
};

// 确保配置文件存在；首次运行时以独占创建方式写入内置发行模板。
// 返回值区分“新建、已存在、失败”，调用方据此决定是否提示而不会覆盖用户配置。
IniCreationResult EnsureDefaultIniFile(const std::filesystem::path& path) {
    HANDLE file = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);

    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
            return IniCreationResult::AlreadyExists;
        }
        log::Warn(loc::Format(L"Log.Config.CreateFailed",
                              {path.wstring(), std::to_wstring(error)}));
        return IniCreationResult::Failed;
    }

    const std::string default_ini = loc::BuildDefaultConfigText(path.filename().wstring());
    const char* cursor = default_ini.data();
    size_t remaining = default_ini.size();
    bool ok = true;
    while (remaining > 0) {
        const DWORD request = static_cast<DWORD>(
            std::min<size_t>(remaining, static_cast<size_t>(std::numeric_limits<DWORD>::max())));
        DWORD written = 0;
        if (!WriteFile(file, cursor, request, &written, nullptr) || written == 0) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }

    if (ok && !FlushFileBuffers(file)) {
        ok = false;
    }
    const DWORD write_error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);

    if (!ok) {
        DeleteFileW(path.c_str());
        log::Warn(loc::Format(L"Log.Config.WriteFailed",
                              {path.wstring(), std::to_wstring(write_error)}));
        return IniCreationResult::Failed;
    }

    log::Info(loc::Format(L"Log.Config.Created", {path.wstring()}));
    return IniCreationResult::Created;
}

// 从指定 INI 节读取布尔值，并兼容 true/false、yes/no、on/off 与 1/0。
// 无法识别或键缺失时返回调用方给出的安全默认值。
bool ReadBool(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, bool fallback) {
    const std::wstring raw = ToLower(Trim(ReadIniString(path, section, key, fallback ? L"true" : L"false")));
    return raw == L"1" || raw == L"true" || raw == L"yes" || raw == L"on";
}

// 读取浮点配置并使用宽字符转换，供音量等允许小数的选项使用。
// 格式错误时不抛异常，直接回退默认值以保证游戏可以继续启动。
float ReadFloat(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, float fallback) {
    const std::wstring raw = Trim(ReadIniString(path, section, key));
    if (raw.empty()) {
        return fallback;
    }
    try {
        return std::stof(raw);
    } catch (...) {
        return fallback;
    }
}

// 按单个分隔符拆分宽字符串，并对每段执行 Trim。
// 空片段会被保留给调用方判断，避免工具函数偷偷改变配置语义。
std::vector<std::wstring> Split(const std::wstring& text, wchar_t delimiter) {
    std::vector<std::wstring> parts;
    std::wstringstream stream(text);
    std::wstring item;
    while (std::getline(stream, item, delimiter)) {
        item = Trim(item);
        if (!item.empty()) {
            parts.push_back(std::move(item));
        }
    }
    return parts;
}

}  // namespace

// 封装 GetPrivateProfileStringW，动态扩大缓冲区以读取较长配置值。
// 返回结果保持宽字符，路径和中日韩文本不会经过本地代码页损失。
std::wstring ReadIniString(const std::filesystem::path& path, const wchar_t* section, const std::wstring& key, const wchar_t* fallback) {
    std::vector<wchar_t> buffer(8192);
    GetPrivateProfileStringW(section, key.c_str(), fallback, buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
    return buffer.data();
}

// 在日志系统初始化前最小化读取 EnableLogging。
// 该函数不能写日志或创建文件，否则会破坏“完全关闭日志”的承诺。
bool ReadLoggingEnabled(const std::filesystem::path& path) {
    // 该函数必须可以在 Logger::Initialize 之前安全调用。使用与普通布尔配置
    // 相同的解析规则，确保预读结果与 Config::Load 中的最终字段一致。
    return ReadLocalBool(path, L"EnableLogging", true);
}

// 读取十进制或 0x 前缀十六进制整数，用于 RVA、偏移和毫秒数。
// 解析失败返回空 optional，使调用方可以区分零值与缺失/错误。
std::optional<uintptr_t> ReadIniInteger(const std::filesystem::path& path, const wchar_t* section, const std::wstring& key) {
    std::wstring raw = Trim(ReadIniString(path, section, key));
    if (raw.empty() || ToLower(raw) == L"auto") {
        return std::nullopt;
    }
    try {
        size_t consumed = 0;
        const uintptr_t value = static_cast<uintptr_t>(std::stoull(raw, &consumed, 0));
        if (consumed != raw.size()) {
            return std::nullopt;
        }
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

// 只更新指定 INI 键并刷新 Windows INI 缓存。
// 不会重建整个文件，因此用户注释和未识别的高级键可以继续保留。
bool WriteIniString(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    if (path.empty()) {
        return false;
    }
    return WritePrivateProfileStringW(section, key, value.c_str(), path.c_str()) != FALSE;
}

// 把布尔值规范化为 true/false 后交给 WriteIniString 持久化。
// 统一格式便于用户手工阅读，也避免 0/1 与 yes/no 混用。
bool WriteIniBool(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, bool value) {
    return WriteIniString(path, section, key, value ? L"true" : L"false");
}

// 以稳定的小数格式写回浮点配置。
// 写入前由业务层负责范围限制，本函数只负责序列化和持久化。
bool WriteIniFloat(const std::filesystem::path& path, const wchar_t* section, const wchar_t* key, float value) {
    std::wostringstream stream;
    stream << std::fixed << std::setprecision(3) << std::clamp(value, 0.0F, 1.0F);
    return WriteIniString(path, section, key, stream.str());
}

// 加载完整配置、创建缺失模板并把相对音乐路径解析到游戏目录。
// 所有外部数值都在这里限制范围，后续模块只接收可用的 Config 快照。
Config Config::Load(const std::filesystem::path& ini_path, const std::filesystem::path& game_binary_directory) {
    (void)EnsureDefaultIniFile(ini_path);

    Config config;
    config.ini_path = ini_path;
    config.language = loc::ActiveLanguage();
    config.enable_logging = ReadLocalBool(ini_path, L"EnableLogging", true);

    std::filesystem::path music = ReadLocalString(ini_path, L"MusicPath", L"..\\..\\Content\\Music");
    if (music.is_relative()) {
        music = game_binary_directory / music;
    }
    std::error_code ec;
    config.music_path = std::filesystem::weakly_canonical(music, ec);
    if (ec) {
        config.music_path = music.lexically_normal();
    }

    config.scan_subdirs = ReadLocalBool(ini_path, L"ScanSubdirs", true);
    config.enable_folder_albums = ReadLocalBool(ini_path, L"EnableFolderAlbums", true);
    config.enable_sidecar_metadata = ReadLocalBool(ini_path, L"EnableSidecarMetadata", true);
    config.sidecar_metadata_override = ReadLocalBool(ini_path, L"SidecarMetadataOverride", false);
    config.start_paused = ReadLocalBool(ini_path, L"StartPaused", false);
    config.enable_hotkeys = ReadLocalBool(ini_path, L"EnableHotkeys", false);
    config.enable_album_controls = ReadLocalBool(ini_path, L"EnableAlbumControls", true);
    config.allow_pause = ReadLocalBool(ini_path, L"AllowPause", true);
    config.allow_skip = ReadLocalBool(ini_path, L"AllowSkip", true);
    config.enable_game_hooks = ReadLocalBool(ini_path, L"EnableGameHooks", true);
    config.local_spotify_replacement_enabled = ReadLocalBool(ini_path, L"LocalSpotifyReplacementEnabled", true);
    config.remember_spotify_mode_in_ini = ReadLocalBool(ini_path, L"RememberSpotifyModeInPluginIni", true);
    config.start_local_music_on_play = ReadLocalBool(ini_path, L"StartLocalMusicOnPlay", true);
    config.start_local_music_on_unpause = ReadLocalBool(ini_path, L"StartLocalMusicOnUnpause", true);
    config.pause_toggles_playback = ReadLocalBool(ini_path, L"PauseTogglesPlayback", true);
    config.respond_to_skip_only_when_active = ReadLocalBool(ini_path, L"RespondToSkipOnlyWhenActive", true);
    config.suppress_navigation_key_skips = ReadLocalBool(ini_path, L"SuppressNavigationKeySkips", false);
    config.navigation_skip_suppression_ms = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NavigationSkipSuppressionMs").value_or(400), 0, 2000));
    config.auto_advance = ReadLocalBool(ini_path, L"AutoAdvance", true);
    config.preload_next_track = ReadLocalBool(ini_path, L"PreloadNextTrack", true);
    config.game_volume_increase_debounce_ms = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"GameVolumeIncreaseDebounceMs").value_or(250), 0, 2000));
    config.start_local_music_on_game_volume = ReadLocalBool(ini_path, L"StartLocalMusicOnGameVolume", true);
    // BypassSpotifyAuthentication 是当前权威配置。为了兼容旧 INI，
    // 当该键不存在时仍读取 BlockSpotifyNetwork，使旧安装自动获得
    // 更安全的纯内存认证旁路。
    const std::wstring bypass_raw = Trim(ReadLocalString(ini_path, L"BypassSpotifyAuthentication"));
    config.bypass_spotify_authentication = bypass_raw.empty()
        ? ReadLocalBool(ini_path, L"BlockSpotifyNetwork", true)
        : ReadLocalBool(ini_path, L"BypassSpotifyAuthentication", true);
    const std::wstring callback_mode = ToLower(Trim(ReadLocalString(ini_path, L"AuthenticationCallbackMode", L"Immediate")));
    config.authentication_callback_mode = callback_mode == L"deferred"
        ? AuthenticationCallbackMode::Deferred
        : AuthenticationCallbackMode::Immediate;
    config.verbose_auth_callbacks = ReadLocalBool(ini_path, L"VerboseAuthCallbacks", false);

    config.hook_game_volume = ReadLocalBool(ini_path, L"HookGameVolume", true);
    const std::wstring volume_source = ToLower(Trim(ReadLocalString(ini_path, L"VolumeSource", L"GameSpotify")));
    config.volume_source = volume_source == L"plugin" ? VolumeSource::Plugin : VolumeSource::GameSpotify;
    if (config.volume_source == VolumeSource::Plugin) {
        config.hook_game_volume = false;
    }
    config.show_empty_library_warning = ReadLocalBool(ini_path, L"ShowEmptyLibraryWarning", true);
    config.show_now_playing_notification = ReadLocalBool(ini_path, L"ShowNowPlayingNotification", true);
    config.show_pause_notification = ReadLocalBool(ini_path, L"ShowPauseNotification", true);
    config.show_resume_notification = ReadLocalBool(ini_path, L"ShowResumeNotification", true);
    config.show_embedded_cover = ReadLocalBool(ini_path, L"ShowEmbeddedCover", true);
    config.show_external_cover = ReadLocalBool(ini_path, L"ShowExternalCover", true);
    config.show_play_mode_in_notification = ReadLocalBool(ini_path, L"ShowPlayModeInNotification", true);
    config.notification_only_when_game_foreground = ReadLocalBool(ini_path, L"NotificationOnlyWhenGameForeground", true);
    config.notification_duration_ms = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationDurationMs").value_or(3000), 500, 15000));
    config.notification_status_duration_ms = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationStatusDurationMs").value_or(1800), 500, 10000));
    config.notification_fade_ms = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationFadeMs").value_or(250), 0, 2000));
    config.notification_scale_percent = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationScalePercent").value_or(100), 50, 200));
    config.notification_opacity_percent = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationOpacityPercent").value_or(92), 40, 100));
    config.enable_high_dpi_awareness = ReadLocalBool(ini_path, L"EnableHighDpiAwareness", true);
    // 兼容旧版统一边距。新 INI 优先使用 X/Y；任一新键缺失时，
    // 该方向会回退到旧 NotificationMargin，再回退到 32。
    const uintptr_t legacy_notification_margin =
        ReadLocalInteger(ini_path, L"NotificationMargin").value_or(32);
    config.notification_margin_x = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationMarginX").value_or(legacy_notification_margin),
        0, 2000));
    config.notification_margin_y = static_cast<uint32_t>(std::clamp<uintptr_t>(
        ReadLocalInteger(ini_path, L"NotificationMarginY").value_or(legacy_notification_margin),
        0, 2000));
    const std::wstring notification_position = ToLower(Trim(ReadLocalString(ini_path, L"NotificationPosition", L"TopRight")));
    if (notification_position == L"topleft") {
        config.notification_position = NotificationPosition::TopLeft;
    } else if (notification_position == L"bottomleft") {
        config.notification_position = NotificationPosition::BottomLeft;
    } else if (notification_position == L"bottomright") {
        config.notification_position = NotificationPosition::BottomRight;
    } else {
        config.notification_position = NotificationPosition::TopRight;
    }

    float volume = ReadLocalFloat(ini_path, L"Volume", 0.5F);
    if (volume > 1.0F) {
        volume /= 100.0F;
    }
    config.volume = std::clamp(volume, 0.0F, 1.0F);

    const std::wstring mode = ToLower(Trim(ReadLocalString(ini_path, L"PlayMode", L"Sequential")));
    if (mode == L"random") {
        config.play_mode = PlayMode::Random;
    } else if (mode == L"singleloop" || mode == L"single_loop") {
        config.play_mode = PlayMode::SingleLoop;
    } else {
        // 缺失、拼写错误或显式 Sequential 均安全回退为顺序播放。
        config.play_mode = PlayMode::Sequential;
    }

    config.extensions.clear();
    for (std::wstring ext : Split(ReadLocalString(ini_path, L"SupportedFormats", L"mp3,wav,ogg"), L',')) {
        ext = ToLower(ext);
        if (!ext.empty() && ext.front() != L'.') {
            ext.insert(ext.begin(), L'.');
        }
        config.extensions.insert(std::move(ext));
    }
    if (config.extensions.empty()) {
        config.extensions = {L".mp3", L".wav", L".ogg"};
    }

    config.strict_version = ReadBool(ini_path, L"Compatibility", L"StrictVersion", false);
    config.require_all_hooks = ReadBool(ini_path, L"Compatibility", L"RequireAllHooks", true);
    config.dry_run_hooks = ReadBool(ini_path, L"Compatibility", L"DryRun", false);
    config.calculate_sha256 = ReadBool(ini_path, L"Compatibility", L"CalculateSha256", false);
    config.expected_timestamp = static_cast<uint32_t>(
        ReadIniInteger(ini_path, L"Compatibility", L"ExpectedTimestamp").value_or(kBuiltInExpectedTimestamp));
    config.expected_image_size = static_cast<uint32_t>(
        ReadIniInteger(ini_path, L"Compatibility", L"ExpectedImageSize").value_or(kBuiltInExpectedImageSize));
    config.expected_sha256 = ToLower(Trim(ReadIniString(
        ini_path, L"Compatibility", L"ExpectedSha256", kBuiltInExpectedSha256)));
    if (const auto frame_offset = ReadIniInteger(ini_path, L"Compatibility", L"FrameCodeOffset")) {
        config.frame_code_offset = *frame_offset <= static_cast<uintptr_t>(std::numeric_limits<int>::max())
                                       ? static_cast<int>(*frame_offset)
                                       : -1;
    } else {
        config.frame_code_offset = -1;
    }

    config.delegate_broadcast_pattern = Trim(ReadIniString(
        ini_path,
        L"Compatibility",
        L"DelegateBroadcastPattern",
        L"4C 8B DC 53 41 54 41 56 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4"));
    config.delegate_broadcast_rva = ReadIniInteger(ini_path, L"Compatibility", L"DelegateBroadcastRva").value_or(0x258250);
    auto read_offset = [&](const wchar_t* key, int fallback) {
        const auto value = ReadIniInteger(ini_path, L"Compatibility", key);
        return value && *value <= static_cast<uintptr_t>(std::numeric_limits<int>::max())
                   ? static_cast<int>(*value)
                   : fallback;
    };
    config.enabled_dispatcher_offset = read_offset(L"EnabledDispatcherOffset", 0x28);
    config.activate_code_dispatcher_offset = read_offset(L"ActivateCodeDispatcherOffset", 0x38);
    config.check_auth_code_dispatcher_offset = read_offset(L"CheckAuthCodeDispatcherOffset", 0x48);
    config.refresh_access_token_dispatcher_offset = read_offset(L"RefreshAccessTokenDispatcherOffset", 0x58);
    config.switch_active_device_dispatcher_offset = read_offset(L"SwitchActiveDeviceDispatcherOffset", 0x68);
    config.service_enabled_offset = read_offset(L"ServiceEnabledOffset", 0x9C);

    log::Info(loc::Format(L"Log.Config.MusicDirectory", {config.music_path.wstring()}));
    return config;
}

}  // namespace localmusic
