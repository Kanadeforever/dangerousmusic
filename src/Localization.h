// 运行时国际化接口：从与 DLL 同名派生的 UTF-8 INI 文件加载文本。
//
// 命名规则：<DLL主文件名>.<language-tag>.ini，例如 dsound.en-us.ini。
// 内置简体中文始终作为最终回退；官方包同时提供 zh-hans 与 en-us，其他
// BCP 47 风格语言标签可由用户自行添加，不需要重新编译插件。
#pragma once
#include "Common.h"

#include <initializer_list>

namespace localmusic::loc {

struct StartupDiagnostic {
    bool warning = false;
    std::wstring message;
};

// 在日志、配置和通知模块启动前初始化语言层。
// Language=auto 时中文 Windows 使用 zh-hans，其他系统使用 en-us。
bool Initialize(const std::filesystem::path& dll_path,
                const std::filesystem::path& main_ini_path);

// 返回最终生效的规范化小写语言标签。
std::wstring ActiveLanguage();
// 返回用户配置中请求的语言值；未指定时为 auto。
std::wstring RequestedLanguage();
// 返回主语言文件路径。使用纯内置回退时仍返回按命名规则推导的候选路径。
std::filesystem::path ActiveLanguageFile();

// 按“当前语言 -> en-us 外部文件 -> 内置 zh-hans”顺序读取文本。
// 未知键返回键名本身，避免错误翻译项静默变成空白。
std::wstring Text(std::wstring_view key);

// 替换文本中的 {0}、{1}……占位符。多余参数被忽略，缺失参数保留原占位符。
std::wstring Format(std::wstring_view key,
                    std::initializer_list<std::wstring> arguments);

// 返回初始化期间记录的语言回退/文件状态诊断。调用方可在 Logger 初始化后写入。
std::vector<StartupDiagnostic> StartupDiagnostics();

// 生成首次运行的主配置模板。注释使用当前语言，键名和值保持稳定英文。
// 返回 UTF-8 BOM + CRLF 文本，可直接写入与 DLL 同名的主 INI。
std::string BuildDefaultConfigText(const std::wstring& dll_file_name);

}  // namespace localmusic::loc
