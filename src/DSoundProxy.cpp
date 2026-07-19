// 系统 DSOUND 代理实现：加载 System32 中的真实 dsound.dll 并转发游戏所需导出。
#include "DSoundProxy.h"

// WIN32_LEAN_AND_MEAN 会省略 dsound.h 依赖的多媒体格式声明，
// 其中包括 WAVEFORMATEX，因此必须先包含 mmreg.h。
#include <mmreg.h>
#include <dsound.h>
#include <objbase.h>

namespace localmusic::dsound_proxy {

HMODULE g_system_dsound = nullptr;
std::once_flag g_load_once;
bool g_loaded = false;

using FnDirectSoundCreate = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND*, LPUNKNOWN);
using FnDirectSoundCreate8 = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN);
using FnDirectSoundEnumerateA = HRESULT(WINAPI*)(LPDSENUMCALLBACKA, LPVOID);
using FnDirectSoundEnumerateW = HRESULT(WINAPI*)(LPDSENUMCALLBACKW, LPVOID);
using FnDirectSoundCaptureCreate = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUNDCAPTURE*, LPUNKNOWN);
using FnDirectSoundCaptureCreate8 = HRESULT(WINAPI*)(LPCGUID, LPDIRECTSOUNDCAPTURE8*, LPUNKNOWN);
using FnDirectSoundCaptureEnumerateA = HRESULT(WINAPI*)(LPDSENUMCALLBACKA, LPVOID);
using FnDirectSoundCaptureEnumerateW = HRESULT(WINAPI*)(LPDSENUMCALLBACKW, LPVOID);
using FnDirectSoundFullDuplexCreate = HRESULT(WINAPI*)(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC,
                                                        HWND, DWORD, LPDIRECTSOUNDFULLDUPLEX*,
                                                        LPDIRECTSOUNDCAPTUREBUFFER8*, LPDIRECTSOUNDBUFFER8*, LPUNKNOWN);
using FnGetDeviceID = HRESULT(WINAPI*)(LPCGUID, LPGUID);
using FnDllCanUnloadNow = HRESULT(WINAPI*)();
using FnDllGetClassObject = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

FnDirectSoundCreate pDirectSoundCreate = nullptr;
FnDirectSoundCreate8 pDirectSoundCreate8 = nullptr;
FnDirectSoundEnumerateA pDirectSoundEnumerateA = nullptr;
FnDirectSoundEnumerateW pDirectSoundEnumerateW = nullptr;
FnDirectSoundCaptureCreate pDirectSoundCaptureCreate = nullptr;
FnDirectSoundCaptureCreate8 pDirectSoundCaptureCreate8 = nullptr;
FnDirectSoundCaptureEnumerateA pDirectSoundCaptureEnumerateA = nullptr;
FnDirectSoundCaptureEnumerateW pDirectSoundCaptureEnumerateW = nullptr;
FnDirectSoundFullDuplexCreate pDirectSoundFullDuplexCreate = nullptr;
FnGetDeviceID pGetDeviceID = nullptr;
FnDllCanUnloadNow pDllCanUnloadNow = nullptr;
FnDllGetClassObject pDllGetClassObject = nullptr;

template <typename T>
// 从系统 dsound.dll 解析一个导出函数并写入对应函数指针。
// 解析失败返回 false，由 LoadOnce 统一记录和决定代理是否可用。
bool Resolve(T& output, const char* name) {
    output = reinterpret_cast<T>(GetProcAddress(g_system_dsound, name));
    return output != nullptr;
}

// 定位 System32 中的真实 dsound.dll，并一次性解析本代理需要转发的全部导出。
// 使用 call_once 保证多线程首次调用时只加载一次，不会重复覆盖函数指针。
void LoadOnce() {
    std::array<wchar_t, MAX_PATH> system_directory{};
    const UINT length = GetSystemDirectoryW(system_directory.data(), static_cast<UINT>(system_directory.size()));
    if (length == 0 || length >= system_directory.size()) {
        return;
    }
    std::filesystem::path path(system_directory.data());
    path /= L"dsound.dll";
    g_system_dsound = LoadLibraryW(path.c_str());
    if (!g_system_dsound) {
        return;
    }

    g_loaded = Resolve(pDirectSoundCreate, "DirectSoundCreate") &&
               Resolve(pDirectSoundCreate8, "DirectSoundCreate8") &&
               Resolve(pDirectSoundEnumerateA, "DirectSoundEnumerateA") &&
               Resolve(pDirectSoundEnumerateW, "DirectSoundEnumerateW") &&
               Resolve(pDirectSoundCaptureCreate, "DirectSoundCaptureCreate") &&
               Resolve(pDirectSoundCaptureCreate8, "DirectSoundCaptureCreate8") &&
               Resolve(pDirectSoundCaptureEnumerateA, "DirectSoundCaptureEnumerateA") &&
               Resolve(pDirectSoundCaptureEnumerateW, "DirectSoundCaptureEnumerateW") &&
               Resolve(pDirectSoundFullDuplexCreate, "DirectSoundFullDuplexCreate") &&
               Resolve(pGetDeviceID, "GetDeviceID") &&
               Resolve(pDllCanUnloadNow, "DllCanUnloadNow") &&
               Resolve(pDllGetClassObject, "DllGetClassObject");
    if (!g_loaded) {
        FreeLibrary(g_system_dsound);
        g_system_dsound = nullptr;
    }
}

// 触发一次性系统 DirectSound 加载，并返回代理是否已准备完成。
// 所有导出转发前都调用它，失败时返回标准错误而不是跳到空指针。
bool EnsureLoaded() {
    std::call_once(g_load_once, LoadOnce);
    return g_loaded;
}

// 释放真实 dsound.dll 句柄并清空所有导出函数指针。
// 仅在安全退出阶段使用，避免仍有游戏线程执行 DirectSound 调用时卸载。
void Unload() {
    // 故意保留真实 DSOUND 模块直到进程结束。引擎关闭阶段仍可能很晚调用导出函数，
    // 若提前卸载系统 DSOUND，后续转发调用会跳入无效地址。
}

}  // namespace localmusic::dsound_proxy

extern "C" {

// 转发系统 DirectSound 导出 DirectSoundCreate，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCreate(LPCGUID guid, LPDIRECTSOUND* output, LPUNKNOWN outer) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCreate(guid, output, outer) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundCreate8，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCreate8(LPCGUID guid, LPDIRECTSOUND8* output, LPUNKNOWN outer) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCreate8(guid, output, outer) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundEnumerateA，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA callback, LPVOID context) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundEnumerateA(callback, context) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundEnumerateW，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW callback, LPVOID context) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundEnumerateW(callback, context) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundCaptureCreate，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID guid, LPDIRECTSOUNDCAPTURE* output, LPUNKNOWN outer) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCaptureCreate(guid, output, outer) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundCaptureCreate8，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID guid, LPDIRECTSOUNDCAPTURE8* output, LPUNKNOWN outer) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCaptureCreate8(guid, output, outer) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundCaptureEnumerateA，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA callback, LPVOID context) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCaptureEnumerateA(callback, context) : E_FAIL;
}
// 转发系统 DirectSound 导出 DirectSoundCaptureEnumerateW，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW callback, LPVOID context) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDirectSoundCaptureEnumerateW(callback, context) : E_FAIL;
}
// 转发系统 DirectSoundFullDuplexCreate，并原样传递捕获/播放设备、缓冲区描述和协作级别。
// 该导出参数较多，必须保持 WINAPI 调用约定和参数顺序；真实 dsound.dll 加载失败时返回 E_FAIL。
HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID capture_guid, LPCGUID render_guid, LPCDSCBUFFERDESC capture_desc,
                                           LPCDSBUFFERDESC render_desc, HWND window, DWORD level,
                                           LPDIRECTSOUNDFULLDUPLEX* full_duplex,
                                           LPDIRECTSOUNDCAPTUREBUFFER8* capture_buffer,
                                           LPDIRECTSOUNDBUFFER8* render_buffer, LPUNKNOWN outer) {
    return localmusic::dsound_proxy::EnsureLoaded()
               ? localmusic::dsound_proxy::pDirectSoundFullDuplexCreate(capture_guid, render_guid, capture_desc, render_desc,
                                                                   window, level, full_duplex, capture_buffer,
                                                                   render_buffer, outer)
               : E_FAIL;
}
// 转发系统 DirectSound 导出 GetDeviceID，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI GetDeviceID(LPCGUID source, LPGUID destination) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pGetDeviceID(source, destination) : E_FAIL;
}
// 转发系统 DirectSound 导出 DllCanUnloadNow，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DllCanUnloadNow() {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDllCanUnloadNow() : S_FALSE;
}
// 转发系统 DirectSound 导出 DllGetClassObject，保持 dsound.dll 代理与游戏调用约定一致。
// 真实导出不可用时返回合适的失败 HRESULT，绝不调用空函数指针。
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* output) {
    return localmusic::dsound_proxy::EnsureLoaded() ? localmusic::dsound_proxy::pDllGetClassObject(clsid, iid, output)
                                               : CLASS_E_CLASSNOTAVAILABLE;
}

}  // extern "C"
