// FMOD C API 动态绑定层：避免依赖 FMOD SDK 头文件和导入库。
// 只声明项目实际使用的最小类型、常量和函数集合。
#pragma once
#include "Common.h"

namespace localmusic {

struct FMOD_SYSTEM;
struct FMOD_SOUND;
struct FMOD_CHANNEL;
struct FMOD_CHANNELGROUP;
struct FMOD_CHANNELCONTROL;
using FMOD_RESULT = int;
using FMOD_BOOL = int;
using FMOD_MODE = unsigned int;
using FMOD_INITFLAGS = unsigned int;
using FMOD_TIMEUNIT = unsigned int;
using FMOD_CHANNELCONTROL_TYPE = int;
using FMOD_CHANNELCONTROL_CALLBACK_TYPE = int;
using FMOD_CHANNELCONTROL_CALLBACK = FMOD_RESULT(WINAPI*)(
    FMOD_CHANNELCONTROL*, FMOD_CHANNELCONTROL_TYPE, FMOD_CHANNELCONTROL_CALLBACK_TYPE, void*, void*);

class FmodApi {
public:
    // 从游戏目录或 FMOD 插件目录加载 fmod64.dll，并解析低层播放 API。
    // 核心函数缺失会整体失败；END 回调等增强接口允许缺失并由播放器启用备用检测。
    bool Load(const std::filesystem::path& game_binary_directory);
    // 清空所有 FMOD 函数指针并释放动态库。
    // 播放器必须先停止 Channel、释放 Sound/System，防止卸载后仍调用 FMOD 代码。
    void Unload();
    // 返回 FMOD 动态库是否已经成功加载。
    // 只检查模块句柄，不代表 System 已经由 LocalPlayer 创建完成。
    bool IsLoaded() const { return module_ != nullptr; }

    // 调用 FMOD_System_Create 创建独立低层音频系统，并把句柄写入 system。
    // 该方法只做动态导出转发；调用方负责检查返回码并在失败时释放后续资源。
    FMOD_RESULT SystemCreate(FMOD_SYSTEM** system) const;
    // 调用 FMOD_System_Init 初始化音频系统，设置最大 Channel 数与初始化标志。
    // extra_driver_data 原样传给 FMOD；成功后必须由 SystemClose/SystemRelease 成对清理。
    FMOD_RESULT SystemInit(FMOD_SYSTEM* system, int max_channels, FMOD_INITFLAGS flags, void* extra_driver_data) const;
    // 调用 FMOD_System_CreateSound 从 UTF-8 文件路径创建流式 Sound。
    // mode 与 exinfo 原样传递；成功得到的 Sound 由 LocalPlayer 负责在切歌或退出时释放。
    FMOD_RESULT SystemCreateSound(FMOD_SYSTEM* system, const char* name, FMOD_MODE mode, void* exinfo, FMOD_SOUND** sound) const;
    // 调用 FMOD_System_PlaySound 为 Sound 创建 Channel，并支持以暂停状态启动。
    // 播放器利用“先暂停创建、设置音量和 END 回调、再解除暂停”避免首帧音量突跳。
    FMOD_RESULT SystemPlaySound(FMOD_SYSTEM* system, FMOD_SOUND* sound, FMOD_CHANNELGROUP* group, FMOD_BOOL paused, FMOD_CHANNEL** channel) const;
    // 调用 FMOD_System_Update 驱动低层混音、流读取与 Channel 回调派发。
    // 该函数由插件工作线程周期调用，不能在 FMOD 回调线程中递归调用。
    FMOD_RESULT SystemUpdate(FMOD_SYSTEM* system) const;
    // 调用 FMOD_System_Close 关闭系统内部设备和混音资源。
    // 只在所有 Channel 与 Sound 已停止释放后调用；随后仍需 SystemRelease 释放对象本身。
    FMOD_RESULT SystemClose(FMOD_SYSTEM* system) const;
    // 调用 FMOD_System_Release 释放由 SystemCreate 返回的系统对象。
    // 调用后原指针立即失效，调用方必须把持有的 system_ 清空。
    FMOD_RESULT SystemRelease(FMOD_SYSTEM* system) const;
    // 调用 FMOD_Sound_Release 释放曲目或预加载 Sound。
    // 释放前必须确保没有仍在使用该 Sound 的活动 Channel。
    FMOD_RESULT SoundRelease(FMOD_SOUND* sound) const;
    // 按指定 FMOD_TIMEUNIT 查询 Sound 长度，当前播放器使用毫秒单位。
    // 该导出属于自动下一曲备用检测；缺失时返回非零错误码而不会解引用空函数指针。
    FMOD_RESULT SoundGetLength(FMOD_SOUND* sound, unsigned int* length, FMOD_TIMEUNIT unit) const;
    // 设置 Channel 暂停状态；非零表示暂停，零表示继续播放。
    // 用于游戏 Pause/Unpause、创建新曲时的安全初始化以及恢复播放。
    FMOD_RESULT ChannelSetPaused(FMOD_CHANNEL* channel, FMOD_BOOL paused) const;
    // 查询 Channel 当前是否暂停，并写入 paused。
    // 返回 FMOD 错误码；调用方不能只依赖输出值判断成功。
    FMOD_RESULT ChannelGetPaused(FMOD_CHANNEL* channel, FMOD_BOOL* paused) const;
    // 查询 Channel 是否仍处于播放生命周期中，并写入 playing。
    // 该接口仅作为 END 回调不可用时的自动下一曲备用信号。
    FMOD_RESULT ChannelIsPlaying(FMOD_CHANNEL* channel, FMOD_BOOL* playing) const;
    // 按指定时间单位读取 Channel 当前播放位置。
    // 导出缺失或查询失败时返回错误码，播放器会继续使用曲长/时钟等其他备用判断。
    FMOD_RESULT ChannelGetPosition(FMOD_CHANNEL* channel, unsigned int* position, FMOD_TIMEUNIT unit) const;
    // 把 0.0–1.0 的最终音量写入当前 Channel。
    // 最终值由“插件 Volume × 游戏持久 Spotify 音量”计算，临时 offset 不在此处叠加。
    FMOD_RESULT ChannelSetVolume(FMOD_CHANNEL* channel, float volume) const;
    // 立即停止指定 Channel，使其不再读取或播放关联 Sound。
    // 导出缺失时返回错误码；停止旧 Channel 后播放器才安全释放旧 Sound。
    FMOD_RESULT ChannelStop(FMOD_CHANNEL* channel) const;
    // 为 Channel 注册或清除 FMOD ChannelControl 回调。
    // 播放器只监听 END 事件，回调线程仅投递带播放代数的原子事件，不直接切歌。
    FMOD_RESULT ChannelSetCallback(FMOD_CHANNEL* channel, FMOD_CHANNELCONTROL_CALLBACK callback) const;
    // 把插件的播放代数上下文指针绑定到 Channel 用户数据。
    // END 回调借此判断事件是否属于当前曲目，防止旧 Channel 的迟到回调误切歌。
    FMOD_RESULT ChannelSetUserData(FMOD_CHANNEL* channel, void* user_data) const;
    // 读取 Channel 上绑定的插件用户数据。
    // 只在 END 回调中使用；导出缺失时整套原生回调能力会被禁用并回退轮询。
    FMOD_RESULT ChannelGetUserData(FMOD_CHANNEL* channel, void** user_data) const;
    // 返回原生 END 回调所需的三个可选导出是否全部可用。
    // 只有回调注册、用户数据写入和读取同时存在时，播放器才启用回调主路径。
    bool HasChannelEndCallbackSupport() const;
    // 把 FMOD_RESULT 转换为便于日志诊断的英文错误文本。
    // 若 fmod64.dll 未导出 FMOD_ErrorString，则返回固定中文占位文本。
    const char* ErrorString(FMOD_RESULT result) const;

    static constexpr FMOD_RESULT Ok = 0;
    static constexpr FMOD_MODE Default = 0x00000000;
    static constexpr FMOD_MODE LoopOff = 0x00000001;
    static constexpr FMOD_MODE Mode2D = 0x00000008;
    static constexpr FMOD_MODE CreateStream = 0x00000080;
    static constexpr FMOD_INITFLAGS InitNormal = 0x00000000;
    static constexpr FMOD_TIMEUNIT TimeUnitMs = 0x00000001;
    static constexpr FMOD_CHANNELCONTROL_TYPE ChannelControlChannel = 0;
    static constexpr FMOD_CHANNELCONTROL_CALLBACK_TYPE ChannelCallbackEnd = 0;

private:
    // 从已加载的 fmod64.dll 解析一个导出并写入强类型函数指针。
    // 返回 false 表示导出缺失；Load 根据该结果决定失败或启用备用方案。
    template <typename T>
    bool Resolve(T& output, const char* name) {
        output = reinterpret_cast<T>(GetProcAddress(module_, name));
        return output != nullptr;
    }

    HMODULE module_ = nullptr;
    bool owns_module_ = false;

    using FnSystemCreate = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM**);
    using FnSystemInit = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*, int, FMOD_INITFLAGS, void*);
    using FnSystemCreateSound = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*, const char*, FMOD_MODE, void*, FMOD_SOUND**);
    using FnSystemPlaySound = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*, FMOD_SOUND*, FMOD_CHANNELGROUP*, FMOD_BOOL, FMOD_CHANNEL**);
    using FnSystemUpdate = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*);
    using FnSystemClose = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*);
    using FnSystemRelease = FMOD_RESULT(WINAPI*)(FMOD_SYSTEM*);
    using FnSoundRelease = FMOD_RESULT(WINAPI*)(FMOD_SOUND*);
    using FnSoundGetLength = FMOD_RESULT(WINAPI*)(FMOD_SOUND*, unsigned int*, FMOD_TIMEUNIT);
    using FnChannelSetPaused = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, FMOD_BOOL);
    using FnChannelGetPaused = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, FMOD_BOOL*);
    using FnChannelIsPlaying = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, FMOD_BOOL*);
    using FnChannelGetPosition = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, unsigned int*, FMOD_TIMEUNIT);
    using FnChannelSetVolume = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, float);
    using FnChannelStop = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*);
    using FnChannelSetCallback = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, FMOD_CHANNELCONTROL_CALLBACK);
    using FnChannelSetUserData = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, void*);
    using FnChannelGetUserData = FMOD_RESULT(WINAPI*)(FMOD_CHANNEL*, void**);
    using FnErrorString = const char*(WINAPI*)(FMOD_RESULT);

    FnSystemCreate system_create_ = nullptr;
    FnSystemInit system_init_ = nullptr;
    FnSystemCreateSound system_create_sound_ = nullptr;
    FnSystemPlaySound system_play_sound_ = nullptr;
    FnSystemUpdate system_update_ = nullptr;
    FnSystemClose system_close_ = nullptr;
    FnSystemRelease system_release_ = nullptr;
    FnSoundRelease sound_release_ = nullptr;
    FnSoundGetLength sound_get_length_ = nullptr;
    FnChannelSetPaused channel_set_paused_ = nullptr;
    FnChannelGetPaused channel_get_paused_ = nullptr;
    FnChannelIsPlaying channel_is_playing_ = nullptr;
    FnChannelGetPosition channel_get_position_ = nullptr;
    FnChannelSetVolume channel_set_volume_ = nullptr;
    FnChannelStop channel_stop_ = nullptr;
    FnChannelSetCallback channel_set_callback_ = nullptr;
    FnChannelSetUserData channel_set_user_data_ = nullptr;
    FnChannelGetUserData channel_get_user_data_ = nullptr;
    FnErrorString error_string_ = nullptr;
};

}  // namespace localmusic
