---
{
  "name": "snake_game",
  "description": "Play a wall-wrapping snake game on the LCD, driven by on-screen direction keys or the board's VOLUME keys.",
  "author": "ESP-Claw contributor",
  "metadata": {
    "category": ["game", "ui"],
    "tags": ["snake", "game", "lvgl", "touch", "button"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"],
    "manage_mode": "web"
  },
  "simulator": {
    "entry": "scripts/snake_game.lua",
    "files": [
      "scripts/snake_game.lua"
    ]
  }
}
---

# Snake Game

Use this skill when the user asks to play snake, 贪吃蛇, or a snake/nibble game on the LCD.

The Lua script drives an LVGL screen: a rounded board, a four-segment snake, and a
food pellet. Eating grows the snake and speeds it up. Running into the snake ends
the game. Walls wrap — leaving one side brings the snake out of the opposite side.

## Requirements

- A display device declared as `display_lcd` in board hardware info.

Touch and the `adc_button_group` keys are both optional. Without touch, the
on-screen direction keys are inert and only the physical keys work; without the
button group, only touch works. The script prints a warning and keeps running in
either case.

## Tool Call Inputs

```json
{
  "path": "{CUR_SKILL_DIR}/scripts/snake_game.lua",
  "args": {}
}
```

| Arg | Default | Meaning |
|-----|---------|---------|
| `run_time_ms` | `1800000` | Safety timeout in milliseconds; MODE exits long before it |
| `start_speed_ms` | `170` | Milliseconds per step at the start |
| `min_speed_ms` | `70` | Fastest step interval the game accelerates to |
| `speed_step_ms` | `4` | How much each food pellet shortens the step interval |

## Controls

| Input | Action |
|-------|--------|
| On-screen 上/下/左/右 | Steer; restarts the game when it is over |
| `VOLUME_UP` / `VOLUME_DOWN` | Steer up / down on boards without touch |
| `MODE` | Exit, handing the screen back to the launcher |
| On-screen 退出 MODE | Same as the MODE key |

## Behavior

The game runs until MODE is pressed or `run_time_ms` elapses. On exit the script
unsubscribes the board keys before releasing the display session, so the launcher's
own key bindings come back.

## Files

- `scripts/snake_game.lua`
