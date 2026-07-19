# `dsound.ini` 配置说明

## 简体中文

发行版普通配置文件只保留用户真正会调整的项目。配置文件应与 DLL 同目录、同主文件名：

```text
dsound.dll
dsound.ini
```

Windows 文件系统通常不区分大小写，因此 `DSOUND.ini` 与 `dsound.ini` 会指向同一个文件。普通配置修改后需要重新启动游戏；播放模式与通知布局支持在游戏内修改并自动写回。

## 完整默认配置

```ini
[LocalMusic]
Language=auto
EnableLogging=true
MusicPath=..\..\Content\Music
PlayMode=Sequential
Volume=0.5
ShowNowPlayingNotification=true
ShowPauseNotification=true
ShowResumeNotification=true
NotificationDurationMs=3000
NotificationScalePercent=100
NotificationOpacityPercent=92
NotificationPosition=TopRight
NotificationMarginX=32
NotificationMarginY=32
```

## `Language`

```ini
Language=auto
```

控制日志、通知、弹窗和首次生成配置注释的运行语言：

- `auto`：中文 Windows UI 使用 `zh-hans`，其他语言使用 `en-us`。
- `zh-hans`、`en-us`：显式选择官方语言。
- 其他小写 BCP 47 风格标签：加载用户语言文件，例如 `Language=ja-jp` 对应 `dsound.ja-jp.ini`。

文件名必须跟随实际 DLL 主文件名。加载失败或单个键缺失时依次回退到 `en-us` 和 DLL 内置 `zh-hans`。

## `EnableLogging`

插件文件日志总开关：

- `true`：启动时创建或截断 `dsound.log`，并写入本次运行的诊断信息；
- `false`：不创建、不截断、也不追加 `dsound.log`。

关闭后必须重启游戏才会生效。已经存在的旧日志会保持原样，插件不会擅自删除用户文件。关闭日志不会影响音乐播放、通知窗口、播放模式保存或布局保存。

此开关只控制插件自己的 `dsound.log`，不控制 Windows 事件查看器、系统崩溃记录或其他程序的日志。

## `MusicPath`

音乐目录。相对路径以 `DangerousDriving\Binaries\Win64` 为基准，也可以使用绝对路径。

默认值：

```ini
MusicPath=..\..\Content\Music
```

对应：

```text
DangerousDriving\Content\Music\
```

程序递归扫描 MP3、WAV 和 OGG。每个实际包含音乐文件的文件夹会建立为一张专辑，根目录散装曲目归入“未分类曲目”。

播放顺序只由路径和文件名决定：

```text
专辑：相对于 MusicPath 的文件夹路径自然排序
曲目：音频主文件名自然排序，完整文件名仅作为同名兜底
```

根目录散装曲目固定排在文件夹专辑之前。自然排序不区分英文大小写，并把 `2` 排在 `10` 前面。内嵌标签、同名 INI、`album.ini` 中的 `TrackNumber`、`DiscNumber`、年份或专辑标题仅用于显示，不参与播放顺序。需要调整时直接重命名文件夹或音频文件。

## `PlayMode`

支持：

- `Sequential`：顺序播放；
- `Random`：全部已扫描音乐生成一次固定会话乱序队列；
- `SingleLoop`：单曲循环。

随机模式中的上一曲和下一曲只移动队列游标，不会重新抽签。

通过以下操作切换模式后会立即写回：

```text
键盘 Home
手柄 LB + 十字键下
```

未知或拼写错误的值回退为 `Sequential`。

## `Volume`

插件永久音量倍率。支持 `0.0-1.0`，也兼容 `0-100`。

```text
FMOD 最终音量 = Volume × 游戏持久 Spotify 音量
```

例如：

```text
Volume=0.20，游戏音量=100% → 最终 20%
Volume=0.20，游戏音量=95%  → 最终 19%
```

游戏进入关卡、菜单或暂停界面时产生的临时 `SetVolumeOffset` 不参与计算。

## 通知开关

### `ShowNowPlayingNotification`

控制换曲、切换专辑和用户主动显示当前状态时的通知。

### `ShowPauseNotification`

游戏真正调用 Spotify `Pause` 时显示“已暂停”。如果关卡暂停菜单没有调用该接口，就不会出现暂停通知。

### `ShowResumeNotification`

现有曲目从暂停恢复时显示“继续播放”。

布局调整、保存和重置预览不受这三个普通通知开关限制，否则用户关闭曲目提示后将无法在游戏内找回布局。

## `NotificationDurationMs`

曲目与专辑通知保持时间，单位毫秒，不含淡入淡出。程序把范围限制在 `500-15000`。

默认：

```ini
NotificationDurationMs=3000
```

暂停、继续、模式和布局状态使用稳定内置时长。

## `NotificationScalePercent`

通知整体大小，范围 `50-200`，默认 `100`。

`100` 时基础窗口约为 `548 × 140` 物理像素，封面约为 `104 × 104` 物理像素。通知不随游戏分辨率同比缩放，也不再乘以 Windows DPI；游戏客户区过小时仍会自动缩小以防越界。

游戏内调整：

```text
手柄 BACK + LB + 十字键上/下
键盘 Alt + = / Alt + -
```

每次变化 `5%`。

## `NotificationOpacityPercent`

通知面板背景不透明度，范围 `40-100`，默认 `92`。文字和封面仍使用自己的不透明度。

游戏内调整：

```text
手柄 BACK + LB + 十字键右/左
键盘直接按 = / -
```

每次变化 `5%`。

## `NotificationPosition`

通知锚点：

```text
TopLeft
TopRight
BottomLeft
BottomRight
```

游戏内移动调整不会改变锚点，只修改 X/Y 边距。重置布局会恢复为 `TopRight`。

## `NotificationMarginX` / `NotificationMarginY`

从所选锚点向游戏客户区内部移动的水平和垂直距离，单位为物理像素，范围 `0-2000`。

以 `TopRight` 为例：

- X 越大，通知越向左；
- Y 越大，通知越向下。

用户不需要记忆锚点方向。游戏内快捷键按画面视觉方向移动，插件会自动换算边距：

```text
手柄：按住 BACK + 十字键
键盘：Shift + -/= 调 X；Ctrl + -/= 调 Y
```

每次变化 `8` 个物理像素。

## 游戏内通知布局完整操作

### 手柄

| 组合 | 功能 |
|---|---|
| 按住 `BACK` + 左/右/上/下 | 向对应画面方向移动 |
| `BACK + LB + 上` | 放大 |
| `BACK + LB + 下` | 缩小 |
| `BACK + LB + 左` | 降低不透明度 |
| `BACK + LB + 右` | 提高不透明度 |
| `BACK + R3` | 重置锚点、位置、大小和透明度 |

手柄方向键采用按住周期互斥状态机。每个方向只在按下沿锁定为“普通、`LB`、`BACK`、`BACK+LB`”之一，完全释放前不会因中途增加或松开修饰键而切换功能；修饰键应先按下或与方向键同时按下。`LB+方向键`、`BACK+方向键`、`BACK+LB+方向键` 会分别只执行专辑/模式、位置、缩放/透明度动作。组合会话期间以及释放后的 750 毫秒吸收窗口内，游戏来源的 `PreviousTrack`、`NextTrack`、`Pause`、`Play`、`Unpause` 五个媒体入口都会被统一拒绝，避免换曲、播放、暂停和恢复互相污染。

LocalMusic 仍会尝试覆盖游戏目录内模块的普通 `XInputGetState`、序号 100 的 `XInputGetStateEx`、`XInputGetKeystroke` 和已缓存函数指针；只有命中这些路径时，十字键、`LB`、`R3` 才会从返回给游戏的输入状态中清除。部分 UE4 输入路径可能绕过扫描，因此累计捕获数为 0 时，游戏菜单仍可能看到方向输入；但上述五个媒体入口的同步硬门不依赖扫描结果，插件组合不会再改变曲目或播放状态。

### 键盘

| 组合 | 功能 |
|---|---|
| `Ctrl + -` | 向上移动 |
| `Ctrl + =` | 向下移动 |
| `Shift + -` | 向左移动 |
| `Shift + =` | 向右移动 |
| `Alt + -` | 缩小 |
| `Alt + =` | 放大 |
| 直接按 `-` | 降低不透明度 |
| 直接按 `=` | 提高不透明度 |
| `Backspace` | 重置锚点、位置、大小和透明度 |

停止调整约 `1000` 毫秒（1 秒）后一次性写入 INI。修饰键解释优先级为 `Alt → Ctrl → Shift → 无修饰键`，避免同时按多个修饰键时修改多个参数。

### 长按与保存

- 第一次按下立即调整；
- 持续按住约 `350` 毫秒后开始连续调整；
- 连续调整间隔约 `60` 毫秒；
- 手柄松开 `BACK`、手柄在调整中断开、输入模块关闭或键盘停止调整时保存；
- 重置立即保存；
- 写入失败时当前进程内预览仍有效，但下次启动会恢复磁盘中的旧值；
- 实际绘制坐标始终保证至少 `48` 像素留在游戏客户区中；
- `BACK + R3` 或 `Backspace` 可随时恢复默认布局。

默认重置值：

```ini
NotificationPosition=TopRight
NotificationMarginX=32
NotificationMarginY=32
NotificationScalePercent=100
NotificationOpacityPercent=92
```

## 固定的发行行为

以下行为不显示在普通 INI 中，但使用稳定内置默认值：

- 递归扫描 MP3/WAV/OGG；
- 文件夹专辑、同名曲目 INI、同名封面和 `album.ini`；
- Insert/Page Up/Home 与 LB+十字键播放控制；
- BACK/Backspace 通知布局控制；
- 自动播放、FMOD END 自动下一曲和下一首预加载；
- 插件音量 × 游戏持久 Spotify 音量；
- 忽略游戏临时 `SetVolumeOffset`；
- 内嵌封面、外部封面、播放模式文字和前台显示限制；
- Per-Monitor V2 高 DPI 通知线程；
- 纯内存 Spotify 认证旁路和游戏 Hook；
- 当前 EXE 的兼容配置及动态验证。

新版使用 `[LocalMusic]` 作为主配置节；旧 `[LocalSpotify]` 中的同名键仅在主节缺失时回退读取。

旧版 `dsound.ini` 中已经存在的高级键仍会被读取。开发者调试高级 Hook、RVA 或未来 EXE 时，参考 `开发者/高级配置参考.ini`；该文件不会被插件自动读取。

## 曲目同名元数据

文件命名：

```text
Song.wav
Song.ini
Song.png
```

`Song.ini`：

```ini
[Track]
Title=歌曲名称
Artist=艺术家
Album=专辑名称
AlbumArtist=专辑艺术家
TrackNumber=1
DiscNumber=1
Year=2026
Genre=Soundtrack
OverrideEmbeddedMetadata=false
Cover=
```

默认只补充内嵌标签缺失字段。`OverrideEmbeddedMetadata=true` 时，该曲目侧挂 INI 的非空字段可以覆盖有效内嵌标签。`TrackNumber` 与 `DiscNumber` 仅作为显示元数据保留，不会改变播放顺序；顺序始终由文件名决定。

## 文件夹专辑元数据

```ini
[Album]
Title=专辑名称
Artist=专辑艺术家
AlbumArtist=专辑艺术家
Year=2026
Genre=Soundtrack
DiscNumber=1
Cover=cover.jpg
```

---

## English

The normal release configuration only exposes settings that regular users are expected to change. The configuration file must be in the same directory and use the same base name as the DLL:

```text
dsound.dll
dsound.ini
```

Windows normally treats filenames case-insensitively, so `DSOUND.ini` and `dsound.ini` refer to the same file. Restart the game after changing normal settings. Play mode and notification layout can also be changed in game and are written back automatically.

## Complete default configuration

```ini
[LocalMusic]
Language=auto
EnableLogging=true
MusicPath=..\..\Content\Music
PlayMode=Sequential
Volume=0.5
ShowNowPlayingNotification=true
ShowPauseNotification=true
ShowResumeNotification=true
NotificationDurationMs=3000
NotificationScalePercent=100
NotificationOpacityPercent=92
NotificationPosition=TopRight
NotificationMarginX=32
NotificationMarginY=32
```

## `Language`

```ini
Language=auto
```

Controls the runtime language of logs, notifications, message boxes, and comments in a newly generated configuration:

- `auto`: Chinese Windows UI uses `zh-hans`; other UI languages use `en-us`.
- `zh-hans` or `en-us`: explicitly selects an official language.
- Another lowercase BCP 47-style tag: loads a user file, for example `Language=ja-jp` maps to `dsound.ja-jp.ini`.

The filename must follow the actual proxy DLL basename. A missing file or individual key falls back to `en-us`, then to the DLL's built-in `zh-hans` text.

## `EnableLogging`

Master switch for the plugin's file log:

- `true`: create or truncate `dsound.log` at startup and write diagnostics for the current run;
- `false`: do not create, truncate, or append `dsound.log`.

Restart the game after changing this setting. Existing log files are left untouched. Disabling logging does not affect playback, notifications, play-mode persistence, or layout persistence.

This switch only controls the plugin's own `dsound.log`; it does not control Windows Event Viewer, operating-system crash records, or logs from other software.

## `MusicPath`

Music directory. Relative paths are resolved from `DangerousDriving\Binaries\Win64`; absolute paths are also accepted.

Default:

```ini
MusicPath=..\..\Content\Music
```

This resolves to:

```text
DangerousDriving\Content\Music\
```

The plugin recursively scans MP3, WAV, and OGG files. Every directory that directly contains music becomes an album, while loose tracks in the music root are grouped under “Unsorted Tracks.”

Playback order is based only on paths and filenames:

```text
Albums: natural sort of folder paths relative to MusicPath
Tracks: natural sort of the audio stem, with the full filename only as a tie-breaker
```

Loose root tracks are always placed before folder albums. Sorting is case-insensitive and places `2` before `10`. Embedded tags, same-name INI files, and `album.ini` fields such as `TrackNumber`, `DiscNumber`, year, or album title are display-only and never affect playback order. Rename folders or audio files to change the order.

## `PlayMode`

Supported values:

- `Sequential`: sequential playback;
- `Random`: create one fixed shuffled queue from every scanned track;
- `SingleLoop`: repeat the current track.

Previous and Next move the queue cursor in Random mode; they do not draw a new random track every time.

The following controls cycle the mode and write it back immediately:

```text
Keyboard Home
Controller LB + D-pad Down
```

Unknown or misspelled values fall back to `Sequential`.

## `Volume`

Permanent plugin volume multiplier. Values from `0.0-1.0` are recommended; `0-100` is also accepted.

```text
Final FMOD volume = Volume × persistent in-game Spotify volume
```

Examples:

```text
Volume=0.20, game volume=100% → final 20%
Volume=0.20, game volume=95%  → final 19%
```

Temporary `SetVolumeOffset` values sent while entering levels, menus, or pause screens are ignored.

## Notification switches

### `ShowNowPlayingNotification`

Controls track-change, album-change, and user-requested current-status notices.

### `ShowPauseNotification`

Shows “Paused” when the game actually calls Spotify `Pause`. If the in-level pause menu does not call that interface, no pause notice is generated.

### `ShowResumeNotification`

Shows “Resumed” when the existing track resumes from pause.

Layout-adjustment, save, and reset previews bypass these three normal notification switches; otherwise a user who disabled track notices could not recover or tune the layout in game.

## `NotificationDurationMs`

Hold time for track and album notices in milliseconds, excluding fade-in and fade-out. The program clamps it to `500-15000`.

Default:

```ini
NotificationDurationMs=3000
```

Pause, resume, play-mode, and layout-status notices use stable built-in durations.

## `NotificationScalePercent`

Overall notification size, clamped to `50-200`, with a default of `100`.

At `100`, the base window is approximately `548 × 140` physical pixels and the artwork is approximately `104 × 104` physical pixels. The notice does not scale proportionally with game resolution and is not multiplied by Windows DPI; it may still shrink automatically when the game client area is too small.

In-game controls:

```text
Controller BACK + LB + D-pad Up/Down
Keyboard Alt + = / Alt + -
```

Each step changes the value by `5%`.

## `NotificationOpacityPercent`

Panel background opacity, clamped to `40-100`, with a default of `92`. Text and artwork use their own opacity.

In-game controls:

```text
Controller BACK + LB + D-pad Right/Left
Keyboard plain = / -
```

Each step changes the value by `5%`.

## `NotificationPosition`

Notification anchor:

```text
TopLeft
TopRight
BottomLeft
BottomRight
```

In-game movement keeps the current anchor and only changes the X/Y margins. Resetting the layout restores `TopRight`.

## `NotificationMarginX` / `NotificationMarginY`

Horizontal and vertical inward distances from the selected anchor, in physical pixels, clamped to `0-2000`.

For `TopRight`:

- larger X moves the notice left;
- larger Y moves the notice down.

Users do not need to remember anchor math. In-game shortcuts move in visual screen direction, and the plugin converts that movement into margins automatically:

```text
Controller: hold BACK + D-pad
Keyboard: Shift + -/= changes X; Ctrl + -/= changes Y
```

Each step changes position by `8` physical pixels.

## Complete in-game notification layout controls

### Controller

| Combination | Action |
|---|---|
| Hold `BACK` + Left/Right/Up/Down | Move in the corresponding screen direction |
| `BACK + LB + Up` | Increase size |
| `BACK + LB + Down` | Decrease size |
| `BACK + LB + Left` | Reduce opacity |
| `BACK + LB + Right` | Increase opacity |
| `BACK + R3` | Reset anchor, position, size, and opacity |

Controller directions use a held-session isolation state machine. Each direction locks to exactly one mode—Plain, `LB`, `BACK`, or `BACK+LB`—on its rising edge and cannot change function until fully released; press the modifier first or together with the direction. `LB+D-pad`, `BACK+D-pad`, and `BACK+LB+D-pad` therefore perform only album/mode, position, or scale/opacity actions respectively. During those sessions and the 750 ms post-release absorption window, all game-originated `PreviousTrack`, `NextTrack`, `Pause`, `Play`, and `Unpause` entries are rejected by one shared media gate, preventing skip/play/pause/resume cross-contamination.

LocalMusic still attempts to capture normal `XInputGetState`, ordinal-100 `XInputGetStateEx`, `XInputGetKeystroke`, and cached XInput pointers in modules under the game directory. The D-pad, `LB`, and `R3` are removed from the state returned to the game only when one of those paths is actually captured. Some UE4 input paths may bypass the scan, so menus may still see directional input when the cumulative capture count is zero; the five-entry synchronous media gate does not depend on that scan and still prevents plugin chords from changing tracks or playback state.

### Keyboard

| Combination | Action |
|---|---|
| `Ctrl + -` | Move up |
| `Ctrl + =` | Move down |
| `Shift + -` | Move left |
| `Shift + =` | Move right |
| `Alt + -` | Decrease size |
| `Alt + =` | Increase size |
| Plain `-` | Reduce opacity |
| Plain `=` | Increase opacity |
| `Backspace` | Reset anchor, position, size, and opacity |

Changes are written after approximately `1000` ms (1 second) without another adjustment. Modifier interpretation priority is `Alt → Ctrl → Shift → no modifier`, preventing multiple parameters from changing when several modifiers are held.

### Hold behavior and persistence

- the first press adjusts immediately;
- holding for about `350` ms starts continuous adjustment;
- continuous steps occur about every `60` ms;
- changes are saved when controller `BACK` is released, when a controller disconnects during adjustment, when the input module shuts down, or when keyboard adjustment becomes idle;
- reset actions save immediately;
- if writing fails, the in-process preview remains active, but the next launch uses the previous on-disk values;
- actual rendering always keeps at least `48` pixels inside the game client area;
- `BACK + R3` or `Backspace` can restore the default layout at any time.

Reset defaults:

```ini
NotificationPosition=TopRight
NotificationMarginX=32
NotificationMarginY=32
NotificationScalePercent=100
NotificationOpacityPercent=92
```

## Fixed release behavior

The following behavior is hidden from the normal INI and uses stable built-in defaults:

- recursive MP3/WAV/OGG scanning;
- folder albums, same-name track INI files, same-name artwork, and `album.ini`;
- Insert/Page Up/Home and LB+D-pad playback controls;
- BACK/Backspace notification layout controls;
- auto-start, FMOD END auto-advance, and next-track preloading;
- plugin volume × persistent in-game Spotify volume;
- ignoring temporary game `SetVolumeOffset` calls;
- embedded artwork, external artwork, play-mode text, and foreground-only display;
- Per-Monitor V2 high-DPI notification thread;
- in-memory Spotify authentication bypass and game hooks;
- compatibility data and dynamic validation for the current executable.

Advanced keys already present in an older `dsound.ini` are still read. Developers testing advanced hooks, RVAs, or a future executable should reference `开发者/高级配置参考.ini`; that file is not loaded automatically.

## Same-name track metadata

File naming:

```text
Song.wav
Song.ini
Song.png
```

`Song.ini`:

```ini
[Track]
Title=Track Title
Artist=Artist
Album=Album Title
AlbumArtist=Album Artist
TrackNumber=1
DiscNumber=1
Year=2026
Genre=Soundtrack
OverrideEmbeddedMetadata=false
Cover=
```

By default, the sidecar only fills missing embedded fields. With `OverrideEmbeddedMetadata=true`, non-empty fields in that track's sidecar may replace valid embedded tags. `TrackNumber` and `DiscNumber` are display metadata only and never change playback order; filenames remain authoritative.

## Folder album metadata

```ini
[Album]
Title=Album Title
Artist=Album Artist
AlbumArtist=Album Artist
Year=2026
Genre=Soundtrack
DiscNumber=1
Cover=cover.jpg
```
