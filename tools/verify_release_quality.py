#!/usr/bin/env python3
"""离线检查发行包的中文源码注释、双语文档和布局功能一致性。

本工具不依赖 Windows SDK，也不会修改源码。它用于在打包前阻止三类回归：
1. 新增 C++ 文件或方法却没有详细简体中文说明；
2. 面向人类的独立文档缺少追加在中文后的英文版本；
3. 内置默认 INI、外部模板和游戏内布局控制出现不一致。

检查采用保守的文本启发式，不能替代代码评审或 MSVC 实际编译，但能覆盖本项目
约定的源码格式。误报时应改进本脚本的识别规则，而不是删除真实注释来绕过检查。
"""
from __future__ import annotations

import ast
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
CHINESE_RE = re.compile(r"[\u3400-\u9fff]")


def require(condition: bool, message: str) -> None:
    """输出一个统一的检查结果，并在失败时抛出带说明的异常。

    参数：
        condition: 真值表示当前约束满足。
        message: 终端中显示的中文检查项目。
    """
    if not condition:
        raise AssertionError(message)
    print(f"[通过] {message}")


def previous_comment(lines: list[str], index: int) -> bool:
    """判断指定行之前最近的非空行是否属于 C/C++ 注释。

    index 使用零基下标。连续 `//`、块注释结尾和块注释中间行都视为说明；
    预处理指令和普通代码不视为方法注释。
    """
    cursor = index - 1
    while cursor >= 0 and not lines[cursor].strip():
        cursor -= 1
    if cursor < 0:
        return False
    text = lines[cursor].lstrip()
    return text.startswith("//") or text.startswith("*") or text.endswith("*/")


def cpp_function_starts(path: Path) -> list[tuple[int, str]]:
    """用项目风格启发式收集 C++ 函数定义的起始行和简化签名。

    识别自由函数、类方法、析构函数和 Win32 回调；忽略 if/for/while、
    lambda、构造器调用以及 catch。返回行号为一基，便于直接定位源码。
    """
    lines = path.read_text(encoding="utf-8-sig").splitlines()
    results: list[tuple[int, str]] = []
    control = re.compile(r"^(if|for|while|switch|catch|else|do)\b")
    for end, line in enumerate(lines):
        stripped = line.strip()
        if not stripped.endswith("{") or ")" not in stripped:
            continue
        if control.match(stripped) or stripped.startswith("}"):
            continue
        start = end
        while start > 0:
            previous = lines[start - 1].strip()
            if not previous or previous.startswith(("//", "/*", "*", "#")):
                break
            if previous.endswith((";", "{", "}")):
                break
            start -= 1
        signature = " ".join(part.strip() for part in lines[start : end + 1])
        if "[&]" in signature or "[](" in signature or "[=" in signature:
            continue
        if signature.startswith(("return ", "throw ")):
            continue
        if re.match(r"^(if|for|while|switch|catch|else)\b", signature) or signature.startswith("}"):
            continue
        # 普通函数定义应包含返回类型、限定名、构造/析构名或 CALLBACK。
        if "::" not in signature and " CALLBACK " not in f" {signature} ":
            allowed_prefixes = (
                "bool ", "void ", "int ", "float ", "double ", "size_t ",
                "DWORD ", "BOOL ", "HWND ", "HMODULE ", "HANDLE ", "LRESULT ",
                "uintptr_t ", "uint32_t ", "uint64_t ", "std::", "Config ",
                "IniCreationResult ", "Gdiplus::", "Track", "Album", "FMOD_",
            )
            if not signature.startswith(allowed_prefixes):
                continue
        results.append((start + 1, signature[:180]))
    return results


def check_cpp_comments() -> None:
    """验证每个 C++ 文件有中文职责说明，并且每个识别到的方法前有注释。"""
    files = sorted(SRC.glob("*.h")) + sorted(SRC.glob("*.cpp"))
    require(bool(files), "找到 C++ 源码文件")
    missing: list[str] = []
    for path in files:
        text = path.read_text(encoding="utf-8-sig")
        first_block = "\n".join(text.splitlines()[:20])
        if "//" not in first_block or not CHINESE_RE.search(first_block):
            missing.append(f"{path.name}: 缺少中文文件职责说明")
        lines = text.splitlines()
        if path.suffix == ".cpp":
            for line_no, signature in cpp_function_starts(path):
                if not previous_comment(lines, line_no - 1):
                    missing.append(f"{path.name}:{line_no}: {signature}")
    require(not missing, "每个 C++ 文件和方法都有相邻的详细中文注释" +
            ("；遗漏：" + " | ".join(missing[:12]) if missing else ""))


def check_python_docstrings() -> None:
    """验证 tools 目录下每个 Python 模块、类和函数都包含中文 docstring。"""
    missing: list[str] = []
    for path in sorted((ROOT / "tools").glob("*.py")):
        tree = ast.parse(path.read_text(encoding="utf-8-sig"), filename=str(path))
        module_doc = ast.get_docstring(tree, clean=False) or ""
        if not CHINESE_RE.search(module_doc):
            missing.append(f"{path.name}: 模块 docstring")
        for node in ast.walk(tree):
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
                doc = ast.get_docstring(node, clean=False) or ""
                if not CHINESE_RE.search(doc):
                    missing.append(f"{path.name}:{node.lineno}: {node.name}")
    require(not missing, "每个 Python 模块、类和函数都有中文 docstring" +
            ("；遗漏：" + " | ".join(missing[:12]) if missing else ""))


def check_bilingual_documents() -> None:
    """检查主要独立文档先包含完整中文，再在后部包含英文版本。"""
    markdown = [
        "README.md",
        "INI配置说明.md",
        "CHANGELOG.md",
        "发行检查清单.md",
    ]
    for name in markdown:
        text = (ROOT / name).read_text(encoding="utf-8-sig")
        chinese = text.find("简体中文")
        english = text.find("English")
        require(chinese >= 0 and english > chinese, f"{name} 为中文在前、英文在后")

    text_pairs = {
        "安装说明.txt": ("【简体中文】", "[English]"),
        "verification-current.txt": ("【简体中文】", "[English]"),
        "文件校验值.txt": ("【简体中文】", "[English]"),
    }
    for name, markers in text_pairs.items():
        text = (ROOT / name).read_text(encoding="utf-8-sig")
        require(markers[0] in text and text.index(markers[1]) > text.index(markers[0]),
                f"{name} 为中文在前、英文在后")

    comment_files = [
        "dsound.ini",
        "开发者/高级配置参考.ini",
        "示例/文件夹专辑元数据模板.ini",
        "示例/曲目同名元数据模板.ini",
    ]
    for name in comment_files:
        text = (ROOT / name).read_text(encoding="utf-8-sig")
        marker = "; English reference"
        require(marker in text and text.index(marker) > 0,
                f"{name} 在中文内容后追加英文注释参考")


def check_layout_contract() -> None:
    """核对布局控制映射、持久化键、重置默认值和可见区域保护。"""
    controls = (SRC / "AlbumControls.cpp").read_text(encoding="utf-8-sig")
    overlay = (SRC / "NowPlayingOverlay.cpp").read_text(encoding="utf-8-sig")
    overlay_h = (SRC / "NowPlayingOverlay.h").read_text(encoding="utf-8-sig")
    for token in (
        "kBack", "kRightThumb", "kLeftShoulder", "VK_BACK", "VK_OEM_MINUS",
        "VK_OEM_PLUS", "kInitialRepeatDelayMs = 350", "kRepeatIntervalMs = 60",
        "kKeyboardSaveDelayMs = 1000",
    ):
        require(token in controls, f"布局输入实现包含 {token}")
    require("AdjustLayout(-kPositionStepPixels" in controls and
            "AdjustLayout(0, 0, 0, -kOpacityStepPercent)" in controls,
            "手柄布局模式覆盖移动与透明度")
    require("KeyDown(VK_MENU)" in controls and "KeyDown(VK_CONTROL)" in controls and
            "KeyDown(VK_SHIFT)" in controls,
            "键盘 -/= 使用 Alt、Ctrl、Shift 和无修饰分层")
    require("ResetLayout()" in controls and "SaveLayout()" in controls,
            "输入层调用通知布局重置与保存")
    for token in ("InstallGameInputCapture", "RefreshGameInputCapture",
                  "HookedXInputGetState", "HookedXInputGetStateEx",
                  "HookedXInputGetKeystroke", "FilterGameInput", "FilterGameKeystroke",
                  "PatchModuleImports", "PatchModuleCachedPointers",
                  "kLayoutCaptureMask", "IMAGE_DIRECTORY_ENTRY_IMPORT"):
        require(token in controls, f"游戏侧 XInput 多路径捕获实现包含 {token}")
    require("ordinal == 100" in controls and "ordinal == 7" in controls,
            "输入捕获覆盖扩展状态与离散按键事件入口")
    require("kLayoutCaptureMask = kDirectionMask | kLeftShoulder | kRightThumb" in controls,
            "BACK 布局模式完整捕获十字键、LB 与 R3")
    require("if (!back_down && (raw_buttons & kLayoutCaptureMask) == 0)" in controls,
            "松开 BACK 后持续过滤残留按键直到全部释放")
    require("void AdjustLayout" in overlay_h and "void SaveLayout" in overlay_h and
            "void ResetLayout" in overlay_h,
            "通知类公开布局调整、保存和重置契约")
    for token in (
        "kDefaultMarginX = 32", "kDefaultMarginY = 32", "kDefaultScalePercent = 100",
        "kDefaultOpacityPercent = 92", "kMinimumVisiblePixels = 48",
        'L"NotificationMarginX"', 'L"NotificationMarginY"',
        'L"NotificationScalePercent"', 'L"NotificationOpacityPercent"',
    ):
        require(token in overlay, f"通知布局实现包含 {token}")


def check_i18n_contract() -> None:
    """调用国际化专用检查，验证语言键、占位符、源码调用和发行文件。"""
    import verify_i18n
    require(verify_i18n.main() == 0, "运行时国际化契约检查通过")

def main() -> int:
    """依次执行全部发行质量检查；全部通过时返回零退出码。"""
    check_cpp_comments()
    check_python_docstrings()
    check_bilingual_documents()
    check_layout_contract()
    check_i18n_contract()
    print("\n全部发行质量检查通过。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        raise SystemExit(1)
