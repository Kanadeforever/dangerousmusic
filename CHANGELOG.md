# 变更记录 / Changelog

## 2026-07-19 — 纯源码文档审校版

- 修正《INI配置说明》中仍停留在早期“仅拦截 PreviousTrack/NextTrack”的旧描述，更新为方向会话状态机与五个媒体入口统一硬门。
- 修正 `AlbumControls.h` 顶部注释，明确游戏侧方向键清除只有在命中实际 XInput 路径时成立；媒体硬门不依赖扫描结果。
- 加强发行质量脚本：方法注释必须包含具实质内容的中文，并验证当前输入文档不会退回旧方案。
- 源码包移除对象文件、PDB、构建日志和 Python 缓存，只保留可重建源码、脚本、配置、语言资源与验证记录。
- 运行时代码和严格输入互斥行为未改变。

## 2026-07-19 — Clean source documentation audit

- Updates the stale INI manual text from the early PreviousTrack/NextTrack-only gate to the held-direction state machine and the shared five-entry media gate.
- Clarifies in `AlbumControls.h` that game-side D-pad removal depends on capturing the actual XInput path, while the media gate does not depend on scan results.
- Strengthens release checks so method comments must contain substantive Chinese text and current input documentation cannot regress to the old design.
- Removes object files, PDBs, build logs, and Python caches from the source archive.
- Runtime code and strict input-isolation behavior are unchanged.

## 2026-07-19 — 严格输入互斥修复版

- 将十字键从单帧条件判断改为按住周期状态机；方向键按下沿锁定为普通、`LB`、`BACK` 或 `BACK+LB`，完全释放前不允许切换模式。
- `LB+方向键` 只执行专辑/循环模式动作；`BACK+方向键` 只调整位置；`BACK+LB+方向键` 只调整缩放或透明度。
- 游戏原生 `PreviousTrack`、`NextTrack`、`Pause`、`Play`、`Unpause` 五个媒体入口统一经过同步组合键硬门。
- 先按方向再补按修饰键时不会补触发插件动作，但会立即阻止游戏媒体污染；必须先松开方向，再以正确组合重新按下。
- 组合释放后保留 750 毫秒吸收窗口，拦截 UE4 延迟派发的播放、暂停、恢复或换曲命令。
- 菜单左右导航与有意换曲仍可能汇入相同的游戏媒体入口；不可靠的菜单导航猜测保持默认关闭，避免误伤正常换曲。

## 2026-07-19 — Strict input-isolation fix

- Replaces per-frame modifier checks with a held-direction state machine. A D-pad press locks to Plain, `LB`, `BACK`, or `BACK+LB` until that direction is fully released.
- `LB+D-pad` performs only album/mode actions; `BACK+D-pad` only moves the overlay; `BACK+LB+D-pad` only changes scale or opacity.
- Routes all five game media entries—`PreviousTrack`, `NextTrack`, `Pause`, `Play`, and `Unpause`—through the same synchronous chord gate.
- Adding a modifier after a direction is already held does not retroactively trigger a plugin action, but game media commands are blocked immediately. Release the direction before starting another chord.
- Keeps a 750 ms post-release absorption window for delayed UE4 play, pause, resume, or skip dispatch.
- Menu horizontal navigation and intentional track skips may still converge on the same game entry. The unreliable navigation heuristic remains disabled by default to avoid blocking legitimate skips.

## 2026-07-19 — 发行版：BACK 同步换曲硬门

- 游戏调用 `NextTrack` / `PreviousTrack` 时同步读取原始 XInput 状态。
- 任一手柄按住 `BACK` 时拒绝游戏来源换曲；松开后保留 750 毫秒保护，吸收 UE4 延迟派发的残留方向事件。
- 该保护不依赖 XInput 导入槽位扫描是否命中，因此累计捕获数为 0 时仍能阻止布局调整换歌。
- 已知限制：未命中实际输入路径时，游戏菜单仍可能响应 `BACK + 上/下`；本版不再扩大 Hook 范围。

## 2026-07-19 — Release: synchronous BACK track-skip gate

- Reads raw XInput state synchronously whenever the game calls `NextTrack` or `PreviousTrack`.
- Rejects game-originated track skips while any controller holds `BACK`, with a 750 ms grace period after release for delayed UE4 navigation events.
- This protection does not depend on finding XInput import slots, so it still prevents layout controls from changing tracks when the cumulative capture count is zero.
- Known limitation: if the real input path is not captured, game menus may still react to `BACK + Up/Down`; this release intentionally does not broaden the hook surface further.

## 发行候选运行时 i18n 版

- 新增 `Language=auto`，中文 Windows UI 默认使用 `zh-hans`，其他环境默认使用 `en-us`。
- 新增 `dsound.zh-hans.ini` 与 `dsound.en-us.ini`，覆盖通知、日志、错误弹窗、播放模式和首次生成配置注释。
- 支持用户自建小写 BCP 47 风格语言标签；语言文件名随实际代理 DLL 主文件名派生。
- 回退顺序固定为“指定语言 → en-us → DLL 内置 zh-hans”，并对单个缺失键执行同样回退。
- 保留 DirectSound 导出序号修复和 BACK 布局输入优先级修复。

## Release Candidate - Runtime i18n

- Adds `Language=auto`: Chinese Windows UI defaults to `zh-hans`; other environments default to `en-us`.
- Adds `dsound.zh-hans.ini` and `dsound.en-us.ini` for notifications, logs, error dialogs, play-mode labels, and generated configuration comments.
- Supports user-created lowercase BCP 47-style tags, with language filenames derived from the actual proxy DLL basename.
- Uses a fixed fallback chain of requested language → en-us → built-in zh-hans, including per-key fallback.
- Retains the DirectSound export-ordinal fix and BACK layout input-priority fix.

## 发行候选 DirectSound 序号修复版

- 显式固定 `dsound.def` 中 12 个 DirectSound/COM 导出的序号，避免游戏或系统组件按序号导入时在 DLL 入口前失败。
- 保留上一版的 BACK 布局输入接管、文件名排序、LocalMusic 命名和 LNK4104 修复。
- 本次没有改变播放队列、通知窗口样式或配置语义。

## Release Candidate - DirectSound ordinal fix

- Explicitly pins all 12 DirectSound/COM export ordinals in `dsound.def`, preventing loader-time failures when the game or a system component imports by ordinal.
- Keeps the previous BACK layout input capture, filename-based ordering, LocalMusic naming, and LNK4104 fix.
- This change does not alter playback queues, notification styling, or configuration semantics.

# 更新记录

## 简体中文

## 2026-07-16 — 文件名排序与 XInput 多路径接管修复

- 专辑顺序固定为相对于 `MusicPath` 的文件夹路径自然排序，根目录散装曲目排在最前。
- 专辑内曲目只按音频主文件名自然排序，完整文件名仅用于同名兜底。
- `TrackNumber`、`DiscNumber` 和其他元数据继续用于显示，但不再参与播放顺序。
- XInput 捕获从“只扫描主 EXE 的命名导入”扩展为扫描游戏目录内全部已加载模块。
- 新增序号 100 `XInputGetStateEx`、序号 7/命名 `XInputGetKeystroke` 和运行时缓存函数指针接管。
- 按住 BACK 时，十字键、LB、R3 的状态位与按下/重复/抬起事件都会被游戏侧过滤。
- 输入模块每秒补扫一次延迟加载或稍后解析的游戏模块，避免主菜单使用另一条 XInput 路径时漏接管。

## 2026-07-16 — 修复 MSVC 链接器 LNK4104 警告

- 在 `dsound.def` 中将 COM 标准入口 `DllCanUnloadNow` 和 `DllGetClassObject` 标记为 `PRIVATE`。
- 两个入口仍保留在最终 `dsound.dll` 的导出表中，系统转发和游戏运行行为不变。
- `PRIVATE` 只阻止链接器把这两个 COM 入口写入普通导入库，从而消除 Visual Studio 的 LNK4104 警告。
- 使用 `lld-link` 解析同一 `.def` 并生成导入库，确认语法有效且导入库不再公开这两个名称。

## 2026-07-16 — LocalMusic 命名与初版 BACK 输入接管

- 项目、发行包、文档、日志标题和通知窗口内部名称统一为 LocalMusic。
- 新配置主节改为 `[LocalMusic]`；旧 `[LocalSpotify]` 仅作为兼容回退读取。
- 初版仅接管游戏 EXE 的命名 `XInputGetState` 导入槽位；后续“XInput 多路径接管修复”已取代该单一路径。
- 按住 BACK 时，游戏侧十字键、LB、R3 被完全清除，插件仍读取原始状态编辑布局。
- 松开 BACK 后，若捕获键仍按住，继续屏蔽到全部释放，避免残留输入触发播放/暂停或菜单。
- 键盘布局调整保存延迟由 500 毫秒改为 1 秒，连续微调只在最后一次输入后写盘。

## 2026-07-16 — 游戏内通知布局、透明度调整与重置

- 新增手柄布局模式：按住 BACK 后使用十字键按画面方向移动通知。
- 新增 BACK+LB+上/下调整通知大小，BACK+LB+左/右调整背景透明度。
- 新增 BACK+R3 一键恢复 TopRight、X=32、Y=32、Scale=100、Opacity=92。
- 新增键盘布局控制：Ctrl+-/= 调 Y，Shift+-/= 调 X，Alt+-/= 调大小，直接 -/= 调透明度。
- 新增 Backspace 一键重置通知布局。
- 短按立即精调；按住约 350 毫秒后每 60 毫秒连续调整。
- 手柄松开 BACK 时写回 INI；键盘停止调整 500 毫秒后写回；重置立即写回。
- 手柄在调整中断开或输入模块关闭时补做最终保存，避免最后一次修改丢失。
- 通知布局调整会实时显示 X、Y、缩放和透明度，并保证至少 48 像素留在游戏客户区。
- 修正位置与尺寸耦合：X/Y 偏移不再参与自动缩小计算，移动通知不会意外改变大小。
- 所有 C++ 源码文件补充详细简体中文文件说明与方法契约注释。
- 所有面向用户/开发者的非代码文档改为“完整中文在前、结构一致英文在后”。

## 2026-07-16 — 可完全关闭文件日志

- 普通 `dsound.ini` 新增 `EnableLogging=true`。
- 设为 `false` 后，插件在日志初始化前直接预读该值，不创建、不截断、也不追加 `dsound.log`。
- 已存在的旧日志文件保持原样，避免插件擅自删除用户文件。
- 旧配置缺少该键时默认开启日志，方便升级后出现问题时继续诊断。
- 精简发行配置由 12 个常用设置增加为 13 个。

## 2026-07-16 — 发行候选配置整理

- 普通 `dsound.ini` 从完整开发配置精简为 12 个用户常用设置。
- 认证、Hook、自动播放、自动下一曲、预加载、文件夹专辑、元数据、封面、高 DPI 和兼容参数改用稳定内置默认值，不再显示在普通 INI。
- 保留旧 INI 与手工高级键的读取能力，避免升级后现有配置突然失效。
- 已知不可靠的菜单横向导航换曲抑制改为发行默认关闭，并从普通 INI 隐藏。
- 新增 `开发者/高级配置参考.ini`，仅用于未来 EXE 和 Hook 诊断，不会自动读取。
- 新增 `安装说明.txt`、`发行检查清单.md` 和 `package-release.bat`。
- `package-release.bat <版本号>` 会生成仅包含运行所需文件和用户文档的 Win64 ZIP，并输出 SHA-256。

## 2026-07-16 — 文件夹专辑、同名元数据与全曲库会话随机

- 每个实际包含音乐文件的目录建立为一张专辑；音乐根目录散装曲目归入“未分类曲目”。
- 明确不支持 ZIP 专辑，FMOD 始终读取普通文件路径。
- 新增与音频同主文件名的曲目元数据 INI，例如 `Song.wav` 对应 `Song.ini`。
- 新增与音频同主文件名的外部封面，例如 `Song.wav` 对应 `Song.png`。
- 新增文件夹 `album.ini` 与 `cover/folder/front` 专辑封面回退。
- 元数据按“内嵌标签优先、侧挂 INI 补缺、文件夹专辑补缺”合并；可按单曲或全局允许侧挂覆盖。
- 随机模式改为全曲库固定会话乱序队列，Previous/Next 只移动游标，可以返回真正的上一首。
- 新增固定键盘控制：Insert 上一专辑、PageUp 下一专辑、Home 切换播放模式。
- 新增固定 XInput 控制：LB+左/右切专辑、LB+下切换模式、LB+上显示状态。
- 播放模式按“顺序→随机→单曲循环”循环，并立即写回 `dsound.ini`。
- 切换专辑、模式和显示状态均复用现有自绘通知样式，并显示当前播放模式。
- LB+左/右有效期间加入独立 Spotify 换曲保护，防止游戏再执行一次上一曲/下一曲。
- 曲库仅有一张专辑时不再重播第一首，只显示当前专辑信息。
- 新增中文曲目侧挂 INI 与文件夹 `album.ini` 示例模板。

## 2026-07-15 — 独立 X/Y 坐标与高 DPI 感知

- 将统一 `NotificationMargin` 拆分为 `NotificationMarginX` 和 `NotificationMarginY`，可分别避开计时、排名和其他 HUD。
- 保留旧 `NotificationMargin` 作为缺失新键时的兼容回退，旧 INI 不会失效。
- 新增 `EnableHighDpiAwareness=true`：在插件窗口创建前尝试启用进程级 Per-Monitor V2，并为通知线程设置独立 DPI 上下文。
- 通知布局改为固定物理像素模型：大小只由 `NotificationScalePercent` 控制，不再乘以 Windows DPI，也不随游戏分辨率同比放大。
- 游戏客户区过小时仍会自动缩小通知，防止窗口越界。
- 若游戏 manifest 已经设置 DPI 模式，插件保持原设置，不强行覆盖。

## 2026-07-15 — Windows SDK 编译兼容修复

- 在 `GameHooks.h` 中明确声明 `NativeVolumeFunction`，用于保存原始音量函数 trampoline，避免 MSVC 报未定义类型。
- 将 `Gdiplus::Image::FromStream(stream, FALSE, TRUE)` 改为新版 Windows SDK 支持的两参数形式 `Gdiplus::Image::FromStream(stream, FALSE)`。
- 保留 IStream 在临时 GDI+ Image 生命周期内有效、先销毁 Image 再释放流的安全顺序。

## 2026-07-15 — 自绘曲目信息通知与内嵌封面

- 新增透明、鼠标穿透、不可激活的 Win32 曲目信息通知窗口，不 Hook DirectX Present。
- 换曲时显示“正在播放”、曲名、艺术家、专辑和内嵌封面。
- 收到真实 Spotify Pause/Unpause 控制时显示“已暂停”或“继续播放”。
- MP3 新增 TALB 专辑与 APIC 封面解析；OGG 新增 ALBUM、METADATA_BLOCK_PICTURE 和 COVERART 解析。
- 封面读取和 GDI+ 解码在独立通知线程执行，不阻塞 FMOD 播放或自动下一曲。
- 快速连续切歌只保留最新通知；窗口跟随游戏客户区、多显示器位置和 DPI。
- 新增通知位置、持续时间、淡入淡出、缩放、不透明度、边距和前台显示策略配置。
- 明确限制：关卡暂停菜单未调用 Spotify Pause 时不会显示暂停提示；真正的独占全屏可能遮挡 Win32 分层窗口。

## 2026-07-15 — GitHub 发布文档与顺序播放默认值

- 新增中英双语 GitHub `README.md`。
- 新增中文 `INI配置说明.md`，逐项记录所有配置键、默认值、范围、交互关系和风险。
- 配置与部署文件名统一为精确小写 `dsound.dll` / `dsound.ini`。
- 默认播放模式从随机改为顺序：结构体默认值、读取缺省值、内置 INI 和外部模板全部同步。
- 未知或拼写错误的 `PlayMode` 现在回退为 `Sequential`。
- README 如实标注菜单左右导航仍可能触发换曲，以及关卡暂停菜单不一定暂停音乐。

## 2026-07-15 — 简体中文完善、无缝切歌、菜单安全控制、双层音量恢复

- 包内说明文档、INI 注释、构建提示、验证脚本输出和运行日志统一为简体中文。
- 为关键源码模块补充大量中文注释，重点说明线程安全、FMOD 回调、播放代数、预加载、音量所有权和 Hook 风险边界。
- 恢复并确认最终音量公式：`FMOD = 插件 Volume × 游戏持久 Spotify 音量`。
- `Volume=` 始终作为插件侧永久倍率，不会被 `SetDefaultVolume` 替换。
- `SetDefaultVolume` 只更新游戏侧倍率；临时 `SetVolumeOffset` 继续被拦截并忽略。
- 修正 SpotifyService 的运行时 offset 字段为中性值 `0`；旧实现曾错误写入持久音量百分比，可能造成瞬时变大。
- 增加下一首流预加载，并记录工作线程延迟、慢速冷打开耗时和“已预加载”状态。
- 增加实验性的菜单横向导航输入抑制；实机确认部分延迟 UI 调用仍可能绕过，现作为已知限制保留。
- 增加游戏音量向上变化的短暂稳定性检查，降低音量仍立即生效。
- 记录关卡内暂停菜单不一定调用 Spotify Pause；独立 FMOD 不自动跟随 UE 游戏暂停。
- 修复 `GameHooks.h` 中遗漏的原生音量函数类型声明，避免 MSVC 编译失败。

## 2026-07-15 — 稳定游戏音量、自动首播、FMOD END 回调

- 默认 `Volume` 回退值从 `1.0` 改为 `0.5`。
- 第一次收到游戏持久 Spotify 音量时自动开始播放。
- FMOD Channel 以暂停状态创建，设置音量、用户数据和回调后再解除暂停，避免首帧满音量突发。
- 自动下一曲主路径改为 FMOD 原生 END 回调。
- 回调只投递带播放代数的原子事件，切歌仍由工作线程执行。
- 旧 Channel 的迟到回调会被忽略。
- 增加 `FMOD_Channel_Stop`、回调和用户数据绑定；轮询保留为备用。

## 2026-07-15 — 运行时游戏音量乘法修复（已被当前方案替代）

- 曾将游戏默认音量和 offset 共同乘入 FMOD 输出。
- 后续根据实际体验确认：关卡临时 offset 会造成音量忽大忽小，因此当前版保留“插件永久倍率 × 游戏持久音量”，仅移除临时 offset。
- 游戏仍拥有自己的 UI 和存档行为，插件不持久化游戏音量。

## 2026-07-15 — 自动下一曲 EOF 轮询备用修复

- 增加 `ChannelIsPlaying`、播放位置、曲长和尾部停滞检测。
- 当前版已由 FMOD END callback 作为主路径，此逻辑仅作为兼容性备用。

## 早期版本 — 游戏驱动的本地 Spotify 后端

- 插件从“独立全局播放器”调整为只替换游戏原有 Spotify 后端。
- 跟随游戏的 Play、Pause、Unpause、NextTrack、PreviousTrack 和音量接口。
- 全局热键默认关闭，避免菜单和退出确认界面控制音乐。
- 不修改 Spotify 存档凭据。

## 早期版本 — 认证回调与旧存档兼容

- 认证函数不再仅返回 true，而是补发游戏等待的 UE4 动态多播委托。
- 支持立即或延迟派发认证成功回调。
- 不清理旧存档中的 token，不写回 SaveGame。
- 增加当前 EXE 的 dispatcher 布局和广播函数验证。
---

## 发行候选 BACK 优先级修复版

- 修正 `BACK+LB+十字键` 同时触发布局编辑和普通专辑/播放模式快捷键的问题。
- `BACK` 布局会话现在是插件手柄输入的全局最高优先级；捕获期间所有普通插件手柄动作都会跳过。
- `BACK+LB+左/右` 只调整透明度，`BACK+LB+上/下` 只调整缩放。
- 修正函数指针补丁恢复列表中可能重复记录同一槽位的问题。

## English

## Release Candidate - BACK Priority Fix

- Fixed `BACK+LB+D-pad` triggering both layout editing and normal album/play-mode shortcuts.
- The `BACK` layout session is now the global highest priority for plugin controller input; all normal plugin controller actions are skipped while capture is active.
- `BACK+LB+Left/Right` now only adjusts opacity, and `BACK+LB+Up/Down` only adjusts scale.
- Fixed a duplicate function-pointer patch entry in the restore list.


## 2026-07-16 — Filename ordering and multi-path XInput capture fix

- Album order is now the natural order of folder paths relative to `MusicPath`, with loose root tracks first.
- Tracks inside each album are ordered only by the natural order of the audio stem, using the full filename only as a tie-breaker.
- `TrackNumber`, `DiscNumber`, and other metadata remain available for display but never affect playback order.
- XInput capture now scans every loaded module under the game directory instead of only named imports in the main executable.
- Added capture for ordinal-100 `XInputGetStateEx`, ordinal-7/named `XInputGetKeystroke`, and cached runtime function pointers.
- While BACK is held, D-pad, LB, and R3 state bits plus press/repeat/release events are filtered from the game.
- The input layer rescans once per second for delayed modules and function pointers resolved later.

## 2026-07-16 — Fixed the MSVC linker LNK4104 warnings

- Marked the standard COM exports `DllCanUnloadNow` and `DllGetClassObject` as `PRIVATE` in `dsound.def`.
- Both entry points remain present in the final `dsound.dll` export table, so forwarding and runtime behavior are unchanged.
- `PRIVATE` only prevents the linker from placing these COM entry points in the normal import library, eliminating Visual Studio warning LNK4104.
- Parsed the same `.def` with `lld-link` and generated an import library to confirm valid syntax and that the two names are no longer exposed by the import library.

## 2026-07-16 — LocalMusic naming and initial BACK input capture

- Unified the project, release package, documentation, log title, and internal overlay window name under LocalMusic.
- `[LocalMusic]` is now the primary configuration section; legacy `[LocalSpotify]` is read only as a fallback.
- The initial implementation hooked only named `XInputGetState` imports in the main executable; the later multi-path XInput fix supersedes this single-path approach.
- While BACK is held, the game-side D-pad, LB, and R3 are cleared while the plugin still reads the original state for layout editing.
- After BACK is released, held captured buttons remain filtered until all are released, preventing residual playback/menu actions.
- Increased keyboard layout-save idle delay from 500 ms to 1 second so continuous adjustments produce only one final write.

## 2026-07-16 — In-game notification layout, opacity adjustment, and reset

- Added controller layout mode: hold BACK and use the D-pad to move the notice in visual screen direction.
- Added BACK+LB+Up/Down for size and BACK+LB+Left/Right for panel opacity.
- Added BACK+R3 to restore TopRight, X=32, Y=32, Scale=100, and Opacity=92.
- Added keyboard layout controls: Ctrl+-/= for Y, Shift+-/= for X, Alt+-/= for size, and plain -/= for opacity.
- Added Backspace to reset the notification layout.
- A short press adjusts immediately; holding for about 350 ms repeats every 60 ms.
- Controller changes are written when BACK is released; keyboard changes are written after 500 ms idle; reset writes immediately.
- A final save is performed if a controller disconnects during adjustment or the input module shuts down.
- Layout adjustment shows X, Y, scale, and opacity in real time and keeps at least 48 pixels inside the game client area.
- Decoupled position and size: X/Y offsets no longer influence automatic fit scaling.
- Added detailed Simplified Chinese file-level and method-contract comments to every C++ source file.
- All user/developer-facing non-code documents now contain a complete Chinese section followed by a structurally matching English section.

## 2026-07-16 — File logging can be disabled completely

- Added `EnableLogging=true` to the normal `dsound.ini`.
- When set to `false`, the value is read before logger initialization, so `dsound.log` is not created, truncated, or appended.
- Existing log files are left untouched.
- Older configurations without the key default to enabled logging for upgrade diagnostics.
- The simplified release configuration grew from 12 to 13 visible settings.

## 2026-07-16 — Release-candidate configuration cleanup

- Reduced the normal `dsound.ini` from the complete development configuration to 12 common user settings.
- Authentication, hooks, auto-start, auto-advance, preloading, folder albums, metadata, artwork, high DPI, and compatibility settings now use stable built-in defaults.
- Retained support for advanced keys already present in older or manually edited INI files.
- Disabled and hid the unreliable horizontal-menu skip suppression by default.
- Added `开发者/高级配置参考.ini` for future executable and hook diagnostics; it is not loaded automatically.
- Added installation instructions, a release checklist, and `package-release.bat`.
- `package-release.bat <version>` creates a minimal Win64 runtime ZIP and SHA-256 file.

## 2026-07-16 — Folder albums, same-name metadata, and full-library session shuffle

- Every directory that directly contains music becomes an album; loose root tracks are grouped as “Unsorted Tracks.”
- ZIP albums are explicitly unsupported; FMOD always reads normal file paths.
- Added same-base-name sidecar metadata, for example `Song.wav` with `Song.ini`.
- Added same-base-name external artwork, for example `Song.wav` with `Song.png`.
- Added folder `album.ini` and `cover/folder/front` artwork fallback.
- Metadata merges embedded tags first, then sidecar missing fields, then folder album data; optional override is supported.
- Random mode now creates a fixed full-library session queue. Previous and Next move its cursor and can return to the real previous track.
- Added fixed keyboard controls: Insert previous album, Page Up next album, Home cycle play mode.
- Added fixed XInput controls: LB+Left/Right album switching, LB+Down mode cycling, LB+Up current status.
- Play modes cycle Sequential → Random → SingleLoop and are written immediately to `dsound.ini`.
- Album, mode, and status actions reuse the existing custom notification style.
- Added a temporary Spotify-skip guard while LB+Left/Right is active to avoid an extra game track change.
- A one-album library no longer restarts the first track when switching albums; it only shows album information.
- Added Chinese sidecar and folder-album example templates.

## 2026-07-15 — Independent X/Y coordinates and high-DPI awareness

- Split `NotificationMargin` into `NotificationMarginX` and `NotificationMarginY`.
- Kept legacy `NotificationMargin` as a compatibility fallback when the new keys are absent.
- Added `EnableHighDpiAwareness=true`, requesting process Per-Monitor V2 before the plugin window and a separate DPI context for the notification thread.
- Notification size now uses fixed physical pixels controlled only by `NotificationScalePercent`; it no longer multiplies Windows DPI or game resolution.
- The notice still shrinks automatically when the game client area is too small.
- If the game manifest already configured DPI awareness, the plugin preserves it.

## 2026-07-15 — Windows SDK build compatibility fixes

- Explicitly declared `NativeVolumeFunction` in `GameHooks.h` for the original volume trampoline.
- Replaced the obsolete three-argument `Gdiplus::Image::FromStream` call with the two-argument SDK-supported form.
- Preserved the safe order in which the temporary GDI+ Image is destroyed before its IStream is released.

## 2026-07-15 — Custom now-playing notification and embedded artwork

- Added a transparent, click-through, non-activating Win32 notification window without hooking DirectX Present.
- Track changes show status, title, artist, album, and artwork.
- Real Spotify Pause/Unpause calls show pause or resume notices.
- Added MP3 TALB/APIC and OGG ALBUM/METADATA_BLOCK_PICTURE/COVERART parsing.
- Artwork reading and GDI+ decoding run on the notification thread and do not block FMOD playback.
- Rapid changes keep only the newest notice; the window follows the game client area and multi-monitor coordinates.
- Added position, duration, fade, scale, opacity, margin, and foreground-display settings.
- Documented that the in-level pause menu may not call Spotify Pause and exclusive fullscreen may cover the layered window.

## 2026-07-15 — GitHub documentation and Sequential as the default mode

- Added a bilingual GitHub `README.md`.
- Added an INI guide covering settings, defaults, ranges, interactions, and risks.
- Standardized deployment names as `dsound.dll` and `dsound.ini`.
- Changed the default play mode from random to sequential in the struct, parser fallback, built-in INI, and external template.
- Unknown or misspelled `PlayMode` values now fall back to `Sequential`.
- Documented the remaining menu-horizontal-skip and in-level pause limitations honestly.

## 2026-07-15 — Simplified Chinese polish, seamless transitions, menu-safety work, and restored two-layer volume

- Converted package documentation, INI comments, build messages, verification output, and runtime logs to Simplified Chinese.
- Added extensive source comments for thread safety, FMOD callbacks, playback generations, preloading, volume ownership, and hook boundaries.
- Restored the final formula `FMOD = plugin Volume × persistent in-game Spotify volume`.
- `Volume` remains a permanent plugin multiplier and is never replaced by `SetDefaultVolume`.
- `SetDefaultVolume` only updates the game-side multiplier; temporary `SetVolumeOffset` calls are intercepted and ignored.
- Corrected the SpotifyService runtime offset field to neutral zero, fixing a former transient amplification bug.
- Added next-stream preloading and timing diagnostics.
- Added experimental horizontal-menu input suppression; real testing showed delayed UI calls can bypass it, so it remains a documented limitation.
- Added a short stability check for upward game-volume changes while decreases remain immediate.
- Documented that the in-level pause menu may not call Spotify Pause and independent FMOD does not inherit Unreal pause state.
- Fixed the missing native volume function type declaration in `GameHooks.h`.

## 2026-07-15 — Stable game volume, automatic first playback, and FMOD END callback

- Changed the default `Volume` fallback from `1.0` to `0.5`.
- Started playback automatically when the game first publishes its persistent Spotify volume.
- Created FMOD channels paused, applied volume/user data/callbacks, and only then unpaused to prevent a full-volume first frame.
- Made FMOD's native END callback the primary auto-advance path.
- The callback only posts an atomic event carrying a playback generation; track changes still occur on the worker thread.
- Late callbacks from old channels are ignored.
- Added FMOD Channel Stop, callback, and user-data bindings, with polling retained as fallback.

## 2026-07-15 — Runtime game-volume multiplication experiment (superseded)

- A previous version multiplied both game default volume and runtime offset into FMOD output.
- Testing showed temporary level offsets caused audible jumps, so the current design keeps plugin multiplier × persistent game volume and removes temporary offsets.
- The game still owns its UI and save behavior; the plugin does not persist the game volume.

## 2026-07-15 — Auto-advance EOF polling fallback

- Added ChannelIsPlaying, position, length, and end-stall detection.
- The current version uses the FMOD END callback as the primary path; polling remains a compatibility fallback.

## Early versions — Game-driven local Spotify backend

- Changed from an independent global player to a replacement for the game's original Spotify backend only.
- Followed the game's Play, Pause, Unpause, NextTrack, PreviousTrack, and volume interfaces.
- Disabled global hotkeys by default to avoid controlling music from menus and exit confirmations.
- Did not modify persisted Spotify credentials.

## Early versions — Authentication callbacks and old-save compatibility

- Authentication functions no longer only return true; they also broadcast the UE4 delegates the game waits for.
- Added immediate or deferred successful authentication callbacks.
- Kept old saved tokens intact and did not write SaveGame data.
- Added validation for the current executable's dispatcher layout and delegate broadcast function.

## 2026-07-22 — DLL / ASI 同源双形态与路径锚点修复

- 保持“严格输入互斥修复版”为唯一运行代码基底，没有建立第二套 ASI 源码。
- 同一个 `dsound.dll` 构建产物可直接改名为 `LocalMusic.asi`；CMake 和 BAT 只编译一次，并对两个文件执行逐字节比较与 SHA-256 一致性检查。
- 修复 ASI 位于 `Binaries\Win64\scripts` 时的路径错误：
  - `MusicPath` 相对路径改为始终以游戏主 EXE 所在的 `Binaries\Win64` 为基准；
  - `fmod64.dll` 改为从游戏 EXE 目录和 `Plugins\FMODStudio\Binaries\Win64` 查找；
  - INI、日志和语言文件仍跟随插件实际文件名与所在目录。
- 新增进程级单实例互斥。`dsound.dll` 与 `LocalMusic.asi` 同时载入时，只允许一个副本初始化播放器、通知窗口和游戏 Hook，避免双重播放与重复补丁。
- ASI 检测到游戏目录内的同源 `dsound.dll` 已载入时会短暂让出调度，使承担 DirectSound 代理职责的 DLL 实例优先取得运行权。
- 新增 DLL 版与 ASI 版成套配置和语言文件，发行包分别放入 `DLL版` 与 `ASI版` 目录。
- 更新 README、INI 配置说明、最终版本说明、发行检查清单与源码审校报告；安装方式统一维护在 README 中，不再另建安装说明。

## 2026-07-22 — Single-source DLL / ASI dual deployment

- Kept the strict input-isolation revision as the only runtime codebase; no separate ASI source tree was introduced.
- The compiled `dsound.dll` can be renamed directly to `LocalMusic.asi`. CMake and BAT compile once, then verify byte identity and matching SHA-256 values.
- Fixed ASI path resolution under `Binaries\Win64\scripts`:
  - relative `MusicPath` values now always use the main executable directory;
  - `fmod64.dll` is resolved from the executable directory and `Plugins\FMODStudio\Binaries\Win64`;
  - INI, log, and language files still follow the actual module name and location.
- Added a process-wide singleton so simultaneous DLL and ASI loads cannot create duplicate players, overlays, or hooks.
- Added complete DLL-form and ASI-form companion files and updated all user/developer documentation.

