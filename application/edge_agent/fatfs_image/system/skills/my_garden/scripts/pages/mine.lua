-- 页面：我的 · 小磁（入口）。按《首页设计原型》03 屏：
--   主题化的小磁场景 + 气泡（问候随时段 / 心情变化）→ 心情 / 能量 / 陪伴三张卡 → 小家、花园两张入口卡 →
--   “形象与主题”一排卡：小家·像素 / 极简·点阵 / 中枢·玻璃 / 已安装的 AI 主题 / 个性化 / 一句话生成。
-- 按键：◀▶ 在 小磁 → 小家 → 花园 → 各主题卡 之间移动焦点；M 确认（戳小磁、进入、换主题）；长按 M 在小磁上 = 个性化；SET 回系统首页。
-- 触摸：点哪张卡就是哪张卡（点小磁 = 戳一下）。
local M = {}

local ROW_VISIBLE = 4

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local pet, garden = app.pet, app.garden
  local T = app.T
  local page = { rate = 1000 }
  local focus_ring = app.ring(body) -- 先建焦点框：压在卡片下面，只露出外圈，不挡点按

  app.frame.header(body, { back = "首页", title = { "我的", pet.name() }, chip = "主题 " .. app.theme.name,
    on_chip = function() app.open("custom") end })

  ---------------------------------------------------------------- 焦点目标
  local items = {} -- {id, x, y, w, h, act, label}
  local function add_item(it) items[#items + 1] = it; return #items end

  ---------------------------------------------------------------- 场景：小磁 + 气泡
  local SX, SY, SW, SH = 18, 64, W - 36, 250
  local scene = app.card(body, SX, SY, SW, SH, { fill = S.scene })
  local b = S.border
  local inner = ui.plain(scene, 0, 0, SW - 2 * b, SH - 2 * b) -- 子对象坐标从描边内侧算起
  local iw, ih = SW - 2 * b, SH - 2 * b
  local ctx = { ui = ui, C = C, W = W, H = app.H, date = app.date, now = app.now }
  local sc = (T.scene and app.safe("theme.scene", T.scene, ctx, inner, iw, ih)) or nil
  if not sc then ui.plain(inner, 0, ih - 40, iw, 40, C.surfaceAlt); sc = { ground = ih - 10 } end
  local px = 8
  local mascot_fn = T.mascot or require("themes.house").mascot -- v1 自定义主题没有形象：用像素小磁
  local mx, my = (iw - 16 * px) // 2, math.min(ih - 16 * px - 2, (tonumber(sc.ground) or ih) - 16 * px) -- 站在地上，气泡在右上角，两者不重叠（设计稿 03 的构图）
  local mascot = app.safe("theme.mascot", mascot_fn, ctx, inner, mx, my, px)
  local bubble = app.card(inner, iw - 246, 12, 232, 66, { fill = C.surface, radius = math.min(S.radius, 14), shadow = math.min(S.shadow, 3) })
  local say_l = ui.text(bubble, 12, 6, "", C.text, { w = 210, h = 56 })
  local scene_tap = ui.catcher(scene, SW, SH)
  local i_scene = add_item({ id = "scene", x = SX, y = SY, w = SW, h = SH, label = "戳一戳",
    act = function() pet.cheer("poke"); page.update(app.now()) end, long = function() app.open("custom") end })
  app.input.bind_tap(scene_tap, function() page.focus(i_scene); items[i_scene].act() end, function() page.focus(i_scene); app.open("custom") end)

  ---------------------------------------------------------------- 三张状态卡
  local stats = {}
  local cw3 = (SW - 20) // 3
  for i, name in ipairs({ "心情", "能量", "陪伴" }) do
    local x = SX + (i - 1) * (cw3 + 10)
    local c = app.card(body, x, 326, cw3, 78)
    ui.text(c, 12, 6, name, C.textMuted)
    stats[i] = { v = ui.text(c, 12, 30, "", C.text), seg = ui.seg(c, 12, 62, cw3 - 24 - 2 * b, 7, 10, S.chip, C.surfaceAlt, S.seg_radius) }
  end

  ---------------------------------------------------------------- 小家 / 花园入口卡
  local cw2 = (SW - 10) // 2
  local function entry(x, title, draw_thumb, act)
    local c = app.card(body, x, 416, cw2, 104)
    draw_thumb(c)
    ui.text(c, 90, 16, title, C.text)
    local sub = ui.text(c, 90, 44, "", C.textMuted, { w = cw2 - 100, h = 56 })
    ui.arrow(c, cw2 - 26, 76, "right", 12, S.chip)
    local idx = add_item({ id = title, x = x, y = 416, w = cw2, h = 104, label = "进入", act = act })
    app.input.bind_tap(ui.catcher(c, cw2, 104), function() page.focus(idx); act() end)
    return sub
  end
  local house_sub = entry(SX, "小家", function(c) -- 像素小房子
    local INK = "#2B2118"
    for r, s in ipairs({ { 28, 20 }, { 22, 32 }, { 16, 44 }, { 10, 56 } }) do ui.plain(c, s[1], 10 + r * 6, s[2], 6, r % 2 == 0 and "#D93E27" or "#E5482F") end
    ui.plain(c, 52, 12, 6, 12, INK)
    ui.plain(c, 14, 40, 52, 40, "#FFF1D2", { border = 2, border_color = INK })
    ui.plain(c, 20, 46, 14, 12, "#9FD3EB"); ui.plain(c, 46, 46, 14, 12, "#9FD3EB"); ui.plain(c, 34, 60, 12, 20, "#B5652E")
  end, function() app.open("house") end)
  local garden_sub = entry(SX + cw2 + 10, "花园", function(c) -- 像素小花圃
    ui.plain(c, 10, 74, 70, 10, "#8A5A35"); ui.plain(c, 10, 70, 70, 4, "#5FA35A")
    for i, col in ipairs({ "#F6C445", "#F28C8C", "#7FB7F0" }) do
      local x = 6 + i * 20
      ui.plain(c, x + 4, 46, 3, 24, "#3F8A3A"); ui.plain(c, x, 34, 11, 11, col, { radius = 5 }); ui.plain(c, x + 3, 37, 5, 5, "#FFF1D2", { radius = 2 })
    end
  end, function() app.open("garden") end)

  ---------------------------------------------------------------- 形象与主题
  ui.text(body, SX + 2, 534, "形象与主题", C.text)
  ui.text(body, SX + 112, 534, "只作用于「我的」和小家", C.textMuted)
  local cards = {}
  for _, e in ipairs(app.themes.list) do
    if not e.broken then cards[#cards + 1] = { kind = "theme", e = e } end
  end
  cards[#cards + 1] = { kind = "custom" }
  cards[#cards + 1] = { kind = "vibe" }
  local RW = (SW - (ROW_VISIBLE - 1) * 8) // ROW_VISIBLE
  local row = ui.plain(body, SX - 6, 562, SW + 12, 138) -- 行容器：超出 4 张的卡片靠焦点“滚动”（改位置），触摸用两端的小箭头
  local first_card = #items + 1
  state.offset = state.offset or 0
  local function card_x(i) return 6 + (i - 1 - state.offset) * (RW + 8) end
  for i, cd in ipairs(cards) do
    local c = app.card(row, card_x(i), 6, RW, 122, { fill = C.surface })
    local pv = ui.plain(c, 0, 0, RW - 2 * b, 54, cd.kind == "theme" and cd.e.mod.tokens.bg or C.surfaceAlt)
    local name, tag
    if cd.kind == "theme" then
      local tk = cd.e.mod.tokens
      local r = math.min(20, cd.e.mod.radius or 0) -- 小预览：主题自己的强调色团子 + 一双眼睛
      local face = ui.plain(pv, 12, 14, 40, 34, tk.accent, { radius = r })
      ui.plain(face, 10, 12, 6, 10, tk.onAccent, { radius = math.min(3, r) }); ui.plain(face, 24, 12, 6, 10, tk.onAccent, { radius = math.min(3, r) })
      name, tag = cd.e.name, cd.e.tag ~= "" and cd.e.tag or (cd.e.src == "user" and "AI" or "")
    elseif cd.kind == "custom" then
      local pill = ui.plain(pv, (RW - 2 * b - 64) // 2, 13, 64, 28, S.chip, { radius = 14 })
      ui.text(pill, (64 - ui.text_w(ui.fit(pet.name(), 56))) // 2, 2, ui.fit(pet.name(), 56), S.chip_text)
      name, tag = "个性化", "名字 配色"
    else
      for _, r in ipairs({ { 0, 9, 22, 4 }, { 9, 0, 4, 22 }, { 4, 4, 14, 14 } }) do ui.plain(pv, (RW - 2 * b - 22) // 2 + r[1], 16 + r[2], r[3], r[4], S.chip, { radius = r[3] == 14 and 7 or 2 }) end
      name, tag = "一句话生成", "Vibe Coding"
    end
    ui.text(c, 8, 58, ui.fit(name, RW - 16), C.text)
    ui.text(c, 8, 86, ui.fit(tag, RW - 16), C.textMuted)
    local is_cur = cd.kind == "theme" and cd.e.id == app.theme.id
    if is_cur then app.chip(c, RW - 2 * b - 62, 2, "当前") end
    if (cd.kind == "theme" and cd.e.src == "user") or cd.kind == "vibe" then app.chip(c, RW - 2 * b - 44, is_cur and 32 or 2, "AI", { fill = "#FF3B30", color = "#FFFFFF" }) end
    if is_cur then c:set_style({ border_color = S.chip, border_width = math.max(3, b) }) end
    cd.obj = c
    local act
    if cd.kind == "theme" then act = function() if cd.e.id == app.theme.id then app.frame.toast("已经是这个主题了") else app.set_theme(cd.e.id) end end
    elseif cd.kind == "custom" then act = function() app.open("custom") end
    else act = function() app.open("vibe") end end
    local idx = add_item({ id = cd.kind == "theme" and ("theme:" .. cd.e.id) or cd.kind, row = i, w = RW, h = 122,
      label = cd.kind == "theme" and (is_cur and "当前" or "换上") or (cd.kind == "custom" and "打开" or "看看"), act = act })
    app.input.bind_tap(ui.catcher(c, RW, 122), function() page.focus(idx); act() end)
  end
  -- 卡片多于 4 张：右上角两个小箭头，触摸翻动这一排
  local more = #cards > ROW_VISIBLE
  local function relayout()
    for i, cd in ipairs(cards) do cd.obj:set_pos(card_x(i), 6) end
  end
  if more then
    for k, dir in ipairs({ "left", "right" }) do
      local btn = app.card(body, W - 18 - (3 - k) * 46 + 6, 526, 38, 32, { radius = 16, shadow = 0 })
      ui.arrow(btn, dir == "left" and 13 or 15, 9, dir, 14, C.text)
      app.input.bind_tap(ui.catcher(btn, 38, 32), function()
        state.offset = math.max(0, math.min(#cards - ROW_VISIBLE, state.offset + (dir == "left" and -1 or 1)))
        relayout(); page.focus(state.focus or 1)
      end)
    end
  end

  ---------------------------------------------------------------- 焦点
  local function rect(it)
    if it.row then return SX + (it.row - 1 - state.offset) * (RW + 8), 568, it.w, it.h end
    return it.x, it.y, it.w, it.h
  end
  function page.focus(i)
    state.focus = math.max(1, math.min(#items, i))
    local it = items[state.focus]
    if it.row then -- 让焦点卡片进入可见范围
      if it.row - 1 < state.offset then state.offset = it.row - 1; relayout()
      elseif it.row > state.offset + ROW_VISIBLE then state.offset = it.row - ROW_VISIBLE; relayout() end
    end
    local x, y, w, h = rect(it)
    focus_ring.to(x, y, w, h, S.radius)
    app.refresh_keys()
  end
  function page.focus_id() return '"' .. items[state.focus or 1].id .. '"' end

  ---------------------------------------------------------------- 刷新（1 Hz）与动画（每帧，但形象自己节流）
  local last_say, said_at = nil, 0
  function page.update(now)
    pet.tick()
    ui.set_text(stats[1].v, pet.mood_text()); stats[1].seg.set(pet.level())
    ui.set_text(stats[2].v, pet.energy() .. "%"); stats[2].seg.set(math.floor(pet.energy() / 10 + 0.5))
    ui.set_text(stats[3].v, pet.days() .. " 天"); stats[3].seg.set(math.min(10, math.floor(pet.days() * 10 / 30))) -- 陪伴进度：距离 30 天纪念
    local say = app.say(now)
    if say ~= last_say then last_say = say; said_at = now; ui.set_text(say_l, ui.wrap(say, 206, 2)) end
    ui.set_text(house_sub, ui.fit("6 个房间", cw2 - 100))
    local n, thirsty = garden.count(), garden.thirsty_count()
    ui.set_text(garden_sub, thirsty > 0 and (n .. " 株植物\n" .. thirsty .. " 株口渴") or (n .. " 株植物"))
  end
  local t_anim = 0
  function page.anim(now)
    if not mascot or now - t_anim < 125 then return end
    t_anim = now
    local face = pet.face(now)
    if face == "idle" and now - said_at < 1200 then face = "talk" end
    mascot.set(face, now)
  end

  ---------------------------------------------------------------- 按键
  function page.key(k, kind)
    if k == "prev" and kind == "short" then page.focus((state.focus - 2) % #items + 1); return true end
    if k == "next" and kind == "short" then page.focus(state.focus % #items + 1); return true end
    if k == "ok" and kind == "short" then items[state.focus].act(); return true end
    if k == "ok" and kind == "long" then
      local it = items[state.focus]
      if it.long then it.long() else it.act() end
      return true
    end
    return false
  end
  function page.labels()
    return { prev = "上一个", ok = items[state.focus or 1].label, next = "下一个" }
  end
  -- 后台扫描到新的 AI 主题：重建本页（主题卡一排会多一张）
  local theme_count = #app.themes.list
  function page.rescan() if #app.themes.list ~= theme_count then app.rebuild("themes") end end

  page.focus(state.focus or 1)
  page.update(app.now())
  page.anim(app.now())
  return page
end

return M
