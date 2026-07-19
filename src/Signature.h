// 字节特征码解析与扫描工具，支持 ?? 通配符和相对地址解析。
#pragma once
#include "PEImage.h"

namespace localmusic {

struct BytePattern {
    std::vector<int> bytes;  // -1 表示通配字节。

    // 判断特征码是否没有任何字节。
    // 空特征码不能参与扫描。
    bool Empty() const { return bytes.empty(); }
    // 解析支持 ?? 通配符的十六进制特征码文本。
    // 非法格式返回空 optional。
    static std::optional<BytePattern> Parse(std::wstring_view text);
};

// 在 PE 映像中搜索字节特征码。
// 默认只扫描可执行节。
void* FindPattern(const PEImage& image, const BytePattern& pattern, bool executable_only = true);
// 解析相对 call/jmp 指令的绝对目标。
// 指令和目标都必须位于映像内。
void* ResolveRelativeTarget(const PEImage& image, const uint8_t* instruction, size_t displacement_offset, size_t instruction_size);

}  // namespace localmusic
