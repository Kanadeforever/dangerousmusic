// 自绘当前曲目信息通知层。
//
// 不 Hook DirectX，不接管鼠标和键盘。播放器只投递不可变快照；
// 封面读取、图片解码和绘制全部在独立通知线程中完成。
#pragma once
#include "Common.h"
#include "Config.h"
#include "Metadata.h"

namespace localmusic {

enum class PlaybackNoticeKind {
    TrackChanged,
    Paused,
    Resumed,
    AlbumChanged,
    PlayModeChanged,
    CurrentStatus,
    LayoutAdjusted,
    LayoutSaved,
    LayoutReset,
};

class NowPlayingOverlay {
public:
    // 返回进程级单例。对象故意不在 DLL 卸载阶段自动析构，窗口线程由
    // Shutdown 明确停止，避免静态析构顺序与加载器锁发生竞争。
    static NowPlayingOverlay& Instance();

    // 复制配置并启动独立的 GDI+/Win32 分层窗口线程。初始化失败只关闭
    // 通知功能，不阻止 FMOD 播放器和游戏 Hook 继续工作。
    bool Initialize(const Config& config);

    // 通知窗口线程退出后释放事件句柄和待显示快照。必须在播放器 Shutdown
    // 之前或之后的单线程阶段调用，不能从通知线程自身调用。
    void Shutdown();

    // 构造“正在播放”曲目通知快照并排入单槽队列。
    // 按配置决定是否在状态行显示播放模式。
    void ShowTrack(const std::filesystem::path& path, const TrackMetadata& metadata,
                   const std::wstring& play_mode);
    // 构造“已暂停”通知，保留当前曲目和封面信息。
    // 仅在游戏实际调用 Spotify Pause 时由播放器触发。
    void ShowPaused(const std::filesystem::path& path, const TrackMetadata& metadata,
                    const std::wstring& play_mode);
    // 构造“继续播放”通知并显示当前曲目信息。
    // 不会改变 Channel 状态，播放恢复已由 LocalPlayer 完成。
    void ShowResumed(const std::filesystem::path& path, const TrackMetadata& metadata,
                     const std::wstring& play_mode);
    // 构造专辑切换通知，使用代表曲目的专辑封面来源。
    // 标题、艺术家和曲目数使用专辑级元数据。
    void ShowAlbum(const std::filesystem::path& representative_track,
                   const TrackMetadata& representative_metadata,
                   const std::wstring& album_title,
                   const std::wstring& album_artist,
                   size_t track_count,
                   const std::wstring& play_mode);
    // 构造播放模式切换通知，显示全曲库曲目数。
    // 模式持久化由 LocalPlayer 完成，通知层只展示快照。
    void ShowPlayMode(const std::filesystem::path& current_track,
                      const TrackMetadata& current_metadata,
                      const std::wstring& play_mode,
                      size_t total_tracks);
    // 构造用户主动查询的当前状态通知。
    // 不改变通知配置或播放状态。
    void ShowCurrentStatus(const std::filesystem::path& current_track,
                           const TrackMetadata& current_metadata,
                           const std::wstring& play_mode);

    // 按“屏幕视觉方向”实时调整通知布局。delta_x>0 表示向右，delta_y>0
    // 表示向下；方法会根据 TopLeft/TopRight 等锚点自动换算 MarginX/Y。
    // 缩放与透明度使用百分点增量，修改只留在内存并立即预览。
    void AdjustLayout(int delta_x, int delta_y, int delta_scale, int delta_opacity);

    // 把当前锚点、X/Y、缩放和透明度写回与 DLL 同名的 INI。只更新相关键，
    // 不重写用户的其他配置与注释。
    void SaveLayout();

    // 恢复发行默认值 TopRight/X32/Y32/Scale100/Opacity92，立即写回 INI
    // 并显示重置确认通知。
    void ResetLayout();

    struct Notice {
        PlaybackNoticeKind kind = PlaybackNoticeKind::TrackChanged;
        std::filesystem::path path;
        TrackMetadata metadata;
        std::wstring play_mode;
        bool use_album_cover = false;
        uint64_t serial = 0;
    };

private:
    // 只能通过 Instance 获取。构造函数不创建系统资源。
    NowPlayingOverlay() = default;

    // 将不可变通知快照替换到单槽队列中。快速连续切歌时旧通知会被最新
    // 通知覆盖，避免在 UI 上排队显示过时曲目。
    void QueueNotice(Notice notice);

    // 根据当前布局配置生成“调整中/已保存/已重置”通知文本。
    void ShowLayoutNotice(PlaybackNoticeKind kind);

    // 把传入快照的布局键写入 INI；调用者无需持有 mutex_，也不应从
    // DllMain 的加载器锁上下文调用。
    bool PersistLayout(const Config& snapshot) const;

    // 通知线程主循环：创建窗口、解码封面、处理最新通知、逐帧淡入淡出并
    // 在退出前按正确顺序销毁 GDI+/COM/窗口资源。
    void ThreadMain();

    // 分层窗口过程。窗口永远鼠标穿透且不激活，只处理最小必要消息。
    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

    mutable std::mutex mutex_;
    Config config_;
    std::optional<Notice> pending_notice_;
    std::thread thread_;
    HANDLE wake_event_ = nullptr;
    HANDLE ready_event_ = nullptr;
    std::atomic<bool> initialized_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> ready_succeeded_{false};
    uint64_t next_serial_ = 0;
};

}  // namespace localmusic
