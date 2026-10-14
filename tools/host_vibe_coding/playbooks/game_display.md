---
{
  "name": "game-display",
  "description": "display 逐帧自绘游戏规范：整数绘制参数、主循环错误处理、触摸采样和音频初始化",
  "match": { "task_kind": ["game"], "ui_stack": ["display"] },
  "priority": 10
}
---

优先使用 LVGL 实现游戏；只有确实需要逐像素自绘时才使用 `display`。一旦选择 `display`，
脚本就是全屏帧循环，不要混用 `lvgl`。

## 必守规则

1. display 所有坐标、尺寸、半径、线宽、`font_size` 都必须是整数。物理状态可以用浮点，
   但传给 `display.*` 前必须 `math.floor()` 或 `math.max(1, math.floor(...))`。
   特别是粒子、水果半径不能写 `radius = 2 + math.random() * 3` 后直接 `fill_circle`。

2. 主循环不要用 `pcall(game_loop)` 吞掉错误后 `running=false` 正常退出。设备日志里出现
   `[ERR] game_loop` 后 job 会变成 `done`，上位机会误以为是正常结束。开发版可以在外层
   `pcall` 后 `error(err)` 重新抛出，发布版也不要把绘制错误当正常结束。

   推荐：

   ```lua
   while true do
       game_loop()
       delay.delay_ms(33)
   end
   ```

   如果必须保护清理：

   ```lua
   local ok, err = pcall(game_loop)
   if not ok then
       print("[ERR] game_loop:", err)
       error(err)
   end
   ```

3. `display.begin_frame()`、绘制、`display.present()`、`display.end_frame()` 必须成对出现。
   全屏游戏用 `display.begin_frame({ clear = true, color = "#000000", preserve = false })`。

4. `lcd_touch.sync(handle)` 只在进入主循环前调用一次。主循环里只调用
   `lcd_touch.poll(handle)`，不要每帧 sync 后马上 poll。

5. `audio.new_output()` 推荐直接使用 board_manager 返回的紧凑表：

   ```lua
   local codec, rate, channels, bits = board_manager.get_audio_codec_output_params("audio_dac")
   if codec and rate and channels and bits then
       output = audio.new_output({ codec, rate, channels, bits, volume = 70 })
   end
   ```

   如果改成命名字段，必须写 `sample_rate=rate, channels=channels, bits=bits`，
   不要传 nil 或把 `audio_mod` 当作 self 参数传进去。

6. display 文本只支持 ASCII。中文游戏标题、提示语如果必须显示，改用 LVGL；
   display 脚本里用 `SCORE`、`GAME OVER`、`TAP TO RESTART` 这类 ASCII 文案。

7. 动态列表要限制数量，死亡对象倒序或重建表清理。触摸轨迹点要限制长度。

## 整数工具

```lua
local function ipx(v)
    return math.floor(v or 0)
end

local function iradius(v)
    return math.max(1, math.floor(v or 1))
end
```

调用示例：

```lua
display.fill_circle(ipx(p.x), ipx(p.y), iradius(p.radius), p.color)
display.draw_line(ipx(x1), ipx(y1), ipx(x2), ipx(y2), "#ffffff")
display.draw_text(10, 8, "Score: " .. score, { color = "white", font_size = 26 })
```
