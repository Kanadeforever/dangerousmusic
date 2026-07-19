// 本地播放器核心状态机：管理文件夹专辑、会话播放队列、FMOD Channel、
// 自动切歌、预加载、双层音量与通知状态。
//
// 线程规则：除 FMOD END 回调只投递原子事件外，全部可变播放状态都由 mutex_ 保护。
#pragma once
#include "Common.h"
#include "Config.h"
#include "FmodApi.h"
#include "Metadata.h"

namespace localmusic {

struct Track {
    std::filesystem::path path;
    TrackMetadata metadata;
    size_t album_index = 0;
};

struct Album {
    std::filesystem::path directory;
    AlbumMetadata metadata;
    std::vector<size_t> track_indices;
};

class LocalPlayer {
public:
    // 返回本地播放器进程级单例。
    // 所有游戏 Hook、输入层和工作线程通过同一实例访问受锁保护的播放状态。
    static LocalPlayer& Instance();

    // 复制配置、加载 FMOD、创建 System、扫描曲库并建立初始播放队列。
    // 失败时按已创建资源逆序清理，不让半初始化状态进入 Update。
    bool Initialize(const Config& config, const std::filesystem::path& dll_directory);
    // 停止 Channel、释放预加载/当前 Sound、关闭 FMOD System 并清空曲库状态。
    // 持有播放器互斥锁，确保退出时没有其他控制操作并发修改资源。
    void Shutdown();
    // 工作线程周期更新 FMOD、提交去抖音量并检测自然播放结束。
    // END 回调只投递原子代数，本方法在安全线程中真正执行自动切歌。
    void Update();
    // 重新扫描音乐目录并尽量保留当前曲目和队列位置。
    // 曲目被删除时选择安全替代项，曲库为空则停止播放而不影响游戏运行。
    void Rescan();

    // 响应本地或游戏播放请求：首次启动当前队列曲目，已有 Channel 则解除暂停。
    // 只有 Spotify 替换启用且曲库非空时实际播放。
    void Play();
    // 在允许暂停时暂停当前 FMOD Channel，并发送“已暂停”通知。
    // 重复暂停不会重置位置或重复创建声音。
    void Pause();
    // 恢复已暂停 Channel；尚未创建 Channel 时按当前队列启动。
    // 恢复后重新应用双层音量并发送继续播放通知。
    void Resume();
    // 在 Pause 与 Resume 之间切换。
    // 供媒体键使用，不修改游戏 Spotify 启用状态。
    void TogglePause();
    // 执行插件明确请求的下一曲。
    // 使用固定播放队列游标，随机模式不会重新抽签。
    void Next();
    // 执行插件明确请求的上一曲。
    // 随机模式沿会话队列向前，因此可以真正回到刚才的歌曲。
    void Previous();
    // 处理游戏原生 Spotify NextTrack 调用。
    // 先检查活动状态和抑制窗口，再委托内部队列前进。
    void NextFromGame();
    // 处理游戏原生 Spotify PreviousTrack 调用。
    // 与 NextFromGame 使用相同活动/菜单保护策略。
    void PreviousFromGame();
    // 记录实验性横向菜单输入发生时间。
    // 后续短窗口内的游戏换曲请求可被识别为潜在菜单误触。
    void RecordNavigationInput();
    // 固定手柄组合触发专辑切换时，游戏仍可能同时收到十字键并调用
    // Spotify NextTrack/PreviousTrack。这里提供独立于实验性菜单抑制开关的
    // 短时保护窗口，防止一次 LB+左右同时发生“切专辑后又切一首”。
    void SuppressGameSkipFor(uint32_t duration_ms);

    // 固定的专辑与模式控制。键盘/手柄输入由 AlbumControls 负责检测，
    // 播放器只执行与线程安全相关的状态转换。
    void PreviousAlbum();
    // 切换到下一文件夹专辑并选择该专辑的起始曲目。
    // 专辑列表首尾循环，切换后显示专辑通知。
    void NextAlbum();
    // 按顺序、随机、单曲循环的固定顺序切换播放模式。
    // 不立即切歌；更新队列后写回 INI 并显示模式通知。
    void CyclePlayMode();
    // 把当前曲目、专辑与播放模式快照发送到通知层。
    // 不改变播放、队列或暂停状态。
    void ShowCurrentStatus();

    // 接收游戏持久 Spotify 音量并更新游戏倍率层。
    // 降低立即生效，升高可去抖；插件 Volume 永远保留为独立倍率。
    void SetDefaultVolume(int value);
    // 接收并记录游戏临时音量 offset。
    // 发行策略明确忽略该 offset，防止进入关卡时听感突然变化。
    void SetVolumeOffset(int value);
    // 启用或停用LocalMusic 本地后端，并按需持久化到插件 INI。
    // 停用时暂停本地 Channel，但绝不修改游戏存档或 token。
    void SetSpotifyReplacementEnabled(bool enabled, const wchar_t* reason, bool persist);

    // 查询 FMOD 与曲库是否已完成初始化。
    // 持锁返回一致快照，供 Hook 决定是否报告后端可用。
    bool IsReady() const;
    // 返回当前扫描到的曲目数量。
    // 结果在互斥锁下读取，重扫期间不会看到中间状态。
    size_t TrackCount() const;
    // 返回当前文件夹专辑数量，包括根目录未分类专辑。
    // 仅统计实际含曲目的专辑。
    size_t AlbumCount() const;
    // 返回播放器当前暂停状态。
    // 无活动 Channel 时也按内部状态报告，匹配游戏 Spotify 接口预期。
    bool IsPaused() const;
    // 判断是否存在已请求且可控制的本地播放会话。
    // 游戏来源换曲可用该状态避免菜单阶段提前启动音乐。
    bool IsPlaybackActive() const;
    // 返回本地 Spotify 替换总开关。
    // 该状态来自插件 INI/内存，不读取或写入游戏凭据。
    bool IsSpotifyReplacementEnabled() const;
    // 返回最近提交的游戏持久 Spotify 音量百分比。
    // 待去抖升高值尚未提交时仍报告当前稳定值。
    int ObservedGameVolumePercent() const;
    // 返回插件倍率乘游戏持久倍率后的最终 FMOD 音量。
    // 仅查询，不调用 FMOD 或修改 Channel。
    float EffectiveOutputVolume() const;
    // 返回当前曲目标题。
    // 无曲目时返回空字符串，供游戏 Getter 安全写回。
    std::wstring Title() const;
    // 返回当前曲目艺术家。
    // 元数据已在扫描阶段完成补缺。
    std::wstring Artist() const;
    // 返回每次成功切歌递增的修订号。
    // 可用于 UI/诊断判断曲目是否变化而无需比较字符串。
    uint64_t TrackRevision() const;
    // 返回初始化时复制的配置快照，供只读诊断或辅助模块查询。
    // 调用方不得保存并修改该引用；运行时可变布局由通知层单独管理。
    const Config& GetConfig() const { return config_; }

private:
    // 单例私有构造函数；真正的系统资源创建全部放在 Initialize。
    // 保持构造无副作用，避免 DLL 静态初始化阶段访问 Windows/FMOD。
    LocalPlayer() = default;

    // 创建或复用预加载 Sound，建立暂停态 Channel，设置音量/END 回调后再按请求启动。
    // 整个替换过程持锁且使用播放代数，迟到旧回调不会推进新曲目。
    bool StartTrackLocked(size_t index, bool paused);
    // 停止并释放当前 Channel/Sound，清除与本曲相关的检测状态。
    // 在切歌和退出前调用，避免 FMOD 资源泄漏。
    void ReleaseCurrentSoundLocked();
    // 释放尚未使用的下一首预加载 Sound。
    // 队列、模式或曲库变化后旧预加载不能继续复用。
    void ReleasePreloadedSoundLocked();
    // 统一释放当前与预加载声音资源。
    // System 释放前必须完成该步骤。
    void ReleaseSoundLocked();

    // 播放队列始终保存曲目下标。顺序/单曲循环使用文件夹路径与文件名自然排序队列；
    // 随机模式进入时只生成一次全曲库乱序队列，Previous/Next 只移动游标。
    void RebuildPlayQueueLocked(size_t preferred_track, bool preserve_current);
    // 把队列游标定位到指定曲目下标。
    // 找不到时回退队首，避免游标越界。
    void SetQueuePositionForTrackLocked(size_t track_index);
    // 根据队列游标计算上一/下一曲下标。
    // 队列首尾循环；单曲循环由调用方单独处理。
    size_t ChooseAdjacentTrackLocked(bool backwards) const;
    // 提前创建队列下一首 FMOD Sound 并缓存长度。
    // 失败只影响无缝性，不阻止当前歌曲或后续冷打开播放。
    void PreloadNextTrackLocked();
    // 沿当前固定队列向前或向后移动并启动目标曲目。
    // 成功后更新游标和预加载；失败保持当前状态并记录日志。
    void AdvanceLocked(bool backwards);
    // 按专辑列表切换上一/下一专辑，并启动其第一首或对应随机项。
    // 专辑数量不足两张时只显示状态，不无意义地重播。
    void SwitchAlbumLocked(bool backwards);
    // 在锁内切换播放模式、重建队列、持久化并通知 UI。
    // 保留当前曲目，不因模式变化立即跳歌。
    void CyclePlayModeLocked();
    // 把当前播放模式字符串写回 dsound.ini。
    // 只更新 PlayMode 键，保留其他用户配置和注释。
    void PersistPlayModeLocked();
    // 把内部 PlayMode 枚举转换为简体中文显示文本。
    // 该文本用于日志与通知，不用于 INI 序列化。
    std::wstring PlayModeTextLocked() const;
    // 返回当前曲目所属专辑的显示名称。
    // 索引无效时使用“未分类曲目”等安全回退。
    std::wstring CurrentAlbumTitleLocked() const;

    // 判断当前游戏来源跳曲是否处于强制或实验性抑制窗口。
    // 只影响游戏 Hook，不影响自动下一曲和插件明确快捷键。
    bool ShouldSuppressGameSkipLocked() const;
    // 检查去抖中的游戏音量升高是否已稳定到期。
    // 到期后才提交，降低音量不走该延迟路径。
    void ApplyPendingGameVolumeLocked();
    // 提交稳定的游戏持久音量、应用最终增益并写诊断日志。
    // 不会改写插件 Volume，也不会把值写入游戏存档。
    void CommitGameVolumeLocked(int value, const wchar_t* reason);
    // 计算并设置当前 FMOD Channel 的最终音量。
    // 公式为插件 Volume × 游戏持久 Spotify 音量，临时 offset 不参与。
    void ApplyVolumeLocked();
    // 在已持锁上下文计算最终 0–1 FMOD 增益。
    // VolumeSource=Plugin 时游戏倍率按 100% 处理。
    float EffectiveOutputVolumeLocked() const;
    // 把游戏首次持久音量上报视为 Spotify 会话激活信号。
    // 仅自动启动一次，避免后续每次调音量都重播歌曲。
    void StartFromGameVolumeLocked();
    // 综合 END 回调、IsPlaying、位置停滞和曲长判断当前曲目是否自然结束。
    // 只在未暂停且 AutoAdvance 开启时成立，手动切歌不会被误判为 EOF。
    bool ShouldAutoAdvanceLocked();
    // 根据单曲循环或当前队列游标选择自然结束后的下一首。
    // 记录触发原因并复用 StartTrackLocked，保证回调与备用检测行为一致。
    void AutoAdvanceLocked(const wchar_t* reason);
    // 把本地 Spotify 替换启用状态写入插件 INI。
    // 不触碰 SaveGame.sav、Visited Links 或 Spotify token。
    void PersistSpotifyModeLocked();
    // 递归扫描支持格式、建立文件夹专辑、读取显示元数据，并只按路径/文件名排序。
    // 扫描完成后一次性交换容器，避免其他查询看到半成品曲库。
    void ScanLocked();
    // 把 FMOD 操作名和返回码写入统一错误日志。
    // 调用方决定错误是否可恢复，本函数不修改播放状态。
    void LogFmodError(const wchar_t* operation, FMOD_RESULT result) const;

    struct ChannelCallbackContext {
        LocalPlayer* owner = nullptr;
        uint64_t generation = 0;
    };
    // FMOD 音频线程 END 回调，只记录结束的播放代数和时间。
    // 禁止在回调内切歌、加锁或释放资源，实际推进由 Update 完成。
    static FMOD_RESULT WINAPI OnFmodChannelCallback(
        FMOD_CHANNELCONTROL* channel_control,
        FMOD_CHANNELCONTROL_TYPE control_type,
        FMOD_CHANNELCONTROL_CALLBACK_TYPE callback_type,
        void* command_data_1,
        void* command_data_2);

    mutable std::mutex mutex_;
    Config config_;
    FmodApi fmod_;
    FMOD_SYSTEM* system_ = nullptr;
    FMOD_SOUND* sound_ = nullptr;
    FMOD_CHANNEL* channel_ = nullptr;
    FMOD_SOUND* preloaded_sound_ = nullptr;
    size_t preloaded_index_ = 0;
    unsigned int preloaded_length_ms_ = 0;

    std::vector<Track> tracks_;
    std::vector<Album> albums_;
    std::vector<size_t> play_queue_;
    size_t queue_position_ = 0;
    size_t index_ = 0;

    bool initialized_ = false;
    bool spotify_replacement_enabled_ = false;
    bool playback_requested_ = false;
    bool paused_ = true;
    int game_default_volume_ = 100;
    int pending_game_volume_ = -1;
    uint64_t pending_game_volume_since_ms_ = 0;
    int last_ignored_volume_offset_ = 0;
    bool ignored_play_logged_ = false;
    bool ignored_skip_logged_ = false;
    uint64_t track_revision_ = 0;
    unsigned int current_track_length_ms_ = 0;
    unsigned int last_position_ms_ = 0;
    int near_end_stall_ticks_ = 0;
    uint64_t next_channel_generation_ = 0;
    uint64_t current_channel_generation_ = 0;
    std::atomic<uint64_t> ended_channel_generation_{0};
    std::atomic<uint64_t> ended_channel_tick_ms_{0};
    std::atomic<uint64_t> last_navigation_input_tick_ms_{0};
    std::atomic<uint64_t> forced_game_skip_suppression_until_ms_{0};
    std::atomic<bool> callback_api_active_{false};
    std::vector<std::unique_ptr<ChannelCallbackContext>> callback_contexts_;
    bool callback_unavailable_logged_ = false;
    bool auto_start_from_volume_logged_ = false;
    bool navigation_skip_logged_ = false;
    std::mt19937_64 random_{std::random_device{}()};
};

}  // namespace localmusic
