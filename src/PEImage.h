// 进程内 PE 映像视图：提供节区、RVA、字符串和指针引用查询。
#pragma once
#include "Common.h"

namespace localmusic {

struct PESectionView {
    std::string name;
    uint8_t* begin = nullptr;
    size_t size = 0;
    DWORD characteristics = 0;

    // 返回该 PE 节是否具有执行属性。
    // 用于限制函数和特征码候选。
    bool Executable() const { return (characteristics & IMAGE_SCN_MEM_EXECUTE) != 0; }
    // 返回该 PE 节是否具有读取属性。
    // 用于字符串、指针和对象字段扫描。
    bool Readable() const { return (characteristics & IMAGE_SCN_MEM_READ) != 0; }
    // 返回该 PE 节是否具有写入属性。
    // 仅作视图信息，不直接修改内存保护。
    bool Writable() const { return (characteristics & IMAGE_SCN_MEM_WRITE) != 0; }
    // 判断地址范围是否完整位于当前节内。
    // 包含整数溢出防护。
    bool Contains(const void* address, size_t length = 1) const;
};

class PEImage {
public:
    // 解析指定已加载模块的 PE 头和节表。
    // module=nullptr 时使用主 EXE。
    explicit PEImage(HMODULE module = nullptr);

    // 返回 PE 解析是否成功。
    // 失败时其他查询应安全返回空结果。
    bool Valid() const { return valid_; }
    // 返回映像基址。
    // 仅在 Valid=true 时有意义。
    uint8_t* Base() const { return base_; }
    // 返回 PE OptionalHeader 中的映像大小。
    // 用于版本验证和边界检查。
    size_t SizeOfImage() const { return size_of_image_; }
    // 返回 PE 文件头时间戳。
    // 用于快速匹配兼容版本。
    uint32_t Timestamp() const { return timestamp_; }
    // 返回解析后的节视图列表。
    // 结果在 PEImage 生命周期内保持有效。
    const std::vector<PESectionView>& Sections() const { return sections_; }

    // 判断地址范围是否位于整个映像内。
    // 所有 RVA/指针转换的基础检查。
    bool IsInside(const void* address, size_t length = 1) const;
    // 判断地址范围是否位于可执行节。
    // Hook 目标必须通过此检查。
    bool IsExecutable(const void* address, size_t length = 1) const;
    // 判断地址范围是否位于可读节。
    // 避免扫描或解引用不可读内存。
    bool IsReadable(const void* address, size_t length = 1) const;
    // 把进程地址转换为相对虚拟地址。
    // 越界地址返回零。
    uintptr_t ToRva(const void* address) const;
    // 把 RVA 转换为映像内指针。
    // 越界 RVA 返回空。
    void* FromRva(uintptr_t rva) const;

    // 搜索可读节中的 ASCII 文本。
    // 可要求文本后为零终止符。
    std::vector<uint8_t*> FindAscii(std::string_view text, bool exact_c_string = true) const;
    // 搜索所有指向目标地址的原生指针槽。
    // 返回候选后由上层继续验证结构。
    std::vector<uint8_t*> FindPointerReferences(const void* target) const;

private:
    uint8_t* base_ = nullptr;
    size_t size_of_image_ = 0;
    uint32_t timestamp_ = 0;
    bool valid_ = false;
    std::vector<PESectionView> sections_;
};

// 计算磁盘文件 SHA-256。
// 用于可选严格版本诊断。
std::wstring ComputeFileSha256(const std::filesystem::path& path);

}  // namespace localmusic
