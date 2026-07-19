// PE 映像解析实现：只在主模块映像范围和合法节区内查找，避免越界地址。
#include "PEImage.h"

#include <bcrypt.h>

namespace localmusic {

// 判断地址区间是否完整落在当前 PE 节视图中。
// 包含溢出检查，避免 address+length 回绕导致错误通过。
bool PESectionView::Contains(const void* address, size_t length) const {
    if (!address || length == 0 || !begin) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t section_start = reinterpret_cast<uintptr_t>(begin);
    const uintptr_t end = start + length;
    const uintptr_t section_end = section_start + size;
    return end >= start && start >= section_start && end <= section_end;
}

// 解析已加载模块的 DOS/NT 头与节表，建立只读的进程内 PE 视图。
// 任何头部验证失败都会保持 Valid=false，后续扫描函数据此安全退出。
PEImage::PEImage(HMODULE module) {
    base_ = reinterpret_cast<uint8_t*>(module ? module : GetModuleHandleW(nullptr));
    if (!base_ || !IsReadableAddress(base_, sizeof(IMAGE_DOS_HEADER))) {
        return;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base_ + dos->e_lfanew);
    if (!IsReadableAddress(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return;
    }

    size_of_image_ = nt->OptionalHeader.SizeOfImage;
    timestamp_ = nt->FileHeader.TimeDateStamp;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        char name_buffer[9]{};
        std::memcpy(name_buffer, section[i].Name, 8);
        const size_t requested_size = section[i].Misc.VirtualSize != 0
                                          ? static_cast<size_t>(section[i].Misc.VirtualSize)
                                          : static_cast<size_t>(section[i].SizeOfRawData);
        const size_t available = section[i].VirtualAddress < size_of_image_
                                     ? size_of_image_ - section[i].VirtualAddress
                                     : 0;
        const size_t virtual_size = std::min(requested_size, available);
        if (virtual_size == 0) {
            continue;
        }
        sections_.push_back(PESectionView{
            name_buffer,
            base_ + section[i].VirtualAddress,
            virtual_size,
            section[i].Characteristics,
        });
    }
    valid_ = size_of_image_ != 0 && !sections_.empty();
}

// 判断地址范围是否位于整个映像边界内。
// 用于所有指针解引用前的第一层防护。
bool PEImage::IsInside(const void* address, size_t length) const {
    if (!valid_ || !address || length == 0) {
        return false;
    }
    const uintptr_t start = reinterpret_cast<uintptr_t>(address);
    const uintptr_t image_start = reinterpret_cast<uintptr_t>(base_);
    const uintptr_t end = start + length;
    const uintptr_t image_end = image_start + size_of_image_;
    return end >= start && start >= image_start && end <= image_end;
}

// 判断地址范围是否属于带执行权限的 PE 节。
// Hook 目标和相对调用解析必须通过此检查，避免给数据区打补丁。
bool PEImage::IsExecutable(const void* address, size_t length) const {
    return std::any_of(sections_.begin(), sections_.end(), [&](const PESectionView& section) {
        return section.Executable() && section.Contains(address, length);
    });
}

// 判断地址范围是否属于可读 PE 节。
// 字符串表、指针槽和 UE 对象字段扫描使用该检查。
bool PEImage::IsReadable(const void* address, size_t length) const {
    return std::any_of(sections_.begin(), sections_.end(), [&](const PESectionView& section) {
        return section.Readable() && section.Contains(address, length);
    });
}

// 把进程虚拟地址转换为模块 RVA。
// 地址不在映像内时返回零，日志不会输出无意义的巨大偏移。
uintptr_t PEImage::ToRva(const void* address) const {
    if (!IsInside(address)) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(base_);
}

// 把 RVA 转换为进程内指针并验证范围。
// 未知版本配置中的越界 RVA 会得到空指针而不是直接崩溃。
void* PEImage::FromRva(uintptr_t rva) const {
    if (!valid_ || rva >= size_of_image_) {
        return nullptr;
    }
    return base_ + rva;
}

// 在可读节中搜索 ASCII 文本，可要求后跟零终止符。
// 用于从 UE 函数注册表和字符串引用定位包装函数。
std::vector<uint8_t*> PEImage::FindAscii(std::string_view text, bool exact_c_string) const {
    std::vector<uint8_t*> matches;
    if (!valid_ || text.empty()) {
        return matches;
    }
    for (const auto& section : sections_) {
        if (!section.Readable() || section.Executable() || section.size < text.size()) {
            continue;
        }
        const uint8_t* first = section.begin;
        const uint8_t* last = section.begin + section.size - text.size() + 1;
        for (const uint8_t* cursor = first; cursor < last; ++cursor) {
            if (std::memcmp(cursor, text.data(), text.size()) != 0) {
                continue;
            }
            if (exact_c_string) {
                const bool has_suffix = cursor + text.size() >= section.begin + section.size || cursor[text.size()] != 0;
                if (has_suffix) {
                    continue;
                }
            }
            matches.push_back(const_cast<uint8_t*>(cursor));
        }
    }
    return matches;
}

// 扫描可读节中所有等于目标地址的原生指针槽。
// 返回候选集合后仍由上层验证周边结构，避免仅凭指针值安装 Hook。
std::vector<uint8_t*> PEImage::FindPointerReferences(const void* target) const {
    std::vector<uint8_t*> matches;
    const uintptr_t value = reinterpret_cast<uintptr_t>(target);
    for (const auto& section : sections_) {
        if (!section.Readable() || section.Executable() || section.size < sizeof(value)) {
            continue;
        }
        for (size_t offset = 0; offset + sizeof(value) <= section.size; offset += alignof(uintptr_t)) {
            uintptr_t candidate = 0;
            std::memcpy(&candidate, section.begin + offset, sizeof(candidate));
            if (candidate == value) {
                matches.push_back(section.begin + offset);
            }
        }
    }
    return matches;
}

// 使用 Windows CNG/Bcrypt 计算磁盘文件 SHA-256。
// 只在用户启用严格诊断时执行，避免每次启动都读取整个大型 EXE。
std::wstring ComputeFileSha256(const std::filesystem::path& path) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD received = 0;
    std::vector<uint8_t> object;
    std::vector<uint8_t> digest;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        return {};
    }
    const auto close_algorithm = [&] { BCryptCloseAlgorithmProvider(algorithm, 0); };
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &received, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &received, 0) < 0) {
        close_algorithm();
        return {};
    }
    object.resize(object_size);
    digest.resize(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) {
        close_algorithm();
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        BCryptDestroyHash(hash);
        close_algorithm();
        return {};
    }
    std::array<char, 1024 * 1024> buffer{};
    while (file) {
        file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = file.gcount();
        if (count > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(count), 0) < 0) {
            BCryptDestroyHash(hash);
            close_algorithm();
            return {};
        }
    }
    if (BCryptFinishHash(hash, digest.data(), hash_size, 0) < 0) {
        BCryptDestroyHash(hash);
        close_algorithm();
        return {};
    }
    BCryptDestroyHash(hash);
    close_algorithm();

    std::wostringstream stream;
    stream << std::hex << std::setfill(L'0');
    for (uint8_t byte : digest) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

}  // namespace localmusic
