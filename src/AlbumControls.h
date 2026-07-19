// 固定的专辑、播放模式与通知布局输入层。
//
// 本模块只负责“识别输入并把动作转交给业务模块”，不直接修改播放器队列，
// 也不直接绘制通知窗口。这样可以把 XInput/键盘轮询与 LocalPlayer、
// NowPlayingOverlay 的线程安全状态机分离，便于以后排查按键冲突。
//
// 固定映射：
//   键盘常规：Insert=上一专辑，PageUp=下一专辑，Home=切换播放模式。
//   手柄常规：LB+左/右=切换专辑，LB+下=切换模式，LB+上=显示状态。
//   键盘布局：Ctrl+-/= 调 Y，Shift+-/= 调 X，Alt+-/= 调缩放，
//               无修饰键 -/= 调透明度，Backspace 重置。
//   手柄布局：按住 BACK 后十字键调位置；BACK+LB+上/下调缩放，
//               BACK+LB+左/右调透明度；BACK+R3 重置。
//
// Dangerous Driving 可能通过普通 XInputGetState、序号 100 的
// XInputGetStateEx，或运行时缓存的函数指针读取手柄。本模块会扫描游戏目录内
// 模块的导入表与可写函数指针；命中实际调用路径时，布局会话中的十字键、LB、
// R3 才会从游戏侧输入中清除，并持续过滤到整组按键释放。若 UE4 绕过这些路径，
// 游戏菜单仍可能收到方向输入，因此媒体层另有不依赖扫描结果的五入口同步硬门。
//
// LB、BACK 与方向映射有意不开放重映射：它们是根据 Dangerous Driving
// 的空闲按键选择的固定控制层，避免用户配置到游戏已有操作后产生冲突。
#pragma once
#include "Common.h"

namespace localmusic {

class AlbumControls {
public:
    // 初始化固定输入层。enabled=false 时不加载 XInput，但仍返回成功，
    // 因为输入功能不是游戏启动所必需的核心组件。
    bool Initialize(bool enabled);

    // 每个插件工作循环调用一次。该方法读取键盘/手柄当前状态，执行按下沿、
    // 长按重复和延迟保存；游戏不在前台时只同步状态，不触发任何动作。
    void Update();

    // 停止输入层并释放动态加载的 XInput DLL。若键盘或手柄仍有未落盘的
    // 布局调整，会在通知窗口关闭前补做一次保存；调用后对象可安全销毁。
    void Shutdown();

    // 游戏原生 Spotify 的播放、暂停、恢复和换曲入口在执行前统一调用此门。
    // 只要当前存在 LB/BACK+方向键组合、某个组合方向尚未完全释放，或仍在
    // 释放吸收窗口内，就拒绝游戏来源的媒体命令。插件自身动作不经过此门。
    static bool ShouldBlockGameMediaCommand();

private:
    // XInputGetState 与 XInputGetStateEx 的二进制签名相同。使用 void* 避免
    // 强制依赖 xinput.h，并让项目在较旧 Windows SDK 环境中也能编译。
    using XInputGetStateFunction = DWORD(WINAPI*)(DWORD, void*);

    // XInputGetKeystroke 的最小签名。部分游戏菜单消费离散按键事件而不是
    // XInputGetState 位图，因此布局模式必须同时过滤该接口。
    using XInputGetKeystrokeFunction = DWORD(WINAPI*)(DWORD, DWORD, void*);

    // 记录游戏模块中被替换的函数指针槽位。槽位既可能来自 IAT，也可能是
    // 游戏运行时缓存到可写数据区的 GetProcAddress 结果。replacement 用于
    // 卸载时判断槽位是否仍由本插件持有，防止覆盖后安装的第三方 Hook。
    struct PointerPatch {
        void** slot = nullptr;
        void* original = nullptr;
        void* replacement = nullptr;
    };

    // 表示一个支持“首次立即触发，随后延迟连发”的数字按键状态。
    struct RepeatButton {
        bool down = false;
        uint64_t next_repeat_ms = 0;
    };

    // 一个方向键从按下到完全释放期间只归属一种模式。修饰键中途增减不会
    // 把该方向从普通模式切换到 LB/BACK 模式，必须先松开方向再重新按下。
    // 这条锁定规则是防止普通方向、LB、BACK 和 BACK+LB 相互污染的核心。
    enum class DirectionMode : uint8_t {
        None,
        Plain,
        LeftShoulder,
        Back,
        BackLeftShoulder,
    };

    // 每个 XInput 用户槽位分别保存原始按钮历史、每个方向的锁定模式与布局
    // 调整状态。四个方向独立锁定，可安全处理斜向输入和多个手柄。
    struct ControllerState {
        WORD previous_buttons = 0;
        bool back_down = false;
        bool right_thumb_down = false;
        bool layout_dirty = false;
        std::array<DirectionMode, 4> direction_modes{};
        std::array<RepeatButton, 4> directions{};
    };

    // 判断当前前台顶层窗口是否属于本游戏进程。所有快捷操作都以此为门槛，
    // 防止切到桌面或其他程序时仍然修改音乐/通知设置。
    bool IsGameForeground() const;

    // 返回指定 Win32 虚拟键当前是否按下。这里只读取高位，不消费系统消息。
    bool KeyDown(int virtual_key) const;

    // 更新一个带长按连发的按键。返回 true 表示本帧应执行一次动作：
    // 首次按下立即触发，350ms 后每 60ms 重复；松开时清空重复计时。
    bool ShouldRepeat(RepeatButton& state, bool down, uint64_t now_ms);

    // 游戏失去焦点时同步重复键状态但不产生动作，避免用户按住按键切回游戏
    // 的瞬间被误认为一次新的按下。
    void SynchronizeRepeat(RepeatButton& state, bool down, uint64_t now_ms);

    // 处理 Insert/PageUp/Home 与布局调整键。布局调整后 1000ms 无新操作才
    // 写回 INI，避免连续微调期间反复进行磁盘写入。
    void UpdateKeyboard(bool foreground, uint64_t now_ms);

    // 根据 Alt/Ctrl/Shift 的固定优先级解释 -/=，并把视觉方向增量交给
    // NowPlayingOverlay。返回 true 表示确实修改了一个布局参数。
    bool ApplyKeyboardLayoutStep(bool increase);

    // 轮询至多四个 XInput 用户。BACK 布局会话是插件输入层的全局最高
    // 优先级：只要任意手柄正在按住 BACK，或刚松开 BACK 但捕获按键尚未
    // 全部释放，所有普通插件快捷动作都会被跳过。这样 BACK+LB+方向键
    // 只能用于布局缩放/透明度，不会再落入 LB+方向键的专辑控制。
    void UpdateControllers(bool foreground, uint64_t now_ms);

    // 处理单个手柄中已经锁定为 BACK 或 BACK+LB 的方向会话。只有方向键
    // 在对应修饰键已按住时产生按下沿，才建立布局模式；修饰键中途改变不会
    // 改写方向归属。该方法还负责长按重复、重置和松开 BACK 后持久化。
    bool UpdateControllerLayout(size_t user, WORD buttons, bool foreground, uint64_t now_ms);

    // 处理普通方向和 LB 方向会话。LB 动作只在方向键按下沿且 LB 当前已按住
    // 时锁定并执行一次；先按方向再补按 LB 不会触发第二种动作。
    void UpdateControllerNormal(size_t user, WORD buttons, bool foreground);

    // 安装游戏输入捕获。首次调用会记录插件自身模块与游戏目录，然后扫描
    // 已加载的游戏模块；成功条件是至少接管一个普通或扩展 XInput 状态入口。
    bool InstallGameInputCapture();

    // 周期性重新扫描游戏目录内的模块。它可接管稍后加载的 DLL、延迟解析的
    // XInputGetStateEx 序号入口，以及运行时缓存到可写数据区的函数指针。
    void RefreshGameInputCapture(bool initial_scan);

    // 扫描一个 PE 模块的普通导入表。识别按名称导入的 XInputGetState，
    // 以及按序号 1/100 导入的普通/扩展状态函数，并替换对应 IAT 槽位。
    size_t PatchModuleImports(HMODULE module);

    // 扫描一个游戏模块的可写数据节，把已缓存的 XInput 函数地址替换为 Hook。
    // 覆盖普通/扩展状态函数与 GetKeystroke；仅比较精确函数指针值，不改代码节。
    size_t PatchModuleCachedPointers(HMODULE module);

    // 把单个函数指针槽位替换为指定 Hook。方法负责去重、页面保护、指令缓存
    // 刷新和恢复信息记录；槽位已经被本插件或第三方改写时不会重复覆盖。
    bool PatchPointerSlot(void** slot, void* expected, void* replacement);

    // 判断一个模块是否属于游戏安装目录。系统目录、第三方全局覆盖层、本
    // 插件自身以及 XInput 运行库不会被扫描，避免扩大影响或形成递归。
    bool IsGameModule(HMODULE module) const;

    // 收集当前已加载 XInput DLL 的普通与扩展状态函数地址。结果用于识别游戏
    // 已缓存的函数指针；函数不存在时不会把空地址加入集合。
    void CollectXInputTargets();

    // 恢复 Install/Refresh 修改过的全部函数指针槽位。若其他模块之后又改写
    // 同一槽位，本方法不会强行覆盖第三方 Hook。
    void RemoveGameInputCapture();

    // 游戏调用普通 XInputGetState 时进入的静态桥接函数。先通过插件保存的
    // 原始后端读取状态，再在 BACK 布局模式期间清除十字键、LB 与 R3。
    static DWORD WINAPI HookedXInputGetState(DWORD user_index, void* state);

    // 游戏调用序号 100 的 XInputGetStateEx 时进入的桥接函数。优先调用扩展
    // 后端以保留 Guide 等扩展位；系统不提供时才回退普通 GetState。
    static DWORD WINAPI HookedXInputGetStateEx(DWORD user_index, void* state);

    // 游戏调用 XInputGetKeystroke 时进入的桥接函数。布局模式会吞掉十字键、
    // LB、R3 的按下/重复/抬起事件，并向游戏返回 ERROR_EMPTY。
    static DWORD WINAPI HookedXInputGetKeystroke(DWORD user_index, DWORD reserved, void* keystroke);

    // 对单个 GetKeystroke 事件应用布局捕获。返回 true 表示事件属于被接管的
    // 按键，调用方不得把该事件交给游戏菜单。
    bool FilterGameKeystroke(DWORD requested_user, void* keystroke);

    // 对单个游戏侧 XInput 返回值应用捕获规则。BACK 按住时立即捕获；
    // BACK 松开后若捕获键仍未释放，则继续屏蔽到整组按键全部松开，
    // 防止退出布局模式的边界帧把残留按键泄漏给游戏。
    void FilterGameInput(DWORD user_index, void* state);

    // 当前活动实例供静态 Hook 访问。对象只在工作线程生命周期内有效；
    // 安装 Hook 前发布，移除 Hook 后清空。
    static std::atomic<AlbumControls*> active_instance_;

    bool enabled_ = false;
    bool insert_down_ = false;
    bool page_up_down_ = false;
    bool home_down_ = false;
    bool backspace_down_ = false;
    bool keyboard_layout_dirty_ = false;
    uint64_t keyboard_last_adjust_ms_ = 0;
    uint64_t next_capture_refresh_ms_ = 0;
    RepeatButton minus_repeat_{};
    RepeatButton equals_repeat_{};
    HMODULE self_module_ = nullptr;
    std::filesystem::path game_directory_;
    HMODULE xinput_module_ = nullptr;
    XInputGetStateFunction xinput_get_state_ = nullptr;
    XInputGetStateFunction xinput_get_state_ex_ = nullptr;
    XInputGetKeystrokeFunction xinput_get_keystroke_ = nullptr;
    std::unordered_set<void*> xinput_get_state_targets_;
    std::unordered_set<void*> xinput_get_state_ex_targets_;
    std::unordered_set<void*> xinput_get_keystroke_targets_;
    std::vector<PointerPatch> game_pointer_patches_;
    std::array<std::atomic<bool>, 4> game_capture_latched_{};
    std::array<std::atomic<bool>, 4> layout_mode_active_{};
    std::array<std::atomic<bool>, 4> keystroke_back_down_{};
    std::array<std::atomic<WORD>, 4> keystroke_capture_mask_{};
    // 非零位表示对应方向当前属于插件组合键会话。游戏媒体 Hook 即使早于
    // 工作线程下一次轮询，也会再同步读取原始 XInput；此掩码负责释放边界。
    std::array<std::atomic<WORD>, 4> media_capture_mask_{};
    std::atomic<uint64_t> media_suppression_until_ms_{0};
    std::array<ControllerState, 4> controller_states_{};
};

}  // namespace localmusic
