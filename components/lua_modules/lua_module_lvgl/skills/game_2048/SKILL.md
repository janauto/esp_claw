---
{
  "name": "game_2048",
  "description": "Play a touch-swipe 2048 puzzle game on an LVGL screen that follows the display rotation, with tile pop animation and pixel-style sound effects.",
  "author": "ESP-Claw contributor",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["2048", "puzzle", "lvgl", "touch", "slide", "pixel", "sound"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"],
    "manage_mode": "web"
  },
  "simulator": {
    "entry": "scripts/game_2048.lua",
    "files": [
      "scripts/game_2048.lua"
    ]
  }
}
---

# Game_ 2048

Use this skill when the user asks to play 2048, a sliding number puzzle, or a touch-swipe LCD game.

The Lua script draws an LVGL screen sized from `scr:get_size()`, so it follows the display rotation: on a portrait screen (for example 480x800) the title and score cards sit above a full-width board with a hint and the 新游戏 / 退出 buttons below it; on a landscape screen the board is on the left and the rest sits in a column on the right. Swipe up, down, left, or right on the board to move tiles. Matching tiles merge, score increases, and the game ends when no moves remain. Reaching 2048 shows a banner once; the next swipe dismisses it and play continues.

## Requirements

- A display device declared as `display_lcd` in board hardware info.
- LCD touch input declared as `lcd_touch`.
- Audio output is optional and used for pixel-style move, merge, new-game, and game-over sound effects.

## Tool Call Inputs

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/game_2048.lua",
  "args": {}
}
```

Optional args:

| Arg | Default | Meaning |
|-----|---------|---------|
| `run_time_ms` | `1800000` | Safety timeout in milliseconds; 退出 or MODE exits long before it |
| `sample_rate_hz` | board `audio_dac` rate | Output sample rate for sound effects |

## Controls

| Input | Action |
|-------|--------|
| Swipe on the board | Move tiles; restarts the game when it is over |
| On-screen 新游戏 | Start a new game |
| On-screen 退出 / `MODE` | Exit, handing the screen back to the launcher |

## Behavior

Touch is read with `lcd_touch.poll` only (the LVGL Lua events carry no coordinates, so the script does its own swipe and button hit tests and does not register a second LVGL input device). A move fires as soon as the finger travels 44 px; a release only counts after several empty polls, because readers sharing one touch controller occasionally see an empty sample. The script plays a short move tone after any valid slide and a brighter merge tone when a merge happens. On startup failure it prints a single `[game_2048] ERROR: ...` line which should be reported directly to the user.

## Files

- `scripts/game_2048.lua`
