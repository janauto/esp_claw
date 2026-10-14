#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""Playbook 注册表：对标设备端 claw_skill 的 SKILL.md 机制。

设备端的做法（components/claw_modules/claw_skill）：
* 扫描目录，解析每个 SKILL.md 的 frontmatter，建立 name + summary 的目录；
* 常驻 prompt 里只放这份紧凑目录（claw_skill_read_skills_list）；
* LLM 选中某个 skill 后才读全文（claw_skill_read_document）。

上位机这边不需要 LLM 主动调工具去激活——规划阶段已经产出了 task_kind / ui_stack /
modules，用它们直接匹配即可，省一次往返。匹配规则写在 frontmatter 的 match 字段里，
所以新增一类任务经验只要加一个 .md 文件，不用改 Python。
"""

from __future__ import annotations

import json
import re
from dataclasses import dataclass, field
from pathlib import Path

PLAYBOOK_DIR = Path(__file__).resolve().parent / "playbooks"

#: 单个 playbook 正文的字符上限，防止某个文件写太长挤掉 README。
MAX_PLAYBOOK_CHARS = 9000

_FRONTMATTER_RE = re.compile(r"^---\s*\n(.*?)\n---\s*\n", re.DOTALL)


@dataclass(frozen=True)
class PlaybookMatch:
    """frontmatter 里的 match 条件。任一维度为空表示该维度不限制。

    同一维度内是 OR，不同维度之间是 AND。
    """

    task_kind: tuple[str, ...] = ()
    ui_stack: tuple[str, ...] = ()
    modules: tuple[str, ...] = ()

    def score(self, task_kind: str, ui_stack: str, modules: list[str]) -> int:
        """返回匹配得分；0 表示不匹配。得分高的优先，用于挑最贴合的 playbook。"""
        points = 0
        if self.task_kind:
            if task_kind not in self.task_kind:
                return 0
            points += 4
        if self.ui_stack:
            if ui_stack not in self.ui_stack:
                return 0
            points += 3
        if self.modules:
            hit = [m for m in self.modules if m in modules]
            if not hit:
                return 0
            points += len(hit)
        return points or 1


@dataclass(frozen=True)
class Playbook:
    """一份任务类型专属的实现规范。"""

    name: str
    description: str
    path: Path
    match: PlaybookMatch
    priority: int = 50
    always: bool = False
    _body: str = field(default="", repr=False)

    def body(self) -> str:
        text = self._body.strip()
        if len(text) > MAX_PLAYBOOK_CHARS:
            text = text[:MAX_PLAYBOOK_CHARS].rstrip() + "\n...[playbook 截断]"
        return text


def _parse_frontmatter(text: str) -> tuple[dict, str]:
    """解析 JSON frontmatter，返回 (元数据, 正文)。

    用 JSON 而不是 YAML：标准库就能解析，不给上位机引入额外依赖。
    """
    found = _FRONTMATTER_RE.match(text)
    if not found:
        return {}, text
    try:
        meta = json.loads(found.group(1))
    except json.JSONDecodeError:
        return {}, text
    if not isinstance(meta, dict):
        return {}, text
    return meta, text[found.end():]


def _as_tuple(raw: object) -> tuple[str, ...]:
    if isinstance(raw, str):
        return (raw.strip().lower(),) if raw.strip() else ()
    if isinstance(raw, list):
        return tuple(item.strip().lower() for item in raw if isinstance(item, str) and item.strip())
    return ()


def load_registry(directory: Path | None = None) -> list[Playbook]:
    """扫描目录建立 playbook 注册表。对标 claw_skill_reload_registry()。"""
    root = directory or PLAYBOOK_DIR
    if not root.is_dir():
        return []

    registry: list[Playbook] = []
    for path in sorted(root.glob("*.md")):
        try:
            raw = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        meta, body = _parse_frontmatter(raw)
        name = str(meta.get("name") or path.stem).strip()
        if not name or not body.strip():
            continue
        match_raw = meta.get("match") if isinstance(meta.get("match"), dict) else {}
        try:
            priority = int(meta.get("priority", 50))
        except (TypeError, ValueError):
            priority = 50
        registry.append(
            Playbook(
                name=name,
                description=str(meta.get("description") or "").strip(),
                path=path,
                match=PlaybookMatch(
                    task_kind=_as_tuple(match_raw.get("task_kind")),
                    ui_stack=_as_tuple(match_raw.get("ui_stack")),
                    modules=_as_tuple(match_raw.get("modules")),
                ),
                priority=priority,
                always=bool(meta.get("always")),
                _body=body,
            )
        )
    return registry


def catalog_overview(registry: list[Playbook]) -> str:
    """紧凑目录，对标 claw_skill_read_skills_list()。"""
    return "\n".join(f"- {pb.name}: {pb.description}" for pb in registry if pb.description)


def select(
    registry: list[Playbook],
    *,
    task_kind: str,
    ui_stack: str,
    modules: list[str],
    limit: int = 3,
) -> list[Playbook]:
    """挑出适用的 playbook，按匹配度排序。always=true 的无条件入选。"""
    always = [pb for pb in registry if pb.always]
    scored: list[tuple[int, int, Playbook]] = []
    for pb in registry:
        if pb.always:
            continue
        score = pb.match.score(task_kind, ui_stack, modules)
        if score > 0:
            scored.append((-score, pb.priority, pb))

    scored.sort(key=lambda item: (item[0], item[1]))
    picked = always + [pb for _, _, pb in scored[:limit]]
    return picked


def render(playbooks: list[Playbook]) -> str:
    """把选中的 playbook 正文拼成一段上下文。"""
    blocks = [f"### {pb.name}\n\n{pb.body()}" for pb in playbooks if pb.body()]
    return "\n\n".join(blocks)
