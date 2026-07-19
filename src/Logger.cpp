// 日志实现：启用时写入带 BOM 的 UTF-8 文本，并使用互斥锁保证多线程行完整性。
// 若调用方没有执行 Initialize，g_path 保持为空，所有 Write 调用都会无副作用返回。
#include "Logger.h"

#include "Localization.h"

namespace localmusic::log {
namespace {
std::mutex g_mutex;
std::filesystem::path g_path;

// 移除日志消息中的换行和控制字符，把一次调用限制为一条物理日志行。
// 这样外部文本或元数据不能伪造时间戳、级别或破坏后续日志解析。
std::wstring SanitizeLine(std::wstring value) {
    for (wchar_t& ch : value) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') {
            ch = L' ';
            continue;
        }
        if ((ch >= 0 && ch < 0x20) || ch == 0x7F) {
            ch = L' ';
        }
    }
    return Trim(std::move(value));
}

// 返回当前语言中的日志级别文本。
std::wstring LevelText(Level level) {
    switch (level) {
        case Level::Warning: return loc::Text(L"Logger.Warning");
        case Level::Error: return loc::Text(L"Logger.Error");
        case Level::Info:
        default: return loc::Text(L"Logger.Info");
    }
}

// 生成本地时间与本地化日志级别前缀，并编码为 UTF-8。
// 时间格式固定，便于用户按顺序比对游戏操作与 Hook/播放事件。
std::string TimestampPrefix(Level level) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream stream;
    stream << L'[' << std::setfill(L'0')
           << std::setw(2) << now.wHour << L':'
           << std::setw(2) << now.wMinute << L':'
           << std::setw(2) << now.wSecond << L"] ["
           << LevelText(level) << L"] ";
    return WideToUtf8(stream.str());
}
}

// 设置日志路径并以 UTF-8 BOM/本地化标题初始化本次运行日志。
// 仅在 EnableLogging=true 时调用；关闭日志时本函数完全不会执行。
void Initialize(const std::filesystem::path& path) {
    std::lock_guard lock(g_mutex);
    g_path = path;
    std::ofstream file(g_path, std::ios::binary | std::ios::trunc);
    if (file) {
        // 写入 UTF-8 BOM，避免记事本等 Windows 工具把中日韩/Unicode
        // 曲目信息误判为当前 ANSI 代码页。
        file << "\xEF\xBB\xBF";
        file << WideToUtf8(loc::Text(L"Logger.Title")) << "\n";
        file.flush();
    }
}

// 线程安全地追加一条 UTF-8 日志。
// 未初始化路径为空时立即返回，使所有调用点无需重复判断日志开关。
void Write(Level level, const std::wstring& message) {
    std::lock_guard lock(g_mutex);
    if (g_path.empty()) return;

    std::ofstream file(g_path, std::ios::binary | std::ios::app);
    if (!file) return;

    file << TimestampPrefix(level) << WideToUtf8(SanitizeLine(message)) << "\n";
    file.flush();
}

}  // namespace localmusic::log
