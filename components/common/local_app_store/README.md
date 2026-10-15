# Persistent user application packages

Integrated on 2026-10-15 into `codex/ble-live-apps`. The package/client contract
comes from GitLab `b9332b3c`; the missing C store and home integration are local
implementations. Firmware voice, shared HID and live-app services remain intact.

`CLAW_PATH_DATA/apps/` holds user packages. Never resolve it against SYSTEM or
hard-code the mount point. Each package has schema-1 `app.json`, `main.lua`
(1–65536 bytes), and optional selected/unselected 256×256 LVGL v9 RGB565 icons
(131084 bytes including header). App IDs are 1–32 lowercase ASCII letters,
digits, underscores or hyphens; titles are valid UTF-8 up to 95 bytes.

The root `index.json` holds revision/count/endpoints. `gXXXXXXXX/index.json`
holds at most ten app summaries and prev/next links. Ordinary publication,
rename and removal update the affected group and neighbours without directory
scans. Home navigation caches one group in the store and five descriptors in
the UI; it uses 32-bit indices and at most ten indicator dots. Existing skill
apps retain their own IDs; package home IDs are prefixed `app:`. Package Lua job
names use a stable 112-bit SHA-256 prefix to fit the 31-byte Lua task name limit.

All operations must be externally serialized. Device BLE handlers and home
readers use the same display mutex; headless BLE dispatch is already serial.
Uploads are staged in `.install-<id>` and must be sequential, acknowledged
chunks of at most 1024 bytes. Commit verifies every file size, SHA-256 and icon
header before publication. Duplicate IDs are rejected; no destructive automatic
update or overwrite is performed. `abort` only removes the staging transaction.

Index mutations use `.index-dirty`, flushed JSON `.tmp` files and `.previous`
backups. This works with FAT's refusal to rename over an existing file. Recovery
rebuilds indexes from published package directories, preserving group locations
and existing order. Hidden incomplete/removed folders are never launchable.
A removal hides the directory before updating the index and reclaiming files.
A later retry reclaims the same ID's old hidden removal. Filesystem/hardware
power-failure durability still requires board testing.

`app_remove` refuses exclusive displays and running/queued jobs whose source
path is inside that package. Home-menu removal stops its own deterministic job
first. System apps are immutable; legacy skill uninstall still follows its
original path. Native rename changes only package title, never ID or code.
The native keyboard handles Latin input; Chinese names can be supplied from the
computer manager/CLI. UI publication is deferred until the home is foreground.

The package protocol transports code and icons only. Custom Lua modules,
fonts, audio, posters and lyrics need separate provisioning. The old Web Lua
Studio `skills/SKILL.md` installer remains compatible with the old skill path;
it has not been silently switched to this package protocol.

## Verification

Use Python 3.11+ with `pytest`, `Pillow`, `requests`; install the host client's
requirements for desktop/BLE use. C tests require a compiler, OpenSSL and the
cJSON sources downloaded by the normal IDF configuration:

```sh
python -m pytest -q tools/host_vibe_coding/test_app_install.py tools/host_vibe_coding/test_app_protocol.py tools/host_vibe_coding/test_host_vibe.py tools/host_vibe_coding/test_llm_client.py
python components/common/local_app_store/test_host/run_sanitizers.py
```

On Linux set `APP_TEST_OPENSSL=/usr`; cJSON can be overridden with
`APP_TEST_CJSON=/path/to/cJSON`. The real command handler/storage are compiled;
BLE radio, display ownership and task-list calls are explicit test stubs.
Do not describe these checks as device installation/uninstallation acceptance.

[2026-10-15 integration results and evidence](VALIDATION.md).
