# Application-management integration verification · 2026-10-15

Firmware source commit: `f85a5930c8549f852b1fc44c53c0da6f78d28c95`; branch `codex/ble-live-apps`, based on
`fd9eb06f`. Imported the app-only contract/client files from GitLab `b9332b3c`;
missing C persistence and home integration were implemented locally.

- Host pytest: **111 passed in 57.22s** (package/store, real command dispatcher,
  generator iteration and local fake-provider LLM tests). The old fixture did
  not acknowledge pre-upload job_stop; imported the matching upstream fixture
  fix and added a negative check that unknown stop results prevent upload.
- C storage: **ASan/UBSan passed**, covering 41 installs, indexed navigation,
  empty-group removal, rename and recovery. Additional pytest covers 267-app
  legacy import, malformed indexes, interrupted publication/deletion, JSON
  backups, symlink isolation, long-ID task names and no-scan ordinary mutations.
- Real LVGL native preview: **10 interaction paths passed**: large page index,
  menu, rename input, save/cancel, uninstall confirmation/accept/cancel, removal
  callback at page275, system-app protection. Package descriptors and callbacks
  are explicit preview stand-ins; they do not write a device filesystem.
- Existing weather/AIHOT/stocks cache/format and HTTP regression passed under
  ASan/UBSan. Source files for live services, clip_ui, HID, voice frames and
  WorkBuddy capture are byte-identical to the chosen baseline.
- IDF target build: **passed**, existing esp32_s31_korvo1 configuration, IDF6.1.
  App image 5173920 bytes; 5MiB slot free 68960 bytes. No partition,
  bootloader, sdkconfig or on-device storage was changed by this task.
- Image SHA-256: `e13f6f65739c104724e5a424b1b578ce417343342ae41a9011eede1866f1d511`.

Evidence and restorable deliverables: `/Users/linxiansheng/esp/backclip-backups/app-merge-20261015` (`receipt.json`, logs, SD/UI
checks, app image, local sdkconfig and dependency lock). The source bundle is
created after the documentation commit. No remote push or PR was performed.

**Not verified:** physical BLE installation/download/uninstallation, actual SD
power interruption, reboot persistence, real touch/keys, device rename, and
regression of live apps/voice/HID on this new image. No flashing was performed.
The previously running device image and its acceptance retain their own dates.
