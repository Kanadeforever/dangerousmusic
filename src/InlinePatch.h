// x64 内联补丁与跳板封装。补丁对象负责保存原字节，并在失败或卸载时恢复。
#pragma once
#include "Common.h"

namespace localmusic {

class AbsoluteJumpPatch {
public:
    // 创建未安装的绝对跳转补丁对象，不分配或修改任何内存。
    AbsoluteJumpPatch() = default;
    // 禁止复制补丁所有权，避免两个对象重复恢复同一目标字节。
    AbsoluteJumpPatch(const AbsoluteJumpPatch&) = delete;
    // 禁止复制赋值，确保目标地址和原字节始终只有一个所有者。
    AbsoluteJumpPatch& operator=(const AbsoluteJumpPatch&) = delete;
    // 析构时自动调用 Remove，保证异常路径也能恢复目标原字节。
    // 调用方仍应在已知安全线程阶段显式 Remove，避免析构时机不可控。
    ~AbsoluteJumpPatch();

    // 在目标入口写入 x64 绝对跳转并保存原始字节。
    // 写入前检查目标地址，使用 VirtualProtect/FlushInstructionCache 保证 CPU 看到新指令。
    bool Install(void* target, void* replacement);
    // 恢复 AbsoluteJumpPatch 保存的原始入口字节。
    // 重复调用是安全的，恢复后清空内部状态避免二次写入旧地址。
    void Remove();
    // 返回该补丁对象当前是否持有已安装目标。
    bool Installed() const { return installed_; }
    // 返回被修改的目标入口地址；未安装时为 nullptr。
    void* Target() const { return target_; }

private:
    static constexpr size_t PatchSize = 14;
    void* target_ = nullptr;
    std::array<uint8_t, PatchSize> original_{};
    bool installed_ = false;
};

class TrampolinePatch {
public:
    // 创建未安装的跳板补丁对象，不分配可执行内存。
    TrampolinePatch() = default;
    // 禁止复制跳板及其可执行内存所有权。
    TrampolinePatch(const TrampolinePatch&) = delete;
    // 禁止复制赋值，避免重复释放同一块跳板可执行内存。
    TrampolinePatch& operator=(const TrampolinePatch&) = delete;
    // 析构时恢复目标字节并释放跳板可执行内存。
    // 为避免仍有线程执行跳板，正常流程应在工作线程停止阶段显式 Remove。
    ~TrampolinePatch();

    // 复制目标前导指令到跳板，并把目标入口改写为 x64 绝对跳转。
    // copy_size 必须覆盖完整指令且至少能容纳补丁；成功后 Trampoline 可调用原逻辑。
    bool Install(void* target, void* replacement, size_t copy_size);
    // 恢复目标入口原字节并释放跳板内存。
    // 重复调用安全；恢复后 Installed、Target 与 Trampoline 都回到空状态。
    void Remove();
    // 返回跳板补丁当前是否安装成功并持有资源。
    bool Installed() const { return installed_; }
    // 返回被改写的原函数入口；未安装时为 nullptr。
    void* Target() const { return target_; }
    // 返回可调用原函数前导逻辑的跳板地址；未安装时为空。
    void* Trampoline() const { return trampoline_; }

private:
    static constexpr size_t PatchSize = 14;
    static constexpr size_t JumpSize = 14;
    void* target_ = nullptr;
    void* trampoline_ = nullptr;
    size_t copy_size_ = 0;
    std::array<uint8_t, PatchSize> original_{};
    bool installed_ = false;
};

}  // namespace localmusic
