// UTF-8 日志接口。仅在 EnableLogging=true 时初始化；关闭后不会触碰日志文件。
#pragma once
#include "Common.h"

namespace localmusic::log {

enum class Level {
    Info,
    Warning,
    Error,
};

// 设置并初始化本次运行的 UTF-8 日志文件。
// 只有 EnableLogging=true 时调用；标题使用当前运行语言。
void Initialize(const std::filesystem::path& path);
// 线程安全地追加一条指定级别日志。
// 日志未初始化时静默返回。
void Write(Level level, const std::wstring& message);

// 以“信息”级别写日志。
inline void Info(const std::wstring& message) { Write(Level::Info, message); }
// 以“警告”级别写日志，用于可恢复降级和配置异常。
inline void Warn(const std::wstring& message) { Write(Level::Warning, message); }
// 以“错误”级别写日志，用于核心操作失败但插件仍尽量安全退出或降级。
inline void Error(const std::wstring& message) { Write(Level::Error, message); }

}  // namespace localmusic::log
