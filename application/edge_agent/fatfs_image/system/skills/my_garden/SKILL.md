---
{
  "name": "my_garden",
  "title": "我的 · 小磁",
  "description": "我的 · 小磁：固件 1.0 的「我的」页面，一个用户可客制化的 Lua 花园。包含小磁形象与心情、小家（主题外壳的房间）、花园地块（沙箱里的用户 / AI 植物）、主题与个性化。由系统首页以独占显示方式启动。",
  "author": "Pisen backclip 1.0",
  "metadata": {
    "category": ["ui", "home"],
    "tags": ["garden", "pet", "theme", "lvgl", "touch", "button", "sandbox"],
    "peripherals": ["display"],
    "cap_groups": ["cap_lua"],
    "manage_mode": "readonly",
    "display": "exclusive"
  },
  "execution": {
    "entry": "scripts/main.lua",
    "args": { "page": "mine" },
    "visible": false,
    "order": 1
  },
  "simulator": {
    "entry": "scripts/main.lua"
  }
}
---

# 我的 · 小磁（my_garden）

固件 1.0 的「我的」：用户可以客制化的花园。系统首页点「我的」小组件或应用总览里的「我的 · 小磁」时，
以与首页启动应用相同的方式启动本应用，独占显示：

```text
cap_lua_run_script_async("<SYSTEM>/skills/my_garden/scripts/main.lua", args_json, 0, "my_garden", "display", true, ...)
```

- `args_json`：`{"page":"mine"}`（默认）、`{"page":"house"}`、`{"page":"garden"}`。直达小家或花园时，SET 仍先回到「我的」。
- 退出：在「我的」按 SET 结束主循环，回到系统首页；系统底部上滑也会直接停止本应用（数据平时就勤快落盘）。
- `execution.visible` 为 `false`：入口由系统首页的「我的」小组件与总览负责，不在旧轮播里重复出现。

## 页面

| 页面 | 内容 |
| --- | --- |
| 我的 · 小磁 | 主题化的小磁场景与气泡（问候随时段、心情变化）、心情 / 能量 / 陪伴三张卡、小家与花园入口、形象与主题卡 |
| 小家 | 主题外壳的房子剖面：WorkBuddy、客厅空调、番茄钟、贪吃蛇（可在房间里玩）、喝水提醒、时钟 |
| 花园 | 2 列 × 3 行地块，可翻页；每块一株植物（沙箱里的小 Lua 模块），可添加、移除、换位置、浇水 |
| 种子库 | 8 颗内置种子 + `<DATA>/garden/plants/*.lua` 里的用户 / AI 植物（热重载） |
| 个性化 | 小磁的名字（预设）与强调色 |
| 一句话生成 | 说明如何用手机 App 或电脑生成新植物 / 新主题，列出已装的 AI 作品与载入失败的文件 |

## 按键与触摸

| 键 | 作用 |
| --- | --- |
| VOLUME_DOWN（上一个） | 焦点往前 |
| VOLUME_UP（下一个） | 焦点往后 |
| MODE（M） | 确认；长按 0.6 s = 拿起地块 / 更多操作 |
| SET | 返回上一级；拿起地块时 = 放下；在「我的」= 退出回系统首页 |

触摸：点卡片、点地块（第一下聚焦、第二下打开）、长按地块拿起、左右滑翻页、点底部软键栏等于按对应实体键。

## 数据

全部写在 `<DATA>/garden/`：`garden.json`（布局、植物状态、小磁、设置）、`snapshot.json`（给系统首页「我的」小组件）、
`plants/*.lua`（用户 / AI 植物）、`themes/*.lua`（自定义主题）、`ui_font.ttf`（tileview 用的中文字体副本）。
植物接口见 `scripts/PLANT_API.md`。

## Files

- `scripts/main.lua`：入口
- `scripts/core/`：状态机、外框、输入、协程调度、沙箱、存储、小磁、主题注册表、花园模型
- `scripts/pages/`：各页面
- `scripts/themes/`：小家 · 像素、极简 · 点阵、中枢 · 玻璃
- `scripts/rooms/`：小家的房间应用
- `scripts/plants/`：内置种子
- `scripts/PLANT_API.md`：植物与自定义主题接口（人和 AI 共用）
