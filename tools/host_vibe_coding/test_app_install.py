"""Exercise host packages against the real firmware store on a temporary disk.

Requires a C compiler, OpenSSL headers and the project's managed cJSON sources.
No BLE radio or ESP hardware is needed. Run: python -m pytest test_app_install.py
"""
import asyncio
import base64
import ctypes as ct
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import pytest
from PIL import Image

from app_package import (APP_GROUP_SIZE, CHUNK_SIZE, ICON_HEADER, ICON_SIZE, app_request,
                         build_package, dim_icon, download_app, install_app, load_icon)

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = 'local lvgl = require("lvgl")\nprint("你好")\nlvgl.run()\n'


@pytest.fixture(scope="session")
def firmware(tmp_path_factory):
    component = ROOT / "components/common/local_app_store"
    cjson = ROOT / "application/edge_agent/managed_components/espressif__cjson/cJSON"
    library = tmp_path_factory.mktemp("app-store") / "store.so"
    import os
    cjson = Path(os.environ.get("APP_TEST_CJSON", str(cjson)))
    openssl = Path(os.environ.get("APP_TEST_OPENSSL", "/opt/homebrew/opt/openssl@3"))
    subprocess.run(["cc", "-DLOCAL_APP_HOST", "-DLOCAL_APP_TEST_IO", "-I" + str(openssl / "include"), "-L" + str(openssl / "lib"), "-shared", "-fPIC", "-Wall", "-Wextra", "-Werror", "-g",
                    "-I" + str(component / "include"), "-I" + str(component / "test_host"),
                    "-I" + str(cjson), str(component / "src/local_app_store.c"), str(component / "src/local_app_index.c"), str(cjson / "cJSON.c"),
                    str(component / "test_host/io_probe.c"), str(component / "test_host/bridge_probe.c"),
                    "-I" + str(ROOT / "application/edge_agent/components/ble_host_bridge/src"),
                    str(ROOT / "application/edge_agent/components/ble_host_bridge/src/ble_host_apps.c"), 
                    "-lcrypto", "-o", str(library)], check=True)
    lib = ct.CDLL(str(library))
    lib.cJSON_Parse.argtypes = [ct.c_char_p]
    lib.cJSON_Parse.restype = ct.c_void_p
    lib.cJSON_Delete.argtypes = [ct.c_void_p]
    lib.cJSON_PrintUnformatted.argtypes = [ct.c_void_p]
    lib.cJSON_PrintUnformatted.restype = ct.c_void_p
    lib.cJSON_free.argtypes = [ct.c_void_p]
    lib.local_app_store_create.argtypes = [ct.c_void_p, ct.POINTER(ct.c_void_p)]
    lib.local_app_store_delete.argtypes = [ct.c_void_p]
    lib.local_app_store_begin.argtypes = [ct.c_void_p, ct.c_void_p]
    lib.local_app_store_list.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_void_p)]
    lib.local_app_store_at.argtypes = [ct.c_void_p, ct.c_size_t, ct.POINTER(ct.c_void_p)]
    lib.local_app_store_summary.argtypes = [ct.c_void_p, ct.POINTER(ct.c_void_p)]
    lib.local_app_store_directory.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_void_p, ct.c_size_t]
    lib.local_app_store_reload.argtypes = [ct.c_void_p]
    lib.local_app_store_get.argtypes = [ct.c_void_p, ct.c_char_p, ct.POINTER(ct.c_void_p)]
    lib.local_app_store_read.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_char_p, ct.c_char_p,
                                        ct.c_size_t, ct.c_void_p, ct.c_size_t,
                                        ct.POINTER(ct.c_size_t), ct.POINTER(ct.c_size_t)]
    lib.local_app_store_write.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_char_p, ct.c_size_t, ct.c_void_p, ct.c_size_t]
    for name in ("commit", "abort", "remove"):
        getattr(lib, "local_app_store_" + name).argtypes = [ct.c_void_p, ct.c_char_p]
    lib.local_app_store_rename.argtypes = [ct.c_void_p, ct.c_char_p, ct.c_char_p]
    return lib


class FirmwareBridge:
    """Adapt JSON BLE commands to the production storage implementation."""
    def __init__(self, lib, root):
        self.lib = lib
        self.handle = ct.c_void_p()
        self.root = root
        self.calls = []
        config = ct.c_char_p(str(root).encode())
        assert lib.local_app_store_create(ct.byref(config), ct.byref(self.handle)) == 0

    def close(self):
        self.lib.local_app_store_delete(self.handle)

    def begin(self, manifest):
        obj = self.lib.cJSON_Parse(json.dumps(manifest).encode())
        try:
            return self.lib.local_app_store_begin(self.handle, obj)
        finally:
            self.lib.cJSON_Delete(obj)

    def page(self, offset=0):
        obj = ct.c_void_p()
        assert self.lib.local_app_store_list(self.handle, offset, ct.byref(obj)) == 0
        text = self.lib.cJSON_PrintUnformatted(obj)
        try:
            return json.loads(ct.string_at(text))
        finally:
            self.lib.cJSON_free(text)
            self.lib.cJSON_Delete(obj)

    def listing(self):
        result, offset = [], 0
        while True:
            page = self.page(offset)
            result.extend(page["apps"])
            offset = page["next_offset"]
            if offset is None:
                return result

    def directory(self, app_id):
        path = ct.create_string_buffer(256)
        assert self.lib.local_app_store_directory(self.handle, app_id.encode(), path, len(path)) == 0
        return self.root / path.value.decode()

    def rename(self, app_id, title):
        return self.lib.local_app_store_rename(self.handle, app_id.encode(), title.encode())

    def indexed(self, position=None):
        obj = ct.c_void_p()
        error = (self.lib.local_app_store_summary(self.handle, ct.byref(obj)) if position is None else
                 self.lib.local_app_store_at(self.handle, position, ct.byref(obj)))
        assert error == 0
        text = self.lib.cJSON_PrintUnformatted(obj)
        try:
            return json.loads(ct.string_at(text))
        finally:
            self.lib.cJSON_free(text)
            self.lib.cJSON_Delete(obj)

    async def request(self, command, timeout=None):
        self.calls.append(command)
        op = command["op"]
        app_id = command.get("app_id", "").encode()
        if op == "hello":
            return {"ok": True, "data": {"ops": ["app_begin", "app_write", "app_commit", "app_abort"]}}
        if op == "app_list":
            return {"ok": True, "data": json.dumps(self.page(command.get("offset", 0)))}
        if op == "app_info":
            obj = ct.c_void_p()
            error = self.lib.local_app_store_get(self.handle, app_id, ct.byref(obj))
            if error:
                return {"ok": False, "data": f"error={error}"}
            text = self.lib.cJSON_PrintUnformatted(obj)
            try:
                manifest = json.loads(ct.string_at(text))
                directory = "apps/" + str(self.directory(command["app_id"]).relative_to(self.root))
                result = {"source": "user", "storage_root": str(self.root.parent),
                          "directory": directory, "script": directory + "/main.lua", "manifest": manifest}
                for name in ("icon_selected", "icon_unselected"):
                    result[name] = f"{directory}/{name}.bin" if name + ".bin" in manifest["files"] else None
                return {"ok": True, "data": json.dumps(result)}
            finally:
                self.lib.cJSON_free(text)
                self.lib.cJSON_Delete(obj)
        if op == "app_read":
            data = ct.create_string_buffer(CHUNK_SIZE)
            count, total = ct.c_size_t(), ct.c_size_t()
            error = self.lib.local_app_store_read(self.handle, app_id, command["file"].encode(), command["sha256"].encode(),
                                                 command["offset"], data, command["length"], ct.byref(count), ct.byref(total))
            if error:
                return {"ok": False, "data": f"error={error}"}
            return {"ok": True, "data": {"file": command["file"], "offset": command["offset"],
                    "size": total.value, "eof": command["offset"] + count.value == total.value,
                    "data": base64.b64encode(data.raw[:count.value]).decode()}}
        if op == "app_begin":
            error = self.begin(command["manifest"])
        elif op == "app_write":
            data = base64.b64decode(command["data"])
            error = self.lib.local_app_store_write(self.handle, app_id, command["file"].encode(), command["offset"], data, len(data))
        else:
            error = getattr(self.lib, "local_app_store_" + op[4:])(self.handle, app_id)
        return {"ok": error == 0, "data": "{}" if error == 0 else f"error={error}"}


@pytest.fixture
def bridge(firmware, tmp_path):
    instance = FirmwareBridge(firmware, tmp_path / "apps")
    yield instance
    instance.close()


def test_install_reboot_list_remove(bridge, firmware):
    package = build_package(SCRIPT, "clock", "时钟")
    asyncio.run(install_app(bridge, package))
    assert (bridge.directory("clock") / "main.lua").read_text() == SCRIPT
    fresh = FirmwareBridge(firmware, bridge.root)
    try:
        assert fresh.listing() == [package.manifest]
        assert fresh.begin(package.manifest) != 0  # No overwrite.
        directory = fresh.directory("clock")
        asyncio.run(app_request(fresh, "app_remove", app_id="clock"))
        assert fresh.listing() == []
        assert not directory.exists()
    finally:
        fresh.close()


def test_custom_icons_roundtrip(bridge, tmp_path):
    image = tmp_path / "art.png"
    Image.new("RGBA", (90, 160), (40, 180, 255, 180)).save(image)
    package = build_package(SCRIPT, "custom", "Custom", selected=image)
    asyncio.run(install_app(bridge, package))
    for name, data in package.files.items():
        assert (bridge.directory("custom") / name).read_bytes() == data
        assert hashlib.sha256(data).hexdigest() == package.manifest["files"][name]["sha256"]
    selected = package.files["icon_selected.bin"]
    normal = package.files["icon_unselected.bin"]
    assert len(selected) == ICON_SIZE and selected[:12] == ICON_HEADER
    assert selected != normal
    assert all(b <= a for (a,), (b,) in zip(struct.iter_unpack("<H", selected[12:]), struct.iter_unpack("<H", normal[12:])))
    assert max(len(base64.b64decode(c["data"])) for c in bridge.calls if c["op"] == "app_write") <= CHUNK_SIZE


def test_interrupted_corrupt_out_of_order_and_retry(bridge):
    package = build_package(SCRIPT, "broken", "Broken")
    assert bridge.begin(package.manifest) == 0
    assert bridge.listing() == []
    lib, handle = bridge.lib, bridge.handle
    assert lib.local_app_store_commit(handle, b"broken") != 0
    data = package.files["main.lua"]
    assert lib.local_app_store_write(handle, b"broken", b"main.lua", 2, data, len(data)) != 0
    assert lib.local_app_store_write(handle, b"broken", b"../escape", 0, b"x", 1) != 0
    assert lib.local_app_store_write(handle, b"broken", b"main.lua", 0, b"x" * len(data), len(data)) == 0
    assert lib.local_app_store_commit(handle, b"broken") != 0  # Exact length but wrong SHA.
    assert bridge.listing() == []
    asyncio.run(install_app(bridge, package))  # Begin resets stale staging files.
    assert len(bridge.listing()) == 1


def test_invalid_manifest_and_multiple_groups(bridge):
    for app_id in ("../escape", "a/b", ".hidden", "UPPER", "x" * 33):
        m = build_package(SCRIPT, "valid", "Valid").manifest
        m["app_id"] = app_id
        assert bridge.begin(m) != 0
    m = build_package(SCRIPT, "valid", "Valid").manifest
    m["files"]["../escape"] = m["files"]["main.lua"]
    assert bridge.begin(m) != 0
    count = APP_GROUP_SIZE * 3 + 1
    for i in range(count):
        asyncio.run(install_app(bridge, build_package(SCRIPT, f"app{i}", str(i))))
    assert [m["app_id"] for m in bridge.listing()] == [f"app{i}" for i in range(count)]
    assert [len(bridge.page(i)["apps"]) for i in (0, 10, 20, 30)] == [10, 10, 10, 1]
    summary = bridge.indexed()
    assert summary["count"] == count and summary["group_count"] == 4
    assert summary["first"]["app_id"] == "app0" and summary["last"]["app_id"] == "app30"
    assert bridge.directory("app9").parent.name == "g00000000"
    assert bridge.directory("app10").parent.name == "g00000001"
    assert len(asyncio.run(app_request(bridge, "app_list"))) == count


def test_export_manual_scan_and_invalid_app(bridge):
    package = build_package(SCRIPT, "manual", "Manual")
    target = package.export(bridge.root)
    assert bridge.listing() == [package.manifest]
    with pytest.raises(FileExistsError):
        package.export(bridge.root)
    (target / "main.lua").write_text("truncated")
    assert bridge.listing() == []
    (target / "app.json").write_text("invalid json")
    assert bridge.listing() == []


def test_commit_rejects_invalid_icon_header(bridge):
    package = build_package(SCRIPT, "bad_icon", "Bad icon")
    data = bytes(ICON_SIZE)
    package.files["icon_selected.bin"] = data
    package.manifest["files"]["icon_selected.bin"] = {"size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    with pytest.raises(RuntimeError):
        asyncio.run(install_app(bridge, package))
    assert bridge.listing() == []
    assert not (bridge.root / ".install-bad_icon").exists()


def test_old_firmware_never_starts_install():
    class OldBridge:
        async def request(self, command, timeout=None):
            assert command["op"] == "hello"
            return {"ok": True, "data": '{"ops":["put_lua","run_lua"]}'}
    with pytest.raises(RuntimeError, match="更新固件"):
        asyncio.run(install_app(OldBridge(), build_package(SCRIPT, "test", "Test")))


def test_host_validation_and_bin_import(tmp_path):
    for app_id in ("../x", "Bad", "", "x" * 33):
        with pytest.raises(ValueError):
            build_package(SCRIPT, app_id, "App")
    for title in ("", "\n", "中" * 32):
        with pytest.raises(ValueError):
            build_package(SCRIPT, "test", title)
    for filename in ("app_selected.bin", "app_unselected.bin"):
        data = load_icon(Path(__file__).parent / "assets" / filename)
        assert len(data) == ICON_SIZE
    malformed = tmp_path / "bad.bin"
    malformed.write_bytes(ICON_HEADER)
    with pytest.raises(ValueError):
        load_icon(malformed)


def test_info_and_download_preserve_configuration_code_and_icons(bridge, tmp_path):
    icon = Path(__file__).parent / "assets/app_selected.bin"
    script = SCRIPT + "-- 用户注释" * CHUNK_SIZE + "\n"
    package = build_package(script, "personal", "用户应用", selected=icon, frame_color=0x123456, pill_color=0xFEDCBA)
    asyncio.run(install_app(bridge, package))
    info = asyncio.run(app_request(bridge, "app_info", app_id="personal"))
    assert info["source"] == "user" and info["manifest"] == package.manifest
    assert info["script"] == "apps/g00000000/personal/main.lua"
    assert info["icon_selected"] == "apps/g00000000/personal/icon_selected.bin"
    downloaded = asyncio.run(download_app(bridge, "personal"))
    assert downloaded == package
    destination = downloaded.export(tmp_path / "downloaded-apps")
    assert (destination / "main.lua").read_text() == script
    assert all(call["length"] <= CHUNK_SIZE for call in bridge.calls if call["op"] == "app_read")


def test_rename_updates_title_without_moving_app(bridge):
    package = build_package(SCRIPT, "stable_id", "原名称")
    asyncio.run(install_app(bridge, package))
    directory = bridge.directory("stable_id")
    assert bridge.rename("stable_id", "新名称") == 0
    info = asyncio.run(app_request(bridge, "app_info", app_id="stable_id"))
    assert info["manifest"]["title"] == "新名称"
    assert bridge.directory("stable_id") == directory
    assert (directory / "main.lua").exists()
    assert bridge.rename("stable_id", "   ") != 0
    assert bridge.rename("stable_id", "") != 0


def test_read_default_icons_missing_apps_and_paths(bridge):
    package = build_package(SCRIPT, "plain", "Plain")
    asyncio.run(install_app(bridge, package))
    info = asyncio.run(app_request(bridge, "app_info", app_id="plain"))
    assert info["icon_selected"] is None and info["icon_unselected"] is None
    assert asyncio.run(download_app(bridge, "plain")) == package
    for app_id in ("missing", "game_2048", "../system", ".install-plain"):
        with pytest.raises(RuntimeError):
            asyncio.run(app_request(bridge, "app_info", app_id=app_id))
    spec = package.manifest["files"]["main.lua"]
    for file in ("../plain/main.lua", "app.json", "icon_selected.bin"):
        with pytest.raises(RuntimeError):
            asyncio.run(app_request(bridge, "app_read", app_id="plain", file=file, sha256=spec["sha256"], offset=0, length=1))
    eof = asyncio.run(app_request(bridge, "app_read", app_id="plain", file="main.lua",
                                  sha256=spec["sha256"], offset=spec["size"], length=1))
    assert eof["eof"] and eof["data"] == ""
    for offset, length, sha in ((spec["size"] + 1, 1, spec["sha256"]), (0, CHUNK_SIZE + 1, spec["sha256"]), (0, 1, "0" * 64)):
        with pytest.raises(RuntimeError):
            asyncio.run(app_request(bridge, "app_read", app_id="plain", file="main.lua", sha256=sha, offset=offset, length=length))


def test_download_detects_same_size_corruption_and_manifest_change(bridge):
    package = build_package(SCRIPT, "edited", "Edited")
    asyncio.run(install_app(bridge, package))
    script = bridge.directory("edited") / "main.lua"
    script.write_bytes(b"x" * package.manifest["files"]["main.lua"]["size"])
    with pytest.raises(RuntimeError, match="下载校验失败"):
        asyncio.run(download_app(bridge, "edited"))
    # Even a valid replacement must not be mixed into an old download.
    manifest = package.manifest
    old_hash = manifest["files"]["main.lua"]["sha256"]
    manifest["files"]["main.lua"]["sha256"] = hashlib.sha256(script.read_bytes()).hexdigest()
    (bridge.directory("edited") / "app.json").write_text(json.dumps(manifest))
    with pytest.raises(RuntimeError):
        asyncio.run(app_request(bridge, "app_read", app_id="edited", file="main.lua", sha256=old_hash, offset=0, length=10))


@pytest.mark.parametrize("bad", ["offset", "truncated", "eof", "filename"])
def test_download_rejects_mismatched_chunks(bridge, bad):
    package = build_package(SCRIPT, "read", "Read")
    asyncio.run(install_app(bridge, package))
    class CorruptBridge:
        async def request(self, command, timeout=None):
            result = await bridge.request(command, timeout)
            if command["op"] == "app_read":
                if bad == "offset": result["data"]["offset"] += 1
                if bad == "truncated": result["data"]["data"] = ""
                if bad == "eof": result["data"]["eof"] = False
                if bad == "filename": result["data"]["file"] = "../escape"
            return result
    with pytest.raises(RuntimeError):
        asyncio.run(download_app(CorruptBridge(), "read"))


def test_sparse_groups_endpoints_and_reboot(bridge, firmware):
    count = APP_GROUP_SIZE * 3
    for i in range(count):
        asyncio.run(install_app(bridge, build_package(SCRIPT, f"a{i:03}", str(i))))
    for i in [*range(10, 20), 0, 29]:
        asyncio.run(app_request(bridge, "app_remove", app_id=f"a{i:03}"))
    expected = [f"a{i:03}" for i in [*range(1, 10), *range(20, 29)]]
    assert [bridge.indexed(i)["app_id"] for i in range(len(expected))] == expected
    assert [bridge.indexed(i)["app_id"] for i in reversed(range(len(expected)))] == expected[::-1]
    assert bridge.indexed()["group_count"] == 2
    assert bridge.indexed()["first"]["app_id"] == expected[0]
    assert bridge.indexed()["last"]["app_id"] == expected[-1]
    fresh = FirmwareBridge(firmware, bridge.root)
    try:
        assert [a["app_id"] for a in fresh.listing()] == expected
        for app_id in expected:
            asyncio.run(app_request(fresh, "app_remove", app_id=app_id))
        assert fresh.indexed()["count"] == 0
        assert fresh.indexed()["first"] is None and fresh.indexed()["last"] is None
        asyncio.run(install_app(fresh, build_package(SCRIPT, "again", "Again")))
        assert fresh.indexed(0)["app_id"] == "again"
    finally:
        fresh.close()


def test_legacy_import_dirty_recovery_and_more_than_255_apps(bridge, firmware):
    for i in range(267):
        package = build_package(SCRIPT, f"legacy{i:03}", str(i))
        directory = bridge.root / package.manifest["app_id"]
        directory.mkdir(parents=True)
        (directory / "app.json").write_text(json.dumps(package.manifest))
        (directory / "main.lua").write_bytes(package.files["main.lua"])
    assert bridge.indexed()["count"] == 267
    assert bridge.indexed(266)["directory"].startswith("g0000001a/")
    before = [bridge.indexed(i)["app_id"] for i in range(267)]
    (bridge.root / ".index-dirty").touch()
    (bridge.root / "index.json").write_text("incomplete")
    fresh = FirmwareBridge(firmware, bridge.root)
    try:
        assert fresh.indexed()["count"] == 267
        assert [fresh.indexed(i)["app_id"] for i in range(267)] == before
        assert fresh.indexed(0)["app_id"] == before[0]
        assert fresh.indexed(266)["app_id"] == before[-1]
    finally:
        fresh.close()


def test_offline_export_multiple_groups_refreshes_live_index(bridge):
    for i in range(22):
        package = build_package(SCRIPT, f"export{i:02}", str(i))
        package.export(bridge.root)
        assert bridge.indexed()["count"] == i + 1
    assert bridge.indexed(21)["directory"] == "g00000002/export21"
    assert bridge.indexed(9)["directory"] == "g00000000/export09"
    assert bridge.indexed(10)["directory"] == "g00000001/export10"
    # A cached home reader notices another store instance publishing a revision.
    asyncio.run(app_request(bridge, "app_remove", app_id="export00"))
    assert bridge.indexed(0)["app_id"] == "export01"


def test_navigation_reads_only_small_indexes(bridge, firmware):
    for i in range(41):
        asyncio.run(install_app(bridge, build_package(SCRIPT, f"bounded{i:02}", str(i))))
    # Use a fresh handle, as at boot. No directory traversal or app manifest
    # reads may be required to display the first app and cross the 10-app edge.
    fresh = FirmwareBridge(firmware, bridge.root)
    try:
        firmware.app_test_reset_io()
        assert fresh.indexed()["count"] == 41
        for i in range(12):
            assert fresh.indexed(i)["app_id"] == f"bounded{i:02}"
        assert firmware.app_test_directories() == 0
        assert firmware.app_test_manifests() == 0
        assert firmware.app_test_indexes() <= 16  # summary per lookup + two groups
        firmware.app_test_reset_io()
        assert fresh.indexed(40)["app_id"] == "bounded40"
        assert fresh.indexed(0)["app_id"] == "bounded00"
        assert firmware.app_test_directories() == 0
        assert firmware.app_test_indexes() <= 4
        firmware.app_test_reset_io()
        assert fresh.indexed(39)["app_id"] == "bounded39"
        assert firmware.app_test_indexes() <= 3  # choose tail even from first group
        asyncio.run(app_request(bridge, "app_remove", app_id="bounded00"))
        assert fresh.indexed(0)["app_id"] == "bounded01"
    finally:
        fresh.close()


def test_recover_interrupted_publication_and_deletion(bridge):
    for i in range(12):
        asyncio.run(install_app(bridge, build_package(SCRIPT, f"recover{i:02}", str(i))))
    removed = bridge.directory("recover00")
    (bridge.root / ".index-dirty").touch()
    removed.rename(bridge.root / ".removed-recover00")
    # Simulate a group index lost between FAT unlink and rename.
    (removed.parent / "index.json").unlink()
    assert bridge.indexed()["count"] == 11
    assert "recover00" not in [m["app_id"] for m in bridge.listing()]
    assert not (bridge.root / ".index-dirty").exists()
    # A complete published folder whose index update never happened is adopted.
    package = build_package(SCRIPT, "published", "Published")
    destination = bridge.root / "g00000001/published"
    destination.mkdir()
    (destination / "app.json").write_text(json.dumps(package.manifest))
    (destination / "main.lua").write_bytes(package.files["main.lua"])
    (bridge.root / ".index-dirty").touch()
    assert bridge.indexed()["count"] == 12
    assert bridge.indexed()["last"]["app_id"] == "published"


@pytest.mark.parametrize("offline", [False, True])
def test_import_ignores_unrelated_files_and_handles_group_shaped_id(bridge, offline):
    bridge.root.mkdir()
    (bridge.root / "notes").write_text("not an app")
    (bridge.root / "g00000000").write_text("not a group")
    package = build_package(SCRIPT, "g00000001", "Legacy group-shaped ID")
    directory = bridge.root / "g00000001"
    directory.mkdir()
    (directory / "app.json").write_text(json.dumps(package.manifest))
    (directory / "main.lua").write_bytes(package.files["main.lua"])
    if offline:
        from app_catalog import rebuild
        rebuild(bridge.root)
    assert bridge.indexed()["count"] == 1
    assert bridge.indexed(0)["app_id"] == "g00000001"
    assert bridge.directory("g00000001") == bridge.root / "g00000002/g00000001"
    assert (bridge.root / "notes").read_text() == "not an app"


def test_recover_manifest_backup_and_unmarked_corrupt_summary(bridge):
    package = build_package(SCRIPT, "recover_title", "原名称")
    asyncio.run(install_app(bridge, package))
    directory = bridge.directory("recover_title")
    (directory / "app.json").rename(directory / "app.json.previous")
    (bridge.root / ".index-dirty").touch()
    assert bridge.indexed()["count"] == 1
    assert (directory / "app.json").exists()
    assert bridge.rename("recover_title", "新名称") == 0
    (bridge.root / "index.json").write_text('{"count":1,"revision":99}')
    assert bridge.indexed()["first"]["title"] == "新名称"


def test_normal_install_delete_rename_do_not_scan_directories(bridge, firmware):
    for i in range(12):
        asyncio.run(install_app(bridge, build_package(SCRIPT, f"fast{i:02}", str(i))))
    firmware.app_test_reset_io()
    asyncio.run(install_app(bridge, build_package(SCRIPT, "fast_new", "New")))
    # Publication doesn't enumerate existing directories; one traversal is the
    # app_abort/empty staging cleanup, which is private to this transaction.
    assert firmware.app_test_directories() <= 1
    firmware.app_test_reset_io()
    assert bridge.rename("fast_new", "重命名") == 0
    assert firmware.app_test_directories() == 0
    asyncio.run(app_request(bridge, "app_remove", app_id="fast00"))
    assert bridge.indexed(0)["app_id"] == "fast01"


def test_symlink_app_cannot_escape_storage(bridge, tmp_path):
    package = build_package(SCRIPT, "safe", "Safe")
    asyncio.run(install_app(bridge, package))
    original = bridge.directory("safe")
    outside = tmp_path / "outside"
    original.rename(outside)
    original.symlink_to(outside, target_is_directory=True)
    with pytest.raises(RuntimeError):
        asyncio.run(app_request(bridge, "app_info", app_id="safe"))
    # Removal may unlink the symlink but must never follow it.
    asyncio.run(app_request(bridge, "app_remove", app_id="safe"))
    assert (outside / "main.lua").read_bytes() == package.files["main.lua"]


def test_long_package_ids_have_distinct_stable_job_names(firmware):
    firmware.local_app_store_job_name.argtypes = [ct.c_char_p, ct.c_void_p, ct.c_size_t]
    names = []
    for suffix in ('a', 'b'):
        out = ct.create_string_buffer(32)
        assert firmware.local_app_store_job_name(('x' * 31 + suffix).encode(), out, len(out)) == 0
        assert len(out.value) == 30
        names.append(out.value)
    assert names[0] != names[1]
