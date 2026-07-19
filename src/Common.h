// 公共基础工具：路径、字符串编码、内存可读写性检查和通用标准库依赖。
// 本文件不包含游戏专属逻辑，供全部模块共享。
#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace localmusic {

// 取得指定模块的完整磁盘路径。
// 使用足够大的宽字符缓冲区，失败或被截断时返回空字符串。
inline std::wstring GetModulePath(HMODULE module) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::wstring(buffer.data(), length);
}

// 取得指定模块所在目录。
// 内部复用 GetModulePath，空路径时返回空 filesystem::path。
inline std::filesystem::path GetModuleDirectory(HMODULE module) {
    // 取得指定模块的完整磁盘路径。
    // 使用足够大的宽字符缓冲区，失败或被截断时返回空字符串。
    const std::wstring path = GetModulePath(module);
    return path.empty() ? std::filesystem::path{} : std::filesystem::path(path).parent_path();
}

// 把 Windows 宽字符串转换为 UTF-8。
// 两阶段查询长度并分配精确缓冲区，转换失败返回空字符串。
inline std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
    return result;
}

// 把 UTF-8 文本转换为 Windows 宽字符串。
// 非法 UTF-8 会回退当前 ANSI 代码页，尽量保留旧音乐标签。
inline std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        // 某些旧标签并不是合法 UTF-8。与其显示空标题，不如回退到
        // 当前 Windows ANSI 代码页进行兼容解码。
        const int ansi_required = MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (ansi_required <= 0) {
            return {};
        }
        std::wstring fallback(static_cast<size_t>(ansi_required), L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), fallback.data(), ansi_required);
        return fallback;
    }
    std::wstring result(static_cast<size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), required);
    return result;
}

// 移除宽字符串首尾空白和空字符。
// 全空文本返回空字符串，不修改中间内容。
inline std::wstring Trim(std::wstring value) {
    const auto not_space = [](wchar_t c) { return !iswspace(c) && c != L'\0'; };
    const auto first = std::find_if(value.begin(), value.end(), not_space);
    const auto last = std::find_if(value.rbegin(), value.rend(), not_space).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

// 按宽字符规则把字符串转为小写。
// 用于大小写不敏感的配置键值和扩展名比较。
inline std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

// 验证一段进程内地址当前是否已提交且可读。
// 同时检查区间溢出和跨越 VirtualQuery 区域，供 Hook 解引用前防护。
inline bool IsReadableAddress(const void* address, size_t size = 1) {
    if (!address || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    const DWORD protect = mbi.Protect & 0xFFU;
    if (protect == PAGE_NOACCESS) {
        return false;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + size;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= begin && end <= region_end;
}

// 验证一段进程内地址当前是否具有可写保护。
// 仅接受明确可写页面，拒绝 Guard/NoAccess 和跨区域范围。
inline bool IsWritableAddress(void* address, size_t size = 1) {
    if (!address || size == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) {
        return false;
    }
    const DWORD protect = mbi.Protect & 0xFFU;
    const bool writable = protect == PAGE_READWRITE || protect == PAGE_WRITECOPY ||
                          protect == PAGE_EXECUTE_READWRITE || protect == PAGE_EXECUTE_WRITECOPY;
    if (!writable) {
        return false;
    }
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = begin + size;
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= begin && end <= region_end;
}

}  // namespace localmusic
