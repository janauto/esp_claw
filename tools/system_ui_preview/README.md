# System UI host preview

Renders the firmware's real `ui_squareline` / `system_ui` screen code with LVGL
on the host (macOS/Linux clang) and writes a BMP, so layout changes can be
checked without flashing. Audio, IR, haptics and tasks are stubbed out; only
the screen construction runs.

Needs the LVGL sources fetched by a normal firmware build
(`application/edge_agent/managed_components/lvgl__lvgl`).

```bash
cmake -S tools/system_ui_preview -B tools/system_ui_preview/build -G Ninja
ninja -C tools/system_ui_preview/build
tools/system_ui_preview/build/system_ui_preview 480 800 home home.bmp
```

Screens: `home`, `boot`, `businesscard`, `meeting`, `infrared`, `workbuddy`.
`home:N` starts the carousel on page N. Trailing `x,y[,ms]` arguments are
pressed before the capture; `home:1 240,406,800` long-presses the Pomodoro icon
to open its 卸载 / 详细 / 取消 menu. `PREVIEW_APPS="计数器,天气"` adds installed
apps after the built-in pages (page 9 onwards), as if they lived under
`/sdcard/skills/<name>`.
Carousel icons come from the compiled-in copies under
`ui_squareline/src/images` instead of the SD-card `.bin` files.


2026-10-15 package UI checks: `PREVIEW_PACKAGE_COUNT=267` supplies synthetic
package descriptors to the real carousel, allowing `home:275` (9 built-ins +
267 packages), long-press rename, confirmation and task callback capture.
The preview prints `PREVIEW_HOME`, `PREVIEW_RENAME`, and `PREVIEW_ACTION` lines;
these are UI routing checks, not filesystem writes or device acceptance.
