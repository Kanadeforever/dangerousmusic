// 固定专辑控制与通知布局编辑实现。
//
// XInput 使用运行时动态加载，避免某台机器缺少特定 xinput*.dll 时阻止
// dsound.dll 被游戏加载。所有业务动作都转交给 LocalPlayer 或
// NowPlayingOverlay，本文件不直接持有播放队列和窗口资源。
#include "AlbumControls.h"

#include "LocalPlayer.h"
#include "Logger.h"
#include "Localization.h"
#include "NowPlayingOverlay.h"

#include <tlhelp32.h>

namespace localmusic {
namespace {

// 与 Windows SDK 的 XINPUT_GAMEPAD 二进制布局保持一致。自行声明最小结构
// 可以兼容未安装较新 Windows SDK 的构建环境。
struct MinimalXInputGamepad {
    WORD buttons;
    BYTE left_trigger;
    BYTE right_trigger;
    SHORT thumb_lx;
    SHORT thumb_ly;
    SHORT thumb_rx;
    SHORT thumb_ry;
};

// 与 XINPUT_STATE 保持相同字段顺序；本插件只读取 buttons。
struct MinimalXInputState {
    DWORD packet_number;
    MinimalXInputGamepad gamepad;
};

// 与 XINPUT_KEYSTROKE 保持字段顺序。游戏若通过 XInputGetKeystroke 消费
// 菜单事件，Hook 会读取 VirtualKey、Flags 与 UserIndex 决定是否吞掉事件。
struct MinimalXInputKeystroke {
    WORD virtual_key;
    wchar_t unicode;
    WORD flags;
    BYTE user_index;
    BYTE hid_code;
};

constexpr WORD kDpadUp = 0x0001;
constexpr WORD kDpadDown = 0x0002;
constexpr WORD kDpadLeft = 0x0004;
constexpr WORD kDpadRight = 0x0008;
constexpr WORD kBack = 0x0020;
constexpr WORD kRightThumb = 0x0080;
constexpr WORD kLeftShoulder = 0x0100;
constexpr WORD kDirectionMask = kDpadUp | kDpadDown | kDpadLeft | kDpadRight;
// 布局模式下游戏侧必须完全看不到的按键。BACK 本身保留给游戏，
// 但十字键、LB 和 R3 都由 LocalMusic 独占。
constexpr WORD kLayoutCaptureMask = kDirectionMask | kLeftShoulder | kRightThumb;

// XInputGetKeystroke 使用独立虚拟键范围。这里只声明布局模式需要接管的键；
// 其他 A/B/X/Y、扳机和摇杆方向事件始终原样交给游戏。
constexpr WORD kVkPadLeftShoulder = 0x5805;
constexpr WORD kVkPadDpadUp = 0x5810;
constexpr WORD kVkPadDpadDown = 0x5811;
constexpr WORD kVkPadDpadLeft = 0x5812;
constexpr WORD kVkPadDpadRight = 0x5813;
constexpr WORD kVkPadBack = 0x5815;
constexpr WORD kVkPadRightThumbPress = 0x5817;
constexpr WORD kKeystrokeKeyDown = 0x0001;
constexpr WORD kKeystrokeKeyUp = 0x0002;
constexpr DWORD kXInputErrorEmpty = 4306;

constexpr int kPositionStepPixels = 8;
constexpr int kScaleStepPercent = 5;
constexpr int kOpacityStepPercent = 5;
constexpr uint64_t kInitialRepeatDelayMs = 350;
constexpr uint64_t kRepeatIntervalMs = 60;
constexpr uint64_t kKeyboardSaveDelayMs = 1000;
constexpr uint64_t kInputCaptureRefreshMs = 1000;
constexpr uint64_t kMediaReleaseGuardMs = 750;

// 把十字键常量映射到 directions 数组槽位，便于四个方向复用同一套
// 长按重复逻辑。顺序固定为左、右、上、下。
size_t DirectionIndex(WORD direction) {
    if (direction == kDpadLeft) return 0;
    if (direction == kDpadRight) return 1;
    if (direction == kDpadUp) return 2;
    return 3;
}

// 把 XInputGetKeystroke 的虚拟键映射回状态位掩码，使状态轮询与事件接口
// 共享同一套“直到全部释放”的捕获规则。返回 0 表示该事件不属于布局按键。
WORD CapturedButtonFromVirtualKey(WORD virtual_key) {
    switch (virtual_key) {
        case kVkPadDpadUp: return kDpadUp;
        case kVkPadDpadDown: return kDpadDown;
        case kVkPadDpadLeft: return kDpadLeft;
        case kVkPadDpadRight: return kDpadRight;
        case kVkPadLeftShoulder: return kLeftShoulder;
        case kVkPadRightThumbPress: return kRightThumb;
        default: return 0;
    }
}

}  // namespace

std::atomic<AlbumControls*> AlbumControls::active_instance_{nullptr};

// 初始化输入层并尝试加载可用的 XInput 运行库。键盘控制不依赖 XInput，
// 因此所有 XInput DLL 都加载失败时仍然返回 true。初始化阶段还记录本 DLL
// 和游戏目录，后续只扫描游戏自己的模块，不修改系统或第三方覆盖层。
bool AlbumControls::Initialize(bool enabled) {
    enabled_ = enabled;
    if (!enabled_) {
        log::Info(loc::Text(L"Log.Album.Disabled"));
        return true;
    }

    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&active_instance_), &self_module_);
    game_directory_ = GetModuleDirectory(GetModuleHandleW(nullptr));

    // 按系统常见顺序尝试，优先使用 Windows 8+ 自带的 xinput1_4。
    // 普通 GetState 用于插件轮询；序号 100 的 GetStateEx 若存在则单独保存，
    // 让游戏原本读取 Guide 等扩展按钮时不会因为 Hook 而丢失状态位。
    for (const wchar_t* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        HMODULE module = LoadLibraryW(library);
        if (!module) continue;
        auto function = reinterpret_cast<XInputGetStateFunction>(GetProcAddress(module, "XInputGetState"));
        if (!function) {
            FreeLibrary(module);
            continue;
        }
        xinput_module_ = module;
        xinput_get_state_ = function;
        xinput_get_state_ex_ = reinterpret_cast<XInputGetStateFunction>(
            GetProcAddress(module, MAKEINTRESOURCEA(100)));
        if (!xinput_get_state_ex_) xinput_get_state_ex_ = xinput_get_state_;
        xinput_get_keystroke_ = reinterpret_cast<XInputGetKeystrokeFunction>(
            GetProcAddress(module, "XInputGetKeystroke"));

        for (size_t user = 0; user < game_capture_latched_.size(); ++user) {
            game_capture_latched_[user].store(false, std::memory_order_relaxed);
            layout_mode_active_[user].store(false, std::memory_order_relaxed);
            keystroke_back_down_[user].store(false, std::memory_order_relaxed);
            keystroke_capture_mask_[user].store(0, std::memory_order_relaxed);
            media_capture_mask_[user].store(0, std::memory_order_relaxed);
        }
        active_instance_.store(this, std::memory_order_release);
        const bool capture_installed = InstallGameInputCapture();
        next_capture_refresh_ms_ = GetTickCount64() + kInputCaptureRefreshMs;
        log::Info(loc::Format(capture_installed ? L"Log.Album.EnabledInstalled"
                                               : L"Log.Album.EnabledScanning",
                              {library}));
        return true;
    }

    log::Warn(loc::Text(L"Log.Album.NoXInput"));
    return true;
}

// 释放动态加载的 XInput 模块并清空所有边沿/重复状态。关闭前会检查键盘
// 或任一手柄是否仍有未写盘的布局修改，并只执行一次最终保存；这样游戏在
// 用户仍按着 BACK 或刚完成键盘连调时退出，也不会丢失最后的布局结果。
void AlbumControls::Shutdown() {
    bool layout_dirty = keyboard_layout_dirty_;
    for (const ControllerState& state : controller_states_) {
        layout_dirty = layout_dirty || state.layout_dirty;
    }
    if (layout_dirty) {
        NowPlayingOverlay::Instance().SaveLayout();
    }

    RemoveGameInputCapture();
    active_instance_.store(nullptr, std::memory_order_release);
    xinput_get_state_ = nullptr;
    xinput_get_state_ex_ = nullptr;
    xinput_get_keystroke_ = nullptr;
    if (xinput_module_) {
        FreeLibrary(xinput_module_);
        xinput_module_ = nullptr;
    }
    enabled_ = false;
    keyboard_layout_dirty_ = false;
    next_capture_refresh_ms_ = 0;
    media_suppression_until_ms_.store(0, std::memory_order_relaxed);
    minus_repeat_ = RepeatButton{};
    equals_repeat_ = RepeatButton{};
    controller_states_.fill(ControllerState{});
    xinput_get_state_targets_.clear();
    xinput_get_state_ex_targets_.clear();
    xinput_get_keystroke_targets_.clear();
    game_directory_.clear();
    self_module_ = nullptr;
    for (size_t user = 0; user < game_capture_latched_.size(); ++user) {
        game_capture_latched_[user].store(false, std::memory_order_relaxed);
        layout_mode_active_[user].store(false, std::memory_order_relaxed);
        keystroke_back_down_[user].store(false, std::memory_order_relaxed);
        keystroke_capture_mask_[user].store(0, std::memory_order_relaxed);
        media_capture_mask_[user].store(0, std::memory_order_relaxed);
    }
}

// 判断模块文件是否位于游戏 EXE 目录或其子目录。只扫描游戏自己的模块，
// 避免修改 Windows 系统 DLL、显卡覆盖层、录屏工具或本插件自身的函数指针。
bool AlbumControls::IsGameModule(HMODULE module) const {
    if (!module || module == self_module_ || game_directory_.empty()) return false;
    const std::wstring module_path = GetModulePath(module);
    if (module_path.empty()) return false;
    const std::wstring filename = ToLower(std::filesystem::path(module_path).filename().wstring());
    // 若游戏目录自带 XInput 运行库，不能扫描其内部数据，否则可能把插件保存的
    // 原始后端或 XInput 自身实现改成 Hook，形成递归调用。
    if (filename.rfind(L"xinput", 0) == 0) return false;

    std::wstring candidate = ToLower(std::filesystem::path(module_path).lexically_normal().wstring());
    std::wstring root = ToLower(game_directory_.lexically_normal().wstring());
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    if (candidate.size() <= root.size() || candidate.compare(0, root.size(), root) != 0) return false;
    const wchar_t separator = candidate[root.size()];
    return separator == L'\\' || separator == L'/';
}

// 收集所有当前已加载 XInput 版本的普通与扩展状态函数地址。Dangerous Driving
// 可能链接 xinput1_3，而插件轮询使用 xinput1_4；同时收集所有版本才能识别
// 游戏运行时缓存的真实函数指针。
void AlbumControls::CollectXInputTargets() {
    xinput_get_state_targets_.clear();
    xinput_get_state_ex_targets_.clear();
    xinput_get_keystroke_targets_.clear();
    for (const wchar_t* library : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        HMODULE module = GetModuleHandleW(library);
        if (!module) continue;
        if (void* function = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState"))) {
            xinput_get_state_targets_.insert(function);
        }
        if (void* function = reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(100)))) {
            xinput_get_state_ex_targets_.insert(function);
        }
        if (void* function = reinterpret_cast<void*>(GetProcAddress(module, "XInputGetKeystroke"))) {
            xinput_get_keystroke_targets_.insert(function);
        }
    }
}

// 修改一个精确匹配 expected 的函数指针槽位。页面保护在写入后恢复，
// 并记录 original/replacement 供关闭时安全还原。相同槽位只记录一次。
bool AlbumControls::PatchPointerSlot(void** slot, void* expected, void* replacement) {
    if (!slot || !expected || !replacement || !IsReadableAddress(slot, sizeof(void*))) return false;
    if (std::any_of(game_pointer_patches_.begin(), game_pointer_patches_.end(),
                    [&](const PointerPatch& patch) { return patch.slot == slot; })) {
        return false;
    }
    if (*slot != expected || *slot == replacement) return false;

    DWORD old_protect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old_protect)) return false;
    if (*slot != expected) {
        DWORD ignored = 0;
        VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
        return false;
    }
    *slot = replacement;
    DWORD ignored = 0;
    VirtualProtect(slot, sizeof(void*), old_protect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    game_pointer_patches_.push_back({slot, expected, replacement});
    return true;
}

// 扫描一个游戏模块的普通 PE 导入表。除按名称导入的 XInputGetState 外，
// 还识别 XInput 的序号 1（普通状态）和序号 100（XInputGetStateEx）。后者正是
// 部分 UE4/手柄实现读取菜单输入的路径，旧版只识别名称时会漏掉它。
size_t AlbumControls::PatchModuleImports(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!base || !IsReadableAddress(base, sizeof(IMAGE_DOS_HEADER))) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!IsReadableAddress(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;

    const IMAGE_DATA_DIRECTORY& directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) return 0;

    size_t patched = 0;
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);
    const size_t descriptor_limit = directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (size_t descriptor_index = 0; descriptor_index < descriptor_limit; ++descriptor_index, ++descriptor) {
        if (!IsReadableAddress(descriptor, sizeof(*descriptor)) || descriptor->Name == 0) break;
        const char* imported_name = reinterpret_cast<const char*>(base + descriptor->Name);
        if (!IsReadableAddress(imported_name, 1)) continue;

        std::string library_name(imported_name);
        std::transform(library_name.begin(), library_name.end(), library_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (library_name.rfind("xinput", 0) != 0) continue;

        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->FirstThunk);
        auto* names = descriptor->OriginalFirstThunk != 0
                          ? reinterpret_cast<IMAGE_THUNK_DATA64*>(base + descriptor->OriginalFirstThunk)
                          : nullptr;

        for (size_t index = 0;; ++index) {
            IMAGE_THUNK_DATA64* iat_entry = iat + index;
            if (!IsReadableAddress(iat_entry, sizeof(*iat_entry)) || iat_entry->u1.Function == 0) break;

            enum class StateKind { None, Regular, Extended, Keystroke } kind = StateKind::None;
            if (names && IsReadableAddress(names + index, sizeof(IMAGE_THUNK_DATA64))) {
                const uint64_t name_value = names[index].u1.AddressOfData;
                if (name_value == 0) break;
                if (IMAGE_SNAP_BY_ORDINAL64(name_value)) {
                    const WORD ordinal = static_cast<WORD>(IMAGE_ORDINAL64(name_value));
                    if (ordinal == 1) kind = StateKind::Regular;
                    else if (ordinal == 7) kind = StateKind::Keystroke;
                    else if (ordinal == 100) kind = StateKind::Extended;
                } else {
                    const auto* import_by_name =
                        reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name_value);
                    if (IsReadableAddress(import_by_name, sizeof(WORD) + 1)) {
                        const char* function_name = reinterpret_cast<const char*>(import_by_name->Name);
                        if (std::strcmp(function_name, "XInputGetState") == 0) {
                            kind = StateKind::Regular;
                        } else if (std::strcmp(function_name, "XInputGetStateEx") == 0) {
                            kind = StateKind::Extended;
                        } else if (std::strcmp(function_name, "XInputGetKeystroke") == 0) {
                            kind = StateKind::Keystroke;
                        }
                    }
                }
            } else {
                void* current = reinterpret_cast<void*>(iat_entry->u1.Function);
                if (xinput_get_state_ex_targets_.count(current) != 0) kind = StateKind::Extended;
                else if (xinput_get_state_targets_.count(current) != 0) kind = StateKind::Regular;
                else if (xinput_get_keystroke_targets_.count(current) != 0) kind = StateKind::Keystroke;
            }

            if (kind == StateKind::None) continue;
            void** slot = reinterpret_cast<void**>(&iat_entry->u1.Function);
            void* expected = *slot;
            void* replacement = nullptr;
            if (kind == StateKind::Extended) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetStateEx);
            } else if (kind == StateKind::Regular) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetState);
            } else if (xinput_get_keystroke_) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetKeystroke);
            }
            if (replacement && PatchPointerSlot(slot, expected, replacement)) ++patched;
        }
    }
    return patched;
}

// 扫描模块可写数据节中的已缓存 XInput 函数地址。某些输入代码会在启动时
// GetProcAddress 后保存函数指针，之后完全绕过 IAT；精确地址匹配可以覆盖该
// 路径，同时避免对代码节做内联修改。
size_t AlbumControls::PatchModuleCachedPointers(HMODULE module) {
    auto* base = reinterpret_cast<unsigned char*>(module);
    if (!base || !IsReadableAddress(base, sizeof(IMAGE_DOS_HEADER))) return 0;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (!IsReadableAddress(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return 0;

    size_t patched = 0;
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD section_index = 0; section_index < nt->FileHeader.NumberOfSections;
         ++section_index, ++section) {
        if (!IsReadableAddress(section, sizeof(*section))) break;
        if ((section->Characteristics & IMAGE_SCN_MEM_WRITE) == 0) continue;
        const size_t section_size = std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData);
        if (section_size < sizeof(void*) ||
            section->VirtualAddress >= nt->OptionalHeader.SizeOfImage) continue;
        const size_t bounded_size = std::min<size_t>(
            section_size, nt->OptionalHeader.SizeOfImage - section->VirtualAddress);
        auto* begin = base + section->VirtualAddress;
        if (!IsReadableAddress(begin, bounded_size)) continue;

        const uintptr_t aligned_begin =
            (reinterpret_cast<uintptr_t>(begin) + alignof(void*) - 1) & ~(alignof(void*) - 1);
        const uintptr_t end = reinterpret_cast<uintptr_t>(begin) + bounded_size;
        for (uintptr_t address = aligned_begin; address + sizeof(void*) <= end; address += sizeof(void*)) {
            auto** slot = reinterpret_cast<void**>(address);
            void* current = *slot;
            void* replacement = nullptr;
            if (xinput_get_state_ex_targets_.count(current) != 0) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetStateEx);
            } else if (xinput_get_state_targets_.count(current) != 0) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetState);
            } else if (xinput_get_keystroke_ && xinput_get_keystroke_targets_.count(current) != 0) {
                replacement = reinterpret_cast<void*>(&HookedXInputGetKeystroke);
            }
            if (replacement && PatchPointerSlot(slot, current, replacement)) ++patched;
        }
    }
    return patched;
}

// 枚举当前进程模块并扫描所有位于游戏目录内的 PE。周期刷新使稍后加载的
// 游戏 DLL、延迟导入和运行时缓存指针也能被接管。该方法幂等，已记录槽位
// 不会重复写入或重复加入恢复列表。
void AlbumControls::RefreshGameInputCapture(bool initial_scan) {
    CollectXInputTargets();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (initial_scan) log::Warn(loc::Text(L"Log.Album.EnumFailed"));
        return;
    }

    size_t modules_scanned = 0;
    size_t imports_patched = 0;
    size_t cached_pointers_patched = 0;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (!IsGameModule(entry.hModule)) continue;
            ++modules_scanned;
            imports_patched += PatchModuleImports(entry.hModule);
            cached_pointers_patched += PatchModuleCachedPointers(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    const size_t new_patches = imports_patched + cached_pointers_patched;
    if (new_patches != 0 || initial_scan) {
        log::Info(loc::Format(L"Log.Album.Scan",
                              {std::to_wstring(modules_scanned),
                               std::to_wstring(imports_patched),
                               std::to_wstring(cached_pointers_patched),
                               std::to_wstring(game_pointer_patches_.size())}));
    }
}

// 首次安装游戏输入捕获。即使第一次扫描尚未发现槽位也不影响插件手柄轮询；
// Update 会每秒继续扫描，覆盖延迟加载或稍后缓存的输入函数。
bool AlbumControls::InstallGameInputCapture() {
    RefreshGameInputCapture(true);
    return !game_pointer_patches_.empty();
}

// 恢复所有被本模块修改的函数指针。只在槽位仍指向本 Hook 时恢复，避免覆盖
// ReShade、输入重映射器或其他插件在 LocalMusic 之后安装的 Hook。
void AlbumControls::RemoveGameInputCapture() {
    for (auto it = game_pointer_patches_.rbegin(); it != game_pointer_patches_.rend(); ++it) {
        if (!it->slot || !IsReadableAddress(it->slot, sizeof(void*))) continue;
        DWORD old_protect = 0;
        if (!VirtualProtect(it->slot, sizeof(void*), PAGE_READWRITE, &old_protect)) continue;
        if (*it->slot == it->replacement) {
            *it->slot = it->original;
        }
        DWORD ignored = 0;
        VirtualProtect(it->slot, sizeof(void*), old_protect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), it->slot, sizeof(void*));
    }
    game_pointer_patches_.clear();
}

// 普通 XInputGetState Hook。插件通过自己保存的原始函数指针读取完整状态，
// 因此不会再次经过游戏槽位形成递归；过滤只修改返回给游戏的状态副本。
DWORD WINAPI AlbumControls::HookedXInputGetState(DWORD user_index, void* state) {
    AlbumControls* instance = active_instance_.load(std::memory_order_acquire);
    if (!instance || !instance->xinput_get_state_) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = instance->xinput_get_state_(user_index, state);
    if (result == ERROR_SUCCESS && state) instance->FilterGameInput(user_index, state);
    return result;
}

// 扩展 XInputGetStateEx Hook。优先调用序号 100 的原始入口以保留扩展按钮；
// 当前系统 XInput 不提供该入口时才回退普通 GetState，状态结构保持兼容。
DWORD WINAPI AlbumControls::HookedXInputGetStateEx(DWORD user_index, void* state) {
    AlbumControls* instance = active_instance_.load(std::memory_order_acquire);
    if (!instance) return ERROR_DEVICE_NOT_CONNECTED;
    XInputGetStateFunction function = instance->xinput_get_state_ex_
                                          ? instance->xinput_get_state_ex_
                                          : instance->xinput_get_state_;
    if (!function) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = function(user_index, state);
    if (result == ERROR_SUCCESS && state) instance->FilterGameInput(user_index, state);
    return result;
}

// XInputGetKeystroke Hook。游戏菜单若消费离散按键事件，布局模式会把被
// 接管按键转换成 ERROR_EMPTY；未被接管的事件仍保持原始返回码和内容。
DWORD WINAPI AlbumControls::HookedXInputGetKeystroke(DWORD user_index, DWORD reserved,
                                                      void* keystroke) {
    AlbumControls* instance = active_instance_.load(std::memory_order_acquire);
    if (!instance || !instance->xinput_get_keystroke_) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = instance->xinput_get_keystroke_(user_index, reserved, keystroke);
    if (result == ERROR_SUCCESS && keystroke && instance->FilterGameKeystroke(user_index, keystroke)) {
        return kXInputErrorEmpty;
    }
    return result;
}

// 对 XInputGetKeystroke 的单个事件应用布局捕获。BACK 事件本身继续交给游戏，
// 但它会更新事件接口自己的按住状态；十字键、LB、R3 在 BACK 期间全部吞掉，
// BACK 松开后也会继续吞掉对应抬起事件，直到捕获掩码清空。
bool AlbumControls::FilterGameKeystroke(DWORD requested_user, void* keystroke) {
    if (!keystroke) return false;
    auto* input = static_cast<MinimalXInputKeystroke*>(keystroke);
    DWORD user = input->user_index < game_capture_latched_.size()
                     ? static_cast<DWORD>(input->user_index)
                     : requested_user;
    if (user >= game_capture_latched_.size()) return false;

    const bool key_down = (input->flags & kKeystrokeKeyDown) != 0;
    const bool key_up = (input->flags & kKeystrokeKeyUp) != 0;
    if (input->virtual_key == kVkPadBack) {
        if (key_down) keystroke_back_down_[user].store(true, std::memory_order_relaxed);
        if (key_up) keystroke_back_down_[user].store(false, std::memory_order_relaxed);
        return false;
    }

    const WORD captured_button = CapturedButtonFromVirtualKey(input->virtual_key);
    if (captured_button == 0) return false;

    WORD captured = keystroke_capture_mask_[user].load(std::memory_order_relaxed);
    const bool layout_active = layout_mode_active_[user].load(std::memory_order_relaxed) ||
                               keystroke_back_down_[user].load(std::memory_order_relaxed) ||
                               game_capture_latched_[user].load(std::memory_order_relaxed);
    if (layout_active) captured = static_cast<WORD>(captured | captured_button);

    const bool swallow = layout_active || (captured & captured_button) != 0;
    if (swallow && key_up) captured = static_cast<WORD>(captured & ~captured_button);
    keystroke_capture_mask_[user].store(captured, std::memory_order_relaxed);
    return swallow;
}

// 只过滤返回给游戏的按键副本。插件工作线程调用的是原始 XInputGetState，仍能
// 看见完整状态。BACK 按住后捕获十字键、LB、R3；BACK 已松开但这些键尚未
// 全部释放时继续清零，避免边界帧触发游戏的播放/暂停、换曲或菜单动作。
void AlbumControls::FilterGameInput(DWORD user_index, void* state) {
    if (user_index >= game_capture_latched_.size() || !state) return;
    auto* input = static_cast<MinimalXInputState*>(state);
    const WORD raw_buttons = input->gamepad.buttons;
    const bool back_down = (raw_buttons & kBack) != 0;
    bool latched = game_capture_latched_[user_index].load(std::memory_order_relaxed);
    if (back_down) latched = true;

    if (latched) {
        input->gamepad.buttons = static_cast<WORD>(raw_buttons & ~kLayoutCaptureMask);
        if (!back_down && (raw_buttons & kLayoutCaptureMask) == 0) {
            latched = false;
        }
    }
    game_capture_latched_[user_index].store(latched, std::memory_order_relaxed);
}

// 在游戏媒体 Hook 所在线程同步判定当前是否属于插件组合键会话。
// 这条路径不依赖游戏 XInput 调用槽位是否被扫描到：它直接调用插件保存的
// 原始 XInputGetState，并与工作线程发布的方向会话掩码合并。当前组合和释放
// 吸收窗口都会阻断 Play/Pause/Unpause/Next/Previous，插件自身动作不经过此门。
bool AlbumControls::ShouldBlockGameMediaCommand() {
    AlbumControls* instance = active_instance_.load(std::memory_order_acquire);
    if (!instance || !instance->enabled_) return false;

    const uint64_t now_ms = GetTickCount64();
    bool combo_active = false;
    XInputGetStateFunction get_state = instance->xinput_get_state_;
    for (DWORD user = 0; user < instance->media_capture_mask_.size(); ++user) {
        const WORD captured = instance->media_capture_mask_[user].load(std::memory_order_acquire);
        if (captured != 0) {
            combo_active = true;
            break;
        }
        if (!get_state) continue;
        MinimalXInputState state{};
        if (get_state(user, &state) != ERROR_SUCCESS) continue;
        const WORD buttons = state.gamepad.buttons;
        const bool direction_down = (buttons & kDirectionMask) != 0;
        const bool modifier_down = (buttons & (kBack | kLeftShoulder)) != 0;
        if (direction_down && modifier_down) {
            combo_active = true;
            break;
        }
    }

    if (combo_active) {
        const uint64_t requested_until = now_ms + kMediaReleaseGuardMs;
        uint64_t current = instance->media_suppression_until_ms_.load(std::memory_order_acquire);
        while (current < requested_until &&
               !instance->media_suppression_until_ms_.compare_exchange_weak(
                   current, requested_until, std::memory_order_acq_rel, std::memory_order_acquire)) {
        }
        return true;
    }

    const uint64_t until = instance->media_suppression_until_ms_.load(std::memory_order_acquire);
    return until != 0 && now_ms <= until;
}

// 检查当前前台窗口所属进程。使用进程 ID 而不是标题/类名，避免无边框模式、
// 本地化标题或 UE4 重建窗口时导致误判。
bool AlbumControls::IsGameForeground() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD process_id = 0;
    GetWindowThreadProcessId(foreground, &process_id);
    return process_id == GetCurrentProcessId();
}

// 读取虚拟键高位；低位“自上次调用后按过”语义不适合本模块自己的边沿状态机。
bool AlbumControls::KeyDown(int virtual_key) const {
    return (GetAsyncKeyState(virtual_key) & 0x8000) != 0;
}

// 实现统一长按节奏。系统时间使用 GetTickCount64，避免 32 位计数约 49 天回绕。
bool AlbumControls::ShouldRepeat(RepeatButton& state, bool down, uint64_t now_ms) {
    if (!down) {
        state = RepeatButton{};
        return false;
    }
    if (!state.down) {
        state.down = true;
        state.next_repeat_ms = now_ms + kInitialRepeatDelayMs;
        return true;
    }
    if (now_ms >= state.next_repeat_ms) {
        state.next_repeat_ms = now_ms + kRepeatIntervalMs;
        return true;
    }
    return false;
}

// 在后台状态下只同步“当前正在按住”，并把下次重复时间推迟。这样切回游戏
// 时不会立即触发，但继续按住超过初始延迟后仍能自然开始连发。
void AlbumControls::SynchronizeRepeat(RepeatButton& state, bool down, uint64_t now_ms) {
    if (!down) {
        state = RepeatButton{};
        return;
    }
    state.down = true;
    state.next_repeat_ms = now_ms + kInitialRepeatDelayMs;
}

// 根据修饰键把 -/= 映射为视觉方向或数值变化。increase=true 对应 = 键；
// 优先级固定为 Alt、Ctrl、Shift、无修饰，防止多修饰键同时修改多个属性。
bool AlbumControls::ApplyKeyboardLayoutStep(bool increase) {
    const int sign = increase ? 1 : -1;
    if (KeyDown(VK_MENU)) {
        NowPlayingOverlay::Instance().AdjustLayout(0, 0, sign * kScaleStepPercent, 0);
    } else if (KeyDown(VK_CONTROL)) {
        NowPlayingOverlay::Instance().AdjustLayout(0, sign * kPositionStepPixels, 0, 0);
    } else if (KeyDown(VK_SHIFT)) {
        NowPlayingOverlay::Instance().AdjustLayout(sign * kPositionStepPixels, 0, 0, 0);
    } else {
        NowPlayingOverlay::Instance().AdjustLayout(0, 0, 0, sign * kOpacityStepPercent);
    }
    return true;
}

// 处理键盘常规控制、布局编辑、重置和延迟保存。布局键使用物理按下状态，
// 所以短按可精调，长按可快速移动；Backspace 采用按下沿且立即持久化。
void AlbumControls::UpdateKeyboard(bool foreground, uint64_t now_ms) {
    const bool insert_now = KeyDown(VK_INSERT);
    const bool page_up_now = KeyDown(VK_PRIOR);
    const bool home_now = KeyDown(VK_HOME);
    const bool backspace_now = KeyDown(VK_BACK);
    const bool minus_now = KeyDown(VK_OEM_MINUS);
    const bool equals_now = KeyDown(VK_OEM_PLUS);

    if (foreground) {
        if (insert_now && !insert_down_) LocalPlayer::Instance().PreviousAlbum();
        if (page_up_now && !page_up_down_) LocalPlayer::Instance().NextAlbum();
        if (home_now && !home_down_) LocalPlayer::Instance().CyclePlayMode();

        if (backspace_now && !backspace_down_) {
            NowPlayingOverlay::Instance().ResetLayout();
            keyboard_layout_dirty_ = false;
        }

        bool adjusted = false;
        if (ShouldRepeat(minus_repeat_, minus_now, now_ms)) {
            adjusted = ApplyKeyboardLayoutStep(false) || adjusted;
        }
        if (ShouldRepeat(equals_repeat_, equals_now, now_ms)) {
            adjusted = ApplyKeyboardLayoutStep(true) || adjusted;
        }
        if (adjusted) {
            keyboard_layout_dirty_ = true;
            keyboard_last_adjust_ms_ = now_ms;
        }
    } else {
        // 即使游戏失去焦点也同步边沿和重复状态，防止切回来时误触发。
        SynchronizeRepeat(minus_repeat_, minus_now, now_ms);
        SynchronizeRepeat(equals_repeat_, equals_now, now_ms);
    }

    insert_down_ = insert_now;
    page_up_down_ = page_up_now;
    home_down_ = home_now;
    backspace_down_ = backspace_now;

    // 停止键盘调整 1 秒后一次性写盘。即使游戏刚失去焦点也要完成
    // 已产生修改的保存，避免用户 Alt+Tab 后丢失最后一次调整。
    if (keyboard_layout_dirty_ && now_ms >= keyboard_last_adjust_ms_ + kKeyboardSaveDelayMs) {
        NowPlayingOverlay::Instance().SaveLayout();
        keyboard_layout_dirty_ = false;
    }
}

// 处理 BACK 布局模式。返回 true 表示本手柄当前由布局模式接管，调用方
// 不应继续解释 LB+十字键的专辑/播放模式动作。
bool AlbumControls::UpdateControllerLayout(size_t user, WORD buttons, bool foreground, uint64_t now_ms) {
    ControllerState& state = controller_states_[user];
    const bool back_now = (buttons & kBack) != 0;
    const bool right_thumb_now = (buttons & kRightThumb) != 0;
    const bool lb_now = (buttons & kLeftShoulder) != 0;
    layout_mode_active_[user].store(back_now, std::memory_order_relaxed);

    if (!back_now) {
        // 松开 BACK 是布局会话的保存边界。方向会话本身仍保持锁定到方向键释放，
        // 因此 BACK 松开但方向未松开时不会立刻退化为普通或 LB 动作。
        if (state.back_down && state.layout_dirty) {
            NowPlayingOverlay::Instance().SaveLayout();
            state.layout_dirty = false;
        }
        state.back_down = false;
        state.right_thumb_down = right_thumb_now;
        return false;
    }

    state.back_down = true;
    if (!foreground) {
        const std::array<WORD, 4> directions{kDpadLeft, kDpadRight, kDpadUp, kDpadDown};
        for (size_t index = 0; index < state.directions.size(); ++index) {
            SynchronizeRepeat(state.directions[index],
                              (buttons & directions[index]) != 0, now_ms);
        }
        state.right_thumb_down = right_thumb_now;
        return true;
    }

    // BACK+R3 仍采用独立按下沿，且优先于本帧全部方向调整。
    if (right_thumb_now && !state.right_thumb_down) {
        NowPlayingOverlay::Instance().ResetLayout();
        state.layout_dirty = false;
        state.right_thumb_down = true;
        for (RepeatButton& repeat : state.directions) repeat = RepeatButton{};
        return true;
    }
    state.right_thumb_down = right_thumb_now;

    bool adjusted = false;
    const std::array<WORD, 4> directions{kDpadLeft, kDpadRight, kDpadUp, kDpadDown};
    for (size_t index = 0; index < directions.size(); ++index) {
        const WORD direction = directions[index];
        const DirectionMode mode = state.direction_modes[index];
        const bool exact_back = mode == DirectionMode::Back && back_now && !lb_now;
        const bool exact_back_lb = mode == DirectionMode::BackLeftShoulder && back_now && lb_now;
        const bool repeat_active = (buttons & direction) != 0 && (exact_back || exact_back_lb);
        if (!ShouldRepeat(state.directions[index], repeat_active, now_ms)) continue;

        if (mode == DirectionMode::BackLeftShoulder) {
            if (direction == kDpadLeft) {
                NowPlayingOverlay::Instance().AdjustLayout(0, 0, 0, -kOpacityStepPercent);
            } else if (direction == kDpadRight) {
                NowPlayingOverlay::Instance().AdjustLayout(0, 0, 0, kOpacityStepPercent);
            } else if (direction == kDpadUp) {
                NowPlayingOverlay::Instance().AdjustLayout(0, 0, kScaleStepPercent, 0);
            } else {
                NowPlayingOverlay::Instance().AdjustLayout(0, 0, -kScaleStepPercent, 0);
            }
        } else if (mode == DirectionMode::Back) {
            if (direction == kDpadLeft) {
                NowPlayingOverlay::Instance().AdjustLayout(-kPositionStepPixels, 0, 0, 0);
            } else if (direction == kDpadRight) {
                NowPlayingOverlay::Instance().AdjustLayout(kPositionStepPixels, 0, 0, 0);
            } else if (direction == kDpadUp) {
                NowPlayingOverlay::Instance().AdjustLayout(0, -kPositionStepPixels, 0, 0);
            } else {
                NowPlayingOverlay::Instance().AdjustLayout(0, kPositionStepPixels, 0, 0);
            }
        }
        adjusted = true;
    }
    state.layout_dirty = state.layout_dirty || adjusted;
    return true;
}

// 处理普通方向与 LB 组合。方向键的模式已经在本轮采样阶段按“按下沿时的
// 修饰键”锁定，因此本函数不会在方向保持按下时因 LB 后按而补触发插件动作。
void AlbumControls::UpdateControllerNormal(size_t user, WORD buttons, bool foreground) {
    ControllerState& state = controller_states_[user];
    if (!foreground) return;

    const WORD previous_directions = static_cast<WORD>(state.previous_buttons & kDirectionMask);
    const WORD current_directions = static_cast<WORD>(buttons & kDirectionMask);
    const WORD rising = static_cast<WORD>(current_directions & ~previous_directions);
    if (rising == 0) return;

    // 同一采样最多执行一个 LB 动作；斜向输入按左、右、下、上的固定优先级。
    const std::array<WORD, 4> priority{kDpadLeft, kDpadRight, kDpadDown, kDpadUp};
    for (const WORD direction : priority) {
        if ((rising & direction) == 0) continue;
        const DirectionMode mode = state.direction_modes[DirectionIndex(direction)];
        if (mode != DirectionMode::LeftShoulder) continue;

        if (direction == kDpadLeft) LocalPlayer::Instance().PreviousAlbum();
        else if (direction == kDpadRight) LocalPlayer::Instance().NextAlbum();
        else if (direction == kDpadDown) LocalPlayer::Instance().CyclePlayMode();
        else LocalPlayer::Instance().ShowCurrentStatus();
        break;
    }
}

// 轮询全部手柄并建立“方向按住周期”的互斥状态。每个方向只在按下沿决定
// Plain/LB/BACK/BACK+LB 之一，之后修饰键变化不会改变归属；直到该方向释放。
void AlbumControls::UpdateControllers(bool foreground, uint64_t now_ms) {
    if (!xinput_get_state_) return;

    struct PolledController {
        bool connected = false;
        WORD buttons = 0;
    };
    std::array<PolledController, 4> polled{};
    bool global_back_guard = false;

    for (DWORD user = 0; user < controller_states_.size(); ++user) {
        MinimalXInputState input{};
        const DWORD result = xinput_get_state_(user, &input);
        if (result != ERROR_SUCCESS) {
            if (controller_states_[user].layout_dirty) {
                NowPlayingOverlay::Instance().SaveLayout();
            }
            controller_states_[user] = ControllerState{};
            layout_mode_active_[user].store(false, std::memory_order_relaxed);
            game_capture_latched_[user].store(false, std::memory_order_relaxed);
            keystroke_back_down_[user].store(false, std::memory_order_relaxed);
            keystroke_capture_mask_[user].store(0, std::memory_order_relaxed);
            media_capture_mask_[user].store(0, std::memory_order_release);
            continue;
        }

        ControllerState& state = controller_states_[user];
        const WORD buttons = input.gamepad.buttons;
        const WORD previous_directions = static_cast<WORD>(state.previous_buttons & kDirectionMask);
        const WORD current_directions = static_cast<WORD>(buttons & kDirectionMask);
        const WORD rising = static_cast<WORD>(current_directions & ~previous_directions);
        const WORD released = static_cast<WORD>(previous_directions & ~current_directions);
        const bool back_now = (buttons & kBack) != 0;
        const bool lb_now = (buttons & kLeftShoulder) != 0;

        const std::array<WORD, 4> directions{kDpadLeft, kDpadRight, kDpadUp, kDpadDown};
        for (size_t index = 0; index < directions.size(); ++index) {
            const WORD direction = directions[index];
            if ((released & direction) != 0) {
                state.direction_modes[index] = DirectionMode::None;
                state.directions[index] = RepeatButton{};
            }
            if ((rising & direction) == 0) continue;
            if (back_now && lb_now) state.direction_modes[index] = DirectionMode::BackLeftShoulder;
            else if (back_now) state.direction_modes[index] = DirectionMode::Back;
            else if (lb_now) state.direction_modes[index] = DirectionMode::LeftShoulder;
            else state.direction_modes[index] = DirectionMode::Plain;
        }

        WORD media_mask = 0;
        for (size_t index = 0; index < directions.size(); ++index) {
            if ((current_directions & directions[index]) == 0) continue;
            const DirectionMode mode = state.direction_modes[index];
            if (mode == DirectionMode::LeftShoulder || mode == DirectionMode::Back ||
                mode == DirectionMode::BackLeftShoulder) {
                media_mask = static_cast<WORD>(media_mask | directions[index]);
            }
        }
        media_capture_mask_[user].store(media_mask, std::memory_order_release);

        // 即使方向最初属于 Plain，后来才补按修饰键，也从这一刻起阻断游戏媒体
        // 命令，但不会补触发插件动作；必须松开方向并按正确顺序重新建立组合。
        const bool raw_combo = current_directions != 0 && (back_now || lb_now);
        if (media_mask != 0 || raw_combo) {
            const uint64_t requested_until = now_ms + kMediaReleaseGuardMs;
            uint64_t current = media_suppression_until_ms_.load(std::memory_order_acquire);
            while (current < requested_until &&
                   !media_suppression_until_ms_.compare_exchange_weak(
                       current, requested_until, std::memory_order_acq_rel, std::memory_order_acquire)) {
            }
        }

        polled[user].connected = true;
        polled[user].buttons = buttons;
        global_back_guard = global_back_guard || back_now || state.back_down ||
                            layout_mode_active_[user].load(std::memory_order_relaxed) ||
                            game_capture_latched_[user].load(std::memory_order_relaxed) ||
                            keystroke_back_down_[user].load(std::memory_order_relaxed);
    }

    // BACK 相关方向先执行布局动作；它们永远不会进入 LB 普通动作分支。
    for (DWORD user = 0; user < polled.size(); ++user) {
        if (!polled[user].connected) continue;
        UpdateControllerLayout(user, polled[user].buttons, foreground, now_ms);
    }

    if (!global_back_guard) {
        for (DWORD user = 0; user < polled.size(); ++user) {
            if (!polled[user].connected) continue;
            UpdateControllerNormal(user, polled[user].buttons, foreground);
        }
    }

    // 所有动作判定完成后才提交原始按钮历史，确保本轮各方法看到同一按下沿。
    for (DWORD user = 0; user < polled.size(); ++user) {
        if (polled[user].connected) controller_states_[user].previous_buttons = polled[user].buttons;
    }
}

// 输入层总更新入口。统一读取一次前台状态和时钟，保证键盘、所有手柄与延迟
// 保存使用同一时间基准，减少边界帧中的不一致。
void AlbumControls::Update() {
    if (!enabled_) return;
    const bool foreground = IsGameForeground();
    const uint64_t now_ms = GetTickCount64();

    // 游戏可能在插件初始化后才加载输入模块或解析 XInputGetStateEx。
    // 每秒补扫一次只遍历游戏模块的数据区，确保主菜单实际输入路径最终被接管。
    if (xinput_get_state_ && now_ms >= next_capture_refresh_ms_) {
        RefreshGameInputCapture(false);
        next_capture_refresh_ms_ = now_ms + kInputCaptureRefreshMs;
    }

    UpdateKeyboard(foreground, now_ms);
    UpdateControllers(foreground, now_ms);
}

}  // namespace localmusic
