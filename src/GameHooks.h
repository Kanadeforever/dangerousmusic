// 游戏 Hook 声明：负责定位 SpotifyService 包装函数、安装安全补丁并模拟认证回调。
// 只对已验证地址写补丁，未知 EXE 或布局不匹配时默认拒绝继续。
#pragma once
#include "Config.h"
#include "InlinePatch.h"
#include "PEImage.h"

namespace localmusic {

class GameHooks {
public:
    // 验证 EXE、定位 UE4/原生入口并按配置安装全部 Spotify Hook。
    // RequireAllHooks 开启时任一关键补丁失败都会回滚，避免半安装状态。
    bool Install(const Config& config);
    // 按逆序恢复所有 Hook/跳板并清空活动实例。
    // 只在工作线程退出阶段执行，防止游戏线程跳入已释放代码。
    void Remove();
    // 返回当前是否至少安装了一个有效 Hook。
    // 只读查询，不验证目标字节是否被其他模块再次修改。
    bool Installed() const { return installed_; }

private:
    using FStringCopyFunction = void(__fastcall*)(void* destination, const void* source);
    using DelegateBroadcastFunction = void(__fastcall*)(void* delegate, void* parameters);

    // SetDefaultVolume / SetVolumeOffset 最终会进入带一个 int 参数的原生成员函数。
    // 必须在头文件中明确声明该函数指针类型，否则保存原始 trampoline 地址时，
    // MSVC 会因为 NativeVolumeFunction 未定义而直接编译失败。Win64 下 __fastcall
    // 与平台统一调用约定兼容，这里仍保留标记以准确表达游戏原函数签名。
    using NativeVolumeFunction = void(__fastcall*)(void* service, int value);

    struct HookRequest {
        std::string name;
        void* replacement = nullptr;
        bool required = true;
        void* resolved = nullptr;
    };

    // 按注册表、特征码、RVA 的顺序解析一个 UE4 exec 包装函数。
    // 集中执行日志和最终验证，避免各 Hook 使用不同信任标准。
    void* ResolveWrapper(const std::string& name) const;
    // 通过 UE4 原生函数注册表定位指定 exec 包装函数。
    // 这是首选方式，候选仍需通过 LooksLikeExecWrapper 验证。
    void* ResolveByNameTable(const std::string& name) const;
    // 按 INI 特征码扫描指定包装函数。
    // 仅作为注册表定位失败时的兼容回退。
    void* ResolveByPattern(const std::string& name) const;
    // 按配置中的已知 RVA 获取包装函数候选。
    // 地址必须在可执行节且结构验证通过才会返回。
    void* ResolveByRva(const std::string& name) const;
    // 检查候选代码是否符合 UE4 Blueprint exec 包装函数特征。
    // 验证指令范围和 FFrame 访问，减少未知版本误匹配。
    bool LooksLikeExecWrapper(const uint8_t* address) const;
    // 从已验证包装函数推断 FFrame::Code 字段偏移。
    // 推断失败返回 -1，安装流程不会用猜测值继续写栈。
    int InferFrameCodeOffset(const uint8_t* wrapper) const;
    // 在包装函数尾部寻找跳转到原生实现的目标。
    // 只接受映像内可执行地址，用于无参数原生函数 Hook。
    void* FindTailJumpTarget(const uint8_t* wrapper, size_t search_length = 96) const;
    // 扫描包装函数末段最后一个有效相对 call 目标。
    // 用于参数包装函数，让 UE 正常消费参数后再 Hook 原生目标。
    void* FindLastCallTarget(const uint8_t* wrapper, size_t search_length = 192) const;
    // 比较 PE 时间戳、映像大小和可选 SHA-256。
    // StrictVersion=false 仍要求后续函数结构验证，不等于盲目打补丁。
    bool ValidateExecutableVersion();
    // 安装不需要调用原函数的绝对跳转补丁并登记所有权。
    // 失败时不把未安装对象加入列表，便于统一回滚。
    bool InstallPatch(void* target, void* replacement, const std::wstring& label);
    // 安装可调用原函数的跳板补丁并返回 trampoline。
    // 游戏持久设置等需要保留原逻辑的入口使用该方式。
    bool InstallTrampolinePatch(void* target, void* replacement, size_t copy_size, void** trampoline, const std::wstring& label);

    // 推进 UE4 FFrame 字节码指针，模拟原 exec 包装函数已消费调用。
    // 返回 Getter 结果后必须完成该步骤，否则 Blueprint VM 会重复解析参数。
    void FinishFrame(void* stack) const;
    // 向 Blueprint result 缓冲区写入布尔返回值并结束当前 frame。
    // 写入前验证指针可写，避免损坏 UE VM 状态。
    void ReturnBool(void* stack, void* result, bool value);
    // 通过已解析的 FString 复制辅助函数写回 Unicode 字符串。
    // 失败时返回空字符串并保持 VM 可继续执行。
    void ReturnString(void* stack, void* result, const std::wstring& value);

    enum PendingCallback : uint32_t {
        PendingEnabled = 1u << 0,
        PendingCheckAuthCode = 1u << 1,
        PendingRefreshAccessToken = 1u << 2,
        PendingSwitchActiveDevice = 1u << 3,
    };

    // 登记一个待广播的认证/设备回调。
    // 去重同类请求，避免游戏轮询产生重复委托。
    void QueueCallback(void* service, uint32_t callback, const wchar_t* label);
    // 立即派发当前服务对象已排队的全部回调。
    // 返回是否实际派发，供日志和触发策略判断。
    bool DispatchPendingCallbacks(void* service, const wchar_t* trigger);
    // 按 Deferred 模式在 Update 中推进待回调队列。
    // Immediate 模式下通常没有剩余事件。
    void PumpPendingCallbacks(void* service, const wchar_t* trigger);
    // 按回调类型选择对应 UE 多播委托并写入本地成功结果。
    // 所有字段偏移都先做范围与可读性检查。
    void DispatchCallback(void* service, uint32_t callback);
    // 向指定服务对象偏移处的 UE 动态多播委托广播一个布尔值。
    // 委托函数或字段无效时只记录警告，不调用未知地址。
    void BroadcastBool(void* service, int offset, bool value, const wchar_t* label);
    // 构造并广播设备切换结果。
    // 本地后端始终在内存中报告成功，不联系 Spotify 网络。
    void BroadcastSwitchDevice(void* service, bool switched = true, bool active = true);

    // 返回当前已安装的 GameHooks 实例。
    // 静态 Hook 入口通过它访问配置和回调状态，未安装时返回空。
    static GameHooks* Active();

    // 只返回值、不处理参数的 Blueprint exec Hook。
    static void __fastcall HookHasValidAccessToken(void*, void*, void*);
    // 替换原版 Spotify Getter HookHasValidAccessTokenAndDevice，把本地后端状态写回 Blueprint。
    // 不访问网络或游戏凭据，并正确推进 FFrame。
    static void __fastcall HookHasValidAccessTokenAndDevice(void*, void*, void*);
    // 替换原版 Spotify Getter HookIsEnabled，把本地后端状态写回 Blueprint。
    // 不访问网络或游戏凭据，并正确推进 FFrame。
    static void __fastcall HookIsEnabled(void*, void*, void*);
    // 替换原版 Spotify Getter HookIsPaused，把本地后端状态写回 Blueprint。
    // 不访问网络或游戏凭据，并正确推进 FFrame。
    static void __fastcall HookIsPaused(void*, void*, void*);
    // 替换原版当前曲目信息 Getter HookGetPlayingArtistName，返回本地元数据。
    // 使用 FString 辅助函数保持 Unicode 标题和艺术家。
    static void __fastcall HookGetPlayingArtistName(void*, void*, void*);
    // 替换原版当前曲目信息 Getter HookGetPlayingTrackName，返回本地元数据。
    // 使用 FString 辅助函数保持 Unicode 标题和艺术家。
    static void __fastcall HookGetPlayingTrackName(void*, void*, void*);

    // SpotifyService 原生函数 Hook。动作/网络方法保留 UE4 自动生成的 exec 包装层，
    // 避免自行重写 FFrame 参数解析和蓝图调用约定。
    static void __fastcall HookNativeCheckEnabled(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeCheckAuthCode 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeCheckAuthCode(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeClearAccessTokens 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeClearAccessTokens(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeRefreshAccessToken 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeRefreshAccessToken(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeRequestActivateCode 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeRequestActivateCode(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeSwitchActiveDevice 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeSwitchActiveDevice(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeUpdate 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeUpdate(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeNextTrack 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeNextTrack(void*);
    // 拦截游戏原生 Spotify 操作 HookNativePause 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativePause(void*);
    // 拦截游戏原生 Spotify 操作 HookNativePlay 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativePlay(void*);
    // 拦截游戏原生 Spotify 操作 HookNativePreviousTrack 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativePreviousTrack(void*);
    // 拦截游戏原生 Spotify 操作 HookNativeUnpause 并映射到本地后端/认证回调。
    // 根据入口职责保留必要原逻辑或完全阻断网络访问。
    static void __fastcall HookNativeUnpause(void*);
    // 先调用游戏原始持久音量逻辑，再把同一百分比交给本地播放器。
    // 使用 NativeVolumeFunction 跳板保持游戏设置保存行为。
    static void __fastcall HookSetDefaultVolume(void*, int);
    // 拦截游戏临时音量 offset 并通知本地播放器记录/忽略。
    // 不让关卡 ducking 改变发行版的稳定听感。
    static void __fastcall HookSetVolumeOffset(void*, int);

    Config config_;
    PEImage image_;
    std::vector<std::unique_ptr<AbsoluteJumpPatch>> patches_;
    std::vector<std::unique_ptr<TrampolinePatch>> trampoline_patches_;
    std::unordered_map<std::string, uint8_t*> wrappers_;
    FStringCopyFunction string_copy_ = nullptr;
    NativeVolumeFunction original_set_default_volume_ = nullptr;
    DelegateBroadcastFunction delegate_broadcast_ = nullptr;
    std::atomic<void*> service_object_{nullptr};
    std::mutex callback_mutex_;
    std::vector<uint32_t> pending_callback_queue_;
    std::atomic<bool> update_seen_{false};
    std::atomic<bool> dispatching_callbacks_{false};
    int frame_code_offset_ = -1;
    bool installed_ = false;
};

}  // namespace localmusic
