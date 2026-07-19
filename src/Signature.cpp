// 字节特征码扫描实现。仅在指定 PE 节区中工作，并对相对跳转目标执行范围验证。
#include "Signature.h"

namespace localmusic {

// 把“AA BB ?? CC”文本解析为字节数组，-1 表示通配符。
// 遇到非法十六进制或空模式返回空 optional，阻止不完整特征码参与扫描。
std::optional<BytePattern> BytePattern::Parse(std::wstring_view text) {
    BytePattern result;
    std::wstringstream stream{std::wstring(text)};
    std::wstring token;
    while (stream >> token) {
        if (token == L"?" || token == L"??") {
            result.bytes.push_back(-1);
            continue;
        }
        try {
            size_t consumed = 0;
            const unsigned long value = std::stoul(token, &consumed, 16);
            if (consumed != token.size() || value > 0xFFUL) {
                return std::nullopt;
            }
            result.bytes.push_back(static_cast<int>(value));
        } catch (...) {
            return std::nullopt;
        }
    }
    if (result.bytes.empty()) {
        return std::nullopt;
    }
    return result;
}

// 在 PE 节中搜索已解析字节特征码。
// 可限制到可执行节，匹配仍需由调用方进行函数结构和范围复核。
void* FindPattern(const PEImage& image, const BytePattern& pattern, bool executable_only) {
    if (!image.Valid() || pattern.Empty()) {
        return nullptr;
    }
    for (const auto& section : image.Sections()) {
        if (!section.Readable() || (executable_only && !section.Executable()) || section.size < pattern.bytes.size()) {
            continue;
        }
        const size_t limit = section.size - pattern.bytes.size();
        for (size_t offset = 0; offset <= limit; ++offset) {
            bool matches = true;
            for (size_t i = 0; i < pattern.bytes.size(); ++i) {
                if (pattern.bytes[i] >= 0 && section.begin[offset + i] != static_cast<uint8_t>(pattern.bytes[i])) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return section.begin + offset;
            }
        }
    }
    return nullptr;
}

// 解析 x64 call/jmp 等指令中的 32 位相对位移并计算绝对目标。
// 同时验证指令与目标均在映像内，未知 EXE 不会盲目跟随越界地址。
void* ResolveRelativeTarget(const PEImage& image, const uint8_t* instruction, size_t displacement_offset, size_t instruction_size) {
    if (!instruction || !image.IsExecutable(instruction, instruction_size) || displacement_offset + sizeof(int32_t) > instruction_size) {
        return nullptr;
    }
    int32_t displacement = 0;
    std::memcpy(&displacement, instruction + displacement_offset, sizeof(displacement));
    uint8_t* target = const_cast<uint8_t*>(instruction) + instruction_size + displacement;
    return image.IsExecutable(target) ? target : nullptr;
}

}  // namespace localmusic
