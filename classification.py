#!/usr/bin/env python3
"""在同一个目录内按风云卫星文件名整理数据。

分类目录格式为 ``<目录>/<仪器类别>/<年份>/<原文件名>``。默认只做预览；
确认输出无误后使用 ``--move`` 执行移动，或使用 ``--copy`` 保留源文件。
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import shutil
import sys
from pathlib import Path
from typing import Iterable, Optional, Tuple


CATEGORY_MARKERS = (
    ("_MWRID", "MWRID"),
    ("_MWRI", "MWRI"),
    ("_MERSI", "MERSI"),
    ("_MWHS", "MWHS"),
    ("_MWTS", "MWTS"),
    ("_GIIRS", "GIIRS"),
    ("_AGRI", "AGRI"),
    ("_GHI", "GHI"),
    ("_PMR", "PMR"),
)

DATE_PATTERN = re.compile(r"(20\d{2})(\d{2})(\d{2})")


def classify_category(filename: str) -> Optional[str]:
    """从文件名中提取参考目录使用的仪器类别。"""

    upper_name = filename.upper()
    for marker, category in CATEGORY_MARKERS:
        if marker in upper_name:
            return category
    return None


def extract_year(filename: str) -> Optional[int]:
    """提取文件名中的第一处合法 YYYYMMDD 日期。"""

    for match in DATE_PATTERN.finditer(filename):
        year, month, day = (int(value) for value in match.groups())
        try:
            dt.date(year, month, day)
        except ValueError:
            continue
        return year
    return None


def iter_input_files(source: Path) -> Iterable[Path]:
    """枚举目录中的直接文件，并忽略分类子目录和下载残片。"""

    for path in source.iterdir():
        if not path.is_file():
            continue
        if path.name.startswith("._") or path.name.startswith("."):
            continue
        if path.name.endswith(".part"):
            continue
        yield path


def is_existing_path(path: Path) -> bool:
    """同时识别普通路径和断开的符号链接。"""

    return path.exists() or path.is_symlink()


def unique_path(path: Path) -> Path:
    """为冲突文件生成不覆盖原文件的备用路径。"""

    index = 1
    while is_existing_path(path):
        path = path.with_name(f"{path.name}.{index}")
        index += 1
    return path


def validate_paths(source: Path) -> None:
    if not source.exists():
        raise ValueError(f"输入路径不存在：{source}")

    if not source.is_dir():
        raise ValueError(f"输入路径必须是目录：{source}")


def build_plan(source: Path) -> Tuple[list[Tuple[Path, Path]], list[Tuple[Path, str]]]:
    """返回可分类文件计划，以及未分类/缺日期文件和原因。"""

    plan = []
    skipped = []

    for path in sorted(iter_input_files(source), key=lambda item: str(item)):
        category = classify_category(path.name)
        if category is None:
            skipped.append((path, "无法从文件名识别仪器类别"))
            continue

        year = extract_year(path.name)
        if year is None:
            skipped.append((path, "无法从文件名提取 YYYYMMDD 日期"))
            continue

        plan.append((path, source / category / str(year) / path.name))

    return plan, skipped


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="在输入目录内按仪器类别和年份整理风云卫星数据文件。"
    )
    parser.add_argument(
        "source",
        type=Path,
        help="待整理的目录；该目录同时作为分类输出根目录（必选）",
    )

    action = parser.add_mutually_exclusive_group()
    action.add_argument("--move", action="store_true", help="执行移动")
    action.add_argument("--copy", action="store_true", help="执行复制，保留源文件")
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="只显示计划，不修改文件；默认行为",
    )
    parser.add_argument(
        "--conflict",
        choices=("skip", "rename"),
        default="skip",
        help="目标文件已存在时跳过或添加 .1/.2 后缀（默认：skip）",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    source = args.source.expanduser()

    try:
        validate_paths(source)
        plan, skipped = build_plan(source)
    except ValueError as exc:
        print(f"错误：{exc}", file=sys.stderr)
        return 2

    operation = "dry-run"
    if args.move:
        operation = "move"
    elif args.copy:
        operation = "copy"
    if args.dry_run:
        operation = "dry-run"

    planned = 0
    done = 0
    conflicts = 0
    errors = 0

    for source_path, target_path in plan:
        if is_existing_path(target_path):
            conflicts += 1
            if args.conflict == "rename":
                target_path = unique_path(target_path)
            else:
                print(f"跳过（目标已存在）：{source_path} -> {target_path}")
                continue

        planned += 1
        print(f"{operation}: {source_path} -> {target_path}")
        if operation == "dry-run":
            continue

        try:
            target_path.parent.mkdir(parents=True, exist_ok=True)
            if operation == "move":
                shutil.move(str(source_path), str(target_path))
            else:
                shutil.copy2(str(source_path), str(target_path))
            done += 1
        except OSError as exc:
            errors += 1
            print(f"失败：{source_path}：{exc}", file=sys.stderr)

    for path, reason in skipped:
        print(f"跳过（{reason}）：{path}", file=sys.stderr)

    print(
        f"\n统计：计划 {planned}，已处理 {done}，"
        f"目标冲突 {conflicts}，未识别 {len(skipped)}，失败 {errors}"
    )

    if operation == "dry-run":
        print("当前为预览模式；确认计划后加 --move 执行移动，或加 --copy 执行复制。")

    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
