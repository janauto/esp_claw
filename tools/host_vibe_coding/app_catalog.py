"""SD-card catalog shared with local_app_store: stable groups of ten apps."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
from typing import Any

GROUP_SIZE = 10
NONE = 0xFFFFFFFF
GROUP_PATTERN = re.compile(r"g[0-9a-f]{8}")


def write_json(path: Path, value: Any) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(value, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)


def read_json(path: Path, default: Any) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, NotADirectoryError, ValueError, UnicodeError):
        return default


def valid_manifest(directory: Path) -> dict | None:
    from app_package import validate_download_manifest
    manifest = read_json(directory / "app.json", None)
    try:
        validate_download_manifest(manifest, directory.name)
        if (directory / "main.lua").stat().st_size != manifest["files"]["main.lua"]["size"]:
            return None
    except (ValueError, TypeError, KeyError, OSError):
        return None
    return manifest


def entry(manifest: dict) -> dict:
    return {**{key: manifest[key] for key in ("app_id", "title", "frame_color", "pill_color")},
            **{key: key + ".bin" in manifest["files"] for key in ("icon_selected", "icon_unselected")}}


def rebuild(root: Path) -> dict:
    """Offline repair/import; firmware only scans on recovery or explicit reload."""
    root.mkdir(parents=True, exist_ok=True)
    (root / ".index-dirty").touch()
    old = read_json(root / "index.json", {})
    groups = sorted(p for p in root.iterdir() if p.is_dir() and GROUP_PATTERN.fullmatch(p.name) and not (p / "app.json").exists())
    tail = int(groups[-1].name[1:], 16) if groups else 0
    # Import legacy flat packages. App directories are renamed, never copied or
    # overwritten. Existing grouped apps retain their physical locations.
    for directory in sorted(root.iterdir()):
        if not directory.is_dir() or directory.name.startswith(".") or not valid_manifest(directory):
            continue
        while True:
            if tail >= NONE:
                raise OverflowError("SD catalog group number overflow")
            target_group = root / f"g{tail:08x}"
            occupied = target_group.exists()
            is_group = occupied and target_group.is_dir() and not (target_group / "app.json").exists()
            if not occupied or is_group:
                target_group.mkdir(exist_ok=True)
                if sum(valid_manifest(p) is not None for p in target_group.iterdir() if p.is_dir()) < GROUP_SIZE:
                    break
            tail += 1
        destination = target_group / directory.name
        if destination.exists():
            raise FileExistsError(destination)
        directory.rename(destination)
    groups = sorted(p for p in root.iterdir() if p.is_dir() and GROUP_PATTERN.fullmatch(p.name) and not (p / "app.json").exists())
    catalog = []
    seen = set()
    for directory in groups:
        previous = read_json(directory / "index.json", {}).get("apps", [])
        manifests = {p.name: m for p in directory.iterdir() if p.is_dir() and (m := valid_manifest(p))}
        ordered = [v["app_id"] for v in previous if v.get("app_id") in manifests]
        ordered.extend(sorted(manifests.keys() - set(ordered)))
        if len(ordered) > GROUP_SIZE or len(ordered) != len(set(ordered)):
            raise ValueError(f"Invalid group: {directory}")
        if seen.intersection(ordered):
            raise ValueError("Duplicate app ID in SD catalog")
        seen.update(ordered)
        if ordered:
            catalog.append({"schema": 1, "group": int(directory.name[1:], 16),
                            "prev": NONE, "next": NONE, "apps": [entry(manifests[key]) for key in ordered]})
    for i, group in enumerate(catalog):
        group["prev"] = catalog[i - 1]["group"] if i else NONE
        group["next"] = catalog[i + 1]["group"] if i + 1 < len(catalog) else NONE
        write_json(root / f"g{group['group']:08x}" / "index.json", group)
    summary = {"schema": 1, "revision": (old.get("revision", 0) + 1) & NONE,
               "group_size": GROUP_SIZE, "count": len(seen), "group_count": len(catalog),
               "next_group": int(groups[-1].name[1:], 16) + 1 if groups else 0,
               "first": {**catalog[0]["apps"][0], "group": catalog[0]["group"]} if catalog else None,
               "last": {**catalog[-1]["apps"][-1], "group": catalog[-1]["group"]} if catalog else None}
    write_json(root / "index.json", summary)
    (root / ".index-dirty").unlink()
    return summary


def export_package(package: Any, root: Path) -> Path:
    summary = rebuild(root)
    app_id = package.manifest["app_id"]
    for group in root.glob("g????????"):
        if (group / app_id).exists():
            raise FileExistsError(group / app_id)
    group_id = summary["next_group"]
    if summary["last"]:
        last_id = summary["last"]["group"]
        group = read_json(root / f"g{last_id:08x}" / "index.json", {})
        if len(group["apps"]) < GROUP_SIZE:
            group_id = last_id
    if group_id >= NONE:
        raise OverflowError("SD catalog group number overflow")
    destination = root / f"g{group_id:08x}" / app_id
    staging = root / (".export-" + app_id)
    staging.mkdir(exist_ok=False)
    for name, data in package.files.items():
        (staging / name).write_bytes(data)
    write_json(staging / "app.json", package.manifest)
    (root / ".index-dirty").touch()
    destination.parent.mkdir(exist_ok=True)
    staging.rename(destination)
    rebuild(root)
    return destination
