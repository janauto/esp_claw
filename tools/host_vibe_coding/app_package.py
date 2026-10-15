"""Persistent carousel apps: fixed package layout and acknowledged BLE uploads."""
from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import re
import struct
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

APP_GROUP_SIZE = 10
CHUNK_SIZE = 1024
ICON_SIDE = 256
ICON_HEADER = struct.pack("<BBHHHHH", 0x19, 0x12, 0, ICON_SIDE, ICON_SIDE, ICON_SIDE * 2, 0)
ICON_SIZE = len(ICON_HEADER) + ICON_SIDE * ICON_SIDE * 2
DEFAULT_COLOR = 0xEDC22E


@dataclass
class AppPackage:
    manifest: dict[str, Any]
    files: dict[str, bytes]

    def export(self, apps_dir: Path) -> Path:
        """Export into a card's apps/ directory; refuse to overwrite an app."""
        from app_catalog import export_package
        return export_package(self, apps_dir)


def validate_identity(app_id: str, title: str) -> None:
    if not re.fullmatch(r"[a-z0-9_-]{1,32}", app_id):
        raise ValueError("应用 ID 只能包含小写字母、数字、下划线和短横线，长度为 1–32")
    if not title.strip() or len(title.encode("utf-8")) > 95 or any(ord(c) < 32 or ord(c) == 127 for c in title):
        raise ValueError("应用名称不能为空、不能包含控制字符，且 UTF-8 编码不能超过 95 字节")


def image_to_bin(image: Any) -> bytes:
    """LVGL v9 little-endian RGB565, black backdrop, no compression."""
    pixels = bytearray()
    for r, g, b in struct.iter_unpack("BBB", image.convert("RGB").tobytes()):
        pixels.extend(struct.pack("<H", ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)))
    if image.size != (ICON_SIDE, ICON_SIDE):
        raise ValueError("图标必须为 256×256")
    return ICON_HEADER + pixels


def load_icon(path: Path) -> bytes:
    if path.suffix.lower() == ".bin":
        data = path.read_bytes()
        if len(data) != ICON_SIZE or data[:12] != ICON_HEADER:
            raise ValueError("BIN 图标必须是未压缩的 LVGL v9 RGB565 256×256 图像")
        return data
    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise RuntimeError("导入图片需要 Pillow，请安装 requirements.txt") from exc
    with Image.open(path) as source:
        image = ImageOps.exif_transpose(source).convert("RGBA")
        image = ImageOps.contain(image, (228, 228), Image.Resampling.LANCZOS)
        canvas = Image.new("RGB", (ICON_SIDE, ICON_SIDE), "black")
        canvas.paste(image, ((ICON_SIDE - image.width) // 2, (ICON_SIDE - image.height) // 2), image)
        return image_to_bin(canvas)


def dim_icon(data: bytes) -> bytes:
    """Same geometry, 42% brightness, matching the firmware's idle artwork."""
    pixels = bytearray()
    for (pixel,) in struct.iter_unpack("<H", data[12:]):
        r = round(((pixel >> 11) & 31) * 0.42)
        g = round(((pixel >> 5) & 63) * 0.42)
        b = round((pixel & 31) * 0.42)
        pixels.extend(struct.pack("<H", (r << 11) | (g << 5) | b))
    return ICON_HEADER + pixels


def build_package(script: str, app_id: str, title: str, *, selected: Path | None = None,
                  unselected: Path | None = None, frame_color: int = DEFAULT_COLOR,
                  pill_color: int | None = None) -> AppPackage:
    validate_identity(app_id, title)
    pill_color = frame_color if pill_color is None else pill_color
    if not all(isinstance(c, int) and 0 <= c <= 0xFFFFFF for c in (frame_color, pill_color)):
        raise ValueError("颜色必须是 000000–FFFFFF 的 RGB 值")
    data = (script.rstrip() + "\n").encode("utf-8")
    if not script.strip() or len(data) > 64 * 1024:
        raise ValueError("Lua 脚本不能为空，大小不能超过 64 KiB")
    if unselected and not selected:
        raise ValueError("请先选择选中图标；未选中图标可留空以自动生成暗色版本")
    files = {"main.lua": data}
    if selected:
        files["icon_selected.bin"] = load_icon(selected)
        files["icon_unselected.bin"] = load_icon(unselected) if unselected else dim_icon(files["icon_selected.bin"])
    manifest = {"schema": 1, "app_id": app_id, "title": title.strip(), "frame_color": frame_color,
                "pill_color": pill_color, "files": {
                    name: {"size": len(value), "sha256": hashlib.sha256(value).hexdigest()}
                    for name, value in files.items()}}
    return AppPackage(manifest, files)


async def app_request(bridge: Any, op: str, **fields: Any) -> Any:
    result = await bridge.request({"id": "app-" + uuid.uuid4().hex[:12], "op": op, **fields}, timeout=60)
    if not result.get("ok"):
        error = result.get("error") or result.get("data") or result
        hints = {"ESP_ERR_INVALID_STATE": "同名应用已存在，或设备正在运行应用；请先停止／删除后重试",
                 "ESP_ERR_NO_MEM": "设备内存不足",
                 "ESP_ERR_INVALID_CRC": "文件校验失败，请重新安装",
                 "unknown_op": "当前固件不支持应用安装，请先更新固件"}
        raise RuntimeError(f"{op}: {hints.get(str(error), error)}")
    data = result.get("data", {})
    data = json.loads(data) if isinstance(data, str) and data else data
    if op == "app_list" and "offset" not in fields and isinstance(data, dict):
        # New firmware bounds each response to one group. Older firmware's
        # array response is accepted unchanged.
        items = list(data["apps"])
        previous = -1
        while data.get("next_offset") is not None:
            offset = data["next_offset"]
            if type(offset) is not int or offset <= previous:
                raise RuntimeError("设备返回了无效的应用分页位置")
            previous = offset
            data = await app_request(bridge, op, offset=offset)
            items.extend(data["apps"])
        return items
    return data


def validate_download_manifest(manifest: Any, app_id: str) -> None:
    if not isinstance(manifest, dict) or manifest.get("schema") != 1 or manifest.get("app_id") != app_id:
        raise ValueError("设备返回的应用配置不匹配")
    if not isinstance(manifest.get("title"), str):
        raise ValueError("设备返回的应用名称无效")
    validate_identity(app_id, manifest["title"])
    if any(type(manifest.get(key)) is not int or not 0 <= manifest[key] <= 0xFFFFFF
           for key in ("frame_color", "pill_color")):
        raise ValueError("设备返回的应用颜色无效")
    files = manifest.get("files")
    if not isinstance(files, dict) or "main.lua" not in files:
        raise ValueError("设备应用缺少入口脚本")
    for name, spec in files.items():
        if name not in {"main.lua", "icon_selected.bin", "icon_unselected.bin"} or not isinstance(spec, dict):
            raise ValueError("设备应用包含不支持的文件名")
        size, digest = spec.get("size"), spec.get("sha256")
        if type(size) is not int or not (1 <= size <= 64 * 1024 if name == "main.lua" else size == ICON_SIZE):
            raise ValueError("设备应用文件大小无效")
        if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("设备应用文件摘要无效")


async def download_app(bridge: Any, app_id: str, report: Callable[[str], None] = lambda _: None) -> AppPackage:
    """Read configuration, code and icons without running the device app."""
    validate_identity(app_id, "app")
    info = await app_request(bridge, "app_info", app_id=app_id)
    manifest = info.get("manifest") if isinstance(info, dict) else None
    validate_download_manifest(manifest, app_id)
    files = {}
    for name, spec in manifest["files"].items():
        report(f"正在读取 {name}（{spec['size']} 字节）")
        data = bytearray()
        while len(data) < spec["size"]:
            offset = len(data)
            length = min(CHUNK_SIZE, spec["size"] - offset)
            result = await app_request(bridge, "app_read", app_id=app_id, file=name,
                                       offset=offset, length=length, sha256=spec["sha256"])
            if (not isinstance(result, dict) or result.get("file") != name or
                result.get("offset") != offset or result.get("size") != spec["size"]):
                raise RuntimeError(f"{name}: 设备返回了不匹配的文件分块")
            try:
                chunk = base64.b64decode(result["data"], validate=True)
            except (KeyError, ValueError, TypeError) as exc:
                raise RuntimeError(f"{name}: 文件分块编码无效") from exc
            if len(chunk) != length or result.get("eof") is not (offset + length == spec["size"]):
                raise RuntimeError(f"{name}: 文件分块大小无效或提前结束")
            data.extend(chunk)
        if hashlib.sha256(data).hexdigest() != spec["sha256"]:
            raise RuntimeError(f"{name}: 下载校验失败，请重新获取应用信息")
        if name.endswith(".bin") and data[:12] != ICON_HEADER:
            raise RuntimeError(f"{name}: 图标格式无效")
        files[name] = bytes(data)
    report("应用配置、代码和图标读取完成")
    return AppPackage(manifest, files)


async def install_app(bridge: Any, package: AppPackage, report: Callable[[str], None] = lambda _: None) -> Any:
    hello = await app_request(bridge, "hello")
    required = {"app_begin", "app_write", "app_commit", "app_abort"}
    if not isinstance(hello, dict) or not required.issubset(hello.get("ops", [])):
        raise RuntimeError("当前固件不支持应用安装，请先更新固件")
    app_id = package.manifest["app_id"]
    await app_request(bridge, "app_begin", manifest=package.manifest)
    try:
        for name, data in package.files.items():
            report(f"正在安装 {name}（{len(data)} 字节）")
            for offset in range(0, len(data), CHUNK_SIZE):
                await app_request(bridge, "app_write", app_id=app_id, file=name, offset=offset,
                                  data=base64.b64encode(data[offset:offset + CHUNK_SIZE]).decode("ascii"))
                if offset // CHUNK_SIZE % 16 == 0:
                    report(f"{name}: {min(offset + CHUNK_SIZE, len(data))}/{len(data)}")
        result = await app_request(bridge, "app_commit", app_id=app_id)
    except (Exception, asyncio.CancelledError):
        try:
            await app_request(bridge, "app_abort", app_id=app_id)
        except Exception:
            pass  # A disconnected staging directory is ignored on boot and reset on retry.
        raise
    report("应用已固化；返回设备主页后会刷新应用列表")
    return result
