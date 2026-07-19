// x64 内联补丁实现。所有写入都先修改页面保护，完成后刷新指令缓存。
#include "InlinePatch.h"
#include "Logger.h"
#include "Localization.h"

namespace localmusic {

// 析构时恢复 AbsoluteJumpPatch 改写的目标字节。
// Remove 可重复调用，因此显式卸载后进入析构也不会发生二次恢复。
AbsoluteJumpPatch::~AbsoluteJumpPatch() {
    Remove();
}

// 在目标入口写入 x64 绝对跳转并保存原始字节。
// 写入前检查目标地址，使用 VirtualProtect/FlushInstructionCache 保证 CPU 看到新指令。
bool AbsoluteJumpPatch::Install(void* target, void* replacement) {
    if (installed_ || !target || !replacement || !IsReadableAddress(target, PatchSize)) {
        return false;
    }

    std::array<uint8_t, PatchSize> patch{};
    patch[0] = 0xFF;
    patch[1] = 0x25;
    // FF 25 00000000 表示：jmp qword ptr [rip+0]，后面紧跟 64 位绝对目标地址。
    const uint64_t destination = reinterpret_cast<uint64_t>(replacement);
    std::memcpy(patch.data() + 6, &destination, sizeof(destination));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, PatchSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    std::memcpy(original_.data(), target, PatchSize);
    std::memcpy(target, patch.data(), PatchSize);
    FlushInstructionCache(GetCurrentProcess(), target, PatchSize);
    DWORD ignored = 0;
    VirtualProtect(target, PatchSize, old_protect, &ignored);

    target_ = target;
    installed_ = true;
    return true;
}

// 恢复 AbsoluteJumpPatch 保存的原始入口字节。
// 重复调用是安全的，恢复后清空内部状态避免二次写入旧地址。
void AbsoluteJumpPatch::Remove() {
    if (!installed_ || !target_) {
        return;
    }
    DWORD old_protect = 0;
    if (VirtualProtect(target_, PatchSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
        std::memcpy(target_, original_.data(), PatchSize);
        FlushInstructionCache(GetCurrentProcess(), target_, PatchSize);
        DWORD ignored = 0;
        VirtualProtect(target_, PatchSize, old_protect, &ignored);
    } else {
        log::Warn(loc::Text(L"Log.Patch.RestoreInlineFailed"));
    }
    target_ = nullptr;
    installed_ = false;
}


// 析构时恢复目标入口并释放跳板可执行内存。
// 正常流程应先停止可能执行跳板的线程，再显式 Remove；析构只提供最后的资源安全网。
TrampolinePatch::~TrampolinePatch() {
    Remove();
}

// 复制目标前导指令到可执行跳板，再把目标入口跳转到替换函数。
// 跳板尾部跳回原函数剩余部分，使 Hook 可以选择调用原始逻辑。
bool TrampolinePatch::Install(void* target, void* replacement, size_t copy_size) {
    if (installed_ || !target || !replacement || copy_size < PatchSize || !IsReadableAddress(target, copy_size)) {
        return false;
    }

    const size_t trampoline_size = copy_size + JumpSize;
    auto* trampoline = static_cast<uint8_t*>(VirtualAlloc(nullptr, trampoline_size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) {
        return false;
    }
    std::memcpy(trampoline, target, copy_size);
    trampoline[copy_size + 0] = 0xFF;
    trampoline[copy_size + 1] = 0x25;
    const uint64_t resume = reinterpret_cast<uint64_t>(static_cast<uint8_t*>(target) + copy_size);
    std::memcpy(trampoline + copy_size + 6, &resume, sizeof(resume));

    std::array<uint8_t, PatchSize> patch{};
    patch[0] = 0xFF;
    patch[1] = 0x25;
    const uint64_t destination = reinterpret_cast<uint64_t>(replacement);
    std::memcpy(patch.data() + 6, &destination, sizeof(destination));

    DWORD old_protect = 0;
    if (!VirtualProtect(target, PatchSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    std::memcpy(original_.data(), target, PatchSize);
    std::memcpy(target, patch.data(), PatchSize);
    FlushInstructionCache(GetCurrentProcess(), target, PatchSize);
    FlushInstructionCache(GetCurrentProcess(), trampoline, trampoline_size);
    DWORD ignored = 0;
    VirtualProtect(target, PatchSize, old_protect, &ignored);

    target_ = target;
    trampoline_ = trampoline;
    copy_size_ = copy_size;
    installed_ = true;
    return true;
}

// 恢复 TrampolinePatch 的原始字节并释放跳板内存。
// 只有所有可能调用跳板的线程都停止后才能执行，当前由工作线程退出阶段负责。
void TrampolinePatch::Remove() {
    if (installed_ && target_) {
        DWORD old_protect = 0;
        if (VirtualProtect(target_, PatchSize, PAGE_EXECUTE_READWRITE, &old_protect)) {
            std::memcpy(target_, original_.data(), PatchSize);
            FlushInstructionCache(GetCurrentProcess(), target_, PatchSize);
            DWORD ignored = 0;
            VirtualProtect(target_, PatchSize, old_protect, &ignored);
        } else {
            log::Warn(loc::Text(L"Log.Patch.RestoreTrampolineFailed"));
        }
    }
    if (trampoline_) {
        VirtualFree(trampoline_, 0, MEM_RELEASE);
    }
    target_ = nullptr;
    trampoline_ = nullptr;
    copy_size_ = 0;
    installed_ = false;
}

}  // namespace localmusic
