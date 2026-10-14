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
Carousel icons come from the compiled-in copies under
`ui_squareline/src/images` instead of the SD-card `.bin` files.
