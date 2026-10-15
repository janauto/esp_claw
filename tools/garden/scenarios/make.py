#!/usr/bin/env python3
"""生成场景时间轴 JSON（改了布局或流程后重新运行：python3 tools/garden/scenarios/make.py）。

按键时间轴：Lua 时间（毫秒），遇到 mark 后续步骤顺延 700 ms（Lua 侧 core/input.lua 负责）。
触摸时间轴：墙钟毫秒（Playwright），不顺延；截图用 {"do": "shot"}。坐标是 480×800 竖屏。
"""
import json
import pathlib

HERE = pathlib.Path(__file__).resolve().parent
FIX = HERE.parent / "fixtures"


class Keys:
    def __init__(self, t=2500):
        self.t, self.out = t, []

    def at(self, dt=0, **kw):
        self.t += dt
        self.out.append({"t": self.t, **kw})

    def key(self, name, n=1, gap=300, kind=None):
        for _ in range(n):
            d = {"key": name}
            if kind:
                d["kind"] = kind
            self.at(gap, **d)

    def mark(self, name, wait=900):
        self.at(wait, mark=name)


def save(name, sc):
    (HERE / f"{name}.json").write_text(json.dumps(sc, ensure_ascii=False, indent=1), "utf-8")
    print("写入", name)


# ---------------------------------------------------------------- 01 按键全流程（用户要求的主线）
k = Keys()
k.mark("k01_mine_cards", 300)                  # 我的：小磁场景 + 三张卡
k.key("ok"); k.mark("k02_poke")               # 戳小磁
k.key("next"); k.key("ok"); k.mark("k03_house", 1500)          # 进小家
k.key("next", 2); k.mark("k04_house_room3_live", 1300)        # 焦点到番茄钟：实时预览，小磁走过去
k.key("back"); k.key("next", 2, gap=250)       # 回我的，焦点到主题卡第一张（小家）
k.key("next"); k.mark("k05_focus_minimal", 600)
k.key("ok"); k.mark("k06_switched_minimal", 1300)              # 切到极简
k.key("prev", 3, gap=250); k.key("ok"); k.mark("k07_house_minimal", 1500)  # 回到小家入口再进：极简样式的小家
k.key("back"); k.mark("k08_back_mine", 900)
k.key("next"); k.key("ok"); k.mark("k09_garden", 1500)         # 进花园（焦点 1：向日葵）
k.key("next", 4, gap=250); k.mark("k10_add_tile", 600)          # 第 5 格：种植物
k.key("ok"); k.mark("k11_seeds", 1500)                         # 种子库
k.key("next", 2, gap=250); k.mark("k12_seed_diary", 600)        # 心情日记
k.key("ok"); k.mark("k13_planted", 1500)                       # 种下，回花园并聚焦新植物
k.key("ok", kind="long"); k.mark("k14_picked", 900)            # 长按 M 拿起
k.key("prev", 2, gap=400); k.mark("k15_moved", 700)            # 往前挪两格
k.key("back"); k.mark("k16_dropped", 900)                      # SET = 放下
k.key("prev", 3, gap=250); k.mark("k17_focus_water_all", 600)   # 焦点到页头“全部浇水”
k.key("ok"); k.mark("k18_watered", 1500)
k.key("next", 5, gap=250); k.key("ok"); k.mark("k19_seeds_again", 1500)  # 浇完水焦点回到第 1 株，往后 5 格是“种植物”：再进种子库
k.key("next", 8, gap=220); k.mark("k20_seed_broken_ai", 700)   # AI 种子：坏掉的花
k.key("ok"); k.mark("k21_withered_isolated", 1500)             # 种下：只有它枯萎
k.key("ok"); k.mark("k22_withered_page", 1500)                 # 打开看原因
k.key("back"); k.mark("k23_before_reload", 800)
k.mark("k24_hot_reloaded", 5200)                               # 期间宿主写入修好的文件 → 热重载复活
k.key("back"); k.mark("k25_mine_final", 1200)
k.key("back")                                                  # 顶层 SET：退出
sc1 = {
    "desc": "按键全流程：我的 → 三张卡 → 戳小磁 → 小家（实时预览）→ 回我的切换极简主题 → 极简样式的小家 → 花园 → 种子库种下心情日记 → "
            "长按拿起、移动、SET 放下 → 全部浇水 → 种下故意报错的 AI 植物（只有它枯萎）→ 宿主推送修好的文件，热重载复活 → 顶层 SET 退出",
    "page": "mine", "data": "fixtures/data_ai", "keys": k.out,
    "checks": [
        {"log": r"GD_PAGE house", "min": 2}, {"log": r"GD_TOAST 已换上：极简"}, {"log": r"GD_ADD sys/diary"},
        {"log": r'"mode":"move"', "min": 1}, {"log": r"GD_TOAST 放好了"}, {"log": r"GD_WATER", "min": 3},
        {"log": r"GD_ADD user/broken"}, {"log": r"GD_WITHER broken#\d+ 第 6 行"}, {"log": r"GD_RELOAD user/broken"}, {"log": r"GD_PLANT_LOAD user/broken 修好的花"},
        {"log": r"GD_SNAPSHOT .*\"theme\":\"minimal\""}, {"log": r"GD_QUIT back"}, {"log": r"GD_EXIT"},
    ],
}
# 热重载：在 k23 之后由宿主写入修好的文件（触摸时间轴是墙钟：按截图点数量补上顺延）
marks_before = 0
for s in k.out:
    if s.get("mark") == "k24_hot_reloaded":
        t24 = s["t"]
        break
    if "mark" in s:
        marks_before += 1
sc1["touch"] = [{"t": t24 - 4200 + marks_before * 700, "do": "write", "path": "/storage/garden/plants/broken.lua", "file": "fixtures/broken_fixed.lua"}]
save("01_keys_full_flow", sc1)

# ---------------------------------------------------------------- 02 触摸全流程
T = []
def tap(t, x, y): T.append({"t": t, "do": "tap", "x": x, "y": y})
def shot(t, n): T.append({"t": t, "do": "shot", "name": n})
def drag(t, x, y, x2, y2): T.append({"t": t, "do": "drag", "x": x, "y": y, "x2": x2, "y2": y2, "steps": 12, "step_ms": 12})
def long(t, x, y, ms=1000): T.append({"t": t, "do": "long", "x": x, "y": y, "ms": ms})
t = 2500
shot(t, "t01_mine"); t += 600
tap(t, 240, 230); t += 1200; shot(t, "t02_poke_tap"); t += 400       # 点小磁 = 戳一下
tap(t, 120, 505); t += 1500; shot(t, "t03_house_tap"); t += 400      # 点小家卡片
tap(t, 352, 431); t += 1200; shot(t, "t04_room_focus"); t += 400     # 点贪吃蛇房间：第一下聚焦
tap(t, 352, 431); t += 1500; shot(t, "t05_room_play"); t += 400      # 第二下：在房间里玩
tap(t, 420, 772); t += 1000                                          # 点软键 SET：退出游玩
tap(t, 60, 70); t += 1500; shot(t, "t06_back_pill"); t += 400        # 点页头“我的”返回
tap(t, 296, 668); t += 1500; shot(t, "t07_theme_hub_tap"); t += 400  # 点主题卡“中枢”
tap(t, 350, 505); t += 1600; shot(t, "t08_garden_tap"); t += 400     # 点花园卡片
tap(t, 354, 196); t += 900; shot(t, "t09_plot_focus"); t += 300      # 点第 2 块：聚焦
tap(t, 354, 196); t += 1500; shot(t, "t10_plant_open"); t += 400     # 再点：打开（喝水）
tap(t, 180, 772); t += 700; tap(t, 180, 772); t += 900; shot(t, "t11_drink_softkey"); t += 300  # 点软键 M 两次：喝两杯
tap(t, 88, 707); t += 1200; shot(t, "t12_water_button"); t += 400   # 点“浇水”按钮
tap(t, 60, 70); t += 1500                                            # 返回花园
long(t, 126, 196); t += 1500; shot(t, "t13_long_pick"); t += 400    # 长按第 1 块：拿起
tap(t, 126, 392); t += 1300; shot(t, "t14_tap_target"); t += 400    # 点第 3 块：挪过去并放下
tap(t, 126, 588); t += 900; tap(t, 126, 588); t += 1600; shot(t, "t15_seeds_tap"); t += 400   # “种植物”格：聚焦后再点，进种子库
drag(t, 420, 400, 60, 400); t += 1800; shot(t, "t16_seeds_swipe_page2"); t += 400              # 左滑到第 2 页（tileview）
tap(t, 126, 196); t += 1600; shot(t, "t17_planted_page2"); t += 400                            # 滑过来焦点已在第 2 页第 1 颗：点一下就种下（第 5 株）
tap(t, 354, 588); t += 900; tap(t, 354, 588); t += 1600                                       # 第 6 格“种植物”：再进种子库
tap(t, 354, 196); t += 900; tap(t, 354, 196); t += 1600; shot(t, "t18_planted_6th"); t += 400  # 种第 6 株：花园变成两页
drag(t, 420, 400, 60, 400); t += 1800; shot(t, "t19_garden_swipe_page2"); t += 400             # 花园左滑到第 2 页
drag(t, 60, 400, 420, 400); t += 1800; shot(t, "t20_garden_swipe_back"); t += 400              # 右滑回第 1 页
tap(t, 430, 70); t += 1600; shot(t, "t21_chip_water_all"); t += 400  # 点页头“全部浇水”
tap(t, 420, 772); t += 1500                                          # 软键 SET：回我的
tap(t, 441, 582); t += 1200; shot(t, "t22_row_arrow_right"); t += 400  # 主题卡一排的右箭头（6 张卡时才有）
tap(t, 420, 772); t += 1500                                          # 顶层软键 SET：退出
sc2 = {"desc": "触摸全流程：点小磁、点卡片、房间两段式点按与在房间里玩、页头返回、点主题卡换主题、花园点按聚焦/打开、软键代替实体键、"
               "植物页按钮、长按地块拿起后点目标地块挪过去、左右滑翻页（tileview）、页头全部浇水、顶层软键 SET 退出",
       "page": "mine", "touch": T, "keys": [{"t": t + 3000, "cmd": "quit"}],
       "checks": [{"log": r"GD_PAGE house"}, {"log": r'"play"'}, {"log": r"GD_TOAST 已换上：中枢"}, {"log": r"GD_PAGE plant"},
                  {"log": r"GD_TOAST 拿起了"}, {"log": r"GD_TOAST 放好了"}, {"log": r'"page":"seeds".*"reason":"swipe"'}, {"log": r'"page":"garden".*"reason":"swipe"', "min": 2}, {"log": r"GD_ADD", "min": 6},
                  {"log": r"GD_WATER", "min": 2}, {"log": r"GD_QUIT back"}]}
save("02_touch_full_flow", sc2)

# ---------------------------------------------------------------- 03 AI 植物隔离 + 自定义主题（v1）
k = Keys()
k.mark("a01_garden", 300)
k.key("next", 4); k.key("ok"); k.mark("a02_seeds_ai", 1400)
# 种子顺序：内置 8 颗（countdown counter diary focus pixel quote sunflower water）+ AI（broken escape loop standup syntax）
k.key("next", 10, gap=200); k.key("ok"); k.mark("a03_loop_withered", 1500)         # 第 11 颗 loop：死循环 → 指令预算
k.key("next"); k.key("ok"); k.key("next", 9, gap=200); k.key("ok"); k.mark("a04_escape_withered", 1500)  # 第 10 颗 escape：越权 → 沙箱
k.key("next"); k.key("ok"); k.key("next", 11, gap=200); k.key("ok"); k.mark("a05_standup_ok", 1500)    # 第 12 颗 standup：正常的 AI 植物
k.key("next"); k.key("ok"); k.key("next", 12, gap=200); k.mark("a06_syntax_seed", 700)                  # 第 13 颗 syntax：语法错误
k.key("ok"); k.mark("a07_syntax_reason", 700); k.key("back"); k.key("back")
k.key("prev", 3, gap=250); k.key("ok"); k.mark("a08_loop_page", 1400); k.key("ok", kind="long"); k.mark("a09_more_sheet", 700)
k.key("back"); k.key("back"); k.key("back")
k.key("next", 8, gap=200); k.mark("a10_vibe_card", 600); k.key("ok"); k.mark("a11_vibe_page", 1400); k.key("back")
k.key("prev", 2, gap=200); k.key("ok"); k.mark("a12_radio_mine", 1400)                 # 主题卡：复古电台（AI，v1 接口）
k.key("prev", 5, gap=200); k.key("ok"); k.mark("a13_radio_house", 1500); k.key("back")
sc3 = {"desc": "AI 植物与自定义主题：死循环（指令预算）、越权（沙箱里没有 io/require）、语法错误（种子库标出行号）都只让自己枯萎；"
               "正常的 AI 植物照常运行；植物页长按 M 的更多面板；一句话生成页列出失败原因；主题外壳 v1 接口的 AI 主题（复古电台）装进主题卡并画出自己的小家",
       "page": "garden", "data": "fixtures/data_ai", "keys": k.out,
       "checks": [{"log": r"GD_WITHER loop#\d+ 运行超时"}, {"log": r"GD_WITHER escape#\d+ .*global 'io'"}, {"log": r"GD_PLANT_FAIL user/syntax 第 3 行"},
                  {"log": r"GD_THEME_ADD radio"}, {"log": r"GD_TOAST 已换上：复古电台"}, {"log": r"GD_PAGE vibe"}, {"log": r"GD_ADD user/standup"}]}
save("03_ai_isolation_theme", sc3)

# ---------------------------------------------------------------- 04 启动参数与逐级返回
k = Keys()
k.mark("p01_args_house", 300); k.key("back"); k.mark("p02_set_to_mine", 1200); k.key("back")
save("04_args_house_back", {"desc": "args.page=house 直达小家；SET 先回「我的」，再按 SET 退出（顶层退出）", "page": "house", "keys": k.out,
                            "checks": [{"log": r"GD_START page=house"}, {"log": r'"stack":"mine>house"'}, {"log": r"GD_QUIT back"}, {"log": r"GD_EXIT"}]})

# ---------------------------------------------------------------- 05 读回已保存的数据（持久化）
k = Keys()
k.mark("d01_saved_mine", 300); k.key("next", 2); k.key("ok"); k.mark("d02_saved_garden", 1500); k.key("back"); k.key("back")
save("05_saved_data", {"desc": "读回 garden.json：名字（豆豆）、主题（极简）、自定义强调色、地块顺序与植物状态、陪伴天数都恢复", "page": "mine",
                       "data": "fixtures/data_saved", "keys": k.out,
                       "checks": [{"log": r"status=ok"}, {"log": r"GD_START .*theme=minimal"}, {"log": r'GD_SNAPSHOT .*"pet_name":"豆豆"'}]})

# ---------------------------------------------------------------- 06 损坏的数据
k = Keys()
k.mark("c01_corrupt_reset", 300); k.key("back")
save("06_corrupt_data", {"desc": "garden.json 损坏：另存 .bad、用默认值重建、照常启动；快照照常写出", "page": "mine", "data": "fixtures/data_corrupt",
                         "keys": k.out, "checks": [{"log": r"GD_WARN garden.json 损坏"}, {"log": r"status=reset"}, {"log": r"GD_SNAPSHOT"}]})

# ---------------------------------------------------------------- 07 个性化
k = Keys()
k.key("next", 6, gap=250); k.mark("u01_custom_card", 500); k.key("ok"); k.mark("u02_custom_page", 1300)
k.key("ok"); k.mark("u03_name_changed", 1000)
k.key("next"); k.key("ok"); k.key("ok"); k.mark("u04_color_changed", 1000)
k.key("back"); k.mark("u05_mine_custom", 1300); k.key("back")
save("07_personalize", {"desc": "个性化：换名字（磁磁）、换强调色（番茄）；「我的」页头、标签颜色和快照 pet_name 跟着变", "page": "mine", "keys": k.out,
                        "checks": [{"log": r"GD_TOAST 名字：磁磁"}, {"log": r"GD_TOAST 配色：番茄"}, {"log": r'GD_SNAPSHOT .*"pet_name":"磁磁"'}]})

# ---------------------------------------------------------------- 08 房间：贪吃蛇游玩与喝水联动小磁
k = Keys()
k.key("next", 3, gap=250); k.key("ok"); k.mark("r01_snake_live", 1200)
k.key("ok"); k.key("next", 1, gap=400); k.key("prev", 1, gap=600); k.mark("r02_playing", 400)
k.mark("r03_game_over", 6000); k.key("back")
k.key("next"); k.key("ok"); k.mark("r04_water_room", 1200); k.key("ok"); k.key("ok"); k.mark("r05_drank_two", 900)
k.key("back"); k.key("back"); k.mark("r06_mine_cups", 1300); k.key("back")
save("08_rooms_play_water", {"desc": "小家：贪吃蛇在房间里玩到结束、SET 退出游玩；喝水提醒房间喝两杯 → 小磁今天 2 杯（快照与「我的」气泡跟着变）",
                             "page": "house", "keys": k.out,
                             "checks": [{"log": r'"reason":"play"'}, {"log": r'"reason":"play-end"'}, {"log": r'GD_SNAPSHOT .*"say":"喝好啦，今天第 2 杯水"'}]})

# ---------------------------------------------------------------- 09 满园（11 株，两页）：对象预算与刷新开销
k = Keys()
k.mark("f01_full_page1", 600)
k.key("next", 6, gap=250); k.mark("f02_full_page2", 900)                     # 第 7 株：自动翻到第 2 页
k.key("next", 4, gap=250); k.mark("f03_full_last", 600)                      # 第 11 株（满了没有“种植物”格）
k.key("ok"); k.mark("f04_pixel_page", 1400)                                  # 像素画板全屏页（64 格）
k.key("ok"); k.key("next"); k.key("ok"); k.mark("f05_pixel_painted", 900)
k.key("back"); k.mark("f06_idle_10s", 11000)                                 # 静置 10 秒：看一个完整的 GD_PERF 窗口
k.key("back"); k.key("back")
save("09_full_garden", {"desc": "满园 11 株（两页、没有“种植物”格）：GD_PAGE 对象数在 600 预算内；第 2 页的像素画板（复杂图案按 ui.left() 降级成文字）；静置 10 秒的主循环开销",
                        "page": "garden", "data": "fixtures/data_full", "keys": k.out,
                        "checks": [{"log": r"GD_PAGE garden objs=\d+"}, {"log": r"GD_PAGE plant"}, {"log": r"GD_PERF page=garden", "min": 1}, {"log": r"GD_WITHER", "max": 0}]})

# ---------------------------------------------------------------- 10 没有中文字体副本时的降级（首次运行字体还没复制完 / SD 写不进去）
k = Keys()
k.mark("n01_no_font_page1", 600)
k.key("next", 6, gap=250); k.mark("n02_no_font_page2_by_key", 900)          # 按键翻到第 2 页（普通容器，不用 tileview）
k.key("prev", 6, gap=250); k.mark("n03_no_font_back_page1", 900)
k.key("back"); k.key("back")
save("10_no_font_fallback", {"desc": "args.no_font_copy：不复制字体、不建 tileview，分页降级为普通容器，按键 / 点页码照常翻页，界面文字照常显示",
                             "page": "garden", "data": "fixtures/data_full", "args": {"no_font_copy": True}, "keys": k.out,
                             "checks": [{"log": r"GD_FONT", "max": 0}, {"log": r'"focus":7', "min": 1}, {"log": r"GD_QUIT back"}]})

# ---------------------------------------------------------------- 11 重启后恢复上次选的 AI 主题
k = Keys()
k.mark("s01_restored_ai_theme", 800); k.key("next"); k.key("ok"); k.mark("s02_ai_theme_house", 1500); k.key("back"); k.key("back")
save("11_saved_ai_theme", {"desc": "上次选的是 AI 主题（DATA/garden/themes/radio.lua）：重启后先扫描自定义主题再套用，直接以复古电台启动，小家也用它的样式",
                           "page": "mine", "data": "fixtures/data_saved_ai", "keys": k.out,
                           "checks": [{"log": r"GD_START page=mine theme=radio"}, {"log": r'GD_SNAPSHOT .*"theme":"radio"'}, {"log": r"GD_PAGE house"}]})
