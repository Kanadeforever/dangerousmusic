// DPI 感知辅助模块：在不静态依赖新版 Windows SDK 类型的前提下，
// 尽早请求进程级 Per-Monitor V2，并为通知窗口线程设置独立 DPI 上下文。
//
// 这里使用 GetProcAddress 动态解析 API，目的是同时兼容：
// 1. 新版 Windows 10/11 SDK；
// 2. 仍可编译本项目的较旧 Windows SDK；
// 3. 不支持 Per-Monitor V2 的旧系统。
#pragma once

#include "Common.h"

namespace localmusic {

enum class ProcessDpiAwarenessResult {
    Disabled,
    EnabledPerMonitorV2,
    EnabledPerMonitor,
    EnabledSystemAware,
    AlreadyConfigured,
    Failed,
};

struct ProcessDpiAwarenessReport {
    ProcessDpiAwarenessResult result = ProcessDpiAwarenessResult::Disabled;
    DWORD error = ERROR_SUCCESS;
};

// 尽早为整个游戏进程请求高 DPI 感知。
// 若游戏 EXE 的 manifest 或其他模块已经设置 DPI 模式，Windows 会返回
// ERROR_ACCESS_DENIED；此时保持游戏原有设置，不视为致命错误。
ProcessDpiAwarenessReport EnableProcessHighDpiAwareness();

// 为当前线程请求 Per-Monitor V2。返回旧线程上下文；返回 nullptr 表示
// 当前系统不支持、参数无效或设置失败。调用方应在退出线程前恢复。
HANDLE EnableCurrentThreadPerMonitorV2();
// 恢复先前保存的线程 DPI 上下文。
// 空句柄时安全跳过。
void RestoreCurrentThreadDpiAwareness(HANDLE previous_context);

}  // namespace localmusic
