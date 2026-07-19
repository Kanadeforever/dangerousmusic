#!/usr/bin/env python3
"""离线验证 LocalMusic 运行时国际化资源、源码调用和回退契约。

该工具不需要 Windows SDK。它从 Localization.cpp 提取内置简体中文键，
比较官方语言文件的键与占位符，检查所有 loc::Text/Format 调用，并阻止
用户可见中文重新散落到其他 C++ 实现中。
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
ENTRY_RE = re.compile(r'\{L"([^"]+)",\s*L"((?:\\.|[^"])*)"\},')
USE_RE = re.compile(r'loc::(?:Text|Format)\(L"([^"]+)"')
PLACEHOLDER_RE = re.compile(r"\{(\d+)\}")
CJK_RE = re.compile(r"[\u3400-\u9fff]")


def require(condition: bool, message: str) -> None:
    """在条件失败时抛出带项目名的断言，成功时打印统一通过标记。"""
    if not condition:
        raise AssertionError(message)
    print(f"[通过] {message}")


def parse_language_file(path: Path) -> dict[str, str]:
    """解析项目使用的简单 INI 语言文件并返回完整 Section.Key 字典。"""
    text = path.read_text(encoding="utf-8-sig")
    section = ""
    values: dict[str, str] = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith((";", "#")):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip()
            continue
        if "=" not in line or not section:
            continue
        key, value = line.split("=", 1)
        values[f"{section}.{key.strip()}"] = value.strip()
    return values


def strip_cpp_comments(text: str) -> str:
    """移除 C++ 行注释与块注释，供运行时字符串启发式检查使用。"""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//.*", "", text)


def check_keys() -> None:
    """验证内置字典、两份官方语言文件和源码调用使用同一键集合。"""
    localization = (SRC / "Localization.cpp").read_text(encoding="utf-8-sig")
    entries = dict(ENTRY_RE.findall(localization))
    require(bool(entries), "能够提取内置简体中文语言键")
    zh = parse_language_file(ROOT / "dsound.zh-hans.ini")
    en = parse_language_file(ROOT / "dsound.en-us.ini")
    require(set(zh) == set(entries), "zh-hans 外部文件与内置字典键完全一致")
    require(set(en) == set(entries), "en-us 外部文件与内置字典键完全一致")

    used: set[str] = set()
    for path in SRC.glob("*.cpp"):
        used.update(USE_RE.findall(path.read_text(encoding="utf-8-sig")))
    require(not (used - set(entries)), "所有 loc::Text/Format 调用键均已定义")

    mismatches: list[str] = []
    for key in entries:
        expected = set(PLACEHOLDER_RE.findall(entries[key]))
        if set(PLACEHOLDER_RE.findall(zh[key])) != expected:
            mismatches.append(f"{key}: zh-hans")
        if set(PLACEHOLDER_RE.findall(en[key])) != expected:
            mismatches.append(f"{key}: en-us")
    require(not mismatches, "官方语言文件占位符与内置文本一致" +
            ("；" + " | ".join(mismatches[:12]) if mismatches else ""))


def check_runtime_literals() -> None:
    """确认除 Localization.cpp 外不存在带中文的运行时宽字符串字面量。"""
    hits: list[str] = []
    literal_re = re.compile(r'L"(?:\\.|[^"\\])*"')
    for path in SRC.glob("*.cpp"):
        if path.name == "Localization.cpp":
            continue
        code = strip_cpp_comments(path.read_text(encoding="utf-8-sig"))
        for match in literal_re.finditer(code):
            if CJK_RE.search(match.group(0)):
                line = code.count("\n", 0, match.start()) + 1
                hits.append(f"{path.name}:{line}")
    require(not hits, "用户可见中文只存在于国际化字典" +
            ("；残留：" + " | ".join(hits[:12]) if hits else ""))


def check_distribution_contract() -> None:
    """核对主配置、构建清单和发行脚本包含语言层所需文件与选项。"""
    main_ini = (ROOT / "dsound.ini").read_text(encoding="utf-8-sig")
    require(re.search(r"(?mi)^Language=auto\s*$", main_ini) is not None,
            "主配置默认使用 Language=auto")
    cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8-sig")
    require("src/Localization.cpp" in cmake, "CMake 编译 Localization.cpp")
    package = (ROOT / "package-release.bat").read_text(encoding="utf-8-sig")
    require("dsound.zh-hans.ini" in package and "dsound.en-us.ini" in package,
            "发行脚本复制两份官方语言文件")
    localization = (SRC / "Localization.cpp").read_text(encoding="utf-8-sig")
    require("BuildLanguagePath" in localization and "dll_path.stem()" in localization,
            "语言文件名由实际 DLL 主文件名派生")
    require("g_english" in localization and "BuiltInValue" in localization,
            "实现指定语言到 en-us 再到内置 zh-hans 的分层回退")


def main() -> int:
    """依次执行全部国际化检查，全部通过时返回零退出码。"""
    check_keys()
    check_runtime_literals()
    check_distribution_contract()
    print("\n运行时国际化检查全部通过。")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"[失败] {exc}", file=sys.stderr)
        raise SystemExit(1)
