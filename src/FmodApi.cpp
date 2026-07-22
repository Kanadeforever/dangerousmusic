// FMOD 动态绑定实现：优先复用游戏已加载的 fmod64.dll，否则从游戏插件目录加载。
// 必需导出缺失时初始化失败；END 回调相关导出缺失时降级为轮询。
#include "FmodApi.h"
#include "Logger.h"
#include "Localization.h"

namespace localmusic {

// 从游戏目录或 FMOD 插件目录加载 fmod64.dll，并解析低层播放 API。
// 核心函数缺失会整体失败；END 回调等增强接口允许缺失并由播放器启用备用检测。
bool FmodApi::Load(const std::filesystem::path& game_binary_directory) {
    module_ = GetModuleHandleW(L"fmod64.dll");
    owns_module_ = false;

    if (!module_) {
        const std::vector<std::filesystem::path> candidates{
            game_binary_directory / L"fmod64.dll",
            game_binary_directory / L"..\\..\\Plugins\\FMODStudio\\Binaries\\Win64\\fmod64.dll",
        };
        for (const auto& candidate : candidates) {
            module_ = LoadLibraryW(candidate.lexically_normal().c_str());
            if (module_) {
                owns_module_ = true;
                log::Info(loc::Format(L"Log.Fmod.LoadedPath", {candidate.lexically_normal().wstring()}));
                break;
            }
        }
    } else {
        log::Info(loc::Text(L"Log.Fmod.UsingLoaded"));
    }

    if (!module_) {
        log::Error(loc::Text(L"Log.Fmod.LoadFailed"));
        return false;
    }

    const bool ok = Resolve(system_create_, "FMOD_System_Create") &&
                    Resolve(system_init_, "FMOD_System_Init") &&
                    Resolve(system_create_sound_, "FMOD_System_CreateSound") &&
                    Resolve(system_play_sound_, "FMOD_System_PlaySound") &&
                    Resolve(system_update_, "FMOD_System_Update") &&
                    Resolve(system_close_, "FMOD_System_Close") &&
                    Resolve(system_release_, "FMOD_System_Release") &&
                    Resolve(sound_release_, "FMOD_Sound_Release") &&
                    Resolve(channel_set_paused_, "FMOD_Channel_SetPaused") &&
                    Resolve(channel_get_paused_, "FMOD_Channel_GetPaused") &&
                    Resolve(channel_is_playing_, "FMOD_Channel_IsPlaying") &&
                    Resolve(channel_set_volume_, "FMOD_Channel_SetVolume") &&
                    Resolve(channel_stop_, "FMOD_Channel_Stop");
    // 播放位置/曲长轮询仅作为备用。首选 EOF 路径是 FMOD 原生 END 回调；
    // 只有回调、用户数据设置和用户数据读取三个导出都存在时才启用。
    Resolve(channel_get_position_, "FMOD_Channel_GetPosition");
    Resolve(sound_get_length_, "FMOD_Sound_GetLength");
    const bool callback_ok = Resolve(channel_set_callback_, "FMOD_Channel_SetCallback") &&
                             Resolve(channel_set_user_data_, "FMOD_Channel_SetUserData") &&
                             Resolve(channel_get_user_data_, "FMOD_Channel_GetUserData");
    if (!callback_ok) {
        channel_set_callback_ = nullptr;
        channel_set_user_data_ = nullptr;
        channel_get_user_data_ = nullptr;
        log::Warn(loc::Text(L"Log.Fmod.CallbackUnavailable"));
    }
    Resolve(error_string_, "FMOD_ErrorString");

    if (!ok) {
        log::Error(loc::Text(L"Log.Fmod.MissingExports"));
        Unload();
        return false;
    }
    return true;
}

// 清空所有 FMOD 函数指针并释放动态库。
// 播放器必须先停止 Channel、释放 Sound/System，防止卸载后仍调用 FMOD 代码。
void FmodApi::Unload() {
    if (module_ && owns_module_) {
        FreeLibrary(module_);
    }
    module_ = nullptr;
    owns_module_ = false;
    system_create_ = nullptr;
    system_init_ = nullptr;
    system_create_sound_ = nullptr;
    system_play_sound_ = nullptr;
    system_update_ = nullptr;
    system_close_ = nullptr;
    system_release_ = nullptr;
    sound_release_ = nullptr;
    sound_get_length_ = nullptr;
    channel_set_paused_ = nullptr;
    channel_get_paused_ = nullptr;
    channel_is_playing_ = nullptr;
    channel_get_position_ = nullptr;
    channel_set_volume_ = nullptr;
    channel_stop_ = nullptr;
    channel_set_callback_ = nullptr;
    channel_set_user_data_ = nullptr;
    channel_get_user_data_ = nullptr;
    error_string_ = nullptr;
}

// 调用 FMOD_System_Create 创建独立低层音频系统，并把句柄写入 system。
// 该方法只做动态导出转发；调用方负责检查返回码并在失败时释放后续资源。
FMOD_RESULT FmodApi::SystemCreate(FMOD_SYSTEM** system) const { return system_create_(system); }
// 调用 FMOD_System_Init 初始化音频系统，设置最大 Channel 数与初始化标志。
// extra_driver_data 原样传给 FMOD；成功后必须由 SystemClose/SystemRelease 成对清理。
FMOD_RESULT FmodApi::SystemInit(FMOD_SYSTEM* system, int max_channels, FMOD_INITFLAGS flags, void* extra_driver_data) const { return system_init_(system, max_channels, flags, extra_driver_data); }
// 调用 FMOD_System_CreateSound 从 UTF-8 文件路径创建流式 Sound。
// mode 与 exinfo 原样传递；成功得到的 Sound 由 LocalPlayer 负责在切歌或退出时释放。
FMOD_RESULT FmodApi::SystemCreateSound(FMOD_SYSTEM* system, const char* name, FMOD_MODE mode, void* exinfo, FMOD_SOUND** sound) const { return system_create_sound_(system, name, mode, exinfo, sound); }
// 调用 FMOD_System_PlaySound 为 Sound 创建 Channel，并支持以暂停状态启动。
// 播放器利用“先暂停创建、设置音量和 END 回调、再解除暂停”避免首帧音量突跳。
FMOD_RESULT FmodApi::SystemPlaySound(FMOD_SYSTEM* system, FMOD_SOUND* sound, FMOD_CHANNELGROUP* group, FMOD_BOOL paused, FMOD_CHANNEL** channel) const { return system_play_sound_(system, sound, group, paused, channel); }
// 调用 FMOD_System_Update 驱动低层混音、流读取与 Channel 回调派发。
// 该函数由插件工作线程周期调用，不能在 FMOD 回调线程中递归调用。
FMOD_RESULT FmodApi::SystemUpdate(FMOD_SYSTEM* system) const { return system_update_(system); }
// 调用 FMOD_System_Close 关闭系统内部设备和混音资源。
// 只在所有 Channel 与 Sound 已停止释放后调用；随后仍需 SystemRelease 释放对象本身。
FMOD_RESULT FmodApi::SystemClose(FMOD_SYSTEM* system) const { return system_close_(system); }
// 调用 FMOD_System_Release 释放由 SystemCreate 返回的系统对象。
// 调用后原指针立即失效，调用方必须把持有的 system_ 清空。
FMOD_RESULT FmodApi::SystemRelease(FMOD_SYSTEM* system) const { return system_release_(system); }
// 调用 FMOD_Sound_Release 释放曲目或预加载 Sound。
// 释放前必须确保没有仍在使用该 Sound 的活动 Channel。
FMOD_RESULT FmodApi::SoundRelease(FMOD_SOUND* sound) const { return sound_release_(sound); }
// 按指定 FMOD_TIMEUNIT 查询 Sound 长度，当前播放器使用毫秒单位。
// 该导出属于自动下一曲备用检测；缺失时返回非零错误码而不会解引用空函数指针。
FMOD_RESULT FmodApi::SoundGetLength(FMOD_SOUND* sound, unsigned int* length, FMOD_TIMEUNIT unit) const { return sound_get_length_ ? sound_get_length_(sound, length, unit) : -1; }
// 设置 Channel 暂停状态；非零表示暂停，零表示继续播放。
// 用于游戏 Pause/Unpause、创建新曲时的安全初始化以及恢复播放。
FMOD_RESULT FmodApi::ChannelSetPaused(FMOD_CHANNEL* channel, FMOD_BOOL paused) const { return channel_set_paused_(channel, paused); }
// 查询 Channel 当前是否暂停，并写入 paused。
// 返回 FMOD 错误码；调用方不能只依赖输出值判断成功。
FMOD_RESULT FmodApi::ChannelGetPaused(FMOD_CHANNEL* channel, FMOD_BOOL* paused) const { return channel_get_paused_(channel, paused); }
// 查询 Channel 是否仍处于播放生命周期中，并写入 playing。
// 该接口仅作为 END 回调不可用时的自动下一曲备用信号。
FMOD_RESULT FmodApi::ChannelIsPlaying(FMOD_CHANNEL* channel, FMOD_BOOL* playing) const { return channel_is_playing_(channel, playing); }
// 按指定时间单位读取 Channel 当前播放位置。
// 导出缺失或查询失败时返回错误码，播放器会继续使用曲长/时钟等其他备用判断。
FMOD_RESULT FmodApi::ChannelGetPosition(FMOD_CHANNEL* channel, unsigned int* position, FMOD_TIMEUNIT unit) const { return channel_get_position_ ? channel_get_position_(channel, position, unit) : -1; }
// 把 0.0–1.0 的最终音量写入当前 Channel。
// 最终值由“插件 Volume × 游戏持久 Spotify 音量”计算，临时 offset 不在此处叠加。
FMOD_RESULT FmodApi::ChannelSetVolume(FMOD_CHANNEL* channel, float volume) const { return channel_set_volume_(channel, volume); }
// 立即停止指定 Channel，使其不再读取或播放关联 Sound。
// 导出缺失时返回错误码；停止旧 Channel 后播放器才安全释放旧 Sound。
FMOD_RESULT FmodApi::ChannelStop(FMOD_CHANNEL* channel) const { return channel_stop_ ? channel_stop_(channel) : -1; }
// 为 Channel 注册或清除 FMOD ChannelControl 回调。
// 播放器只监听 END 事件，回调线程仅投递带播放代数的原子事件，不直接切歌。
FMOD_RESULT FmodApi::ChannelSetCallback(FMOD_CHANNEL* channel, FMOD_CHANNELCONTROL_CALLBACK callback) const { return channel_set_callback_ ? channel_set_callback_(channel, callback) : -1; }
// 把插件的播放代数上下文指针绑定到 Channel 用户数据。
// END 回调借此判断事件是否属于当前曲目，防止旧 Channel 的迟到回调误切歌。
FMOD_RESULT FmodApi::ChannelSetUserData(FMOD_CHANNEL* channel, void* user_data) const { return channel_set_user_data_ ? channel_set_user_data_(channel, user_data) : -1; }
// 读取 Channel 上绑定的插件用户数据。
// 只在 END 回调中使用；导出缺失时整套原生回调能力会被禁用并回退轮询。
FMOD_RESULT FmodApi::ChannelGetUserData(FMOD_CHANNEL* channel, void** user_data) const { return channel_get_user_data_ ? channel_get_user_data_(channel, user_data) : -1; }
// 返回原生 END 回调所需的三个可选导出是否全部可用。
// 只有回调注册、用户数据写入和读取同时存在时，播放器才启用回调主路径。
bool FmodApi::HasChannelEndCallbackSupport() const { return channel_set_callback_ && channel_set_user_data_ && channel_get_user_data_; }
// 把 FMOD_RESULT 转换为便于日志诊断的英文错误文本。
// 若 fmod64.dll 未导出 FMOD_ErrorString，则返回固定中文占位文本。
const char* FmodApi::ErrorString(FMOD_RESULT result) const { return error_string_ ? error_string_(result) : "Unknown FMOD error"; }

}  // namespace localmusic
