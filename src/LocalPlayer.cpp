// 本地播放器实现。关键设计：FMOD 回调线程不直接切歌；旧 Channel 使用播放代数隔离；
// INI Volume 与游戏持久音量相乘，临时 SetVolumeOffset 永远不参与最终输出。
#include "LocalPlayer.h"
#include "Logger.h"
#include "Localization.h"
#include "NowPlayingOverlay.h"

namespace localmusic {
namespace {

// Windows 文件系统排序本身是纯字典序，会把 10 放在 2 前面。
// 用户可通过文件夹名和曲目文件名的数字前缀控制顺序，因此这里实现轻量自然排序。
// 比较两个显示/路径文本，数字片段按数值顺序处理，其余字符忽略大小写比较。
// 例如 2 排在 10 前，数值相同时短数字串排在带前导零的形式之前。
bool NaturalTextLess(const std::wstring& left, const std::wstring& right) {
    size_t i = 0;
    size_t j = 0;
    const std::wstring a = ToLower(left);
    const std::wstring b = ToLower(right);
    while (i < a.size() && j < b.size()) {
        if (iswdigit(a[i]) && iswdigit(b[j])) {
            size_t ai = i;
            size_t bj = j;
            while (ai < a.size() && a[ai] == L'0') ++ai;
            while (bj < b.size() && b[bj] == L'0') ++bj;
            size_t ae = ai;
            size_t be = bj;
            while (ae < a.size() && iswdigit(a[ae])) ++ae;
            while (be < b.size() && iswdigit(b[be])) ++be;
            const size_t a_digits = ae - ai;
            const size_t b_digits = be - bj;
            if (a_digits != b_digits) return a_digits < b_digits;
            const int compare = a.compare(ai, a_digits, b, bj, b_digits);
            if (compare != 0) return compare < 0;
            // 数值相同则较短的原始数字串优先，例如 2 在 002 前面。
            const size_t a_full = ae - i;
            const size_t b_full = be - j;
            if (a_full != b_full) return a_full < b_full;
            i = ae;
            j = be;
            continue;
        }
        if (a[i] != b[j]) return a[i] < b[j];
        ++i;
        ++j;
    }
    return a.size() < b.size();
}

}  // namespace

// 返回本地播放器进程级单例。
// 所有游戏 Hook、输入层和工作线程通过同一实例访问受锁保护的播放状态。
LocalPlayer& LocalPlayer::Instance() {
    static LocalPlayer instance;
    return instance;
}

// 复制配置、加载 FMOD、创建 System、扫描曲库并建立初始播放队列。
// 失败时按已创建资源逆序清理，不让半初始化状态进入 Update。
bool LocalPlayer::Initialize(const Config& config, const std::filesystem::path& dll_directory) {
    std::lock_guard lock(mutex_);
    if (initialized_) {
        return true;
    }
    config_ = config;
    // INI 的 Volume 是永久插件倍率。游戏侧倍率初始化为中性 100%，
    // 随后由 SetDefaultVolume 发布用户在游戏中保存的 Spotify 音量。
    game_default_volume_ = 100;
    pending_game_volume_ = -1;
    pending_game_volume_since_ms_ = 0;
    last_ignored_volume_offset_ = 0;
    ended_channel_generation_.store(0, std::memory_order_release);
    ended_channel_tick_ms_.store(0, std::memory_order_release);
    last_navigation_input_tick_ms_.store(0, std::memory_order_release);
    current_channel_generation_ = 0;
    callback_unavailable_logged_ = false;
    auto_start_from_volume_logged_ = false;
    navigation_skip_logged_ = false;

    if (!fmod_.Load(dll_directory)) {
        return false;
    }
    callback_api_active_.store(true, std::memory_order_release);
    FMOD_RESULT result = fmod_.SystemCreate(&system_);
    if (result != FmodApi::Ok || !system_) {
        LogFmodError(L"FMOD_System_Create", result);
        fmod_.Unload();
        return false;
    }
    result = fmod_.SystemInit(system_, 32, FmodApi::InitNormal, nullptr);
    if (result != FmodApi::Ok) {
        LogFmodError(L"FMOD_System_Init", result);
        fmod_.SystemRelease(system_);
        system_ = nullptr;
        fmod_.Unload();
        return false;
    }

    initialized_ = true;
    spotify_replacement_enabled_ = config_.local_spotify_replacement_enabled;
    playback_requested_ = false;
    paused_ = true;
    ignored_play_logged_ = false;
    ignored_skip_logged_ = false;
    ScanLocked();

    if (tracks_.empty()) {
        log::Warn(loc::Text(L"Log.Player.NoTracks"));
    } else {
        log::Info(spotify_replacement_enabled_
                      ? loc::Text(L"Log.Player.LibraryReadyEnabled")
                      : loc::Format(L"Log.Player.LibraryReadyDisabled", {config_.ini_path.filename().wstring()}));
    }
    return true;
}

// 停止 Channel、释放预加载/当前 Sound、关闭 FMOD System 并清空曲库状态。
// 持有播放器互斥锁，确保退出时没有其他控制操作并发修改资源。
void LocalPlayer::Shutdown() {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return;
    }
    callback_api_active_.store(false, std::memory_order_release);
    ReleaseSoundLocked();
    if (system_) {
        fmod_.SystemClose(system_);
        fmod_.SystemRelease(system_);
        system_ = nullptr;
    }
    fmod_.Unload();
    initialized_ = false;
    spotify_replacement_enabled_ = false;
    playback_requested_ = false;
    paused_ = true;
    ignored_play_logged_ = false;
    ignored_skip_logged_ = false;
    ended_channel_generation_.store(0, std::memory_order_release);
    ended_channel_tick_ms_.store(0, std::memory_order_release);
    last_navigation_input_tick_ms_.store(0, std::memory_order_release);
    pending_game_volume_ = -1;
    pending_game_volume_since_ms_ = 0;
    current_channel_generation_ = 0;
    tracks_.clear();
    albums_.clear();
    play_queue_.clear();
    queue_position_ = 0;
    index_ = 0;
    // 回调上下文会一直保留到单例生命周期结束。即使 ChannelStop 之后
    // 仍有回调正在退出，也绝不能让它访问已经释放的内存。
}

// 工作线程周期更新 FMOD、提交去抖音量并检测自然播放结束。
// END 回调只投递原子代数，本方法在安全线程中真正执行自动切歌。
void LocalPlayer::Update() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !system_) {
        return;
    }
    fmod_.SystemUpdate(system_);
    ApplyPendingGameVolumeLocked();
    if (!channel_ || paused_ || tracks_.empty() || !config_.auto_advance) {
        ended_channel_generation_.store(0, std::memory_order_release);
        ended_channel_tick_ms_.store(0, std::memory_order_release);
        return;
    }

    const uint64_t ended_generation = ended_channel_generation_.exchange(0, std::memory_order_acq_rel);
    const uint64_t callback_tick = ended_channel_tick_ms_.exchange(0, std::memory_order_acq_rel);
    if (ended_generation != 0 && ended_generation == current_channel_generation_) {
        const uint64_t now = GetTickCount64();
        const uint64_t delay = callback_tick != 0 && now >= callback_tick ? now - callback_tick : 0;
        const std::wstring reason = loc::Format(L"Log.Player.EndCallback", {std::to_wstring(delay)});
        AutoAdvanceLocked(reason.c_str());
        return;
    }

    if (ShouldAutoAdvanceLocked()) {
        AutoAdvanceLocked(loc::Text(L"Log.Player.EndPolling").c_str());
    }
}

// 综合 END 回调、IsPlaying、位置停滞和曲长判断当前曲目是否自然结束。
// 只在未暂停且 AutoAdvance 开启时成立，手动切歌不会被误判为 EOF。
bool LocalPlayer::ShouldAutoAdvanceLocked() {
    if (!channel_) {
        return false;
    }

    FMOD_BOOL playing = 0;
    const FMOD_RESULT playing_result = fmod_.ChannelIsPlaying(channel_, &playing);
    if (playing_result == FmodApi::Ok && !playing) {
        return true;
    }

    // 某些 FMOD 流/Channel 在文件结束后仍会持续报告 isPlaying=true，
    // 直到显式停止。因此备用路径还会比较播放位置与曲长，并检测尾部停滞。
    if (!sound_ || current_track_length_ms_ == 0) {
        return false;
    }

    unsigned int position_ms = 0;
    const FMOD_RESULT position_result = fmod_.ChannelGetPosition(channel_, &position_ms, FmodApi::TimeUnitMs);
    if (position_result != FmodApi::Ok) {
        return false;
    }

    constexpr unsigned int TailWindowMs = 250;
    constexpr unsigned int MovementToleranceMs = 2;
    constexpr int RequiredStallTicks = 4;

    if (position_ms + TailWindowMs < current_track_length_ms_) {
        last_position_ms_ = position_ms;
        near_end_stall_ticks_ = 0;
        return false;
    }

    if (position_ms >= current_track_length_ms_) {
        return true;
    }

    if (position_ms <= last_position_ms_ + MovementToleranceMs) {
        ++near_end_stall_ticks_;
    } else {
        near_end_stall_ticks_ = 0;
    }
    last_position_ms_ = position_ms;
    return near_end_stall_ticks_ >= RequiredStallTicks;
}

// 根据单曲循环或当前队列游标选择自然结束后的下一首。
// 记录触发原因并复用 StartTrackLocked，保证回调与备用检测行为一致。
void LocalPlayer::AutoAdvanceLocked(const wchar_t* reason) {
    log::Info(loc::Format(L"Log.Player.AutoNext", {reason ? std::wstring(reason) : loc::Text(L"Log.Player.TrackEnded")}));
    if (config_.play_mode == PlayMode::SingleLoop) {
        StartTrackLocked(index_, false);
    } else {
        AdvanceLocked(false);
    }
}

// 重新扫描音乐目录并尽量保留当前曲目和队列位置。
// 曲目被删除时选择安全替代项，曲库为空则停止播放而不影响游戏运行。
void LocalPlayer::Rescan() {
    std::lock_guard lock(mutex_);
    if (!initialized_) return;
    const std::filesystem::path previous_path =
        !tracks_.empty() && index_ < tracks_.size() ? tracks_[index_].path : std::filesystem::path{};
    ReleasePreloadedSoundLocked();
    ScanLocked();
    if (tracks_.empty()) {
        ReleaseSoundLocked();
        ++track_revision_;
        log::Warn(loc::Text(L"Log.Player.RescanEmpty"));
        return;
    }
    if (channel_ && tracks_[index_].path != previous_path) {
        StartTrackLocked(index_, paused_);
    } else if (playback_requested_ && !paused_ && !channel_) {
        StartTrackLocked(index_, false);
    } else {
        PreloadNextTrackLocked();
    }
}

// 响应本地或游戏播放请求：首次启动当前队列曲目，已有 Channel 则解除暂停。
// 只有 Spotify 替换启用且曲库非空时实际播放。
void LocalPlayer::Play() {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return;
    }
    if (!spotify_replacement_enabled_) {
        if (!ignored_play_logged_) {
            log::Info(loc::Format(L"Log.Player.PlayDisabled", {config_.ini_path.filename().wstring()}));
            ignored_play_logged_ = true;
        }
        return;
    }
    if (!config_.start_local_music_on_play) {
        if (!ignored_play_logged_) {
            log::Info(loc::Text(L"Log.Player.PlayOptionDisabled"));
            ignored_play_logged_ = true;
        }
        return;
    }
    const bool was_paused = paused_;
    playback_requested_ = true;
    paused_ = false;
    if (tracks_.empty()) {
        log::Info(loc::Text(L"Log.Player.PlayEmpty"));
        return;
    }
    if (!channel_) {
        StartTrackLocked(index_, false);
    } else {
        fmod_.ChannelSetPaused(channel_, 0);
        if (was_paused) {
            NowPlayingOverlay::Instance().ShowResumed(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
        }
    }
}

// 在允许暂停时暂停当前 FMOD Channel，并发送“已暂停”通知。
// 重复暂停不会重置位置或重复创建声音。
void LocalPlayer::Pause() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_pause || !spotify_replacement_enabled_) {
        return;
    }
    if (paused_ && config_.pause_toggles_playback && playback_requested_) {
        paused_ = false;
        if (!tracks_.empty()) {
            if (!channel_) {
                StartTrackLocked(index_, false);
            } else {
                fmod_.ChannelSetPaused(channel_, 0);
                NowPlayingOverlay::Instance().ShowResumed(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
            }
        }
        return;
    }
    if (!playback_requested_) {
        return;
    }
    paused_ = true;
    if (channel_) {
        fmod_.ChannelSetPaused(channel_, 1);
        if (!tracks_.empty()) {
            NowPlayingOverlay::Instance().ShowPaused(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
        }
    }
}

// 恢复已暂停 Channel；尚未创建 Channel 时按当前队列启动。
// 恢复后重新应用双层音量并发送继续播放通知。
void LocalPlayer::Resume() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !spotify_replacement_enabled_) {
        return;
    }
    if (!config_.start_local_music_on_unpause && !playback_requested_) {
        return;
    }
    const bool was_paused = paused_;
    playback_requested_ = true;
    paused_ = false;
    if (tracks_.empty()) {
        log::Info(loc::Text(L"Log.Player.ResumeEmpty"));
        return;
    }
    if (!channel_) {
        StartTrackLocked(index_, false);
    } else {
        fmod_.ChannelSetPaused(channel_, 0);
        if (was_paused) {
            NowPlayingOverlay::Instance().ShowResumed(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
        }
    }
}

// 在 Pause 与 Resume 之间切换。
// 供媒体键使用，不修改游戏 Spotify 启用状态。
void LocalPlayer::TogglePause() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_pause || !spotify_replacement_enabled_ || !playback_requested_) {
        return;
    }
    if (paused_) {
        paused_ = false;
        if (!tracks_.empty()) {
            if (!channel_) {
                StartTrackLocked(index_, false);
            } else {
                fmod_.ChannelSetPaused(channel_, 0);
                NowPlayingOverlay::Instance().ShowResumed(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
            }
        }
    } else {
        paused_ = true;
        if (channel_) {
            fmod_.ChannelSetPaused(channel_, 1);
            if (!tracks_.empty()) {
                NowPlayingOverlay::Instance().ShowPaused(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
            }
        }
    }
}

// 执行插件明确请求的下一曲。
// 使用固定播放队列游标，随机模式不会重新抽签。
void LocalPlayer::Next() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_skip) {
        return;
    }
    if (config_.respond_to_skip_only_when_active && (!spotify_replacement_enabled_ || !playback_requested_)) {
        if (!ignored_skip_logged_) {
            log::Info(loc::Text(L"Log.Player.SkipInactive"));
            ignored_skip_logged_ = true;
        }
        return;
    }
    AdvanceLocked(false);
}

// 执行插件明确请求的上一曲。
// 随机模式沿会话队列向前，因此可以真正回到刚才的歌曲。
void LocalPlayer::Previous() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_skip) {
        return;
    }
    if (config_.respond_to_skip_only_when_active && (!spotify_replacement_enabled_ || !playback_requested_)) {
        if (!ignored_skip_logged_) {
            log::Info(loc::Text(L"Log.Player.SkipInactive"));
            ignored_skip_logged_ = true;
        }
        return;
    }
    AdvanceLocked(true);
}

// 处理游戏原生 Spotify NextTrack 调用。
// 先检查活动状态和抑制窗口，再委托内部队列前进。
void LocalPlayer::NextFromGame() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_skip) {
        return;
    }
    if (ShouldSuppressGameSkipLocked()) {
        if (!navigation_skip_logged_) {
            log::Info(loc::Text(L"Log.Player.SkipNavigationNext"));
            navigation_skip_logged_ = true;
        }
        return;
    }
    navigation_skip_logged_ = false;
    if (config_.respond_to_skip_only_when_active && (!spotify_replacement_enabled_ || !playback_requested_)) {
        return;
    }
    AdvanceLocked(false);
}

// 处理游戏原生 Spotify PreviousTrack 调用。
// 与 NextFromGame 使用相同活动/菜单保护策略。
void LocalPlayer::PreviousFromGame() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || !config_.allow_skip) {
        return;
    }
    if (ShouldSuppressGameSkipLocked()) {
        if (!navigation_skip_logged_) {
            log::Info(loc::Text(L"Log.Player.SkipNavigationPrevious"));
            navigation_skip_logged_ = true;
        }
        return;
    }
    navigation_skip_logged_ = false;
    if (config_.respond_to_skip_only_when_active && (!spotify_replacement_enabled_ || !playback_requested_)) {
        return;
    }
    AdvanceLocked(true);
}

// 记录实验性横向菜单输入发生时间。
// 后续短窗口内的游戏换曲请求可被识别为潜在菜单误触。
void LocalPlayer::RecordNavigationInput() {
    last_navigation_input_tick_ms_.store(GetTickCount64(), std::memory_order_release);
}

// 设置一个绝对到期时间，强制暂时忽略游戏来源换曲。
// 固定 LB/BACK 组合使用它，避免同一十字键动作又触发游戏 Spotify 跳曲。
void LocalPlayer::SuppressGameSkipFor(uint32_t duration_ms) {
    const uint64_t now = GetTickCount64();
    const uint64_t requested_until = now + static_cast<uint64_t>(duration_ms);
    uint64_t current = forced_game_skip_suppression_until_ms_.load(std::memory_order_acquire);
    while (current < requested_until &&
           !forced_game_skip_suppression_until_ms_.compare_exchange_weak(
               current, requested_until, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
}

// 切换到上一文件夹专辑并选择该专辑的起始曲目。
// 单专辑曲库不会重播当前歌曲。
void LocalPlayer::PreviousAlbum() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || tracks_.empty() || albums_.empty()) return;
    SwitchAlbumLocked(true);
}

// 切换到下一文件夹专辑并选择该专辑的起始曲目。
// 专辑列表首尾循环，切换后显示专辑通知。
void LocalPlayer::NextAlbum() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || tracks_.empty() || albums_.empty()) return;
    SwitchAlbumLocked(false);
}

// 按顺序、随机、单曲循环的固定顺序切换播放模式。
// 不立即切歌；更新队列后写回 INI 并显示模式通知。
void LocalPlayer::CyclePlayMode() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || tracks_.empty()) return;
    CyclePlayModeLocked();
}

// 把当前曲目、专辑与播放模式快照发送到通知层。
// 不改变播放、队列或暂停状态。
void LocalPlayer::ShowCurrentStatus() {
    std::lock_guard lock(mutex_);
    if (!initialized_ || tracks_.empty()) return;
    NowPlayingOverlay::Instance().ShowCurrentStatus(
        tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
}

// 接收游戏持久 Spotify 音量并更新游戏倍率层。
// 降低立即生效，升高可去抖；插件 Volume 永远保留为独立倍率。
void LocalPlayer::SetDefaultVolume(int value) {
    std::lock_guard lock(mutex_);
    if (config_.volume_source != VolumeSource::GameSpotify) {
        return;
    }
    const int requested = std::clamp(value, 0, 100);

    // 降低音量属于安全操作，立即生效。向上增加则短暂延迟，因为游戏
    // 打开或重建设置界面时可能先上报瞬时 100%，随后才发布真实保存值。
    if (requested <= game_default_volume_ || config_.game_volume_increase_debounce_ms == 0) {
        pending_game_volume_ = -1;
        pending_game_volume_since_ms_ = 0;
        CommitGameVolumeLocked(requested, loc::Text(L"Log.Player.VolumeApplied").c_str());
        StartFromGameVolumeLocked();
        return;
    }

    pending_game_volume_ = requested;
    pending_game_volume_since_ms_ = GetTickCount64();
    log::Info(loc::Format(L"Log.Player.VolumePending", {
        std::to_wstring(requested),
        std::to_wstring(game_default_volume_),
        std::to_wstring(static_cast<int>(std::lround(EffectiveOutputVolumeLocked() * 100.0F))) + L"%"}));
    StartFromGameVolumeLocked();
}

// 接收并记录游戏临时音量 offset。
// 发行策略明确忽略该 offset，防止进入关卡时听感突然变化。
void LocalPlayer::SetVolumeOffset(int value) {
    std::lock_guard lock(mutex_);
    if (config_.volume_source != VolumeSource::GameSpotify) {
        return;
    }
    last_ignored_volume_offset_ = std::clamp(value, -100, 100);
    // Dangerous Driving 在进入/离开关卡时用此调用执行临时状态压低。
    // 把它应用到本地音乐会导致音量跳变，因此这里只记录并忽略；
    // 游戏持久设置仍作为游戏侧唯一有效倍率。
    log::Info(loc::Format(L"Log.Player.VolumeOffsetIgnored", {
        std::to_wstring(last_ignored_volume_offset_),
        std::to_wstring(static_cast<int>(std::lround(config_.volume * 100.0F))),
        std::to_wstring(game_default_volume_),
        std::to_wstring(static_cast<int>(std::lround(EffectiveOutputVolumeLocked() * 100.0F)))}));
}

// 把游戏首次持久音量上报视为 Spotify 会话激活信号。
// 仅自动启动一次，避免后续每次调音量都重播歌曲。
void LocalPlayer::StartFromGameVolumeLocked() {
    if (!config_.start_local_music_on_game_volume || !spotify_replacement_enabled_ || playback_requested_) {
        return;
    }
    playback_requested_ = true;
    paused_ = false;
    if (!auto_start_from_volume_logged_) {
        log::Info(loc::Text(L"Log.Player.AutoPlayVolume"));
        auto_start_from_volume_logged_ = true;
    }
    if (tracks_.empty()) {
        log::Info(loc::Text(L"Log.Player.AutoPlayEmpty"));
        return;
    }
    if (!channel_) {
        StartTrackLocked(index_, false);
    } else {
        ApplyVolumeLocked();
        fmod_.ChannelSetPaused(channel_, 0);
    }
}

// 启用或停用LocalMusic 本地后端，并按需持久化到插件 INI。
// 停用时暂停本地 Channel，但绝不修改游戏存档或 token。
void LocalPlayer::SetSpotifyReplacementEnabled(bool enabled, const wchar_t* reason, bool persist) {
    std::lock_guard lock(mutex_);
    if (!initialized_) {
        return;
    }
    if (spotify_replacement_enabled_ == enabled) {
        return;
    }
    spotify_replacement_enabled_ = enabled;
    ignored_play_logged_ = false;
    ignored_skip_logged_ = false;
    if (!enabled) {
        playback_requested_ = false;
        paused_ = true;
        ReleaseSoundLocked();
    }
    log::Info(loc::Format(L"Log.Player.BackendChanged", {
        loc::Text(enabled ? L"Log.Player.Enabled" : L"Log.Player.Disabled"),
        reason ? std::wstring(reason) : std::wstring()}));
    if (persist && config_.remember_spotify_mode_in_ini) {
        PersistSpotifyModeLocked();
    }
}

// 查询 FMOD 与曲库是否已完成初始化。
// 持锁返回一致快照，供 Hook 决定是否报告后端可用。
bool LocalPlayer::IsReady() const {
    std::lock_guard lock(mutex_);
    return initialized_ && !tracks_.empty();
}

// 返回当前扫描到的曲目数量。
// 结果在互斥锁下读取，重扫期间不会看到中间状态。
size_t LocalPlayer::TrackCount() const {
    std::lock_guard lock(mutex_);
    return tracks_.size();
}

// 返回当前文件夹专辑数量，包括根目录未分类专辑。
// 仅统计实际含曲目的专辑。
size_t LocalPlayer::AlbumCount() const {
    std::lock_guard lock(mutex_);
    return albums_.size();
}

// 返回播放器当前暂停状态。
// 无活动 Channel 时也按内部状态报告，匹配游戏 Spotify 接口预期。
bool LocalPlayer::IsPaused() const {
    std::lock_guard lock(mutex_);
    return paused_;
}

// 判断是否存在已请求且可控制的本地播放会话。
// 游戏来源换曲可用该状态避免菜单阶段提前启动音乐。
bool LocalPlayer::IsPlaybackActive() const {
    std::lock_guard lock(mutex_);
    return spotify_replacement_enabled_ && playback_requested_ && !paused_;
}

// 返回本地 Spotify 替换总开关。
// 该状态来自插件 INI/内存，不读取或写入游戏凭据。
bool LocalPlayer::IsSpotifyReplacementEnabled() const {
    std::lock_guard lock(mutex_);
    return spotify_replacement_enabled_;
}

// 返回最近提交的游戏持久 Spotify 音量百分比。
// 待去抖升高值尚未提交时仍报告当前稳定值。
int LocalPlayer::ObservedGameVolumePercent() const {
    std::lock_guard lock(mutex_);
    return std::clamp(game_default_volume_, 0, 100);
}

// 返回插件倍率乘游戏持久倍率后的最终 FMOD 音量。
// 仅查询，不调用 FMOD 或修改 Channel。
float LocalPlayer::EffectiveOutputVolume() const {
    std::lock_guard lock(mutex_);
    return EffectiveOutputVolumeLocked();
}

// 返回当前曲目标题。
// 无曲目时返回空字符串，供游戏 Getter 安全写回。
std::wstring LocalPlayer::Title() const {
    std::lock_guard lock(mutex_);
    return !spotify_replacement_enabled_ || !playback_requested_ || tracks_.empty() ? L"" : tracks_[index_].metadata.title;
}

// 返回当前曲目艺术家。
// 元数据已在扫描阶段完成补缺。
std::wstring LocalPlayer::Artist() const {
    std::lock_guard lock(mutex_);
    return !spotify_replacement_enabled_ || !playback_requested_ || tracks_.empty() ? L"" : tracks_[index_].metadata.artist;
}

// 返回每次成功切歌递增的修订号。
// 可用于 UI/诊断判断曲目是否变化而无需比较字符串。
uint64_t LocalPlayer::TrackRevision() const {
    std::lock_guard lock(mutex_);
    return track_revision_;
}

// 创建或复用预加载 Sound，建立暂停态 Channel，设置音量/END 回调后再按请求启动。
// 整个替换过程持锁且使用播放代数，迟到旧回调不会推进新曲目。
bool LocalPlayer::StartTrackLocked(size_t index, bool paused) {
    if (!initialized_ || tracks_.empty() || index >= tracks_.size()) {
        return false;
    }

    FMOD_SOUND* prepared_sound = nullptr;
    unsigned int prepared_length_ms = 0;
    const bool using_preloaded = preloaded_sound_ && preloaded_index_ == index;
    if (using_preloaded) {
        prepared_sound = preloaded_sound_;
        prepared_length_ms = preloaded_length_ms_;
        preloaded_sound_ = nullptr;
        preloaded_index_ = 0;
        preloaded_length_ms_ = 0;
    } else {
        ReleasePreloadedSoundLocked();
        const std::string path_utf8 = WideToUtf8(tracks_[index].path.wstring());
        const uint64_t open_begin = GetTickCount64();
        const FMOD_RESULT create_result = fmod_.SystemCreateSound(
            system_, path_utf8.c_str(), FmodApi::CreateStream | FmodApi::LoopOff | FmodApi::Mode2D,
            nullptr, &prepared_sound);
        const uint64_t open_ms = GetTickCount64() - open_begin;
        if (create_result != FmodApi::Ok || !prepared_sound) {
            LogFmodError(L"FMOD_System_CreateSound", create_result);
            return false;
        }
        if (open_ms >= 100) {
            log::Info(loc::Format(L"Log.Player.ColdOpen", {std::to_wstring(open_ms)}));
        }
        (void)fmod_.SoundGetLength(prepared_sound, &prepared_length_ms, FmodApi::TimeUnitMs);
    }

    ReleaseCurrentSoundLocked();
    sound_ = prepared_sound;

    // 始终以暂停状态创建 Channel，先安装回调、绑定用户数据并设置正确音量，
    // 再解除暂停，确保第一帧可听采样不会以错误音量播放。
    FMOD_RESULT result = fmod_.SystemPlaySound(system_, sound_, nullptr, 1, &channel_);
    if (result != FmodApi::Ok || !channel_) {
        LogFmodError(L"FMOD_System_PlaySound", result);
        fmod_.SoundRelease(sound_);
        sound_ = nullptr;
        channel_ = nullptr;
        return false;
    }

    ended_channel_generation_.store(0, std::memory_order_release);
    ended_channel_tick_ms_.store(0, std::memory_order_release);
    current_channel_generation_ = ++next_channel_generation_;
    if (fmod_.HasChannelEndCallbackSupport()) {
        auto context = std::make_unique<ChannelCallbackContext>();
        context->owner = this;
        context->generation = current_channel_generation_;
        ChannelCallbackContext* raw_context = context.get();
        callback_contexts_.push_back(std::move(context));

        const FMOD_RESULT user_data_result = fmod_.ChannelSetUserData(channel_, raw_context);
        const FMOD_RESULT callback_result = user_data_result == FmodApi::Ok
            ? fmod_.ChannelSetCallback(channel_, &LocalPlayer::OnFmodChannelCallback)
            : user_data_result;
        if (callback_result != FmodApi::Ok) {
            log::Warn(loc::Text(L"Log.Player.CallbackInstallFailed"));
            fmod_.ChannelSetUserData(channel_, nullptr);
        }
    } else if (!callback_unavailable_logged_) {
        log::Warn(loc::Text(L"Log.Player.CallbackUnsupported"));
        callback_unavailable_logged_ = true;
    }

    current_track_length_ms_ = prepared_length_ms;
    last_position_ms_ = 0;
    near_end_stall_ticks_ = 0;
    if (current_track_length_ms_ == 0) {
        unsigned int length_ms = 0;
        if (fmod_.SoundGetLength(sound_, &length_ms, FmodApi::TimeUnitMs) == FmodApi::Ok) {
            current_track_length_ms_ = length_ms;
        }
    }
    if (current_track_length_ms_ != 0) {
        log::Info(loc::Format(L"Log.Player.TrackLength", {std::to_wstring(current_track_length_ms_)}));
    } else {
        log::Warn(loc::Text(L"Log.Player.TrackLengthFailed"));
    }

    SetQueuePositionForTrackLocked(index);
    paused_ = paused;
    ApplyVolumeLocked();
    if (!paused_) {
        const FMOD_RESULT unpause_result = fmod_.ChannelSetPaused(channel_, 0);
        if (unpause_result != FmodApi::Ok) {
            LogFmodError(L"FMOD_Channel_SetPaused(false)", unpause_result);
        }
    }
    ++track_revision_;
    const std::wstring& artist = tracks_[index_].metadata.artist;
    const std::wstring& title = tracks_[index_].metadata.title;
    if (!artist.empty() && !title.empty()) {
        log::Info(loc::Format(L"Log.Player.NowPlayingArtist", {
            artist, title, using_preloaded ? loc::Text(L"Log.Player.PreloadedSuffix") : std::wstring()}));
    } else if (!title.empty()) {
        log::Info(loc::Format(L"Log.Player.NowPlaying", {
            title, using_preloaded ? loc::Text(L"Log.Player.PreloadedSuffix") : std::wstring()}));
    } else {
        log::Info(loc::Format(L"Log.Player.NowPlaying", {
            tracks_[index_].path.filename().wstring(),
            using_preloaded ? loc::Text(L"Log.Player.PreloadedSuffix") : std::wstring()}));
    }

    // 通知层只接收当前曲目的不可变快照。封面解析在通知线程中完成，
    // 因此这里不会因为大尺寸 APIC 图片阻塞 FMOD 切歌。
    if (paused_) {
        NowPlayingOverlay::Instance().ShowPaused(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
    } else {
        NowPlayingOverlay::Instance().ShowTrack(tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked());
    }

    // 新 Channel 已经开始发声后才预加载下一首。即使磁盘较慢，
    // 也不会阻塞当前切换，只会为下一次自然切歌提前准备。
    PreloadNextTrackLocked();
    return true;
}

// 停止并释放当前 Channel/Sound，清除与本曲相关的检测状态。
// 在切歌和退出前调用，避免 FMOD 资源泄漏。
void LocalPlayer::ReleaseCurrentSoundLocked() {
    ended_channel_generation_.store(0, std::memory_order_release);
    ended_channel_tick_ms_.store(0, std::memory_order_release);
    current_channel_generation_ = 0;
    if (channel_) {
        if (fmod_.HasChannelEndCallbackSupport()) {
            fmod_.ChannelSetCallback(channel_, nullptr);
            fmod_.ChannelSetUserData(channel_, nullptr);
        }
        fmod_.ChannelStop(channel_);
        channel_ = nullptr;
    }
    if (sound_) {
        fmod_.SoundRelease(sound_);
        sound_ = nullptr;
    }
    current_track_length_ms_ = 0;
    last_position_ms_ = 0;
    near_end_stall_ticks_ = 0;
}

// 释放尚未使用的下一首预加载 Sound。
// 队列、模式或曲库变化后旧预加载不能继续复用。
void LocalPlayer::ReleasePreloadedSoundLocked() {
    if (preloaded_sound_) {
        fmod_.SoundRelease(preloaded_sound_);
        preloaded_sound_ = nullptr;
    }
    preloaded_index_ = 0;
    preloaded_length_ms_ = 0;
}

// 统一释放当前与预加载声音资源。
// System 释放前必须完成该步骤。
void LocalPlayer::ReleaseSoundLocked() {
    ReleaseCurrentSoundLocked();
    ReleasePreloadedSoundLocked();
}

// 按当前播放模式重建曲目下标队列，并可保留当前曲目。
// 随机模式只在这里生成一次全曲库乱序，之后 Previous/Next 只移动游标。
void LocalPlayer::RebuildPlayQueueLocked(size_t preferred_track, bool preserve_current) {
    play_queue_.clear();
    queue_position_ = 0;
    if (tracks_.empty()) {
        index_ = 0;
        return;
    }

    play_queue_.reserve(tracks_.size());
    for (size_t i = 0; i < tracks_.size(); ++i) play_queue_.push_back(i);

    if (config_.play_mode == PlayMode::Random) {
        if (preserve_current && preferred_track < tracks_.size()) {
            play_queue_.erase(std::remove(play_queue_.begin(), play_queue_.end(), preferred_track),
                              play_queue_.end());
            std::shuffle(play_queue_.begin(), play_queue_.end(), random_);
            play_queue_.insert(play_queue_.begin(), preferred_track);
        } else {
            std::shuffle(play_queue_.begin(), play_queue_.end(), random_);
        }
    }

    if (preserve_current && preferred_track < tracks_.size()) {
        SetQueuePositionForTrackLocked(preferred_track);
    } else {
        queue_position_ = 0;
        index_ = play_queue_.front();
    }
}

// 把队列游标定位到指定曲目下标。
// 找不到时回退队首，避免游标越界。
void LocalPlayer::SetQueuePositionForTrackLocked(size_t track_index) {
    if (play_queue_.empty()) {
        index_ = track_index < tracks_.size() ? track_index : 0;
        queue_position_ = 0;
        return;
    }
    const auto found = std::find(play_queue_.begin(), play_queue_.end(), track_index);
    if (found != play_queue_.end()) {
        queue_position_ = static_cast<size_t>(std::distance(play_queue_.begin(), found));
        index_ = track_index;
    }
}

// 根据队列游标计算上一/下一曲下标。
// 队列首尾循环；单曲循环由调用方单独处理。
size_t LocalPlayer::ChooseAdjacentTrackLocked(bool backwards) const {
    if (play_queue_.empty()) return tracks_.empty() ? 0 : index_;
    if (backwards) {
        const size_t position = queue_position_ == 0 ? play_queue_.size() - 1 : queue_position_ - 1;
        return play_queue_[position];
    }
    return play_queue_[(queue_position_ + 1) % play_queue_.size()];
}

// 提前创建队列下一首 FMOD Sound 并缓存长度。
// 失败只影响无缝性，不阻止当前歌曲或后续冷打开播放。
void LocalPlayer::PreloadNextTrackLocked() {
    ReleasePreloadedSoundLocked();
    if (!config_.preload_next_track || !initialized_ || !system_ || tracks_.size() < 2 ||
        !playback_requested_ || config_.play_mode == PlayMode::SingleLoop) {
        return;
    }

    const size_t candidate = ChooseAdjacentTrackLocked(false);
    const std::string path_utf8 = WideToUtf8(tracks_[candidate].path.wstring());
    const uint64_t begin = GetTickCount64();
    FMOD_SOUND* candidate_sound = nullptr;
    const FMOD_RESULT result = fmod_.SystemCreateSound(
        system_, path_utf8.c_str(), FmodApi::CreateStream | FmodApi::LoopOff | FmodApi::Mode2D,
        nullptr, &candidate_sound);
    const uint64_t elapsed = GetTickCount64() - begin;
    if (result != FmodApi::Ok || !candidate_sound) {
        if (candidate_sound) fmod_.SoundRelease(candidate_sound);
        log::Warn(loc::Text(L"Log.Player.PreloadFailed"));
        return;
    }

    unsigned int length_ms = 0;
    (void)fmod_.SoundGetLength(candidate_sound, &length_ms, FmodApi::TimeUnitMs);
    preloaded_sound_ = candidate_sound;
    preloaded_index_ = candidate;
    preloaded_length_ms_ = length_ms;
    if (elapsed >= 100) log::Info(loc::Format(L"Log.Player.PreloadDone", {std::to_wstring(elapsed)}));
}

// 沿当前固定队列向前或向后移动并启动目标曲目。
// 成功后更新游标和预加载；失败保持当前状态并记录日志。
void LocalPlayer::AdvanceLocked(bool backwards) {
    if (tracks_.empty()) return;
    const size_t expected = ChooseAdjacentTrackLocked(backwards);
    const size_t next = !backwards && preloaded_sound_ && preloaded_index_ == expected
        ? preloaded_index_ : expected;
    SetQueuePositionForTrackLocked(next);
    if (playback_requested_) {
        StartTrackLocked(next, paused_);
    } else {
        ReleasePreloadedSoundLocked();
        ++track_revision_;
    }
}

// 按专辑列表切换上一/下一专辑，并启动其第一首或对应随机项。
// 专辑数量不足两张时只显示状态，不无意义地重播。
void LocalPlayer::SwitchAlbumLocked(bool backwards) {
    if (albums_.empty() || tracks_.empty()) return;
    if (albums_.size() < 2) {
        const Album& album = albums_.front();
        const size_t representative_index = album.track_indices.empty() ? index_ : album.track_indices.front();
        NowPlayingOverlay::Instance().ShowAlbum(
            tracks_[representative_index].path, tracks_[representative_index].metadata,
            album.metadata.title,
            !album.metadata.album_artist.empty() ? album.metadata.album_artist : album.metadata.artist,
            album.track_indices.size(), PlayModeTextLocked());
        log::Info(loc::Text(L"Log.Player.OneAlbum"));
        return;
    }
    const size_t current_album = tracks_[index_].album_index < albums_.size()
        ? tracks_[index_].album_index : 0;
    const size_t target_album = backwards
        ? (current_album == 0 ? albums_.size() - 1 : current_album - 1)
        : (current_album + 1) % albums_.size();
    if (albums_[target_album].track_indices.empty()) return;

    const size_t target_track = albums_[target_album].track_indices.front();
    ReleasePreloadedSoundLocked();
    SetQueuePositionForTrackLocked(target_track);
    if (playback_requested_) StartTrackLocked(target_track, paused_);
    else ++track_revision_;

    const Album& album = albums_[target_album];
    const Track& representative = tracks_[target_track];
    const std::wstring album_artist = !album.metadata.album_artist.empty()
        ? album.metadata.album_artist : album.metadata.artist;
    NowPlayingOverlay::Instance().ShowAlbum(
        representative.path, representative.metadata, album.metadata.title, album_artist,
        album.track_indices.size(), PlayModeTextLocked());
    log::Info(loc::Format(backwards ? L"Log.Player.AlbumPrevious" : L"Log.Player.AlbumNext", {
        album.metadata.title, std::to_wstring(album.track_indices.size())}));
}

// 在锁内切换播放模式、重建队列、持久化并通知 UI。
// 保留当前曲目，不因模式变化立即跳歌。
void LocalPlayer::CyclePlayModeLocked() {
    switch (config_.play_mode) {
        case PlayMode::Sequential: config_.play_mode = PlayMode::Random; break;
        case PlayMode::Random: config_.play_mode = PlayMode::SingleLoop; break;
        case PlayMode::SingleLoop:
        default: config_.play_mode = PlayMode::Sequential; break;
    }
    ReleasePreloadedSoundLocked();
    RebuildPlayQueueLocked(index_, true);
    PersistPlayModeLocked();
    PreloadNextTrackLocked();
    NowPlayingOverlay::Instance().ShowPlayMode(
        tracks_[index_].path, tracks_[index_].metadata, PlayModeTextLocked(), tracks_.size());
    log::Info(loc::Format(L"Log.Player.PlayModeChanged", {PlayModeTextLocked(), config_.ini_path.filename().wstring()}));
}

// 把当前播放模式字符串写回 dsound.ini。
// 只更新 PlayMode 键，保留其他用户配置和注释。
void LocalPlayer::PersistPlayModeLocked() {
    const wchar_t* value = L"Sequential";
    if (config_.play_mode == PlayMode::Random) value = L"Random";
    else if (config_.play_mode == PlayMode::SingleLoop) value = L"SingleLoop";
    if (!WriteIniString(config_.ini_path, L"LocalMusic", L"PlayMode", value)) {
        log::Warn(loc::Format(L"Log.Player.PlayModeWriteFailed", {config_.ini_path.filename().wstring()}));
    }
}

// 把内部 PlayMode 枚举转换为简体中文显示文本。
// 该文本用于日志与通知，不用于 INI 序列化。
std::wstring LocalPlayer::PlayModeTextLocked() const {
    switch (config_.play_mode) {
        case PlayMode::Random: return loc::Text(L"PlayMode.Random");
        case PlayMode::SingleLoop: return loc::Text(L"PlayMode.SingleLoop");
        case PlayMode::Sequential:
        default: return loc::Text(L"PlayMode.Sequential");
    }
}

// 返回当前曲目所属专辑的显示名称。
// 索引无效时使用“未分类曲目”等安全回退。
std::wstring LocalPlayer::CurrentAlbumTitleLocked() const {
    if (tracks_.empty() || albums_.empty() || tracks_[index_].album_index >= albums_.size()) return {};
    return albums_[tracks_[index_].album_index].metadata.title;
}

// 判断当前游戏来源跳曲是否处于强制或实验性抑制窗口。
// 只影响游戏 Hook，不影响自动下一曲和插件明确快捷键。
bool LocalPlayer::ShouldSuppressGameSkipLocked() const {
    const uint64_t now = GetTickCount64();
    const uint64_t forced_until =
        forced_game_skip_suppression_until_ms_.load(std::memory_order_acquire);
    if (forced_until != 0 && now <= forced_until) {
        return true;
    }
    if (!config_.suppress_navigation_key_skips || config_.navigation_skip_suppression_ms == 0) {
        return false;
    }

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

    const uint64_t input_tick = last_navigation_input_tick_ms_.load(std::memory_order_acquire);
    return input_tick != 0 && now >= input_tick &&
           now - input_tick <= config_.navigation_skip_suppression_ms;
}

// 检查去抖中的游戏音量升高是否已稳定到期。
// 到期后才提交，降低音量不走该延迟路径。
void LocalPlayer::ApplyPendingGameVolumeLocked() {
    if (pending_game_volume_ < 0) {
        return;
    }
    const uint64_t now = GetTickCount64();
    if (now < pending_game_volume_since_ms_ ||
        now - pending_game_volume_since_ms_ < config_.game_volume_increase_debounce_ms) {
        return;
    }
    const int value = pending_game_volume_;
    pending_game_volume_ = -1;
    pending_game_volume_since_ms_ = 0;
    CommitGameVolumeLocked(value, loc::Text(L"Log.Player.VolumeStableSuffix").c_str());
}

// 提交稳定的游戏持久音量、应用最终增益并写诊断日志。
// 不会改写插件 Volume，也不会把值写入游戏存档。
void LocalPlayer::CommitGameVolumeLocked(int value, const wchar_t* reason) {
    game_default_volume_ = std::clamp(value, 0, 100);
    ApplyVolumeLocked();
    log::Info(loc::Format(L"Log.Player.PersistentVolume", {
        std::to_wstring(game_default_volume_),
        std::to_wstring(static_cast<int>(std::lround(config_.volume * 100.0F))),
        std::to_wstring(static_cast<int>(std::lround(EffectiveOutputVolumeLocked() * 100.0F))),
        reason ? std::wstring(reason) : loc::Text(L"Log.Player.VolumeApplied"),
        loc::Text(L"Log.Player.OffsetIgnoredSuffix")}));
}

// 计算并设置当前 FMOD Channel 的最终音量。
// 公式为插件 Volume × 游戏持久 Spotify 音量，临时 offset 不参与。
void LocalPlayer::ApplyVolumeLocked() {
    if (!channel_) {
        return;
    }
    fmod_.ChannelSetVolume(channel_, EffectiveOutputVolumeLocked());
}

// 在已持锁上下文计算最终 0–1 FMOD 增益。
// VolumeSource=Plugin 时游戏倍率按 100% 处理。
float LocalPlayer::EffectiveOutputVolumeLocked() const {
    const float plugin_gain = std::clamp(config_.volume, 0.0F, 1.0F);
    if (config_.volume_source == VolumeSource::Plugin) {
        return plugin_gain;
    }
    const float game_gain = static_cast<float>(std::clamp(game_default_volume_, 0, 100)) / 100.0F;
    return std::clamp(plugin_gain * game_gain, 0.0F, 1.0F);
}

// FMOD 音频线程 END 回调，只记录结束的播放代数和时间。
// 禁止在回调内切歌、加锁或释放资源，实际推进由 Update 完成。
FMOD_RESULT WINAPI LocalPlayer::OnFmodChannelCallback(
    FMOD_CHANNELCONTROL* channel_control,
    FMOD_CHANNELCONTROL_TYPE control_type,
    FMOD_CHANNELCONTROL_CALLBACK_TYPE callback_type,
    void*,
    void*) {
    if (!channel_control || control_type != FmodApi::ChannelControlChannel ||
        callback_type != FmodApi::ChannelCallbackEnd) {
        return FmodApi::Ok;
    }

    LocalPlayer& player = LocalPlayer::Instance();
    if (!player.callback_api_active_.load(std::memory_order_acquire)) {
        return FmodApi::Ok;
    }
    void* raw_context = nullptr;
    auto* channel = reinterpret_cast<FMOD_CHANNEL*>(channel_control);
    if (player.fmod_.ChannelGetUserData(channel, &raw_context) != FmodApi::Ok || !raw_context) {
        return FmodApi::Ok;
    }

    auto* context = static_cast<ChannelCallbackContext*>(raw_context);
    if (context->owner == &player && context->generation != 0) {
        player.ended_channel_tick_ms_.store(GetTickCount64(), std::memory_order_release);
        player.ended_channel_generation_.store(context->generation, std::memory_order_release);
    }
    return FmodApi::Ok;
}

// 递归扫描支持格式、建立文件夹专辑、读取显示元数据，并严格按文件夹路径/文件名排序。
// 扫描完成后一次性交换容器，避免其他查询看到半成品曲库。
void LocalPlayer::ScanLocked() {
    const std::filesystem::path previous_path =
        !tracks_.empty() && index_ < tracks_.size() ? tracks_[index_].path : std::filesystem::path{};
    tracks_.clear();
    albums_.clear();
    play_queue_.clear();
    queue_position_ = 0;

    std::error_code ec;
    if (!std::filesystem::exists(config_.music_path, ec)) {
        std::filesystem::create_directories(config_.music_path, ec);
    }
    if (ec || !std::filesystem::is_directory(config_.music_path, ec)) {
        log::Error(loc::Format(L"Log.Player.MusicDirectoryUnavailable", {config_.music_path.wstring()}));
        index_ = 0;
        return;
    }

    std::unordered_map<std::wstring, std::vector<std::filesystem::path>> grouped_files;
    std::unordered_map<std::wstring, std::filesystem::path> grouped_directories;
    auto add_file = [&](const std::filesystem::directory_entry& entry) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error)) return;
        const std::wstring extension = ToLower(entry.path().extension().wstring());
        if (config_.extensions.find(extension) == config_.extensions.end()) return;
        const std::filesystem::path directory = config_.enable_folder_albums
            ? entry.path().parent_path() : config_.music_path;
        const std::wstring key = ToLower(directory.lexically_normal().wstring());
        grouped_files[key].push_back(entry.path());
        grouped_directories[key] = directory;
    };

    if (config_.scan_subdirs) {
        for (std::filesystem::recursive_directory_iterator it(
                 config_.music_path, std::filesystem::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            add_file(*it);
        }
    } else {
        for (std::filesystem::directory_iterator it(
                 config_.music_path, std::filesystem::directory_options::skip_permission_denied, ec), end;
             it != end && !ec; it.increment(ec)) {
            add_file(*it);
        }
    }

    std::vector<std::wstring> group_keys;
    group_keys.reserve(grouped_files.size());
    for (const auto& item : grouped_files) group_keys.push_back(item.first);
    std::sort(group_keys.begin(), group_keys.end(), [&](const std::wstring& left,
                                                        const std::wstring& right) {
        const std::filesystem::path& left_dir = grouped_directories[left];
        const std::filesystem::path& right_dir = grouped_directories[right];
        const bool left_root = left_dir.lexically_normal() == config_.music_path.lexically_normal();
        const bool right_root = right_dir.lexically_normal() == config_.music_path.lexically_normal();
        if (left_root != right_root) return left_root;
        std::error_code relative_error;
        const std::wstring left_name = std::filesystem::relative(left_dir, config_.music_path,
                                                                  relative_error).wstring();
        relative_error.clear();
        const std::wstring right_name = std::filesystem::relative(right_dir, config_.music_path,
                                                                   relative_error).wstring();
        return NaturalTextLess(left_name, right_name);
    });

    for (const std::wstring& key : group_keys) {
        const std::filesystem::path directory = grouped_directories[key];
        Album album;
        album.directory = directory;
        album.metadata = ReadAlbumMetadata(directory, config_.music_path);
        const size_t album_index = albums_.size();

        std::vector<Track> album_tracks;
        for (const std::filesystem::path& path : grouped_files[key]) {
            Track track;
            track.path = path;
            track.album_index = album_index;
            track.metadata = ReadTrackMetadata(path, &album.metadata,
                                               config_.enable_sidecar_metadata,
                                               config_.sidecar_metadata_override);
            album_tracks.push_back(std::move(track));
        }
        // 正式发行版的播放顺序只由文件名决定。内嵌标签、同名 INI 中的
        // TrackNumber/DiscNumber 仍可用于弹窗显示，但绝不再参与排序；用户只需
        // 修改文件名或添加数字前缀，就能得到跨格式一致、可预测的播放顺序。
        std::sort(album_tracks.begin(), album_tracks.end(), [](const Track& left, const Track& right) {
            const std::wstring left_stem = left.path.stem().wstring();
            const std::wstring right_stem = right.path.stem().wstring();
            if (NaturalTextLess(left_stem, right_stem)) return true;
            if (NaturalTextLess(right_stem, left_stem)) return false;
            // 主文件名完全相同时用包含扩展名的文件名兜底，保证比较器严格稳定。
            return NaturalTextLess(left.path.filename().wstring(), right.path.filename().wstring());
        });

        for (Track& track : album_tracks) {
            const size_t track_index = tracks_.size();
            album.track_indices.push_back(track_index);
            tracks_.push_back(std::move(track));
        }
        if (!album.track_indices.empty()) albums_.push_back(std::move(album));
    }

    size_t preferred_track = 0;
    bool preserve_current = false;
    if (!previous_path.empty()) {
        const auto found = std::find_if(tracks_.begin(), tracks_.end(), [&](const Track& track) {
            return track.path == previous_path;
        });
        if (found != tracks_.end()) {
            preferred_track = static_cast<size_t>(std::distance(tracks_.begin(), found));
            preserve_current = true;
        }
    }
    RebuildPlayQueueLocked(preferred_track, preserve_current);
    log::Info(loc::Format(L"Log.Player.ScanComplete", {
        std::to_wstring(tracks_.size()), std::to_wstring(albums_.size()), PlayModeTextLocked()}));
}

// 把本地 Spotify 替换启用状态写入插件 INI。
// 不触碰 SaveGame.sav、Visited Links 或 Spotify token。
void LocalPlayer::PersistSpotifyModeLocked() {
    if (!WriteIniBool(config_.ini_path, L"LocalMusic", L"LocalSpotifyReplacementEnabled", spotify_replacement_enabled_)) {
        log::Warn(loc::Format(L"Log.Player.BackendWriteFailed", {config_.ini_path.filename().wstring()}));
    }
}

// 把 FMOD 操作名和返回码写入统一错误日志。
// 调用方决定错误是否可恢复，本函数不修改播放状态。
void LocalPlayer::LogFmodError(const wchar_t* operation, FMOD_RESULT result) const {
    log::Error(loc::Format(L"Log.Player.FmodOperationFailed", {operation ? std::wstring(operation) : std::wstring(), Utf8ToWide(fmod_.ErrorString(result)), std::to_wstring(result)}));
}

}  // namespace localmusic
