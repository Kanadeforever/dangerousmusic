// 游戏 Hook 实现：解析 UE4 原生注册表，保留参数包装层，并把 Spotify 控制转发到本地播放器。
// 认证仅在内存中模拟成功，不读取、清除或写回游戏持久 Spotify 凭据。
#include "GameHooks.h"

#include "AlbumControls.h"
#include "LocalPlayer.h"
#include "Logger.h"
#include "Localization.h"
#include "Signature.h"

namespace localmusic {
namespace {

std::atomic<GameHooks*> g_active_hooks{nullptr};

struct FStringProxy {
    wchar_t* data = nullptr;
    int32_t count = 0;
    int32_t capacity = 0;
};

// 把地址/RVA 格式化为十六进制宽字符串。
// 统一日志格式，便于对照反汇编和兼容配置。
std::wstring Hex(uintptr_t value) {
    std::wostringstream stream;
    stream << L"0x" << std::hex << std::uppercase << value;
    return stream.str();
}

// 把 ASCII 函数名转换为宽字符串用于日志。
// UE 注册名保持字节不变，不进行区域相关转换。
std::wstring Widen(std::string_view value) {
    return Utf8ToWide(value);
}

// 检查推断出的 UE 对象字段偏移是否在合理范围。
// 拒绝负值和异常大值，未知 EXE 不会按错误偏移写内存。
bool IsOffsetSane(int offset, int limit = 0x2000) {
    return offset >= 0 && offset < limit;
}

}  // namespace

// 返回当前已安装的 GameHooks 实例。
// 静态 Hook 入口通过它访问配置和回调状态，未安装时返回空。
GameHooks* GameHooks::Active() {
    return g_active_hooks.load(std::memory_order_acquire);
}

// 验证 EXE、定位 UE4/原生入口并按配置安装全部 Spotify Hook。
// RequireAllHooks 开启时任一关键补丁失败都会回滚，避免半安装状态。
bool GameHooks::Install(const Config& config) {
    if (installed_) {
        return true;
    }
    config_ = config;
    image_ = PEImage(GetModuleHandleW(nullptr));
    if (!image_.Valid()) {
        log::Error(loc::Text(L"Log.Hooks.InvalidPe"));
        return false;
    }
    if (!ValidateExecutableVersion()) {
        return false;
    }

    // 只有纯返回值的 Blueprint exec 包装函数会被直接替换。
    // 所有动作/网络方法都保留 UE4 原始 exec 包装层，使 FFrame 参数处理和
    // 自动生成逻辑保持不变；插件只补丁包装层最终调用的原生 C++ 目标。
    std::vector<HookRequest> exec_requests{
        {"IsPaused", reinterpret_cast<void*>(&HookIsPaused), true},
        {"GetPlayingArtistName", reinterpret_cast<void*>(&HookGetPlayingArtistName), true},
        {"GetPlayingTrackName", reinterpret_cast<void*>(&HookGetPlayingTrackName), true},
    };
    if (config_.bypass_spotify_authentication) {
        // 这些 Blueprint 状态查询是游戏界面需要观察的认证结果。
        // 它们始终报告成功，但不会读取、清除或写入 SaveGame.sav 中的 refresh token。
        exec_requests.insert(exec_requests.begin(), {
            {"HasValidAccessToken", reinterpret_cast<void*>(&HookHasValidAccessToken), true},
            {"HasValidAccessTokenAndDevice", reinterpret_cast<void*>(&HookHasValidAccessTokenAndDevice), true},
            {"IsEnabled", reinterpret_cast<void*>(&HookIsEnabled), true},
        });
    }

    std::vector<HookRequest> native_requests{
        {"NextTrack", reinterpret_cast<void*>(&HookNativeNextTrack), true},
        {"Pause", reinterpret_cast<void*>(&HookNativePause), true},
        {"Play", reinterpret_cast<void*>(&HookNativePlay), true},
        {"PreviousTrack", reinterpret_cast<void*>(&HookNativePreviousTrack), true},
        {"Unpause", reinterpret_cast<void*>(&HookNativeUnpause), true},
    };
    if (config_.bypass_spotify_authentication) {
        // 这些方法在原游戏中是异步的。若只做空操作，蓝图加载逻辑会永远等待
        // 多播 dispatcher。下面的专用 Hook 会排队等价的成功通知，并在下一次
        // 安全的 SpotifyService 游戏线程入口派发；Update 只是额外备用泵，
        // 不是唯一派发入口。
        native_requests.push_back({"CheckEnabled", reinterpret_cast<void*>(&HookNativeCheckEnabled), true});
        native_requests.push_back({"CheckAuthCode", reinterpret_cast<void*>(&HookNativeCheckAuthCode), true});
        native_requests.push_back({"ClearAccessTokens", reinterpret_cast<void*>(&HookNativeClearAccessTokens), true});
        native_requests.push_back({"RefreshAccessToken", reinterpret_cast<void*>(&HookNativeRefreshAccessToken), true});
        native_requests.push_back({"RequestActivateCode", reinterpret_cast<void*>(&HookNativeRequestActivateCode), true});
        native_requests.push_back({"SwitchActiveDevice", reinterpret_cast<void*>(&HookNativeSwitchActiveDevice), false});
        native_requests.push_back({"Update", reinterpret_cast<void*>(&HookNativeUpdate), true});
    }

    bool missing_required = false;
    auto resolve_wrapper = [&](HookRequest& request) {
        void* wrapper = ResolveWrapper(request.name);
        if (!wrapper) {
            const std::wstring message = loc::Format(L"Log.Hooks.ResolveFailed", {Widen(request.name)});
            request.required ? log::Error(message) : log::Warn(message);
            missing_required = missing_required || request.required;
            return static_cast<uint8_t*>(nullptr);
        }
        wrappers_[request.name] = static_cast<uint8_t*>(wrapper);
        log::Info(loc::Format(L"Log.Hooks.ExecResolved", {Widen(request.name), Hex(image_.ToRva(wrapper))}));
        return static_cast<uint8_t*>(wrapper);
    };

    for (auto& request : exec_requests) {
        request.resolved = resolve_wrapper(request);
    }
    for (auto& request : native_requests) {
        uint8_t* wrapper = resolve_wrapper(request);
        if (!wrapper) {
            continue;
        }
        request.resolved = FindTailJumpTarget(wrapper);
        if (!request.resolved) {
            const std::wstring message = loc::Format(L"Log.Hooks.NativeResolveFailed", {Widen(request.name)});
            request.required ? log::Error(message) : log::Warn(message);
            missing_required = missing_required || request.required;
        } else {
            log::Info(loc::Format(L"Log.Hooks.NativeResolved", {Widen(request.name), Hex(image_.ToRva(request.resolved))}));
        }
    }

    if (missing_required && config_.require_all_hooks) {
        log::Error(loc::Text(L"Log.Hooks.RequiredMissing"));
        wrappers_.clear();
        return false;
    }

    // 两个 FString getter 最后都会尾跳到引擎的 FString 拷贝函数。
    // 调用该函数可让 UE 使用自己的分配器构造返回字符串，避免跨分配器释放。
    if (const auto iterator = wrappers_.find("GetPlayingArtistName"); iterator != wrappers_.end()) {
        string_copy_ = reinterpret_cast<FStringCopyFunction>(FindTailJumpTarget(iterator->second));
    }
    if (!string_copy_) {
        log::Error(loc::Text(L"Log.Hooks.FStringFailed"));
        if (config_.require_all_hooks) {
            wrappers_.clear();
            return false;
        }
    } else {
        log::Info(loc::Format(L"Log.Hooks.FStringResolved", {Hex(image_.ToRva(reinterpret_cast<void*>(string_copy_)))}));
    }

    if (config_.frame_code_offset >= 0) {
        frame_code_offset_ = config_.frame_code_offset;
    } else {
        for (const auto& request : exec_requests) {
            const auto iterator = wrappers_.find(request.name);
            if (iterator == wrappers_.end()) {
                continue;
            }
            const int inferred = InferFrameCodeOffset(iterator->second);
            if (inferred >= 0) {
                frame_code_offset_ = inferred;
                break;
            }
        }
    }
    if (!IsOffsetSane(frame_code_offset_, 0x200)) {
        log::Error(loc::Text(L"Log.Hooks.FrameOffsetFailed"));
        wrappers_.clear();
        return false;
    }
    log::Info(loc::Format(L"Log.Hooks.FrameOffset", {Hex(static_cast<uintptr_t>(frame_code_offset_))}));
    log::Info(loc::Text(L"Log.Hooks.NoCredentialWrites"));
    if (config_.bypass_spotify_authentication) {
        log::Info(loc::Text(L"Log.Hooks.AuthEnabled"));

        // Dispatcher 字段偏移属于类布局数据，不是普通代码地址。未知 EXE 即使某个
        // RVA 恰好落在可执行内存中，也绝不能直接复用当前布局；未来 EXE 必须先
        // 明确更新兼容配置。
        const bool callback_profile_matches =
            config_.expected_timestamp != 0 && config_.expected_timestamp == image_.Timestamp() &&
            config_.expected_image_size != 0 && config_.expected_image_size == image_.SizeOfImage();
        if (!callback_profile_matches) {
            log::Error(loc::Text(L"Log.Hooks.CallbackProfileMismatch"));
            wrappers_.clear();
            return false;
        }

        void* delegate_helper = nullptr;
        if (!config_.delegate_broadcast_pattern.empty()) {
            if (const auto pattern = BytePattern::Parse(config_.delegate_broadcast_pattern)) {
                delegate_helper = FindPattern(image_, *pattern, true);
                if (delegate_helper) {
                    log::Info(loc::Format(L"Log.Hooks.DelegatePatternResolved", {Hex(image_.ToRva(delegate_helper))}));
                }
            } else {
                log::Warn(loc::Text(L"Log.Hooks.DelegatePatternInvalid"));
            }
        }
        if (!delegate_helper) {
            delegate_helper = image_.FromRva(config_.delegate_broadcast_rva);
            if (delegate_helper) {
                log::Info(loc::Format(L"Log.Hooks.DelegateRvaResolved", {Hex(config_.delegate_broadcast_rva)}));
            }
        }
        const bool helper_valid = delegate_helper && image_.IsExecutable(delegate_helper, 16);
        const bool offsets_valid = IsOffsetSane(config_.enabled_dispatcher_offset, 0x400) &&
                                   IsOffsetSane(config_.check_auth_code_dispatcher_offset, 0x400) &&
                                   IsOffsetSane(config_.refresh_access_token_dispatcher_offset, 0x400) &&
                                   IsOffsetSane(config_.switch_active_device_dispatcher_offset, 0x400) &&
                                   IsOffsetSane(config_.service_enabled_offset, 0x400);
        if (!helper_valid || !offsets_valid) {
            log::Error(loc::Text(L"Log.Hooks.DispatcherInvalid"));
            wrappers_.clear();
            return false;
        }
        delegate_broadcast_ = reinterpret_cast<DelegateBroadcastFunction>(delegate_helper);
        log::Info(config_.authentication_callback_mode == AuthenticationCallbackMode::Immediate
                      ? loc::Text(L"Log.Hooks.CallbackImmediate")
                      : loc::Text(L"Log.Hooks.CallbackDeferred"));
    } else {
        log::Warn(loc::Text(L"Log.Hooks.AuthDisabled"));
    }

    // SetDefaultVolume 和 SetVolumeOffset 带参数，因此 Hook 它们最终调用的
    // 原生 C++ 目标，让 UE 正常 exec 包装层继续负责读取和校验参数。
    void* default_volume_native = nullptr;
    void* volume_offset_native = nullptr;
    if (config_.hook_game_volume) {
        if (void* wrapper = ResolveWrapper("SetDefaultVolume")) {
            default_volume_native = FindLastCallTarget(static_cast<uint8_t*>(wrapper));
            log::Info(default_volume_native
                ? loc::Format(L"Log.Hooks.DefaultVolumeResolved", {Hex(image_.ToRva(default_volume_native))})
                : loc::Text(L"Log.Hooks.DefaultVolumeMissing"));
        }
        if (void* wrapper = ResolveWrapper("SetVolumeOffset")) {
            volume_offset_native = FindLastCallTarget(static_cast<uint8_t*>(wrapper));
            log::Info(volume_offset_native
                ? loc::Format(L"Log.Hooks.OffsetVolumeResolved", {Hex(image_.ToRva(volume_offset_native))})
                : loc::Text(L"Log.Hooks.OffsetVolumeMissing"));
        }
    }

    if (config_.dry_run_hooks) {
        log::Warn(loc::Text(L"Log.Hooks.DryRun"));
        wrappers_.clear();
        return true;
    }

    g_active_hooks.store(this, std::memory_order_release);
    bool all_installed = true;
    for (const auto& request : exec_requests) {
        if (!request.resolved) {
            continue;
        }
        if ((request.name == "GetPlayingArtistName" || request.name == "GetPlayingTrackName") && !string_copy_) {
            continue;
        }
        all_installed = InstallPatch(request.resolved, request.replacement, loc::Format(L"Log.Hooks.ExecPatchLabel", {Widen(request.name)})) && all_installed;
    }
    for (const auto& request : native_requests) {
        if (!request.resolved) {
            continue;
        }
        all_installed = InstallPatch(request.resolved, request.replacement, loc::Format(L"Log.Hooks.NativePatchLabel", {Widen(request.name)})) && all_installed;
    }
    if (default_volume_native) {
        // SetDefaultVolume 先执行游戏原逻辑，使游戏自己的设置和存档路径保持权威。
        // 跳板长度基于已验证的 2020 版 EXE 函数序言；配置变化时音量 Hook 会
        // 安全失败，而不会盲目覆盖指令流。
        all_installed = InstallTrampolinePatch(default_volume_native, reinterpret_cast<void*>(&HookSetDefaultVolume), 15,
                                               reinterpret_cast<void**>(&original_set_default_volume_),
                                               loc::Text(L"Log.Hooks.DefaultVolumePatchLabel")) && all_installed;
    }
    if (volume_offset_native) {
        // SetVolumeOffset 是原版的网络/应用路径。替换它可阻止真实 Spotify API 调用，
        // 同时 SetDefaultVolume 原逻辑仍可先更新游戏自身设置，再尾跳到这里。
        all_installed = InstallPatch(volume_offset_native, reinterpret_cast<void*>(&HookSetVolumeOffset), loc::Text(L"Log.Hooks.OffsetVolumePatchLabel")) && all_installed;
    }

    if (!all_installed && config_.require_all_hooks) {
        log::Error(loc::Text(L"Log.Hooks.PatchWriteFailed"));
        Remove();
        return false;
    }
    installed_ = !patches_.empty();
    if (installed_) {
        log::Info(config_.bypass_spotify_authentication
                      ? loc::Text(config_.authentication_callback_mode == AuthenticationCallbackMode::Immediate
                                      ? L"Log.Hooks.InstalledImmediate"
                                      : L"Log.Hooks.InstalledDeferred")
                      : loc::Text(L"Log.Hooks.InstalledNoAuth"));
    }
    return installed_;
}

// 按逆序恢复所有 Hook/跳板并清空活动实例。
// 只在工作线程退出阶段执行，防止游戏线程跳入已释放代码。
void GameHooks::Remove() {
    for (auto iterator = patches_.rbegin(); iterator != patches_.rend(); ++iterator) {
        (*iterator)->Remove();
    }
    g_active_hooks.store(nullptr, std::memory_order_release);
    patches_.clear();
    wrappers_.clear();
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        pending_callback_queue_.clear();
    }
    service_object_.store(nullptr, std::memory_order_release);
    update_seen_.store(false, std::memory_order_release);
    dispatching_callbacks_.store(false, std::memory_order_release);
    delegate_broadcast_ = nullptr;
    installed_ = false;
}

// 比较 PE 时间戳、映像大小和可选 SHA-256。
// StrictVersion=false 仍要求后续函数结构验证，不等于盲目打补丁。
bool GameHooks::ValidateExecutableVersion() {
    const uint32_t actual_timestamp = image_.Timestamp();
    const uint32_t actual_size = static_cast<uint32_t>(image_.SizeOfImage());
    log::Info(loc::Format(L"Log.Hooks.ExeProfile", {Hex(actual_timestamp), Hex(actual_size)}));

    bool matches = true;
    if (config_.expected_timestamp != 0 && config_.expected_timestamp != actual_timestamp) {
        matches = false;
        log::Warn(loc::Text(L"Log.Hooks.TimestampMismatch"));
    }
    if (config_.expected_image_size != 0 && config_.expected_image_size != actual_size) {
        matches = false;
        log::Warn(loc::Text(L"Log.Hooks.ImageSizeMismatch"));
    }
    const bool should_hash = config_.calculate_sha256 || (config_.strict_version && !config_.expected_sha256.empty());
    if (should_hash) {
        const std::wstring digest = ToLower(ComputeFileSha256(GetModulePath(GetModuleHandleW(nullptr))));
        if (digest.empty()) {
            log::Warn(loc::Text(L"Log.Hooks.ShaFailed"));
            matches = config_.expected_sha256.empty() && matches;
        } else {
            log::Info(loc::Format(L"Log.Hooks.Sha", {digest}));
            if (!config_.expected_sha256.empty() && digest != config_.expected_sha256) {
                matches = false;
                log::Warn(loc::Text(L"Log.Hooks.ShaMismatch"));
            }
        }
    }
    if (!matches && config_.strict_version) {
        log::Error(loc::Text(L"Log.Hooks.StrictMismatch"));
        return false;
    }
    if (!matches) {
        log::Warn(loc::Text(L"Log.Hooks.UnknownProfile"));
    }
    return true;
}

// 按注册表、特征码、RVA 的顺序解析一个 UE4 exec 包装函数。
// 集中执行日志和最终验证，避免各 Hook 使用不同信任标准。
void* GameHooks::ResolveWrapper(const std::string& name) const {
    if (void* result = ResolveByNameTable(name)) {
        return result;
    }
    if (void* result = ResolveByPattern(name)) {
        return result;
    }
    return ResolveByRva(name);
}

// 通过 UE4 原生函数注册表定位指定 exec 包装函数。
// 这是首选方式，候选仍需通过 LooksLikeExecWrapper 验证。
void* GameHooks::ResolveByNameTable(const std::string& name) const {
    auto references_for = [&](std::string_view entry_name) {
        std::vector<uint8_t*> references;
        for (uint8_t* string_address : image_.FindAscii(entry_name, true)) {
            const auto found = image_.FindPointerReferences(string_address);
            references.insert(references.end(), found.begin(), found.end());
        }
        return references;
    };

    auto wrapper_after_name = [&](uint8_t* reference) -> uint8_t* {
        if (!reference) {
            return nullptr;
        }
        // UE4 原生注册数组保存 {ANSI 名称指针, exec 指针}。只能读取名称之后的槽位；
        // 若向前读取，在名称连续排列时可能静默解析成前一个函数。
        uint8_t* slot = reference + sizeof(uintptr_t);
        if (!image_.IsReadable(slot, sizeof(uintptr_t))) {
            return nullptr;
        }
        uintptr_t candidate_value = 0;
        std::memcpy(&candidate_value, slot, sizeof(candidate_value));
        auto* candidate = reinterpret_cast<uint8_t*>(candidate_value);
        return LooksLikeExecWrapper(candidate) ? candidate : nullptr;
    };

    const auto references = references_for(name);
    if (references.empty()) {
        return nullptr;
    }

    // “Play”和“Pause”会出现在许多无关 UE 类中，因此必须要求它们与唯一的
    // SpotifyService 项位于同一个紧凑注册表邻域，避免 Hook 错误对象。
    const bool generic_name = name == "Play" || name == "Pause";
    std::vector<uint8_t*> spotify_anchors;
    if (generic_name) {
        for (const std::string_view anchor : {"HasValidAccessToken", "NextTrack", "Unpause", "Update"}) {
            const auto found = references_for(anchor);
            spotify_anchors.insert(spotify_anchors.end(), found.begin(), found.end());
        }
    }

    for (uint8_t* reference : references) {
        if (generic_name) {
            const uintptr_t current = reinterpret_cast<uintptr_t>(reference);
            const bool near_spotify_table = std::any_of(spotify_anchors.begin(), spotify_anchors.end(), [&](uint8_t* anchor) {
                const uintptr_t other = reinterpret_cast<uintptr_t>(anchor);
                const uintptr_t distance = current > other ? current - other : other - current;
                return distance <= 0x200;
            });
            if (!near_spotify_table) {
                continue;
            }
        }
        if (uint8_t* candidate = wrapper_after_name(reference)) {
            return candidate;
        }
    }
    return nullptr;
}

// 按 INI 特征码扫描指定包装函数。
// 仅作为注册表定位失败时的兼容回退。
void* GameHooks::ResolveByPattern(const std::string& name) const {
    const std::wstring key = Widen(name);
    const std::wstring raw = Trim(ReadIniString(config_.ini_path, L"Patterns", key));
    if (raw.empty()) {
        return nullptr;
    }
    const auto parsed = BytePattern::Parse(raw);
    if (!parsed) {
        log::Warn(loc::Format(L"Log.Hooks.InvalidPattern", {key}));
        return nullptr;
    }
    auto* match = static_cast<uint8_t*>(FindPattern(image_, *parsed, true));
    if (!match) {
        return nullptr;
    }
    const intptr_t adjustment = static_cast<intptr_t>(ReadIniInteger(config_.ini_path, L"PatternOffsets", key).value_or(0));
    uint8_t* candidate = match + adjustment;
    return LooksLikeExecWrapper(candidate) ? candidate : nullptr;
}

// 按配置中的已知 RVA 获取包装函数候选。
// 地址必须在可执行节且结构验证通过才会返回。
void* GameHooks::ResolveByRva(const std::string& name) const {
    const auto rva = ReadIniInteger(config_.ini_path, L"RVA", Widen(name));
    if (!rva) {
        return nullptr;
    }
    void* candidate = image_.FromRva(*rva);
    return LooksLikeExecWrapper(static_cast<uint8_t*>(candidate)) ? candidate : nullptr;
}

// 检查候选代码是否符合 UE4 Blueprint exec 包装函数特征。
// 验证指令范围和 FFrame 访问，减少未知版本误匹配。
bool GameHooks::LooksLikeExecWrapper(const uint8_t* address) const {
    if (!address || !image_.IsExecutable(address, 16)) {
        return false;
    }
    // UE4 原生 exec 包装函数会通过 RDX 操作 FFrame::Code；同一邻域的注册辅助函数
    // 不会这样做，因此可借此区分两类表项。
    const size_t scan = 128;
    if (!image_.IsExecutable(address, scan)) {
        return false;
    }
    for (size_t i = 0; i + 4 <= scan; ++i) {
        if ((address[i] == 0x48 && address[i + 1] == 0x8B && address[i + 2] == 0x42) ||
            (address[i] == 0x48 && address[i + 1] == 0x39 && address[i + 2] == 0x7A)) {
            const uint8_t displacement = address[i + 3];
            if (displacement <= 0x80) {
                return true;
            }
        }
        if (i + 7 <= scan &&
            ((address[i] == 0x48 && address[i + 1] == 0x8B && address[i + 2] == 0x82) ||
             (address[i] == 0x48 && address[i + 1] == 0x39 && address[i + 2] == 0xBA))) {
            uint32_t displacement = 0;
            std::memcpy(&displacement, address + i + 3, sizeof(displacement));
            if (displacement <= 0x200) {
                return true;
            }
        }
    }
    return false;
}

// 从已验证包装函数推断 FFrame::Code 字段偏移。
// 推断失败返回 -1，安装流程不会用猜测值继续写栈。
int GameHooks::InferFrameCodeOffset(const uint8_t* wrapper) const {
    if (!wrapper || !image_.IsExecutable(wrapper, 64)) {
        return -1;
    }
    for (size_t i = 0; i + 4 <= 64; ++i) {
        if ((wrapper[i] == 0x48 && wrapper[i + 1] == 0x8B && wrapper[i + 2] == 0x42) ||
            (wrapper[i] == 0x48 && wrapper[i + 1] == 0x39 && wrapper[i + 2] == 0x7A)) {
            return wrapper[i + 3];
        }
        if (i + 7 <= 64 &&
            ((wrapper[i] == 0x48 && wrapper[i + 1] == 0x8B && wrapper[i + 2] == 0x82) ||
             (wrapper[i] == 0x48 && wrapper[i + 1] == 0x39 && wrapper[i + 2] == 0xBA))) {
            uint32_t displacement = 0;
            std::memcpy(&displacement, wrapper + i + 3, sizeof(displacement));
            return displacement <= static_cast<uint32_t>(std::numeric_limits<int>::max())
                       ? static_cast<int>(displacement)
                       : -1;
        }
    }
    return -1;
}

// 在包装函数尾部寻找跳转到原生实现的目标。
// 只接受映像内可执行地址，用于无参数原生函数 Hook。
void* GameHooks::FindTailJumpTarget(const uint8_t* wrapper, size_t search_length) const {
    if (!wrapper || !image_.IsExecutable(wrapper, search_length)) {
        return nullptr;
    }
    for (size_t i = 0; i + 5 <= search_length; ++i) {
        if (wrapper[i] == 0xE9) {
            return ResolveRelativeTarget(image_, wrapper + i, 1, 5);
        }
        if (wrapper[i] == 0xC3 || (i + 1 < search_length && wrapper[i] == 0xCC && wrapper[i + 1] == 0xCC)) {
            break;
        }
    }
    return nullptr;
}

// 扫描包装函数末段最后一个有效相对 call 目标。
// 用于参数包装函数，让 UE 正常消费参数后再 Hook 原生目标。
void* GameHooks::FindLastCallTarget(const uint8_t* wrapper, size_t search_length) const {
    if (!wrapper || !image_.IsExecutable(wrapper, search_length)) {
        return nullptr;
    }
    void* last = nullptr;
    for (size_t i = 0; i + 5 <= search_length; ++i) {
        if (wrapper[i] == 0xE8) {
            if (void* target = ResolveRelativeTarget(image_, wrapper + i, 1, 5)) {
                last = target;
            }
            i += 4;
            continue;
        }
        if (wrapper[i] == 0xC3) {
            break;
        }
    }
    return last;
}

// 安装不需要调用原函数的绝对跳转补丁并登记所有权。
// 失败时不把未安装对象加入列表，便于统一回滚。
bool GameHooks::InstallPatch(void* target, void* replacement, const std::wstring& label) {
    auto patch = std::make_unique<AbsoluteJumpPatch>();
    if (!patch->Install(target, replacement)) {
        log::Error(loc::Format(L"Log.Hooks.PatchInstallFailed", {label}));
        return false;
    }
    patches_.push_back(std::move(patch));
    return true;
}

// 安装可调用原函数的跳板补丁并返回 trampoline。
// 游戏持久设置等需要保留原逻辑的入口使用该方式。
bool GameHooks::InstallTrampolinePatch(void* target, void* replacement, size_t copy_size, void** trampoline, const std::wstring& label) {
    auto patch = std::make_unique<TrampolinePatch>();
    if (!patch->Install(target, replacement, copy_size)) {
        log::Error(loc::Format(L"Log.Hooks.PatchInstallFailed", {label}));
        return false;
    }
    if (trampoline) {
        *trampoline = patch->Trampoline();
    }
    trampoline_patches_.push_back(std::move(patch));
    return true;
}

// 推进 UE4 FFrame 字节码指针，模拟原 exec 包装函数已消费调用。
// 返回 Getter 结果后必须完成该步骤，否则 Blueprint VM 会重复解析参数。
void GameHooks::FinishFrame(void* stack) const {
    if (!stack || !IsOffsetSane(frame_code_offset_, 0x200)) {
        return;
    }
    auto** code = reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(stack) + frame_code_offset_);
    if (!IsWritableAddress(code, sizeof(*code))) {
        return;
    }
    uint8_t* current = *code;
    if (current && IsReadableAddress(current)) {
        // 无参数 UE4 exec 包装函数会消费 EX_EndFunctionParms 字节。
        *code = current + 1;
    }
}

// 向 Blueprint result 缓冲区写入布尔返回值并结束当前 frame。
// 写入前验证指针可写，避免损坏 UE VM 状态。
void GameHooks::ReturnBool(void* stack, void* result, bool value) {
    FinishFrame(stack);
    if (result && IsWritableAddress(result, 1)) {
        *static_cast<uint8_t*>(result) = value ? 1 : 0;
    }
}

// 通过已解析的 FString 复制辅助函数写回 Unicode 字符串。
// 失败时返回空字符串并保持 VM 可继续执行。
void GameHooks::ReturnString(void* stack, void* result, const std::wstring& value) {
    FinishFrame(stack);
    if (!result || !string_copy_) {
        return;
    }
    const wchar_t* text = value.empty() ? L"" : value.c_str();
    const size_t length = std::min<size_t>(value.size(), static_cast<size_t>(std::numeric_limits<int32_t>::max() - 1));
    FStringProxy source{
        const_cast<wchar_t*>(text),
        static_cast<int32_t>(length + 1),
        static_cast<int32_t>(length + 1),
    };
    string_copy_(result, &source);
}

// 替换原版 Spotify Getter HookHasValidAccessToken，把本地后端状态写回 Blueprint。
// 不访问网络或游戏凭据，并正确推进 FFrame。
void __fastcall GameHooks::HookHasValidAccessToken(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        const bool enabled = LocalPlayer::Instance().IsSpotifyReplacementEnabled();
        self->ReturnBool(stack, result, enabled);
        if (enabled) self->PumpPendingCallbacks(service, L"HasValidAccessToken");
    }
}
// 替换原版 Spotify Getter HookHasValidAccessTokenAndDevice，把本地后端状态写回 Blueprint。
// 不访问网络或游戏凭据，并正确推进 FFrame。
void __fastcall GameHooks::HookHasValidAccessTokenAndDevice(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        const bool enabled = LocalPlayer::Instance().IsSpotifyReplacementEnabled();
        self->ReturnBool(stack, result, enabled);
        if (enabled) self->PumpPendingCallbacks(service, L"HasValidAccessTokenAndDevice");
    }
}
// 替换原版 Spotify Getter HookIsEnabled，把本地后端状态写回 Blueprint。
// 不访问网络或游戏凭据，并正确推进 FFrame。
void __fastcall GameHooks::HookIsEnabled(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        const bool enabled = LocalPlayer::Instance().IsSpotifyReplacementEnabled();
        self->ReturnBool(stack, result, enabled);
        if (enabled) self->PumpPendingCallbacks(service, L"IsEnabled");
    }
}
// 替换原版 Spotify Getter HookIsPaused，把本地后端状态写回 Blueprint。
// 不访问网络或游戏凭据，并正确推进 FFrame。
void __fastcall GameHooks::HookIsPaused(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        self->ReturnBool(stack, result, LocalPlayer::Instance().IsPaused());
        self->PumpPendingCallbacks(service, L"IsPaused");
    }
}
// 替换原版当前曲目信息 Getter HookGetPlayingArtistName，返回本地元数据。
// 使用 FString 辅助函数保持 Unicode 标题和艺术家。
void __fastcall GameHooks::HookGetPlayingArtistName(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        self->ReturnString(stack, result, LocalPlayer::Instance().Artist());
        self->PumpPendingCallbacks(service, L"GetPlayingArtistName");
    }
}
// 替换原版当前曲目信息 Getter HookGetPlayingTrackName，返回本地元数据。
// 使用 FString 辅助函数保持 Unicode 标题和艺术家。
void __fastcall GameHooks::HookGetPlayingTrackName(void* service, void* stack, void* result) {
    if (auto* self = Active()) {
        self->ReturnString(stack, result, LocalPlayer::Instance().Title());
        self->PumpPendingCallbacks(service, L"GetPlayingTrackName");
    }
}

// 登记一个待广播的认证/设备回调。
// 去重同类请求，避免游戏轮询产生重复委托。
void GameHooks::QueueCallback(void* service, uint32_t callback, const wchar_t* label) {
    if (!service) {
        log::Warn(loc::Format(L"Log.Hooks.EmptyService", {label}));
        return;
    }
    service_object_.store(service, std::memory_order_release);

    if (config_.authentication_callback_mode == AuthenticationCallbackMode::Immediate) {
        bool expected = false;
        if (dispatching_callbacks_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (config_.verbose_auth_callbacks) {
                log::Info(loc::Format(L"Log.Hooks.DispatchImmediate", {label}));
            }
            DispatchCallback(service, callback);
            dispatching_callbacks_.store(false, std::memory_order_release);
            // 继续清空因委托同步重入 Hook 方法而新加入的回调。
            DispatchPendingCallbacks(service, loc::Text(L"Log.Hooks.ImmediateCleanup").c_str());
        } else {
            std::lock_guard<std::mutex> lock(callback_mutex_);
            if (std::find(pending_callback_queue_.begin(), pending_callback_queue_.end(), callback) ==
                pending_callback_queue_.end()) {
                pending_callback_queue_.push_back(callback);
            }
        }
        return;
    }

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (std::find(pending_callback_queue_.begin(), pending_callback_queue_.end(), callback) ==
            pending_callback_queue_.end()) {
            pending_callback_queue_.push_back(callback);
            queued = true;
        }
    }
    if (config_.verbose_auth_callbacks) {
        log::Info(loc::Format(queued ? L"Log.Hooks.CallbackQueued" : L"Log.Hooks.CallbackMerged", {label}));
    }

    // 延迟模式刻意避免在当前请求调用栈中广播。立即模式是默认值，因为真实存档下
    // 游戏加载界面对延迟认证完成的等待时间过长。
}

// 向指定服务对象偏移处的 UE 动态多播委托广播一个布尔值。
// 委托函数或字段无效时只记录警告，不调用未知地址。
void GameHooks::BroadcastBool(void* service, int offset, bool value, const wchar_t* label) {
    if (!service || !delegate_broadcast_ || !IsOffsetSane(offset, 0x400)) {
        log::Warn(loc::Format(L"Log.Hooks.BroadcastFailed", {label}));
        return;
    }
    uint8_t parameter = value ? 1 : 0;
    delegate_broadcast_(static_cast<uint8_t*>(service) + offset, &parameter);
    if (config_.verbose_auth_callbacks) {
        log::Info(loc::Format(L"Log.Hooks.CallbackBroadcast", {label, loc::Text(value ? L"General.Yes" : L"General.No")}));
    }
}

// 构造并广播设备切换结果。
// 本地后端始终在内存中报告成功，不联系 Spotify 网络。
void GameHooks::BroadcastSwitchDevice(void* service, bool switched, bool active) {
    if (!service || !delegate_broadcast_ || !IsOffsetSane(config_.switch_active_device_dispatcher_offset, 0x400)) {
        log::Warn(loc::Text(L"Log.Hooks.SwitchBroadcastFailed"));
        return;
    }
    struct SwitchDeviceParameters {
        uint8_t switched_device;
        uint8_t has_active_device;
    } parameters{switched ? uint8_t{1} : uint8_t{0}, active ? uint8_t{1} : uint8_t{0}};
    delegate_broadcast_(static_cast<uint8_t*>(service) + config_.switch_active_device_dispatcher_offset, &parameters);
    if (config_.verbose_auth_callbacks) {
        log::Info(loc::Format(L"Log.Hooks.SwitchBroadcast", {
            loc::Text(switched ? L"General.Yes" : L"General.No"),
            loc::Text(active ? L"General.Yes" : L"General.No")}));
    }
}

// 立即派发当前服务对象已排队的全部回调。
// 返回是否实际派发，供日志和触发策略判断。
bool GameHooks::DispatchPendingCallbacks(void* service, const wchar_t* trigger) {
    bool expected = false;
    if (!dispatching_callbacks_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }

    if (!service) {
        service = service_object_.load(std::memory_order_acquire);
    } else {
        service_object_.store(service, std::memory_order_release);
    }
    if (!service) {
        dispatching_callbacks_.store(false, std::memory_order_release);
        return false;
    }

    // 广播前先把有序批次移出队列。若 dispatcher 同步调用另一个已 Hook 的 Spotify
    // 方法，新请求会追加到已经清空的队列中，留待后续安全入口处理。
    std::vector<uint32_t> pending;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        pending.swap(pending_callback_queue_);
    }
    if (pending.empty()) {
        dispatching_callbacks_.store(false, std::memory_order_release);
        return false;
    }

    if (config_.verbose_auth_callbacks) {
        log::Info(loc::Format(L"Log.Hooks.ProcessingQueue", {
            trigger ? std::wstring(trigger) : loc::Text(L"Log.Hooks.UnknownEntry")}));
    }

    for (const uint32_t callback : pending) {
        DispatchCallback(service, callback);
    }

    dispatching_callbacks_.store(false, std::memory_order_release);
    return true;
}

// 按 Deferred 模式在 Update 中推进待回调队列。
// Immediate 模式下通常没有剩余事件。
void GameHooks::PumpPendingCallbacks(void* service, const wchar_t* trigger) {
    if (config_.authentication_callback_mode == AuthenticationCallbackMode::Deferred) {
        DispatchPendingCallbacks(service, trigger);
    }
}

// 按回调类型选择对应 UE 多播委托并写入本地成功结果。
// 所有字段偏移都先做范围与可读性检查。
void GameHooks::DispatchCallback(void* service, uint32_t callback) {
    switch (callback) {
    case PendingEnabled:
        if (IsOffsetSane(config_.service_enabled_offset, 0x400)) {
            auto* enabled = static_cast<uint8_t*>(service) + config_.service_enabled_offset;
            if (IsWritableAddress(enabled, 1)) {
                *enabled = 1;
            }
        }
        BroadcastBool(service, config_.enabled_dispatcher_offset, true, L"EnabledDispatcher");
        break;
    case PendingCheckAuthCode:
        BroadcastBool(service, config_.check_auth_code_dispatcher_offset, true, L"CheckAuthCodeDispatcher");
        break;
    case PendingRefreshAccessToken:
        BroadcastBool(service, config_.refresh_access_token_dispatcher_offset, true, L"RefreshAccessTokenDispatcher");
        break;
    case PendingSwitchActiveDevice:
        BroadcastSwitchDevice(service);
        break;
    default:
        log::Warn(loc::Text(L"Log.Hooks.UnknownCallback"));
        break;
    }
}

// 拦截游戏原生 Spotify 操作 HookNativeCheckEnabled 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeCheckEnabled(void* service) {
    if (auto* self = Active()) {
        if (!LocalPlayer::Instance().IsSpotifyReplacementEnabled()) {
            if (self->delegate_broadcast_) self->BroadcastBool(service, self->config_.enabled_dispatcher_offset, false, L"EnabledDispatcher");
            return;
        }
        self->PumpPendingCallbacks(service, loc::Format(L"Log.Hooks.RetryEntry", {L"CheckEnabled"}).c_str());
        self->QueueCallback(service, PendingEnabled, L"CheckEnabled");
    }
}
// 拦截游戏原生 Spotify 操作 HookNativeCheckAuthCode 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeCheckAuthCode(void* service) {
    LocalPlayer::Instance().SetSpotifyReplacementEnabled(true, L"CheckAuthCode", true);
    if (auto* self = Active()) {
        self->PumpPendingCallbacks(service, loc::Format(L"Log.Hooks.RetryEntry", {L"CheckAuthCode"}).c_str());
        self->QueueCallback(service, PendingCheckAuthCode, L"CheckAuthCode");
        self->QueueCallback(service, PendingEnabled, L"CheckEnabled");
        self->QueueCallback(service, PendingRefreshAccessToken, L"RefreshAccessToken");
        self->QueueCallback(service, PendingSwitchActiveDevice, L"SwitchActiveDevice");
    }
}
// 拦截游戏原生 Spotify 操作 HookNativeClearAccessTokens 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeClearAccessTokens(void* service) {
    // Dangerous Driving 的正常账号界面可能清除 Spotify 凭据。本插件不是账号实现，
    // 而是本地音乐后端，因此不应关闭本地播放，也绝不能修改 SaveGame.sav。
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"ClearAccessTokens");
    log::Info(loc::Text(L"Log.Hooks.ClearIgnored"));
}
// 拦截游戏原生 Spotify 操作 HookNativeRefreshAccessToken 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeRefreshAccessToken(void* service) {
    if (auto* self = Active()) {
        if (!LocalPlayer::Instance().IsSpotifyReplacementEnabled()) {
            if (self->delegate_broadcast_) self->BroadcastBool(service, self->config_.refresh_access_token_dispatcher_offset, false, L"RefreshAccessTokenDispatcher");
            return;
        }
        self->PumpPendingCallbacks(service, loc::Format(L"Log.Hooks.RetryEntry", {L"RefreshAccessToken"}).c_str());
        self->QueueCallback(service, PendingRefreshAccessToken, L"RefreshAccessToken");
    }
}
// 拦截游戏原生 Spotify 操作 HookNativeRequestActivateCode 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeRequestActivateCode(void* service) {
    LocalPlayer::Instance().SetSpotifyReplacementEnabled(true, L"RequestActivateCode", true);
    if (auto* self = Active()) {
        self->PumpPendingCallbacks(service, L"RequestActivateCode");
        self->QueueCallback(service, PendingEnabled, L"CheckEnabled");
        self->QueueCallback(service, PendingCheckAuthCode, L"CheckAuthCode");
        self->QueueCallback(service, PendingRefreshAccessToken, L"RefreshAccessToken");
        self->QueueCallback(service, PendingSwitchActiveDevice, L"SwitchActiveDevice");
    }
    log::Info(loc::Text(L"Log.Hooks.ActivationAccepted"));
}
// 拦截游戏原生 Spotify 操作 HookNativeSwitchActiveDevice 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeSwitchActiveDevice(void* service) {
    if (auto* self = Active()) {
        if (!LocalPlayer::Instance().IsSpotifyReplacementEnabled()) {
            if (self->delegate_broadcast_) self->BroadcastSwitchDevice(service, false, false); // 未激活路径仍需完成等待，但不能进入 Spotify 模式。
            return;
        }
        self->PumpPendingCallbacks(service, loc::Format(L"Log.Hooks.RetryEntry", {L"SwitchActiveDevice"}).c_str());
        self->QueueCallback(service, PendingSwitchActiveDevice, L"SwitchActiveDevice");
    }
}
// 拦截游戏原生 Spotify 操作 HookNativeUpdate 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeUpdate(void* service) {
    if (auto* self = Active()) {
        if (!self->update_seen_.exchange(true, std::memory_order_acq_rel) && self->config_.verbose_auth_callbacks) {
            log::Info(loc::Text(L"Log.Hooks.UpdateActive"));
        }
        self->PumpPendingCallbacks(service, L"SpotifyService::Update");
    }
}
// 拦截游戏原生 Spotify 操作 HookNativeNextTrack 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeNextTrack(void* service) {
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"NextTrack");
    if (AlbumControls::ShouldBlockGameMediaCommand()) return;
    LocalPlayer::Instance().NextFromGame();
}
// 拦截游戏原生 Spotify 操作 HookNativePause 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativePause(void* service) {
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"Pause");
    if (AlbumControls::ShouldBlockGameMediaCommand()) return;
    LocalPlayer::Instance().Pause();
}
// 拦截游戏原生 Spotify 操作 HookNativePlay 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativePlay(void* service) {
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"Play");
    if (AlbumControls::ShouldBlockGameMediaCommand()) return;
    LocalPlayer::Instance().Play();
}
// 拦截游戏原生 Spotify 操作 HookNativePreviousTrack 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativePreviousTrack(void* service) {
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"PreviousTrack");
    if (AlbumControls::ShouldBlockGameMediaCommand()) return;
    LocalPlayer::Instance().PreviousFromGame();
}
// 拦截游戏原生 Spotify 操作 HookNativeUnpause 并映射到本地后端/认证回调。
// 根据入口职责保留必要原逻辑或完全阻断网络访问。
void __fastcall GameHooks::HookNativeUnpause(void* service) {
    if (auto* self = Active()) self->PumpPendingCallbacks(service, L"Unpause");
    if (AlbumControls::ShouldBlockGameMediaCommand()) return;
    LocalPlayer::Instance().Resume();
}
// 先调用游戏原始持久音量逻辑，再把同一百分比交给本地播放器。
// 使用 NativeVolumeFunction 跳板保持游戏设置保存行为。
void __fastcall GameHooks::HookSetDefaultVolume(void* service, int value) {
    // 游戏持久 Spotify 设置是游戏侧倍率；INI Volume 始终是插件侧倍率。
    // 原游戏函数仍负责 UI 和存档行为。
    LocalPlayer::Instance().SetDefaultVolume(value);
    if (auto* self = Active(); self && self->original_set_default_volume_) {
        self->original_set_default_volume_(service, value);
    }
}
// 拦截游戏临时音量 offset 并通知本地播放器记录/忽略。
// 不让关卡 ducking 改变发行版的稳定听感。
void __fastcall GameHooks::HookSetVolumeOffset(void* service, int value) {
    LocalPlayer::Instance().SetVolumeOffset(value);
    // 游戏临时 offset 对本地播放刻意忽略。进入关卡时必须保持稳定，
    // 不能让替代音乐因为临时状态值突然变大或变小。
    constexpr int kServiceRuntimeVolumeOffset = 0x110;
    if (service) {
        auto* field = static_cast<uint8_t*>(service) + kServiceRuntimeVolumeOffset;
        if (!IsWritableAddress(field, sizeof(int))) {
            return;
        }
        // 这是“偏移”字段，不是绝对音量字段。旧实现曾把持久百分比（例如 50）写入此处，
        // 可能被游戏短暂解释为 +50，从而造成音量突然变大；正确中性值必须是 0。
        constexpr int kNeutralOffset = 0;
        std::memcpy(field, &kNeutralOffset, sizeof(kNeutralOffset));
    }
}

}  // namespace localmusic
