// 运行时国际化实现：解析 UTF-8/UTF-16 INI、规范化语言标签并执行分层回退。
#include "Localization.h"

#include <iterator>

namespace localmusic::loc {
namespace {

using Dictionary = std::unordered_map<std::wstring, std::wstring>;

std::mutex g_mutex;
Dictionary g_selected;
Dictionary g_english;
std::wstring g_requested = L"auto";
std::wstring g_active = L"zh-hans";
std::filesystem::path g_active_file;
std::filesystem::path g_dll_path;
std::vector<StartupDiagnostic> g_diagnostics;

struct BuiltInEntry {
    const wchar_t* key;
    const wchar_t* value;
};

// 内置简体中文是不可删除的最终回退。外部语言文件可以覆盖任意单个键；
// 即使文件缺失、损坏或翻译不完整，日志和通知也不会出现空白。
constexpr BuiltInEntry kBuiltInZhHans[] = {
    {L"General.AppName", L"LocalMusic"},
    {L"General.Yes", L"是"},
    {L"General.No", L"否"},
    {L"General.Milliseconds", L"毫秒"},

    {L"Logger.Title", L"LocalMusic 日志"},
    {L"Logger.Info", L"信息"},
    {L"Logger.Warning", L"警告"},
    {L"Logger.Error", L"错误"},

    {L"Localization.Active", L"运行语言={0}"},
    {L"Localization.FileLoaded", L"已加载语言文件：{0}"},
    {L"Localization.InvalidTag", L"Language 值无效，已按 auto 处理：{0}"},
    {L"Localization.RequestedMissing", L"指定语言文件不存在或无法读取，已回退到 en-us：{0}"},
    {L"Localization.EnglishMissing", L"en-us 语言文件不存在或无法读取，已回退到内置 zh-hans"},

    {L"Notification.NowPlaying", L"正在播放"},
    {L"Notification.Paused", L"已暂停"},
    {L"Notification.Resumed", L"继续播放"},
    {L"Notification.AlbumChanged", L"已切换专辑"},
    {L"Notification.PlayMode", L"播放模式"},
    {L"Notification.CurrentStatus", L"当前播放"},
    {L"Notification.LayoutAdjusted", L"通知布局调整"},
    {L"Notification.LayoutSaved", L"通知布局已保存"},
    {L"Notification.LayoutReset", L"通知布局已重置"},
    {L"Notification.UnknownAlbum", L"未命名专辑"},
    {L"Notification.LocalAlbum", L"本地专辑"},
    {L"Notification.TrackCount", L"{0} 首曲目"},
    {L"Notification.LibraryCount", L"全曲库 · {0} 首"},
    {L"Notification.LocalArtist", L"本地音乐"},
    {L"Notification.LayoutDetails", L"缩放 {0}% · 透明度 {1}%"},
    {L"Notification.LayoutAutoSave", L"松开 BACK 或停止键盘调整后自动保存"},
    {L"Notification.LayoutWritten", L"已写入 {0}"},
    {L"Notification.WindowTitle", L"Dangerous Driving 当前曲目信息"},

    {L"PlayMode.Sequential", L"顺序播放"},
    {L"PlayMode.Random", L"随机播放"},
    {L"PlayMode.SingleLoop", L"单曲循环"},

    {L"Metadata.Uncategorized", L"未分类曲目"},
    {L"Metadata.LocalMusic", L"本地音乐"},

    {L"MessageBox.EmptyLibraryTitle", L"LocalMusic"},
    {L"MessageBox.EmptyLibraryBody", L"没有找到可播放的本地音乐。\n\n音乐目录：{0}\n\n游戏会继续启动。认证将在内存中直接视为成功，存档中的 token 不会被读取、清空或写回；没有曲目时不会播放 BGM。\n添加音乐后可按 F5 重新扫描；播放器仍会等待游戏发出播放指令。"},

    {L"Config.Header", L"LocalMusic——发行版用户配置"},
    {L"Config.PlaceFile", L"请把本文件与 {0} 一起放在 DangerousDriving\\Binaries\\Win64。"},
    {L"Config.HiddenOptions", L"未显示的播放、认证、Hook、专辑、封面、预加载和高 DPI 选项使用稳定内置值。"},
    {L"Config.Restart", L"修改普通配置后需要重新启动游戏；PlayMode 与通知布局可在游戏内修改并自动保存。"},
    {L"Config.LayoutHeader", L"游戏内通知布局调整："},
    {L"Config.Controller1", L"手柄：按住 BACK + 十字键移动；BACK + LB + 上/下调整大小；"},
    {L"Config.Controller2", L"      BACK + LB + 左/右调整透明度；BACK + R3 重置布局。"},
    {L"Config.Keyboard1", L"键盘：Ctrl + -/= 调整 Y；Shift + -/= 调整 X；Alt + -/= 调整大小；"},
    {L"Config.Keyboard2", L"      直接按 -/= 调整透明度；Backspace 重置布局。"},
    {L"Config.Language", L"运行语言：auto=按 Windows UI 语言；也可填写 zh-hans、en-us 或用户自建语言标签。"},
    {L"Config.Logging", L"是否写入 {0}。false 时不创建、不截断、也不追加日志文件。"},
    {L"Config.MusicPath", L"音乐目录。相对路径以 DangerousDriving\\Binaries\\Win64 为基准。"},
    {L"Config.PlayMode", L"播放模式：Sequential=顺序；Random=全曲库会话乱序；SingleLoop=单曲循环。"},
    {L"Config.Volume1", L"插件永久音量倍率。最终音量 = Volume × 游戏内持久 Spotify 音量。"},
    {L"Config.Volume2", L"支持 0.0-1.0，也兼容 0-100。"},
    {L"Config.Notifications", L"曲目信息、暂停和继续播放通知。"},
    {L"Config.Duration", L"换曲通知保持时间，单位毫秒，不含淡入淡出。"},
    {L"Config.ScaleOpacity", L"通知整体大小和背景不透明度，单位百分比；游戏内调整会写回这两个值。"},
    {L"Config.Position", L"通知锚点：TopLeft、TopRight、BottomLeft、BottomRight。"},
    {L"Config.Margin1", L"从锚点向游戏画面内部移动的水平/垂直距离，单位物理像素；"},
    {L"Config.Margin2", L"游戏内移动通知时会写回这两个值。"},

    {L"Log.DllMain.PluginLoaded", L"插件已加载，路径：{0}"},
    {L"Log.DllMain.DpiPerMonitorV2", L"已为游戏进程启用 Per-Monitor V2 高 DPI 感知"},
    {L"Log.DllMain.DpiPerMonitor", L"已为游戏进程启用 Per-Monitor 高 DPI 感知（系统不支持 V2）"},
    {L"Log.DllMain.DpiSystem", L"已为游戏进程启用系统级高 DPI 感知（兼容回退）"},
    {L"Log.DllMain.DpiAlready", L"游戏进程已有 DPI 感知配置；保持原设置，通知线程将独立请求 Per-Monitor V2"},
    {L"Log.DllMain.DpiFailed", L"无法设置游戏进程 DPI 感知，错误码={0}；通知仍会启动，但可能受 Windows DPI 虚拟化影响"},
    {L"Log.DllMain.DpiDisabled", L"EnableHighDpiAwareness=false：不修改游戏或通知线程的 DPI 感知模式"},
    {L"Log.DllMain.PlayerInitFailed", L"本地 FMOD 播放器初始化失败"},
    {L"Log.DllMain.EmptyLibrary", L"启动时没有本地曲目；游戏集成将在静音状态等待"},
    {L"Log.DllMain.HooksUnavailable", L"未安装游戏接口集成；本地播放和热键功能仍可使用"},
    {L"Log.DllMain.WorkerStopped", L"插件工作线程已停止"},

    {L"Log.Config.CreateFailed", L"无法创建 {0}（Win32 错误 {1}）；将继续使用内置默认配置"},
    {L"Log.Config.WriteFailed", L"无法完成写入 {0}（Win32 错误 {1}）；已删除不完整文件并使用内置默认配置"},
    {L"Log.Config.Created", L"已生成默认配置：{0}"},
    {L"Log.Config.MusicDirectory", L"音乐目录={0}"},

    {L"Log.Fmod.LoadedPath", L"已从以下路径加载 FMOD：{0}"},
    {L"Log.Fmod.UsingLoaded", L"正在使用游戏已经加载的 fmod64.dll"},
    {L"Log.Fmod.LoadFailed", L"无法加载 fmod64.dll"},
    {L"Log.Fmod.CallbackUnavailable", L"FMOD END 回调导出不可用；自动下一曲将使用轮询备用方案"},
    {L"Log.Fmod.MissingExports", L"fmod64.dll 缺少一个或多个必需的 C API 导出"},

    {L"Log.Album.Disabled", L"EnableAlbumControls=false：固定专辑、播放模式与布局快捷键已关闭"},
    {L"Log.Album.EnabledInstalled", L"固定输入与通知布局控制已启用；手柄接口={0}；游戏 XInput 输入捕获已安装"},
    {L"Log.Album.EnabledScanning", L"固定输入与通知布局控制已启用；手柄接口={0}；暂未找到游戏 XInput 调用槽位，将继续周期扫描"},
    {L"Log.Album.NoXInput", L"未找到可用 XInputGetState；键盘快捷键仍可使用，手柄组合键不可用"},
    {L"Log.Album.EnumFailed", L"无法枚举游戏模块；XInput 输入捕获未安装"},
    {L"Log.Album.Scan", L"XInput 输入捕获扫描：游戏模块={0}，新导入槽位={1}，新缓存指针={2}，累计={3}"},

    {L"Log.Patch.RestoreInlineFailed", L"关闭时无法恢复内联补丁"},
    {L"Log.Patch.RestoreTrampolineFailed", L"关闭时无法恢复跳板补丁"},

    {L"Log.Overlay.EventFailed", L"无法创建曲目信息通知线程事件；通知功能已关闭"},
    {L"Log.Overlay.InitFailed", L"曲目信息通知窗口初始化失败；播放后端将继续正常运行"},
    {L"Log.Overlay.Enabled", L"自绘曲目信息通知已启用（透明 Win32 窗口，不 Hook DirectX）"},
    {L"Log.Overlay.Layout", L"通知布局：固定物理像素，缩放={0}%，透明度={1}%，X={2}，Y={3}"},
    {L"Log.Overlay.LayoutSaved", L"通知布局已保存：X={0}，Y={1}，缩放={2}%，透明度={3}%"},
    {L"Log.Overlay.LayoutWriteFailed", L"通知布局写入 INI 失败；当前进程中的预览仍然有效"},
    {L"Log.Overlay.LayoutResetWriteFailed", L"通知布局已在内存中重置，但写入 INI 失败"},
    {L"Log.Overlay.LayoutReset", L"通知布局已重置为 TopRight / X32 / Y32 / 100% / 92%"},

    {L"Log.Player.NoTracks", L"没有找到支持的音乐文件；将在静音状态等待游戏 Spotify 播放命令"},
    {L"Log.Player.LibraryReadyEnabled", L"本地曲库已就绪；LocalMusic 本地后端已启用并将跟随游戏播放命令"},
    {L"Log.Player.LibraryReadyDisabled", L"本地曲库已就绪；LocalMusic 本地后端已在 {0} 中关闭"},
    {L"Log.Player.EndCallback", L"收到 FMOD END 回调（工作线程延迟 {0} 毫秒）"},
    {L"Log.Player.EndPolling", L"FMOD 轮询备用路径检测到曲目结束"},
    {L"Log.Player.AutoNext", L"自动下一曲：{0}"},
    {L"Log.Player.TrackEnded", L"曲目已结束"},
    {L"Log.Player.RescanEmpty", L"重新扫描后仍未找到支持的音乐文件；继续保持静音"},
    {L"Log.Player.PlayDisabled", L"游戏调用了 Spotify Play，但 LocalMusic 本地后端已在 {0} 中关闭"},
    {L"Log.Player.PlayOptionDisabled", L"游戏调用了 Spotify Play；由于 StartLocalMusicOnPlay=false，本地播放保持空闲"},
    {L"Log.Player.PlayEmpty", L"游戏请求播放，但本地曲库为空；继续静音运行"},
    {L"Log.Player.ResumeEmpty", L"游戏请求恢复播放，但本地曲库为空；继续静音运行"},
    {L"Log.Player.SkipInactive", L"本地 Spotify 播放尚未激活，已忽略换曲请求"},
    {L"Log.Player.SkipNavigationNext", L"已忽略由菜单横向导航引起的游戏下一曲调用"},
    {L"Log.Player.SkipNavigationPrevious", L"已忽略由菜单横向导航引起的游戏上一曲调用"},
    {L"Log.Player.VolumePending", L"游戏 Spotify 音量上调至 {0}%，已进入稳定性检查；当前游戏音量层仍为 {1}%，FMOD 最终音量={2}"},
    {L"Log.Player.VolumeOffsetIgnored", L"游戏 Spotify 临时音量偏移={0}，已忽略；插件倍率={1}%，游戏持久音量={2}%，FMOD 最终音量={3}"},
    {L"Log.Player.AutoPlayVolume", L"游戏持久 Spotify 音量初始化已触发自动播放"},
    {L"Log.Player.AutoPlayEmpty", L"自动播放已激活，但本地曲库为空；继续静音运行"},
    {L"Log.Player.BackendChanged", L"LocalMusic 本地后端已{0}，原因：{1}"},
    {L"Log.Player.Enabled", L"启用"},
    {L"Log.Player.Disabled", L"关闭"},
    {L"Log.Player.ColdOpen", L"冷流打开耗时 {0} 毫秒"},
    {L"Log.Player.CallbackInstallFailed", L"自动下一曲：安装 FMOD END 回调失败；继续使用轮询备用方案"},
    {L"Log.Player.CallbackUnsupported", L"自动下一曲：FMOD END 回调支持不可用；使用轮询备用方案"},
    {L"Log.Player.TrackLength", L"检测到曲目长度：{0} 毫秒"},
    {L"Log.Player.TrackLengthFailed", L"无法读取 FMOD 曲长；自动下一曲将依赖回调或 ChannelIsPlaying"},
    {L"Log.Player.NowPlayingArtist", L"正在播放：{0} - {1}{2}"},
    {L"Log.Player.NowPlaying", L"正在播放：{0}{1}"},
    {L"Log.Player.PreloadedSuffix", L" [已预加载]"},
    {L"Log.Player.PreloadFailed", L"无法预加载下一首；切换时将执行冷流打开"},
    {L"Log.Player.PreloadDone", L"下一首已预加载，耗时 {0} 毫秒"},
    {L"Log.Player.OneAlbum", L"曲库中只有一张专辑，未执行专辑切换"},
    {L"Log.Player.AlbumPrevious", L"已切换到上一张专辑：{0}（{1} 首）"},
    {L"Log.Player.AlbumNext", L"已切换到下一张专辑：{0}（{1} 首）"},
    {L"Log.Player.PlayModeChanged", L"播放模式已切换为：{0}；已写入 {1}"},
    {L"Log.Player.PlayModeWriteFailed", L"无法把播放模式写入 {0}"},
    {L"Log.Player.VolumeStableSuffix", L"通过稳定性检查后生效"},
    {L"Log.Player.PersistentVolume", L"游戏 Spotify 持久音量={0}%，插件倍率={1}%，FMOD 最终音量={2}%（{3}{4}）"},
    {L"Log.Player.VolumeApplied", L"已应用"},
    {L"Log.Player.OffsetIgnoredSuffix", L"；临时 offset 已忽略"},
    {L"Log.Player.MusicDirectoryUnavailable", L"音乐目录不可用：{0}"},
    {L"Log.Player.ScanComplete", L"已扫描 {0} 首本地曲目，建立 {1} 张文件夹专辑；当前模式={2}"},
    {L"Log.Player.BackendWriteFailed", L"无法把 LocalMusic 本地后端状态写入 {0}"},
    {L"Log.Player.FmodOperationFailed", L"{0} 失败：{1} ({2})"},

    {L"Log.Hooks.InvalidPe", L"主程序不是有效的 64 位 PE 映像"},
    {L"Log.Hooks.ResolveFailed", L"无法定位：{0}"},
    {L"Log.Hooks.ExecResolved", L"已定位 UE4 exec 包装函数：{0}，RVA {1}"},
    {L"Log.Hooks.NativeResolveFailed", L"无法定位原生目标：{0}"},
    {L"Log.Hooks.NativeResolved", L"已定位原生函数：{0}，RVA {1}"},
    {L"Log.Hooks.RequiredMissing", L"未能定位全部必需 Hook；没有修改任何游戏代码"},
    {L"Log.Hooks.FStringFailed", L"无法推导 UE4 FString 拷贝函数；字符串 Hook 不安全"},
    {L"Log.Hooks.FStringResolved", L"已定位 FString 拷贝辅助函数，RVA {0}"},
    {L"Log.Hooks.FrameOffsetFailed", L"无法推断 FFrame::Code 偏移；拒绝安装 exec Hook"},
    {L"Log.Hooks.FrameOffset", L"FFrame::Code 偏移={0}"},
    {L"Log.Hooks.NoCredentialWrites", L"不会修改 Spotify 持久凭据和游戏存档数据"},
    {L"Log.Hooks.AuthEnabled", L"认证旁路已启用：本地后端跟随游戏播放命令；已保存的 Spotify token 会被忽略且绝不修改"},
    {L"Log.Hooks.CallbackProfileMismatch", L"认证回调布局与当前 EXE 配置不匹配；请先更新 Compatibility 参数"},
    {L"Log.Hooks.DelegatePatternResolved", L"已通过特征码定位 UE4 委托广播函数，RVA {0}"},
    {L"Log.Hooks.DelegatePatternInvalid", L"DelegateBroadcastPattern 无效；尝试配置的 RVA 备用地址"},
    {L"Log.Hooks.DelegateRvaResolved", L"已通过 RVA 备用地址定位 UE4 委托广播函数，RVA {0}"},
    {L"Log.Hooks.DispatcherInvalid", L"认证 dispatcher 兼容参数无效；拒绝安装认证旁路 Hook"},
    {L"Log.Hooks.CallbackImmediate", L"认证成功回调将立即派发"},
    {L"Log.Hooks.CallbackDeferred", L"认证成功回调将延迟到下一个安全的 SpotifyService 游戏线程入口"},
    {L"Log.Hooks.AuthDisabled", L"认证旁路已关闭：游戏原始 Spotify 认证路径仍会执行"},
    {L"Log.Hooks.DefaultVolumeResolved", L"已定位原生函数：SetDefaultVolume，RVA {0}"},
    {L"Log.Hooks.DefaultVolumeMissing", L"无法定位原生 SetDefaultVolume；仍可使用 INI 音量"},
    {L"Log.Hooks.OffsetVolumeResolved", L"已定位原生函数：SetVolumeOffset，RVA {0}"},
    {L"Log.Hooks.OffsetVolumeMissing", L"无法定位原生 SetVolumeOffset；仍可使用 INI 音量"},
    {L"Log.Hooks.DryRun", L"DryRun=true：兼容性检查通过，但没有安装任何 Hook"},
    {L"Log.Hooks.PatchWriteFailed", L"写入补丁失败；正在恢复已经安装的全部补丁"},
    {L"Log.Hooks.InstalledImmediate", L"Dangerous Driving Spotify Hook 已安装：游戏驱动、立即认证回调模式"},
    {L"Log.Hooks.InstalledDeferred", L"Dangerous Driving Spotify Hook 已安装：游戏驱动、延迟认证回调模式"},
    {L"Log.Hooks.InstalledNoAuth", L"Dangerous Driving Spotify 播放 Hook 已安装，但未启用认证旁路"},
    {L"Log.Hooks.ExeProfile", L"EXE 时间戳={0}，映像大小={1}"},
    {L"Log.Hooks.TimestampMismatch", L"EXE 时间戳与配置不一致"},
    {L"Log.Hooks.ImageSizeMismatch", L"EXE 映像大小与配置不一致"},
    {L"Log.Hooks.ShaFailed", L"无法计算 EXE SHA-256"},
    {L"Log.Hooks.Sha", L"EXE SHA-256={0}"},
    {L"Log.Hooks.ShaMismatch", L"EXE SHA-256 与配置不一致"},
    {L"Log.Hooks.StrictMismatch", L"StrictVersion=true 且 EXE 不匹配；已禁用 Hook"},
    {L"Log.Hooks.UnknownProfile", L"未知 EXE 配置：仅对通过动态验证的地址继续处理"},
    {L"Log.Hooks.InvalidPattern", L"字节特征码无效：{0}"},
    {L"Log.Hooks.PatchInstallFailed", L"安装失败：{0} 补丁"},
    {L"Log.Hooks.EmptyService", L"认证旁路收到空 SpotifyService，入口：{0}"},
    {L"Log.Hooks.DispatchImmediate", L"立即派发认证回调：{0}"},
    {L"Log.Hooks.CallbackQueued", L"已排队认证成功回调：{0}"},
    {L"Log.Hooks.CallbackMerged", L"认证回调已在队列中，已合并重复请求：{0}"},
    {L"Log.Hooks.BroadcastFailed", L"无法派发 {0} 回调"},
    {L"Log.Hooks.CallbackBroadcast", L"已派发认证回调：{0}={1}"},
    {L"Log.Hooks.SwitchBroadcastFailed", L"无法派发 SwitchActiveDevice 回调"},
    {L"Log.Hooks.SwitchBroadcast", L"已派发认证回调：SwitchActiveDevice={0}/激活={1}"},
    {L"Log.Hooks.ProcessingQueue", L"正在从以下入口处理排队的认证回调：{0}"},
    {L"Log.Hooks.UnknownEntry", L"未知 SpotifyService 入口"},
    {L"Log.Hooks.UnknownCallback", L"已忽略未知的认证回调标识"},
    {L"Log.Hooks.RetryEntry", L"{0} 重试入口"},
    {L"Log.Hooks.ClearIgnored", L"本地后端已忽略 ClearAccessTokens，未修改已保存凭据"},
    {L"Log.Hooks.ActivationAccepted", L"已把 Spotify 授权请求接受为本地替代后端模式"},
    {L"Log.Hooks.UpdateActive", L"SpotifyService::Update 已激活，可作为认证回调派发入口"},
    {L"Log.Hooks.ImmediateCleanup", L"立即回调后的队列清理"},
    {L"Log.Hooks.ExecPatchLabel", L"蓝图包装函数 {0}"},
    {L"Log.Hooks.NativePatchLabel", L"原生函数 {0}"},
    {L"Log.Hooks.DefaultVolumePatchLabel", L"原生 SetDefaultVolume 跳板"},
    {L"Log.Hooks.OffsetVolumePatchLabel", L"原生 SetVolumeOffset"},
};

// 统一语言键比较形式：去除首尾空白并转为小写，外部 INI 键不区分大小写。
std::wstring CanonicalKey(std::wstring value) {
    value = ToLower(Trim(std::move(value)));
    return value;
}

// 规范化用户语言标签：下划线转连字符、转小写，并拒绝不安全的路径字符。
std::wstring NormalizeLanguageTag(std::wstring value) {
    value = ToLower(Trim(std::move(value)));
    std::replace(value.begin(), value.end(), L'_', L'-');
    if (value.empty()) return L"auto";
    if (value == L"auto") return value;
    if (value.size() > 63 || value.front() == L'-' || value.back() == L'-') return {};
    bool previous_hyphen = false;
    for (const wchar_t ch : value) {
        const bool hyphen = ch == L'-';
        if (!(hyphen || (ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9'))) return {};
        if (hyphen && previous_hyphen) return {};
        previous_hyphen = hyphen;
    }
    return value;
}

// 根据 Windows 用户 UI 语言选择官方自动语言；所有中文区域统一使用简体资源。
std::wstring DetectAutomaticLanguage() {
    wchar_t locale_name[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale_name, LOCALE_NAME_MAX_LENGTH) > 0) {
        const std::wstring locale = ToLower(locale_name);
        if (locale == L"zh" || locale.rfind(L"zh-", 0) == 0) return L"zh-hans";
    }
    const LANGID language = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(language) == LANG_CHINESE) return L"zh-hans";
    return L"en-us";
}

// 读取语言文件并支持 UTF-8 BOM、UTF-16LE 与旧系统 ANSI 回退；失败时清空 ok。
std::wstring DecodeTextFile(const std::filesystem::path& path, bool& ok) {
    ok = false;
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (!file.good() && !file.eof()) return {};
    if (bytes.empty()) {
        ok = true;
        return {};
    }

    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        const size_t count = (bytes.size() - 2) / 2;
        std::wstring result(count, L'\0');
        for (size_t i = 0; i < count; ++i) {
            const unsigned char lo = static_cast<unsigned char>(bytes[2 + i * 2]);
            const unsigned char hi = static_cast<unsigned char>(bytes[3 + i * 2]);
            result[i] = static_cast<wchar_t>(lo | (static_cast<uint16_t>(hi) << 8));
        }
        ok = true;
        return result;
    }

    size_t offset = 0;
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }
    const std::string_view payload(bytes.data() + offset, bytes.size() - offset);
    if (payload.empty()) {
        ok = true;
        return {};
    }

    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, payload.data(),
                                    static_cast<int>(payload.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (count <= 0) {
        code_page = CP_ACP;
        flags = 0;
        count = MultiByteToWideChar(code_page, flags, payload.data(),
                                    static_cast<int>(payload.size()), nullptr, 0);
    }
    if (count <= 0) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(code_page, flags, payload.data(), static_cast<int>(payload.size()),
                        result.data(), count);
    ok = true;
    return result;
}

// 还原语言 INI 中的换行、制表符和反斜杠转义，未知转义保持原样。
std::wstring Unescape(std::wstring value) {
    std::wstring result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != L'\\' || i + 1 >= value.size()) {
            result.push_back(value[i]);
            continue;
        }
        const wchar_t next = value[++i];
        switch (next) {
            case L'n': result.push_back(L'\n'); break;
            case L'r': result.push_back(L'\r'); break;
            case L't': result.push_back(L'\t'); break;
            case L'\\': result.push_back(L'\\'); break;
            default:
                result.push_back(L'\\');
                result.push_back(next);
                break;
        }
    }
    return result;
}

// 把简单 INI 解析为规范化 Section.Key 字典；忽略注释、空行和无效行。
Dictionary ParseIniDictionary(const std::filesystem::path& path, bool& ok) {
    bool decoded = false;
    const std::wstring content = DecodeTextFile(path, decoded);
    ok = false;
    if (!decoded) return {};

    Dictionary result;
    std::wistringstream stream(content);
    std::wstring section;
    std::wstring line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = Trim(std::move(line));
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        const size_t equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        const std::wstring key = Trim(line.substr(0, equals));
        if (section.empty() || key.empty()) continue;
        result[CanonicalKey(section + L"." + key)] = Unescape(Trim(line.substr(equals + 1)));
    }
    ok = true;
    return result;
}

// 从主配置读取 Language，并兼容旧 LocalSpotify 节；文件不存在时返回 auto。
std::wstring ReadLanguageSetting(const std::filesystem::path& path) {
    bool ok = false;
    const Dictionary values = ParseIniDictionary(path, ok);
    if (!ok) return L"auto";
    const auto primary = values.find(L"localmusic.language");
    if (primary != values.end()) return primary->second;
    const auto legacy = values.find(L"localspotify.language");
    return legacy != values.end() ? legacy->second : L"auto";
}

// 根据实际代理 DLL 主文件名构造同目录语言文件路径，避免把 dsound 名称写死。
std::filesystem::path BuildLanguagePath(const std::filesystem::path& dll_path,
                                        const std::wstring& language) {
    return dll_path.parent_path() /
           (dll_path.stem().wstring() + L"." + language + L".ini");
}

// 查询不可删除的内置简体中文最终回退；未知键返回空指针。
const wchar_t* BuiltInValue(std::wstring_view canonical_key) {
    for (const auto& entry : kBuiltInZhHans) {
        if (canonical_key == CanonicalKey(entry.key)) return entry.value;
    }
    return nullptr;
}

// 在调用方已持锁时按当前语言、英文、内置中文顺序查找单个文本键。
std::wstring LookupUnlocked(std::wstring_view key) {
    const std::wstring canonical = CanonicalKey(std::wstring(key));
    if (const auto found = g_selected.find(canonical); found != g_selected.end() && !found->second.empty()) {
        return found->second;
    }
    if (const auto found = g_english.find(canonical); found != g_english.end() && !found->second.empty()) {
        return found->second;
    }
    if (const wchar_t* built_in = BuiltInValue(canonical)) return built_in;
    return std::wstring(key);
}

// 在调用方已持锁时替换连续编号占位符；缺少参数时保留占位符便于诊断。
std::wstring FormatUnlocked(std::wstring_view key,
                            std::initializer_list<std::wstring> arguments) {
    std::wstring result = LookupUnlocked(key);
    size_t index = 0;
    for (const std::wstring& argument : arguments) {
        const std::wstring token = L"{" + std::to_wstring(index++) + L"}";
        size_t offset = 0;
        while ((offset = result.find(token, offset)) != std::wstring::npos) {
            result.replace(offset, token.size(), argument);
            offset += argument.size();
        }
    }
    return result;
}

// 把本地化文本逐行写成 UTF-8 INI 注释，并统一使用 CRLF 行尾。
void AppendComment(std::ostringstream& output, const std::wstring& text) {
    if (text.empty()) {
        output << ";\r\n";
        return;
    }
    std::wistringstream lines(text);
    std::wstring line;
    while (std::getline(lines, line)) {
        output << "; " << WideToUtf8(line) << "\r\n";
    }
}

}  // namespace

// 初始化语言层并记录回退诊断；只读取外部文件，不修改用户语言资源。
bool Initialize(const std::filesystem::path& dll_path,
                const std::filesystem::path& main_ini_path) {
    std::lock_guard lock(g_mutex);
    g_selected.clear();
    g_english.clear();
    g_diagnostics.clear();
    g_dll_path = dll_path;

    const std::wstring raw_requested = ReadLanguageSetting(main_ini_path);
    std::wstring requested = NormalizeLanguageTag(raw_requested);
    if (requested.empty()) {
        g_diagnostics.push_back({true, L"Localization.InvalidTag\n" + raw_requested});
        requested = L"auto";
    }
    g_requested = requested;
    std::wstring desired = requested == L"auto" ? DetectAutomaticLanguage() : requested;

    const std::filesystem::path english_path = BuildLanguagePath(dll_path, L"en-us");
    bool english_ok = false;
    g_english = ParseIniDictionary(english_path, english_ok);

    const std::filesystem::path selected_path = BuildLanguagePath(dll_path, desired);
    bool selected_ok = false;
    Dictionary selected = desired == L"en-us"
        ? g_english
        : ParseIniDictionary(selected_path, selected_ok);
    if (desired == L"en-us") selected_ok = english_ok;

    if (selected_ok) {
        g_selected = std::move(selected);
        g_active = desired;
        g_active_file = selected_path;
        g_diagnostics.push_back({false, L"Localization.FileLoaded\n" + selected_path.wstring()});
    } else if (desired != L"en-us" && english_ok) {
        g_selected = g_english;
        g_active = L"en-us";
        g_active_file = english_path;
        g_diagnostics.push_back({true, L"Localization.RequestedMissing\n" + selected_path.wstring()});
        g_diagnostics.push_back({false, L"Localization.FileLoaded\n" + english_path.wstring()});
    } else {
        g_selected.clear();
        g_active = L"zh-hans";
        g_active_file = selected_path;
        if (desired != L"en-us") {
            g_diagnostics.push_back({true, L"Localization.RequestedMissing\n" + selected_path.wstring()});
        }
        g_diagnostics.push_back({true, L"Localization.EnglishMissing"});
    }

    return true;
}

// 返回最终生效语言标签的线程安全快照。
std::wstring ActiveLanguage() {
    std::lock_guard lock(g_mutex);
    return g_active;
}

// 返回主配置请求语言标签的线程安全快照。
std::wstring RequestedLanguage() {
    std::lock_guard lock(g_mutex);
    return g_requested;
}

// 返回当前主语言文件候选路径，供诊断和工具显示。
std::filesystem::path ActiveLanguageFile() {
    std::lock_guard lock(g_mutex);
    return g_active_file;
}

// 对外提供线程安全的无参数文本查询。
std::wstring Text(std::wstring_view key) {
    std::lock_guard lock(g_mutex);
    return LookupUnlocked(key);
}

// 对外提供线程安全的占位符格式化查询。
std::wstring Format(std::wstring_view key,
                    std::initializer_list<std::wstring> arguments) {
    std::lock_guard lock(g_mutex);
    return FormatUnlocked(key, arguments);
}

// 把初始化阶段暂存的键和参数翻译为最终日志消息，并附加生效语言。
std::vector<StartupDiagnostic> StartupDiagnostics() {
    std::lock_guard lock(g_mutex);
    std::vector<StartupDiagnostic> result;
    result.reserve(g_diagnostics.size() + 1);
    for (const auto& diagnostic : g_diagnostics) {
        const size_t separator = diagnostic.message.find(L'\n');
        const std::wstring key = separator == std::wstring::npos
            ? diagnostic.message : diagnostic.message.substr(0, separator);
        if (separator == std::wstring::npos) {
            result.push_back({diagnostic.warning, LookupUnlocked(key)});
        } else {
            const std::wstring argument = diagnostic.message.substr(separator + 1);
            std::wstring text = LookupUnlocked(key);
            const std::wstring token = L"{0}";
            if (const size_t position = text.find(token); position != std::wstring::npos) {
                text.replace(position, token.size(), argument);
            }
            result.push_back({diagnostic.warning, std::move(text)});
        }
    }
    result.push_back({false, FormatUnlocked(L"Localization.Active", {g_active})});
    return result;
}

// 生成与代理 DLL 主文件名匹配的精简主配置；注释随当前语言变化。
std::string BuildDefaultConfigText(const std::wstring& file_name) {
    std::filesystem::path supplied(file_name);
    const std::wstring stem = supplied.stem().wstring();
    const std::wstring dll_file_name = ToLower(supplied.extension().wstring()) == L".dll"
        ? supplied.filename().wstring()
        : stem + L".dll";
    const std::wstring log_file = stem + L".log";

    std::ostringstream output;
    output << "\xEF\xBB\xBF";
    AppendComment(output, Text(L"Config.Header"));
    AppendComment(output, Format(L"Config.PlaceFile", {dll_file_name}));
    AppendComment(output, Text(L"Config.HiddenOptions"));
    AppendComment(output, Text(L"Config.Restart"));
    AppendComment(output, L"");
    AppendComment(output, Text(L"Config.LayoutHeader"));
    AppendComment(output, Text(L"Config.Controller1"));
    AppendComment(output, Text(L"Config.Controller2"));
    AppendComment(output, Text(L"Config.Keyboard1"));
    AppendComment(output, Text(L"Config.Keyboard2"));
    output << "\r\n[LocalMusic]\r\n";
    AppendComment(output, Text(L"Config.Language"));
    output << "Language=auto\r\n\r\n";
    AppendComment(output, Format(L"Config.Logging", {log_file}));
    output << "EnableLogging=true\r\n\r\n";
    AppendComment(output, Text(L"Config.MusicPath"));
    output << "MusicPath=..\\..\\Content\\Music\r\n\r\n";
    AppendComment(output, Text(L"Config.PlayMode"));
    output << "PlayMode=Sequential\r\n\r\n";
    AppendComment(output, Text(L"Config.Volume1"));
    AppendComment(output, Text(L"Config.Volume2"));
    output << "Volume=0.5\r\n\r\n";
    AppendComment(output, Text(L"Config.Notifications"));
    output << "ShowNowPlayingNotification=true\r\n"
              "ShowPauseNotification=true\r\n"
              "ShowResumeNotification=true\r\n\r\n";
    AppendComment(output, Text(L"Config.Duration"));
    output << "NotificationDurationMs=3000\r\n\r\n";
    AppendComment(output, Text(L"Config.ScaleOpacity"));
    output << "NotificationScalePercent=100\r\n"
              "NotificationOpacityPercent=92\r\n\r\n";
    AppendComment(output, Text(L"Config.Position"));
    output << "NotificationPosition=TopRight\r\n\r\n";
    AppendComment(output, Text(L"Config.Margin1"));
    AppendComment(output, Text(L"Config.Margin2"));
    output << "NotificationMarginX=32\r\n"
              "NotificationMarginY=32\r\n";
    return output.str();
}

}  // namespace localmusic::loc
