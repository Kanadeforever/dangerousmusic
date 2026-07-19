// 使用 GDI+ 与 UpdateLayeredWindow 绘制当前曲目通知。
// 此模块不读取播放器锁内的可变状态；播放器只投递不可变的通知快照。
#include "NowPlayingOverlay.h"

#include "Logger.h"
#include "Localization.h"
#include "DpiAwareness.h"

#include <objbase.h>
#include <gdiplus.h>

namespace localmusic {
namespace {

constexpr wchar_t kWindowClassName[] = L"LocalMusicNowPlayingOverlay";
constexpr int kBaseWidth = 548;
constexpr int kBaseHeight = 140;
constexpr int kBaseCoverSize = 104;
constexpr size_t kArtworkCacheLimit = 16;
constexpr uint32_t kDefaultMarginX = 32;
constexpr uint32_t kDefaultMarginY = 32;
constexpr uint32_t kDefaultScalePercent = 100;
constexpr uint32_t kDefaultOpacityPercent = 92;

struct CachedArtwork {
    std::wstring path;
    std::unique_ptr<Gdiplus::Bitmap> bitmap;
};

struct GameWindowSearch {
    DWORD process_id = 0;
    HWND best_window = nullptr;
    uint64_t best_area = 0;
};

// 枚举本进程可见顶层窗口并选择客户区最大的游戏窗口。
// 排除通知窗口自身、被拥有窗口和过小工具窗口。
BOOL CALLBACK FindGameWindowCallback(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<GameWindowSearch*>(parameter);
    DWORD process_id = 0;
    GetWindowThreadProcessId(window, &process_id);
    if (process_id != search->process_id || !IsWindowVisible(window) ||
        GetWindow(window, GW_OWNER) != nullptr) {
        return TRUE;
    }

    wchar_t class_name[128]{};
    GetClassNameW(window, class_name, 128);
    if (lstrcmpW(class_name, kWindowClassName) == 0) {
        return TRUE;
    }

    RECT client{};
    if (!GetClientRect(window, &client)) {
        return TRUE;
    }
    const LONG width = client.right - client.left;
    const LONG height = client.bottom - client.top;
    if (width < 640 || height < 360) {
        return TRUE;
    }
    const uint64_t area = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
    if (area > search->best_area) {
        search->best_area = area;
        search->best_window = window;
    }
    return TRUE;
}

// 执行窗口枚举并返回当前最可能的 Dangerous Driving 主窗口。
// 游戏重建窗口或切换模式时每帧重新查找可恢复。
HWND FindGameWindow() {
    GameWindowSearch search;
    search.process_id = GetCurrentProcessId();
    EnumWindows(&FindGameWindowCallback, reinterpret_cast<LPARAM>(&search));
    return search.best_window;
}


// 向 GDI+ GraphicsPath 添加四个圆角弧并闭合。
// 统一面板、阴影和封面的圆角几何。
void AddRoundedRectangle(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rectangle, float radius) {
    const float diameter = std::max(1.0F, radius * 2.0F);
    path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0F, 90.0F);
    path.AddArc(rectangle.GetRight() - diameter, rectangle.Y, diameter, diameter, 270.0F, 90.0F);
    path.AddArc(rectangle.GetRight() - diameter, rectangle.GetBottom() - diameter,
                diameter, diameter, 0.0F, 90.0F);
    path.AddArc(rectangle.X, rectangle.GetBottom() - diameter, diameter, diameter, 90.0F, 90.0F);
    path.CloseFigure();
}

// 读取当前通知所需封面字节并用 GDI+ 解码、裁剪为独立方形位图。
// 严格维持 IStream 生命周期，损坏封面返回空而不影响通知文字。
std::unique_ptr<Gdiplus::Bitmap> DecodeArtwork(const NowPlayingOverlay::Notice& notice,
                                                   const Config& config) {
    const TrackArtwork artwork = ReadTrackArtwork(
        notice.path, notice.metadata, config.show_embedded_cover,
        config.show_external_cover, notice.use_album_cover);
    if (artwork.Empty() || artwork.bytes.size() > static_cast<size_t>(std::numeric_limits<UINT>::max())) {
        return nullptr;
    }

    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, artwork.bytes.size());
    if (!memory) {
        return nullptr;
    }
    void* destination = GlobalLock(memory);
    if (!destination) {
        GlobalFree(memory);
        return nullptr;
    }
    std::memcpy(destination, artwork.bytes.data(), artwork.bytes.size());
    GlobalUnlock(memory);

    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream)) || !stream) {
        GlobalFree(memory);
        return nullptr;
    }

    // Windows SDK 中 Gdiplus::Image::FromStream 的公开重载只接收 IStream* 与
    // useEmbeddedColorManagement 两个参数。旧代码传入第三个参数会在新版 SDK 下
    // 报“没有匹配的重载”。流的生命周期仍由下方逻辑保证覆盖 Image 生命周期。
    std::unique_ptr<Gdiplus::Image> source(Gdiplus::Image::FromStream(stream, FALSE));
    if (!source || source->GetLastStatus() != Gdiplus::Ok || source->GetWidth() == 0 || source->GetHeight() == 0) {
        source.reset();
        stream->Release();
        return nullptr;
    }

    // 克隆为固定大小的独立位图。这样释放 IStream 后，缓存中的位图仍然安全。
    constexpr INT cache_size = 256;
    auto decoded = std::make_unique<Gdiplus::Bitmap>(cache_size, cache_size, PixelFormat32bppPARGB);
    if (decoded->GetLastStatus() != Gdiplus::Ok) {
        decoded.reset();
        source.reset();
        stream->Release();
        return nullptr;
    }
    Gdiplus::Graphics graphics(decoded.get());
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float source_width = static_cast<float>(source->GetWidth());
    const float source_height = static_cast<float>(source->GetHeight());
    const float source_size = std::min(source_width, source_height);
    const float source_x = (source_width - source_size) * 0.5F;
    const float source_y = (source_height - source_size) * 0.5F;
    graphics.DrawImage(source.get(), Gdiplus::RectF(0.0F, 0.0F, static_cast<float>(cache_size),
                                                    static_cast<float>(cache_size)),
                       source_x, source_y, source_size, source_size, Gdiplus::UnitPixel);
    // GDI+ 要求创建 Image 时使用的 IStream 在 Image 生命周期内保持有效。
    // 先销毁临时 Image，再释放拥有 HGLOBAL 的 IStream。
    source.reset();
    stream->Release();
    return decoded;
}

// 按曲目路径和封面来源构造缓存键，复用最近解码的位图。
// 缓存有固定上限，淘汰最旧项避免长时间游戏无限增长内存。
Gdiplus::Bitmap* FindOrLoadArtwork(std::vector<CachedArtwork>& cache,
                                   const NowPlayingOverlay::Notice& notice,
                                   const Config& config) {
    if (notice.path.empty() ||
        (!config.show_embedded_cover && !config.show_external_cover)) {
        return nullptr;
    }
    // 同一音频文件可能先显示曲目封面、随后显示专辑封面，因此缓存键必须
    // 包含封面模式与所有外部来源，不能只使用音频路径。
    const std::wstring key = notice.path.wstring() + L"|" +
        notice.metadata.track_cover_path.wstring() + L"|" +
        notice.metadata.sidecar_cover_path.wstring() + L"|" +
        notice.metadata.album_cover_path.wstring() + L"|" +
        (notice.use_album_cover ? L"album" : L"track") + L"|" +
        (config.show_embedded_cover ? L"embedded" : L"noembedded");
    const auto found = std::find_if(cache.begin(), cache.end(), [&](const CachedArtwork& item) {
        return item.path == key;
    });
    if (found != cache.end()) return found->bitmap.get();

    CachedArtwork item;
    item.path = key;
    item.bitmap = DecodeArtwork(notice, config);
    cache.push_back(std::move(item));
    if (cache.size() > kArtworkCacheLimit) cache.erase(cache.begin());
    return cache.back().bitmap.get();
}

// 根据通知类型生成中文状态标题，并按配置附加播放模式。
// 布局调整、保存和重置与曲目通知复用同一种视觉组件。
std::wstring StatusText(const NowPlayingOverlay::Notice& notice) {
    std::wstring status;
    switch (notice.kind) {
        case PlaybackNoticeKind::Paused: status = loc::Text(L"Notification.Paused"); break;
        case PlaybackNoticeKind::Resumed: status = loc::Text(L"Notification.Resumed"); break;
        case PlaybackNoticeKind::AlbumChanged: status = loc::Text(L"Notification.AlbumChanged"); break;
        case PlaybackNoticeKind::PlayModeChanged: status = loc::Text(L"Notification.PlayMode"); break;
        case PlaybackNoticeKind::CurrentStatus: status = loc::Text(L"Notification.CurrentStatus"); break;
        case PlaybackNoticeKind::LayoutAdjusted: status = loc::Text(L"Notification.LayoutAdjusted"); break;
        case PlaybackNoticeKind::LayoutSaved: status = loc::Text(L"Notification.LayoutSaved"); break;
        case PlaybackNoticeKind::LayoutReset: status = loc::Text(L"Notification.LayoutReset"); break;
        case PlaybackNoticeKind::TrackChanged:
        default: status = loc::Text(L"Notification.NowPlaying"); break;
    }
    if (!notice.play_mode.empty() && notice.kind != PlaybackNoticeKind::PlayModeChanged) {
        status += L" · " + notice.play_mode;
    }
    return status;
}

// 为不同通知类型选择状态强调色。
// 只改变标题色，不影响用户配置的面板透明度。
Gdiplus::Color StatusColor(PlaybackNoticeKind kind) {
    switch (kind) {
        case PlaybackNoticeKind::Paused:
            return Gdiplus::Color(255, 255, 196, 92);
        case PlaybackNoticeKind::Resumed:
            return Gdiplus::Color(255, 118, 220, 143);
        case PlaybackNoticeKind::AlbumChanged:
            return Gdiplus::Color(255, 188, 145, 255);
        case PlaybackNoticeKind::PlayModeChanged:
            return Gdiplus::Color(255, 255, 154, 205);
        case PlaybackNoticeKind::CurrentStatus:
            return Gdiplus::Color(255, 140, 210, 255);
        case PlaybackNoticeKind::LayoutAdjusted:
            return Gdiplus::Color(255, 116, 205, 255);
        case PlaybackNoticeKind::LayoutSaved:
            return Gdiplus::Color(255, 118, 220, 143);
        case PlaybackNoticeKind::LayoutReset:
            return Gdiplus::Color(255, 255, 196, 92);
        case PlaybackNoticeKind::TrackChanged:
        default:
            return Gdiplus::Color(255, 110, 214, 255);
    }
}

// 判断游戏主窗口是否为当前前台根窗口。
// 无边框子窗口获得焦点时仍按同一根窗口识别。
bool GameIsForeground(HWND game_window) {
    if (!game_window) {
        return false;
    }
    HWND foreground = GetForegroundWindow();
    if (!foreground) {
        return false;
    }
    return GetAncestor(foreground, GA_ROOT) == GetAncestor(game_window, GA_ROOT);
}

// 把游戏客户区矩形转换为屏幕物理像素坐标。
// 失败时通知隐藏，不使用陈旧坐标覆盖其他程序。
bool GameClientRectangle(HWND game_window, RECT& rectangle) {
    RECT client{};
    if (!game_window || !GetClientRect(game_window, &client)) {
        return false;
    }
    POINT top_left{client.left, client.top};
    POINT bottom_right{client.right, client.bottom};
    if (!ClientToScreen(game_window, &top_left) || !ClientToScreen(game_window, &bottom_right)) {
        return false;
    }
    rectangle = {top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

// 按当前布局、缩放、透明度和动画进度绘制一帧分层窗口。
// 位置与大小解耦，并保证至少 48 像素留在游戏客户区内。
void RenderNotice(HWND window, HWND game_window, const NowPlayingOverlay::Notice& notice,
                  Gdiplus::Bitmap* cover, const Config& config, float animation_alpha) {
    RECT game_rectangle{};
    if (!GameClientRectangle(game_window, game_rectangle)) {
        ShowWindow(window, SW_HIDE);
        return;
    }

    // 通知使用固定物理像素模型：Windows DPI 只决定坐标 API 是否被虚拟化，
    // 不再参与组件尺寸计算。用户可通过 NotificationScalePercent 明确控制
    // 大小，通过 NotificationMarginX/Y 分别避开计时、排名等 HUD 区域。
    const float user_scale = static_cast<float>(config.notification_scale_percent) / 100.0F;
    const int margin_x = static_cast<int>(config.notification_margin_x);
    const int margin_y = static_cast<int>(config.notification_margin_y);
    const float requested_scale = user_scale;
    // X/Y 只负责位置，绝不能影响组件大小。自动适配只根据游戏客户区本身
    // 计算，因此用户把通知从角落移到 HUD 空白处时不会出现意外缩小。
    constexpr int kFitPadding = 16;
    const float available_width = static_cast<float>(
        std::max(1L, game_rectangle.right - game_rectangle.left - kFitPadding * 2));
    const float available_height = static_cast<float>(
        std::max(1L, game_rectangle.bottom - game_rectangle.top - kFitPadding * 2));
    const float fit_scale = std::min(available_width / static_cast<float>(kBaseWidth),
                                     available_height / static_cast<float>(kBaseHeight));
    // 极小窗口或过高用户缩放时自动收缩，保证通知主体仍能绘制。
    const float scale = std::max(0.58F, std::min(requested_scale, fit_scale));
    const int width = std::max(300, static_cast<int>(std::lround(kBaseWidth * scale)));
    const int height = std::max(82, static_cast<int>(std::lround(kBaseHeight * scale)));

    int x = game_rectangle.left + margin_x;
    int y = game_rectangle.top + margin_y;
    const bool right = config.notification_position == NotificationPosition::TopRight ||
                       config.notification_position == NotificationPosition::BottomRight;
    const bool bottom = config.notification_position == NotificationPosition::BottomLeft ||
                        config.notification_position == NotificationPosition::BottomRight;
    if (right) {
        x = game_rectangle.right - margin_x - width;
    }
    if (bottom) {
        y = game_rectangle.bottom - margin_y - height;
    }

    // 即使用户保存了过大的 X/Y，至少保留 48 像素在客户区中，防止通知
    // 完全不可见。该保护只约束实际绘制坐标，不偷偷改写用户的 INI；用户可用
    // Backspace 或 BACK+R3 恢复默认布局。
    constexpr int kMinimumVisiblePixels = 48;
    x = std::clamp(x, static_cast<int>(game_rectangle.left) - width + kMinimumVisiblePixels,
                   static_cast<int>(game_rectangle.right) - kMinimumVisiblePixels);
    y = std::clamp(y, static_cast<int>(game_rectangle.top) - height + kMinimumVisiblePixels,
                   static_cast<int>(game_rectangle.bottom) - kMinimumVisiblePixels);

    // 淡入淡出同时附带很小的水平滑动，但窗口始终不获取焦点。
    const int slide = static_cast<int>(std::lround((1.0F - animation_alpha) * 18.0F * scale));
    x += right ? slide : -slide;

    BITMAPINFO bitmap_info{};
    bitmap_info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap_info.bmiHeader.biWidth = width;
    bitmap_info.bmiHeader.biHeight = -height;
    bitmap_info.bmiHeader.biPlanes = 1;
    bitmap_info.bmiHeader.biBitCount = 32;
    bitmap_info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &bitmap_info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!dib || !pixels) {
        if (dib) DeleteObject(dib);
        return;
    }
    HDC memory_dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old_bitmap = SelectObject(memory_dc, dib);

    Gdiplus::Bitmap surface(width, height, width * 4, PixelFormat32bppPARGB,
                            static_cast<BYTE*>(pixels));
    Gdiplus::Graphics graphics(&surface);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    graphics.Clear(Gdiplus::Color(0, 0, 0, 0));

    const float radius = 14.0F * scale;
    Gdiplus::GraphicsPath shadow_path;
    AddRoundedRectangle(shadow_path,
                        Gdiplus::RectF(4.0F * scale, 5.0F * scale,
                                       static_cast<float>(width) - 8.0F * scale,
                                       static_cast<float>(height) - 8.0F * scale), radius);
    Gdiplus::SolidBrush shadow(Gdiplus::Color(90, 0, 0, 0));
    graphics.FillPath(&shadow, &shadow_path);

    Gdiplus::GraphicsPath panel_path;
    AddRoundedRectangle(panel_path,
                        Gdiplus::RectF(0.0F, 0.0F, static_cast<float>(width) - 6.0F * scale,
                                       static_cast<float>(height) - 6.0F * scale), radius);
    const BYTE panel_alpha = static_cast<BYTE>(std::clamp<uint32_t>(
        config.notification_opacity_percent * 255U / 100U, 0U, 255U));
    Gdiplus::SolidBrush panel(Gdiplus::Color(panel_alpha, 21, 23, 27));
    graphics.FillPath(&panel, &panel_path);

    const float cover_margin = 18.0F * scale;
    const float cover_size = static_cast<float>(kBaseCoverSize) * scale;
    const Gdiplus::RectF cover_rectangle(cover_margin, cover_margin, cover_size, cover_size);
    Gdiplus::GraphicsPath cover_path;
    AddRoundedRectangle(cover_path, cover_rectangle, 9.0F * scale);
    graphics.SetClip(&cover_path);
    if (cover) {
        graphics.DrawImage(cover, cover_rectangle, 0.0F, 0.0F,
                           static_cast<float>(cover->GetWidth()), static_cast<float>(cover->GetHeight()),
                           Gdiplus::UnitPixel);
    } else {
        Gdiplus::SolidBrush fallback_background(Gdiplus::Color(255, 48, 52, 61));
        graphics.FillRectangle(&fallback_background, cover_rectangle);
        Gdiplus::Font note_font(L"Segoe UI Symbol", 48.0F * scale, Gdiplus::FontStyleRegular,
                                Gdiplus::UnitPixel);
        Gdiplus::SolidBrush note_brush(Gdiplus::Color(230, 215, 220, 230));
        Gdiplus::StringFormat centered;
        centered.SetAlignment(Gdiplus::StringAlignmentCenter);
        centered.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        graphics.DrawString(L"♪", -1, &note_font, cover_rectangle, &centered, &note_brush);
    }
    graphics.ResetClip();

    const float text_x = cover_rectangle.GetRight() + 18.0F * scale;
    const float text_width = static_cast<float>(width) - text_x - 24.0F * scale;
    Gdiplus::Font status_font(L"Segoe UI", 13.0F * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font title_font(L"Segoe UI", 20.0F * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::Font detail_font(L"Segoe UI", 14.0F * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush status_brush(StatusColor(notice.kind));
    Gdiplus::SolidBrush title_brush(Gdiplus::Color(255, 250, 250, 252));
    Gdiplus::SolidBrush detail_brush(Gdiplus::Color(225, 196, 201, 211));
    Gdiplus::StringFormat line_format;
    line_format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    line_format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);

    const std::wstring status = StatusText(notice);
    graphics.DrawString(status.c_str(), -1, &status_font,
                        Gdiplus::RectF(text_x, 18.0F * scale, text_width, 22.0F * scale),
                        &line_format, &status_brush);

    std::wstring title = notice.metadata.title;
    if (title.empty()) {
        title = notice.path.stem().wstring();
    }
    graphics.DrawString(title.c_str(), -1, &title_font,
                        Gdiplus::RectF(text_x, 43.0F * scale, text_width, 31.0F * scale),
                        &line_format, &title_brush);

    std::wstring artist = notice.metadata.artist.empty() ? loc::Text(L"Notification.LocalArtist") : notice.metadata.artist;
    graphics.DrawString(artist.c_str(), -1, &detail_font,
                        Gdiplus::RectF(text_x, 79.0F * scale, text_width, 23.0F * scale),
                        &line_format, &detail_brush);
    if (!notice.metadata.album.empty()) {
        graphics.DrawString(notice.metadata.album.c_str(), -1, &detail_font,
                            Gdiplus::RectF(text_x, 104.0F * scale, text_width, 23.0F * scale),
                            &line_format, &detail_brush);
    }

    HDC screen_dc = GetDC(nullptr);
    POINT destination{x, y};
    POINT source{0, 0};
    SIZE size{width, height};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(
        std::clamp<int>(static_cast<int>(std::lround(animation_alpha * 255.0F)), 0, 255));
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(window, screen_dc, &destination, &size, memory_dc, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen_dc);
    SetWindowPos(window, HWND_TOPMOST, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOSENDCHANGING);

    SelectObject(memory_dc, old_bitmap);
    DeleteDC(memory_dc);
    DeleteObject(dib);
}

}  // namespace

// 返回通知层进程级单例。
// 使用显式 Shutdown 管理窗口线程，避免 DLL 静态析构顺序问题。
NowPlayingOverlay& NowPlayingOverlay::Instance() {
    // 有意让单例随进程结束统一回收，避免 DLL_PROCESS_DETACH 时静态析构
    // 与仍在退出的窗口线程发生顺序竞争。
    static NowPlayingOverlay* instance = new NowPlayingOverlay();
    return *instance;
}

// 复制配置、创建同步事件并等待通知窗口线程完成初始化。
// 失败只禁用非核心 UI，播放器和 Hook 仍可继续运行。
bool NowPlayingOverlay::Initialize(const Config& config) {
    std::lock_guard lock(mutex_);
    if (initialized_.load(std::memory_order_acquire)) {
        return true;
    }
    config_ = config;
    if (!config_.show_now_playing_notification && !config_.show_pause_notification &&
        !config_.show_resume_notification && !config_.enable_album_controls) {
        return true;
    }

    wake_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_event_ || !ready_event_) {
        if (wake_event_) CloseHandle(wake_event_);
        if (ready_event_) CloseHandle(ready_event_);
        wake_event_ = nullptr;
        ready_event_ = nullptr;
        log::Warn(loc::Text(L"Log.Overlay.EventFailed"));
        return false;
    }

    stop_.store(false, std::memory_order_release);
    ready_succeeded_.store(false, std::memory_order_release);
    thread_ = std::thread(&NowPlayingOverlay::ThreadMain, this);
    const DWORD wait_result = WaitForSingleObject(ready_event_, 3000);
    const bool ready = wait_result == WAIT_OBJECT_0 &&
                       ready_succeeded_.load(std::memory_order_acquire);
    if (!ready) {
        stop_.store(true, std::memory_order_release);
        SetEvent(wake_event_);
        if (thread_.joinable()) thread_.join();
        CloseHandle(wake_event_);
        CloseHandle(ready_event_);
        wake_event_ = nullptr;
        ready_event_ = nullptr;
        log::Warn(loc::Text(L"Log.Overlay.InitFailed"));
        return false;
    }
    initialized_.store(true, std::memory_order_release);
    log::Info(loc::Text(L"Log.Overlay.Enabled"));
    log::Info(loc::Format(L"Log.Overlay.Layout", {
        std::to_wstring(config_.notification_scale_percent),
        std::to_wstring(config_.notification_opacity_percent),
        std::to_wstring(config_.notification_margin_x),
        std::to_wstring(config_.notification_margin_y)}));
    return true;
}

// 通知线程退出后关闭事件句柄并清空待处理通知。
// 保证 GDI+/COM 资源已在线程内按正确顺序释放。
void NowPlayingOverlay::Shutdown() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    if (wake_event_) SetEvent(wake_event_);
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    pending_notice_.reset();
    if (wake_event_) CloseHandle(wake_event_);
    if (ready_event_) CloseHandle(ready_event_);
    wake_event_ = nullptr;
    ready_event_ = nullptr;
}

// 构造“正在播放”曲目通知快照并排入单槽队列。
// 按配置决定是否在状态行显示播放模式。
void NowPlayingOverlay::ShowTrack(const std::filesystem::path& path,
                                    const TrackMetadata& metadata,
                                    const std::wstring& play_mode) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::TrackChanged;
    notice.path = path;
    notice.metadata = metadata;
    {
        std::lock_guard lock(mutex_);
        notice.play_mode = config_.show_play_mode_in_notification ? play_mode : std::wstring{};
    }
    QueueNotice(std::move(notice));
}

// 构造“已暂停”通知，保留当前曲目和封面信息。
// 仅在游戏实际调用 Spotify Pause 时由播放器触发。
void NowPlayingOverlay::ShowPaused(const std::filesystem::path& path,
                                   const TrackMetadata& metadata,
                                   const std::wstring& play_mode) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::Paused;
    notice.path = path;
    notice.metadata = metadata;
    {
        std::lock_guard lock(mutex_);
        notice.play_mode = config_.show_play_mode_in_notification ? play_mode : std::wstring{};
    }
    QueueNotice(std::move(notice));
}

// 构造“继续播放”通知并显示当前曲目信息。
// 不会改变 Channel 状态，播放恢复已由 LocalPlayer 完成。
void NowPlayingOverlay::ShowResumed(const std::filesystem::path& path,
                                    const TrackMetadata& metadata,
                                    const std::wstring& play_mode) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::Resumed;
    notice.path = path;
    notice.metadata = metadata;
    {
        std::lock_guard lock(mutex_);
        notice.play_mode = config_.show_play_mode_in_notification ? play_mode : std::wstring{};
    }
    QueueNotice(std::move(notice));
}

// 构造专辑切换通知，使用代表曲目的专辑封面来源。
// 标题、艺术家和曲目数使用专辑级元数据。
void NowPlayingOverlay::ShowAlbum(const std::filesystem::path& representative_track,
                                  const TrackMetadata& representative_metadata,
                                  const std::wstring& album_title,
                                  const std::wstring& album_artist,
                                  size_t track_count,
                                  const std::wstring& play_mode) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::AlbumChanged;
    notice.path = representative_track;
    notice.metadata = representative_metadata;
    notice.metadata.title = album_title.empty() ? loc::Text(L"Notification.UnknownAlbum") : album_title;
    notice.metadata.artist = album_artist.empty() ? loc::Text(L"Notification.LocalAlbum") : album_artist;
    notice.metadata.album = loc::Format(L"Notification.TrackCount", {std::to_wstring(track_count)});
    notice.play_mode = play_mode;
    notice.use_album_cover = true;
    QueueNotice(std::move(notice));
}

// 构造播放模式切换通知，显示全曲库曲目数。
// 模式持久化由 LocalPlayer 完成，通知层只展示快照。
void NowPlayingOverlay::ShowPlayMode(const std::filesystem::path& current_track,
                                     const TrackMetadata& current_metadata,
                                     const std::wstring& play_mode,
                                     size_t total_tracks) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::PlayModeChanged;
    notice.path = current_track;
    notice.metadata = current_metadata;
    notice.metadata.title = play_mode;
    notice.metadata.artist = loc::Format(L"Notification.LibraryCount", {std::to_wstring(total_tracks)});
    notice.metadata.album.clear();
    notice.play_mode = play_mode;
    QueueNotice(std::move(notice));
}

// 构造用户主动查询的当前状态通知。
// 不改变通知配置或播放状态。
void NowPlayingOverlay::ShowCurrentStatus(const std::filesystem::path& current_track,
                                          const TrackMetadata& current_metadata,
                                          const std::wstring& play_mode) {
    Notice notice;
    notice.kind = PlaybackNoticeKind::CurrentStatus;
    notice.path = current_track;
    notice.metadata = current_metadata;
    notice.play_mode = play_mode;
    QueueNotice(std::move(notice));
}

// 实时修改内存中的布局参数，并立即显示带当前数值的预览通知。位置增量
// 使用屏幕视觉方向，内部根据锚点转换成“从角落向内”的 Margin 增减。
void NowPlayingOverlay::AdjustLayout(int delta_x, int delta_y, int delta_scale, int delta_opacity) {
    if (!initialized_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(mutex_);
        const bool right = config_.notification_position == NotificationPosition::TopRight ||
                           config_.notification_position == NotificationPosition::BottomRight;
        const bool bottom = config_.notification_position == NotificationPosition::BottomLeft ||
                            config_.notification_position == NotificationPosition::BottomRight;
        const int next_x = static_cast<int>(config_.notification_margin_x) + (right ? -delta_x : delta_x);
        const int next_y = static_cast<int>(config_.notification_margin_y) + (bottom ? -delta_y : delta_y);
        config_.notification_margin_x = static_cast<uint32_t>(std::clamp(next_x, 0, 2000));
        config_.notification_margin_y = static_cast<uint32_t>(std::clamp(next_y, 0, 2000));
        config_.notification_scale_percent = static_cast<uint32_t>(std::clamp(
            static_cast<int>(config_.notification_scale_percent) + delta_scale, 50, 200));
        config_.notification_opacity_percent = static_cast<uint32_t>(std::clamp(
            static_cast<int>(config_.notification_opacity_percent) + delta_opacity, 40, 100));
    }
    ShowLayoutNotice(PlaybackNoticeKind::LayoutAdjusted);
}

// 仅更新 INI 中与通知布局相关的五个键。WritePrivateProfileStringW 会保留
// 其他节和值；最后的空写调用用于刷新 Windows 的 INI 缓存。
bool NowPlayingOverlay::PersistLayout(const Config& snapshot) const {
    if (snapshot.ini_path.empty()) return false;
    const std::wstring margin_x = std::to_wstring(snapshot.notification_margin_x);
    const std::wstring margin_y = std::to_wstring(snapshot.notification_margin_y);
    const std::wstring scale = std::to_wstring(snapshot.notification_scale_percent);
    const std::wstring opacity = std::to_wstring(snapshot.notification_opacity_percent);
    const wchar_t* position = L"TopRight";
    switch (snapshot.notification_position) {
        case NotificationPosition::TopLeft: position = L"TopLeft"; break;
        case NotificationPosition::BottomLeft: position = L"BottomLeft"; break;
        case NotificationPosition::BottomRight: position = L"BottomRight"; break;
        case NotificationPosition::TopRight:
        default: break;
    }
    const std::wstring ini_path = snapshot.ini_path.wstring();
    const wchar_t* path = ini_path.c_str();
    const bool ok = WritePrivateProfileStringW(L"LocalMusic", L"NotificationPosition", position, path) != FALSE &&
        WritePrivateProfileStringW(L"LocalMusic", L"NotificationMarginX", margin_x.c_str(), path) != FALSE &&
        WritePrivateProfileStringW(L"LocalMusic", L"NotificationMarginY", margin_y.c_str(), path) != FALSE &&
        WritePrivateProfileStringW(L"LocalMusic", L"NotificationScalePercent", scale.c_str(), path) != FALSE &&
        WritePrivateProfileStringW(L"LocalMusic", L"NotificationOpacityPercent", opacity.c_str(), path) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path);
    return ok;
}

// 保存当前布局快照。磁盘写入不在 mutex_ 内进行，避免慢速磁盘或安全软件
// 扫描 INI 时阻塞通知线程读取配置。
void NowPlayingOverlay::SaveLayout() {
    if (!initialized_.load(std::memory_order_acquire)) return;
    Config snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = config_;
    }
    if (PersistLayout(snapshot)) {
        log::Info(loc::Format(L"Log.Overlay.LayoutSaved", {
            std::to_wstring(snapshot.notification_margin_x),
            std::to_wstring(snapshot.notification_margin_y),
            std::to_wstring(snapshot.notification_scale_percent),
            std::to_wstring(snapshot.notification_opacity_percent)}));
        ShowLayoutNotice(PlaybackNoticeKind::LayoutSaved);
    } else {
        log::Warn(loc::Text(L"Log.Overlay.LayoutWriteFailed"));
    }
}

// 恢复发行默认布局并立即持久化。重置同时恢复锚点、位置、缩放和透明度，
// 让用户即使把通知移出可见区域也能通过固定快捷键找回。
void NowPlayingOverlay::ResetLayout() {
    if (!initialized_.load(std::memory_order_acquire)) return;
    Config snapshot;
    {
        std::lock_guard lock(mutex_);
        config_.notification_position = NotificationPosition::TopRight;
        config_.notification_margin_x = kDefaultMarginX;
        config_.notification_margin_y = kDefaultMarginY;
        config_.notification_scale_percent = kDefaultScalePercent;
        config_.notification_opacity_percent = kDefaultOpacityPercent;
        snapshot = config_;
    }
    if (!PersistLayout(snapshot)) {
        log::Warn(loc::Text(L"Log.Overlay.LayoutResetWriteFailed"));
    } else {
        log::Info(loc::Text(L"Log.Overlay.LayoutReset"));
    }
    ShowLayoutNotice(PlaybackNoticeKind::LayoutReset);
}

// 生成布局状态通知。该通知允许 path 为空，不尝试读取封面，而是使用默认
// 音乐图标；数值来自同一锁内快照，确保四项显示彼此一致。
void NowPlayingOverlay::ShowLayoutNotice(PlaybackNoticeKind kind) {
    Config snapshot;
    {
        std::lock_guard lock(mutex_);
        snapshot = config_;
    }
    Notice notice;
    notice.kind = kind;
    notice.metadata.title = L"X=" + std::to_wstring(snapshot.notification_margin_x) +
                            L"  Y=" + std::to_wstring(snapshot.notification_margin_y);
    notice.metadata.artist = loc::Format(L"Notification.LayoutDetails", {
        std::to_wstring(snapshot.notification_scale_percent),
        std::to_wstring(snapshot.notification_opacity_percent)});
    notice.metadata.album = kind == PlaybackNoticeKind::LayoutAdjusted
        ? loc::Text(L"Notification.LayoutAutoSave")
        : loc::Format(L"Notification.LayoutWritten", {snapshot.ini_path.filename().wstring()});
    QueueNotice(std::move(notice));
}

// 单槽通知队列入口。布局通知不依赖音频路径，因此允许 path 为空；其他通知
// 仍要求有效曲目路径，避免无意义的空“正在播放”窗口。
void NowPlayingOverlay::QueueNotice(Notice notice) {
    const bool layout_notice = notice.kind == PlaybackNoticeKind::LayoutAdjusted ||
                               notice.kind == PlaybackNoticeKind::LayoutSaved ||
                               notice.kind == PlaybackNoticeKind::LayoutReset;
    if (!initialized_.load(std::memory_order_acquire) || (notice.path.empty() && !layout_notice)) return;
    std::lock_guard lock(mutex_);
    if ((notice.kind == PlaybackNoticeKind::TrackChanged && !config_.show_now_playing_notification) ||
        (notice.kind == PlaybackNoticeKind::Paused && !config_.show_pause_notification) ||
        (notice.kind == PlaybackNoticeKind::Resumed && !config_.show_resume_notification)) {
        return;
    }
    notice.serial = ++next_serial_;
    pending_notice_ = std::move(notice);
    SetEvent(wake_event_);
}

// 处理分层通知窗口的最小 Win32 消息集。
// 窗口鼠标穿透、不激活，关闭消息只隐藏而不终止游戏。
LRESULT CALLBACK NowPlayingOverlay::WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            ShowWindow(window, SW_HIDE);
            return 0;
        default:
            return DefWindowProcW(window, message, wparam, lparam);
    }
}

// 运行通知窗口消息循环、封面缓存、淡入淡出与实时布局重绘。
// 所有 GDI+/COM 对象都在本线程创建和销毁，避免跨线程资源使用。
void NowPlayingOverlay::ThreadMain() {
    // 即使游戏进程已经由 EXE manifest 设置了其他 DPI 模式，也为本通知线程
    // 单独请求 Per-Monitor V2。这样 GetClientRect/ClientToScreen 返回物理像素，
    // Windows 不会再对分层窗口做 DPI 位图虚拟化。
    const HANDLE previous_dpi_context = config_.enable_high_dpi_awareness
        ? EnableCurrentThreadPerMonitorV2()
        : nullptr;

    const HRESULT com_result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ULONG_PTR gdiplus_token = 0;
    Gdiplus::GdiplusStartupInput startup_input;
    const bool gdiplus_ready = Gdiplus::GdiplusStartup(&gdiplus_token, &startup_input, nullptr) == Gdiplus::Ok;

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&NowPlayingOverlay::WindowProcedure), &module);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.hInstance = module;
    window_class.lpfnWndProc = &NowPlayingOverlay::WindowProcedure;
    window_class.lpszClassName = kWindowClassName;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const ATOM atom = RegisterClassExW(&window_class);

    HWND window = nullptr;
    if (gdiplus_ready && (atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)) {
        window = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kWindowClassName, loc::Text(L"Notification.WindowTitle").c_str(), WS_POPUP,
            0, 0, 1, 1, nullptr, nullptr, module, nullptr);
    }

    ready_succeeded_.store(window != nullptr, std::memory_order_release);
    SetEvent(ready_event_);
    if (!window) {
        if (atom != 0) {
            UnregisterClassW(kWindowClassName, module);
        }
        if (gdiplus_ready) Gdiplus::GdiplusShutdown(gdiplus_token);
        if (SUCCEEDED(com_result)) CoUninitialize();
        RestoreCurrentThreadDpiAwareness(previous_dpi_context);
        return;
    }

    std::vector<CachedArtwork> artwork_cache;
    std::optional<Notice> active_notice;
    uint64_t active_since = 0;
    Gdiplus::Bitmap* active_cover = nullptr;

    while (!stop_.load(std::memory_order_acquire)) {
        ResetEvent(wake_event_);
        std::optional<Notice> incoming_notice;
        Config artwork_config;
        {
            std::lock_guard lock(mutex_);
            artwork_config = config_;
            if (pending_notice_) {
                incoming_notice = std::move(pending_notice_);
                pending_notice_.reset();
            }
        }
        if (incoming_notice) {
            active_notice = std::move(incoming_notice);
            // 文件读取和 GDI+ 解码必须在 Overlay 锁外执行，否则一次损坏或
            // 超大封面会反向阻塞播放器投递新的换曲通知。
            active_cover = FindOrLoadArtwork(artwork_cache, *active_notice, artwork_config);
            bool superseded = false;
            {
                std::lock_guard lock(mutex_);
                superseded = pending_notice_ && pending_notice_->serial > active_notice->serial;
            }
            if (superseded) {
                // 解码期间又发生了切歌：不要让已经过时的通知闪现一帧。
                active_notice.reset();
                active_cover = nullptr;
            } else {
                // 从封面解码完成后开始计时，确保大封面不会吞掉用户可见时长。
                active_since = GetTickCount64();
            }
        }

        DWORD wait_ms = INFINITE;
        if (active_notice) {
            Config config_snapshot;
            {
                std::lock_guard lock(mutex_);
                config_snapshot = config_;
            }
            const uint64_t now = GetTickCount64();
            const uint64_t elapsed = now >= active_since ? now - active_since : 0;
            const bool long_notice = active_notice->kind == PlaybackNoticeKind::TrackChanged ||
                                     active_notice->kind == PlaybackNoticeKind::AlbumChanged;
            const uint32_t hold = long_notice ? config_snapshot.notification_duration_ms
                                              : config_snapshot.notification_status_duration_ms;
            const uint32_t fade = config_snapshot.notification_fade_ms;
            const uint64_t total = static_cast<uint64_t>(hold) + static_cast<uint64_t>(fade) * 2ULL;
            if (elapsed >= total) {
                ShowWindow(window, SW_HIDE);
                active_notice.reset();
                active_cover = nullptr;
            } else {
                float alpha = 1.0F;
                if (fade != 0 && elapsed < fade) {
                    alpha = static_cast<float>(elapsed) / static_cast<float>(fade);
                } else if (fade != 0 && elapsed > static_cast<uint64_t>(fade) + hold) {
                    alpha = static_cast<float>(total - elapsed) / static_cast<float>(fade);
                }
                alpha = std::clamp(alpha, 0.0F, 1.0F);

                HWND game_window = FindGameWindow();
                if (config_snapshot.notification_only_when_game_foreground &&
                    !GameIsForeground(game_window)) {
                    ShowWindow(window, SW_HIDE);
                } else {
                    RenderNotice(window, game_window, *active_notice, active_cover, config_snapshot, alpha);
                }
                wait_ms = 16;
            }
        }

        const DWORD wait_result = MsgWaitForMultipleObjectsEx(
            1, &wake_event_, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait_result == WAIT_OBJECT_0 + 1) {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    stop_.store(true, std::memory_order_release);
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
    }

    ShowWindow(window, SW_HIDE);
    active_cover = nullptr;
    active_notice.reset();
    // 所有 GDI+ 对象必须在 GdiplusShutdown 之前销毁。
    artwork_cache.clear();
    DestroyWindow(window);
    if (atom != 0) {
        UnregisterClassW(kWindowClassName, module);
    }
    Gdiplus::GdiplusShutdown(gdiplus_token);
    if (SUCCEEDED(com_result)) {
        CoUninitialize();
    }
    RestoreCurrentThreadDpiAwareness(previous_dpi_context);
}

}  // namespace localmusic
