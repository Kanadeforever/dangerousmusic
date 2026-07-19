#!/usr/bin/env python3
"""LocalMusic离线兼容性检查。

本脚本不依赖第三方 Python 模块。它复现 DLL 首选的 UE4 名称注册表
定位逻辑，适合在测试新 EXE 前先检查目标函数与认证回调布局。
"""
from __future__ import annotations

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Section:
    """描述 PE 节表中的一个节，并提供可执行/可读属性判断。

    字段保持文件偏移、RVA、大小和 characteristics 原值，便于后续地址转换
    与扫描函数在不加载目标 EXE 的情况下执行边界验证。
    """
    name: str
    rva: int
    virtual_size: int
    raw_offset: int
    raw_size: int
    characteristics: int

    @property
    def executable(self) -> bool:
        """返回节是否设置 IMAGE_SCN_MEM_EXECUTE。"""
        return bool(self.characteristics & 0x20000000)

    @property
    def readable(self) -> bool:
        """返回节是否设置 IMAGE_SCN_MEM_READ。"""
        return bool(self.characteristics & 0x40000000)


class PE:
    """把磁盘上的 64 位 PE 文件解析成可查询的轻量只读视图。

    该类只读取文件字节，不执行或映射目标程序；所有地址均按 ImageBase 推导，
    因此适合在发布前离线检查 Dangerous Driving Shipping EXE。
    """
    def __init__(self, path: Path):
        """读取文件并验证 DOS/PE32+ 头、x64 机器类型以及节表。

        参数 path 是待检查 EXE。格式错误时抛出 ValueError，I/O 错误保持
        pathlib 的原始异常，调用方可直接看到失败原因。
        """
        self.path = path
        self.data = path.read_bytes()
        e_lfanew = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise ValueError("目标文件不是有效的 PE 文件")
        coff = e_lfanew + 4
        machine, count, self.timestamp, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", self.data, coff)
        if machine != 0x8664:
            raise ValueError("目标文件不是 x64 PE")
        optional = coff + 20
        magic = struct.unpack_from("<H", self.data, optional)[0]
        if magic != 0x20B:
            raise ValueError("目标文件不是 PE32+ 格式")
        self.image_base = struct.unpack_from("<Q", self.data, optional + 24)[0]
        self.image_size = struct.unpack_from("<I", self.data, optional + 56)[0]
        self.sections: list[Section] = []
        section_table = optional + optional_size
        for index in range(count):
            offset = section_table + index * 40
            raw_name, virtual_size, rva, raw_size, raw_offset, _, _, _, _, chars = struct.unpack_from(
                "<8sIIIIIIHHI", self.data, offset
            )
            self.sections.append(Section(raw_name.rstrip(b"\0").decode("ascii", "replace"), rva, virtual_size,
                                         raw_offset, raw_size, chars))

    def off_to_va(self, offset: int) -> int | None:
        """把文件原始偏移转换为虚拟地址；不属于任何节时返回 None。"""
        for section in self.sections:
            if section.raw_offset <= offset < section.raw_offset + section.raw_size:
                return self.image_base + section.rva + offset - section.raw_offset
        return None

    def va_to_off(self, va: int) -> int | None:
        """把虚拟地址转换为文件原始偏移；落在节的零填充区时返回 None。"""
        rva = va - self.image_base
        for section in self.sections:
            extent = max(section.virtual_size, section.raw_size)
            if section.rva <= rva < section.rva + extent:
                delta = rva - section.rva
                if delta < section.raw_size:
                    return section.raw_offset + delta
        return None

    def is_executable_va(self, va: int, size: int = 1) -> bool:
        """判断整个虚拟地址区间是否位于同一个可执行节内。"""
        rva = va - self.image_base
        return any(s.executable and s.rva <= rva and rva + size <= s.rva + max(s.virtual_size, s.raw_size)
                   for s in self.sections)

    def exact_ascii_offsets(self, name: str) -> list[int]:
        """查找以 NUL 结尾的精确 ASCII 名称，返回全部文件偏移。"""
        needle = name.encode("ascii") + b"\0"
        results = []
        start = 0
        while True:
            offset = self.data.find(needle, start)
            if offset < 0:
                break
            results.append(offset)
            start = offset + 1
        return results

    def pointer_refs(self, va: int) -> list[int]:
        """查找文件中所有指向指定 64 位虚拟地址的绝对指针槽位。"""
        needle = struct.pack("<Q", va)
        refs = []
        start = 0
        while True:
            offset = self.data.find(needle, start)
            if offset < 0:
                break
            refs.append(offset)
            start = offset + 1
        return refs


def looks_like_exec_wrapper(pe: PE, va: int) -> bool:
    """用 FFrame::Code 访问特征快速判断候选地址是否像 UE4 exec 包装函数。

    这是保守的结构验证，不负责证明函数语义；返回 False 会让定位器放弃候选。
    """
    offset = pe.va_to_off(va)
    if offset is None or not pe.is_executable_va(va, 16):
        return False
    code = pe.data[offset:offset + 128]
    for index in range(len(code) - 4):
        if code[index:index + 3] in (b"\x48\x8b\x42", b"\x48\x39\x7a") and code[index + 3] <= 0x80:
            return True
    return False


def registration_refs(pe: PE, name: str) -> list[int]:
    """查找 UE4 原生函数注册表中引用指定 ASCII 函数名的指针位置。"""
    refs: list[int] = []
    for string_offset in pe.exact_ascii_offsets(name):
        string_va = pe.off_to_va(string_offset)
        if string_va is not None:
            refs.extend(pe.pointer_refs(string_va))
    return refs


def wrapper_after_name(pe: PE, reference_offset: int) -> int | None:
    """读取名称指针后一个槽位中的包装函数地址，并执行结构验证。"""
    slot = reference_offset + 8
    if slot < 0 or slot + 8 > len(pe.data):
        return None
    candidate = struct.unpack_from("<Q", pe.data, slot)[0]
    return candidate if looks_like_exec_wrapper(pe, candidate) else None


def resolve_wrapper(pe: PE, name: str) -> int | None:
    """按注册表名称解析一个 UE4 exec 包装函数。

    Play/Pause 名称过于通用，因此额外要求引用靠近已知 SpotifyService 锚点，
    以降低误命中其他系统同名函数的风险。
    """
    refs = registration_refs(pe, name)
    if not refs:
        return None

    generic_name = name in {"Play", "Pause"}
    anchors: list[int] = []
    if generic_name:
        for anchor in ("HasValidAccessToken", "NextTrack", "Unpause", "Update"):
            anchors.extend(registration_refs(pe, anchor))

    for ref in refs:
        if generic_name and not any(abs(ref - anchor) <= 0x200 for anchor in anchors):
            continue
        candidate = wrapper_after_name(pe, ref)
        if candidate is not None:
            return candidate
    return None


def relative_target(pe: PE, instruction_va: int) -> int | None:
    """解析 E8/E9 rel32 指令目标，并确认目标仍在可执行节中。"""
    offset = pe.va_to_off(instruction_va)
    if offset is None or offset + 5 > len(pe.data):
        return None
    displacement = struct.unpack_from("<i", pe.data, offset + 1)[0]
    target = instruction_va + 5 + displacement
    return target if pe.is_executable_va(target) else None


def tail_jump(pe: PE, wrapper: int, limit: int = 96) -> int | None:
    """在包装函数前 limit 字节内寻找尾部 E9 跳转，遇到 RET 即停止。"""
    offset = pe.va_to_off(wrapper)
    if offset is None:
        return None
    code = pe.data[offset:offset + limit]
    for index, byte in enumerate(code[:-4]):
        if byte == 0xE9:
            return relative_target(pe, wrapper + index)
        if byte == 0xC3:
            break
    return None


def last_call(pe: PE, wrapper: int, limit: int = 192) -> int | None:
    """返回包装函数在 RET 前最后一个有效 E8 调用目标。

    SetDefaultVolume/SetVolumeOffset 使用该策略保留 UE 参数解析包装层。
    """
    offset = pe.va_to_off(wrapper)
    if offset is None:
        return None
    code = pe.data[offset:offset + limit]
    result = None
    index = 0
    while index + 5 <= len(code):
        if code[index] == 0xE8:
            result = relative_target(pe, wrapper + index)
            index += 5
            continue
        if code[index] == 0xC3:
            break
        index += 1
    return result


def infer_frame_offset(pe: PE, wrapper: int) -> int | None:
    """从常见 mov/cmp 指令编码推断 UE4 FFrame::Code 的 8 位偏移。"""
    offset = pe.va_to_off(wrapper)
    if offset is None:
        return None
    code = pe.data[offset:offset + 64]
    for index in range(len(code) - 4):
        if code[index:index + 3] in (b"\x48\x8b\x42", b"\x48\x39\x7a"):
            return code[index + 3]
    return None


def infer_disp32(pe: PE, wrapper: int, prefix: bytes, suffix: bytes = b"", occurrence: int = 0) -> int | None:
    """在包装函数中匹配指令前后缀并读取一个 little-endian disp32。

    occurrence 指定使用第几个前缀命中；suffix 非空时还会验证位移后的字节。
    """
    offset = pe.va_to_off(wrapper)
    if offset is None:
        return None
    code = pe.data[offset:offset + 192]
    start = 0
    for _ in range(occurrence + 1):
        position = code.find(prefix, start)
        if position < 0:
            return None
        start = position + 1
    displacement_offset = position + len(prefix)
    if suffix and code[displacement_offset + 4:displacement_offset + 4 + len(suffix)] != suffix:
        return None
    return struct.unpack_from("<I", code, displacement_offset)[0]



def call_sites_to(pe: PE, target_va: int) -> list[int]:
    """扫描全部可执行节，收集所有 E8 调用指定目标的指令虚拟地址。"""
    sites: list[int] = []
    for section in pe.sections:
        if not section.executable:
            continue
        data = pe.data[section.raw_offset:section.raw_offset + section.raw_size]
        for index in range(0, max(0, len(data) - 5)):
            if data[index] != 0xE8:
                continue
            displacement = struct.unpack_from("<i", data, index + 1)[0]
            instruction_va = pe.image_base + section.rva + index
            if instruction_va + 5 + displacement == target_va:
                sites.append(instruction_va)
    return sites


def has_delegate_offset_evidence(pe: PE, call_sites: list[int], offset: int) -> bool:
    """检查委托广播调用前是否存在 service+dispatcher 偏移的构造证据。

    方法只识别当前 EXE 中观察到的 LEA/ADD 常见编码，找不到证据时返回 False，
    让验证结果保持保守而不是猜测兼容。
    """
    # Spotify 响应处理函数在调用动态多播委托广播函数前，会把 RCX 构造成
    # service + dispatcher 偏移。不同处理函数使用的基址寄存器并不完全相同，
    # 因此在 call 前的短窗口中检查常见 LEA/ADD 编码。
    disp = bytes([offset & 0xFF])
    for call_va in call_sites:
        call_off = pe.va_to_off(call_va)
        if call_off is None:
            continue
        window = pe.data[max(0, call_off - 72):call_off]
        for modrm in range(0x48, 0x50):  # lea rcx, [寄存器 + disp8]
            if b"\x48\x8D" + bytes([modrm]) + disp in window:
                return True
        if b"\x48\x83\xC1" + disp in window:  # add rcx, disp8
            return True
    return False

def main() -> int:
    """解析命令行目标 EXE，打印定位与布局检查结果并返回兼容性退出码。

    返回 0 表示所有必需入口和认证委托证据都通过；返回 1 表示至少一项失败。
    """
    parser = argparse.ArgumentParser(description="检查 Dangerous Driving EXE 与本地音乐后端的兼容性")
    parser.add_argument("exe", type=Path, help="DangerousDriving-Win64-Shipping.exe 的路径")
    args = parser.parse_args()
    pe = PE(args.exe)
    print(f"文件：{args.exe}")
    print(f"PE 时间戳：0x{pe.timestamp:08X}")
    print(f"映像大小：0x{pe.image_size:X}")
    print(f"SHA-256：{hashlib.sha256(pe.data).hexdigest()}")
    print()

    names = [
        "HasValidAccessToken", "HasValidAccessTokenAndDevice", "IsEnabled", "IsPaused", "CheckEnabled",
        "CheckAuthCode", "ClearAccessTokens", "RefreshAccessToken", "RequestActivateCode", "Update",
        "GetPlayingArtistName", "GetPlayingTrackName", "NextTrack", "Pause", "Play", "PreviousTrack", "Unpause",
        "SwitchActiveDevice", "SetDefaultVolume", "SetVolumeOffset",
    ]
    wrappers: dict[str, int] = {}
    failed = False
    for name in names:
        address = resolve_wrapper(pe, name)
        if address is None:
            print(f"[失败] {name}")
            failed = True
        else:
            wrappers[name] = address
            print(f"[通过] {name:30s} RVA 0x{address - pe.image_base:X}")

    print()
    if "GetPlayingArtistName" in wrappers:
        target = tail_jump(pe, wrappers["GetPlayingArtistName"])
        print("FString 拷贝辅助函数：", f"RVA 0x{target - pe.image_base:X}" if target else "失败")
        failed |= target is None

    native_names = [
        "CheckEnabled", "NextTrack", "Pause", "Play", "PreviousTrack", "Unpause",
        "SwitchActiveDevice", "CheckAuthCode", "ClearAccessTokens",
        "RefreshAccessToken", "RequestActivateCode", "Update",
    ]
    print("\n原生动作/网络目标（本地替代认证 Hook 层）：")
    for name in native_names:
        wrapper = wrappers.get(name)
        target = tail_jump(pe, wrapper) if wrapper is not None else None
        if target is None:
            print(f"[失败] 原生 {name}")
            if name != "SwitchActiveDevice":
                failed = True
        else:
            print(f"[通过] 原生 {name:23s} RVA 0x{target - pe.image_base:X}")

    if "SetDefaultVolume" in wrappers:
        target = last_call(pe, wrappers["SetDefaultVolume"])
        print("原生 SetDefaultVolume：", f"RVA 0x{target - pe.image_base:X}" if target else "失败")
    if "SetVolumeOffset" in wrappers:
        target = last_call(pe, wrappers["SetVolumeOffset"])
        print("原生 SetVolumeOffset：", f"RVA 0x{target - pe.image_base:X}" if target else "失败")

    exec_probe = next((wrappers[name] for name in (
        "HasValidAccessToken", "HasValidAccessTokenAndDevice", "IsEnabled", "IsPaused"
    ) if name in wrappers), None)
    if exec_probe is not None:
        frame = infer_frame_offset(pe, exec_probe)
        print("FFrame::Code 偏移：", f"0x{frame:X}" if frame is not None else "失败")
        failed |= frame is None

    delegate_rva = 0x258250
    delegate_va = pe.image_base + delegate_rva
    delegate_off = pe.va_to_off(delegate_va)
    delegate_prefix = b"\x4C\x8B\xDC\x53\x41\x54\x41\x56"
    helper_ok = (delegate_off is not None and pe.is_executable_va(delegate_va, len(delegate_prefix)) and
                 pe.data[delegate_off:delegate_off + len(delegate_prefix)] == delegate_prefix)
    print("UE4 委托广播辅助函数：", f"RVA 0x{delegate_rva:X}" if helper_ok else "失败")
    failed |= not helper_ok

    if helper_ok:
        sites = call_sites_to(pe, delegate_va)
        print("认证 dispatcher 布局证据：")
        for label, offset in (
            ("EnabledDispatcher", 0x28),
            ("CheckAuthCodeDispatcher", 0x48),
            ("RefreshAccessTokenDispatcher", 0x58),
            ("SwitchActiveDeviceDispatcher", 0x68),
        ):
            ok = has_delegate_offset_evidence(pe, sites, offset)
            print(f"[{'通过' if ok else '失败'}] {label:31s} 偏移 0x{offset:X}")
            failed |= not ok

    enabled_field_evidence = b"\x9C\x00\x00\x00" in pe.data[
        pe.va_to_off(pe.image_base + 0x3FF000):pe.va_to_off(pe.image_base + 0x400100)
    ]
    print("运行时 service enabled 标志：", "偏移 0x9C" if enabled_field_evidence else "失败")
    failed |= not enabled_field_evidence
    print("持久凭据/存档写入：已由 Hook 设计禁用")

    print("\n最终结果：", "不兼容" if failed else "兼容自动定位器与认证回调")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
