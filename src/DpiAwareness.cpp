// DPI 感知辅助实现。
#include "DpiAwareness.h"

namespace localmusic {
namespace {

// DPI_AWARENESS_CONTEXT 是一个伪句柄。为了避免要求特定版本的 Windows SDK
// 定义这些宏，这里使用微软文档规定的固定值动态调用 API。
HANDLE PerMonitorAwareV2Context() {
    return reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
}

using SetProcessDpiAwarenessContextFunction = BOOL(WINAPI*)(HANDLE);
using SetThreadDpiAwarenessContextFunction = HANDLE(WINAPI*)(HANDLE);
using SetProcessDpiAwarenessFunction = HRESULT(WINAPI*)(int);
using SetProcessDpiAwareFunction = BOOL(WINAPI*)();

}  // namespace

// 按 Per-Monitor V2、Per-Monitor、System Aware 的顺序请求进程 DPI 感知。
// 若游戏 manifest 已先设置模式则报告 AlreadyConfigured，不把正常拒绝误当成致命错误。
ProcessDpiAwarenessReport EnableProcessHighDpiAwareness() {
    ProcessDpiAwarenessReport report;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        const auto set_process_context = reinterpret_cast<SetProcessDpiAwarenessContextFunction>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_process_context) {
            SetLastError(ERROR_SUCCESS);
            if (set_process_context(PerMonitorAwareV2Context())) {
                report.result = ProcessDpiAwarenessResult::EnabledPerMonitorV2;
                return report;
            }

            report.error = GetLastError();
            // 进程默认 DPI 模式只能设置一次。游戏 manifest 已声明 DPI 模式时，
            // ERROR_ACCESS_DENIED 是预期结果，不能再用旧 API 强行覆盖。
            if (report.error == ERROR_ACCESS_DENIED) {
                report.result = ProcessDpiAwarenessResult::AlreadyConfigured;
                return report;
            }
        }
    }

    // Windows 8.1+ 回退：PROCESS_PER_MONITOR_DPI_AWARE = 2。
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        const auto set_process_awareness = reinterpret_cast<SetProcessDpiAwarenessFunction>(
            GetProcAddress(shcore, "SetProcessDpiAwareness"));
        if (set_process_awareness) {
            const HRESULT result = set_process_awareness(2);
            FreeLibrary(shcore);
            if (SUCCEEDED(result)) {
                report.result = ProcessDpiAwarenessResult::EnabledPerMonitor;
                report.error = ERROR_SUCCESS;
                return report;
            }
            if (result == E_ACCESSDENIED) {
                report.result = ProcessDpiAwarenessResult::AlreadyConfigured;
                report.error = ERROR_ACCESS_DENIED;
                return report;
            }
            report.error = static_cast<DWORD>(result);
        } else {
            FreeLibrary(shcore);
        }
    }

    // Vista+ 最后回退：系统 DPI 感知。该模式不如 Per-Monitor V2，
    // 但仍可关闭 DPI-unaware 进程的位图虚拟化。
    if (user32) {
        const auto set_process_aware = reinterpret_cast<SetProcessDpiAwareFunction>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (set_process_aware && set_process_aware()) {
            report.result = ProcessDpiAwarenessResult::EnabledSystemAware;
            report.error = ERROR_SUCCESS;
            return report;
        }
        if (report.error == ERROR_SUCCESS) {
            report.error = GetLastError();
        }
    }

    report.result = ProcessDpiAwarenessResult::Failed;
    return report;
}

// 只为通知窗口线程切换到 Per-Monitor V2 DPI 上下文。
// 返回旧上下文句柄，调用方必须在线程退出前恢复，避免影响同线程后续窗口。
HANDLE EnableCurrentThreadPerMonitorV2() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return nullptr;
    }
    const auto set_thread_context = reinterpret_cast<SetThreadDpiAwarenessContextFunction>(
        GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    if (!set_thread_context) {
        return nullptr;
    }
    return set_thread_context(PerMonitorAwareV2Context());
}

// 恢复 EnableCurrentThreadPerMonitorV2 返回的线程 DPI 上下文。
// 传入空句柄时安全跳过，兼容旧系统缺少相关 API 的情况。
void RestoreCurrentThreadDpiAwareness(HANDLE previous_context) {
    if (!previous_context) {
        return;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    const auto set_thread_context = reinterpret_cast<SetThreadDpiAwarenessContextFunction>(
        GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
    if (set_thread_context) {
        (void)set_thread_context(previous_context);
    }
}

}  // namespace localmusic
