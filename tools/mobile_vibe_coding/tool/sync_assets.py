#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""把电脑端上位机的上下文资产导出到 Flutter assets。

唯一事实来源仍然是 tools/host_vibe_coding：模块目录写在 lua_module_catalog.py 里，
playbook 和模块 README 是仓库里的真文件。手机端打包时读不到仓库，所以在这里
一次性导出成 assets，APK 自带一份快照。

新增模块或改了 README 之后重跑本脚本即可：

    python tool/sync_assets.py

刻意不复制 host_vibe_coding 里的任何 .py：那边的逻辑由 Dart 侧重写，
只有数据（目录表 / playbook / README）需要同步。
"""

from __future__ import annotations

import json
import shutil
import sys
from dataclasses import asdict
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent
HOST_TOOL_DIR = PROJECT_ROOT.parent / "host_vibe_coding"
ASSETS_DIR = PROJECT_ROOT / "assets"

sys.path.insert(0, str(HOST_TOOL_DIR))

import lua_module_catalog as catalog  # noqa: E402
import playbook_registry  # noqa: E402


def export_catalog() -> dict:
    """模块目录导出成 JSON。doc 用组件名，对应 assets/lua_modules/<组件>.md。"""
    modules = []
    for item in catalog.CATALOG:
        modules.append(
            {
                "require": item.require,
                "component": item.component,
                "tier": item.tier,
                "summary": item.summary,
                "keywords": list(item.keywords),
            }
        )
    return {
        "tier_labels": dict(catalog.TIER_LABELS),
        "implied": {k: list(v) for k, v in catalog.IMPLIED_MODULES.items()},
        "per_doc_limit": catalog.DEFAULT_PER_DOC_LIMIT,
        "total_limit": catalog.DEFAULT_TOTAL_LIMIT,
        "modules": modules,
    }


def copy_module_docs() -> list[str]:
    """按组件名复制 README，返回实际导出的组件列表。

    多个 require 共享同一个组件 README（比如 lcd / lcd_touch），
    所以按 component 去重，Dart 侧再按 require -> component 映射回去。
    """
    out_dir = ASSETS_DIR / "lua_modules"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    exported: list[str] = []
    missing: list[str] = []
    for component in sorted({item.component for item in catalog.CATALOG}):
        source = catalog.LUA_MODULES_DIR / component / "README.md"
        if not source.exists():
            missing.append(component)
            continue
        shutil.copyfile(source, out_dir / f"{component}.md")
        exported.append(component)

    if missing:
        print(f"警告：{len(missing)} 个组件没有 README，Dart 侧会跳过：{', '.join(missing)}")
    return exported


def copy_playbooks() -> list[str]:
    out_dir = ASSETS_DIR / "playbooks"
    if out_dir.exists():
        shutil.rmtree(out_dir)
    out_dir.mkdir(parents=True)

    names: list[str] = []
    for source in sorted(playbook_registry.PLAYBOOK_DIR.glob("*.md")):
        shutil.copyfile(source, out_dir / source.name)
        names.append(source.name)
    return names


def main() -> None:
    if not HOST_TOOL_DIR.is_dir():
        raise SystemExit(f"找不到电脑端上位机目录：{HOST_TOOL_DIR}")
    ASSETS_DIR.mkdir(parents=True, exist_ok=True)

    catalog_data = export_catalog()
    (ASSETS_DIR / "catalog.json").write_text(
        json.dumps(catalog_data, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    components = copy_module_docs()
    playbooks = copy_playbooks()

    # Flutter 运行时无法列目录，要靠这份清单才知道有哪些 playbook 可加载。
    manifest = {
        "generated_from": "tools/host_vibe_coding",
        "module_docs": components,
        "playbooks": playbooks,
    }
    (ASSETS_DIR / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )

    print(f"模块条目：{len(catalog_data['modules'])}")
    print(f"README：{len(components)} 份")
    print(f"playbook：{len(playbooks)} 份")


if __name__ == "__main__":
    main()
