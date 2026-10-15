-- 主题 C · 小家：像素风。沿用主题外壳 themes/house.lua（样机已验证），按 1.0 首页设计稿调整：
--   * 颜色、房间墙色对齐《首页设计原型》的「我的 · 小磁」与「小家」两屏；
--   * 小磁换成设计稿里的“磁铁耳朵”形象（左耳红色 N 极、右耳银色 S 极）；
--   * v2 扩展：mascot（形象）、scene（「我的」场景底图）、style（卡片风格）、say（主题台词）；
--   * f.hub_only = true 时只画小家的房子剖面（状态栏、软键栏、首页、控制中心改由系统外框负责）。
-- 性能：全部是不透明矩形；公仔精灵按行合并（约 40 个色块），只在表情/眨眼变化时重建。
local T = {
  id = "house", name = "小家", tag = "像素", digits = "block", radius = 0, status_h = 56, keybar_h = 100, budget = { fps = 4 },
  tokens = { bg = "#F2E2BF", surface = "#FFF7E3", surfaceAlt = "#E9D6AE", line = "#2B2118", text = "#2B2118", textMuted = "#6E5E49",
    accent = "#F08A24", onAccent = "#1E140A", danger = "#D8553A", success = "#4F8F4A" },
  style = { border = 3, shadow = 4, radius = 8, ink = "#2B2118", seg_radius = 2, chip = "#F08A24", chip_text = "#1E140A", focus = "#F08A24" },
  say = "欢迎回家，今天想玩什么",
}
local INK, YEL, BLUE, WOOD, GRASS = "#2B2118", "#F6C445", "#4F86C6", "#B07A4A", "#5FA35A"
-- 每个房间一种墙色（按应用 id），像真正的不同房间
local WALL = { workbuddy = "#CFE3F5", ac = "#D9F0D5", pomodoro = "#F9E2B8", snake = "#E6D9F5", water = "#D3EEF0", clock = "#F5D9D9" }

-- 小磁 16×16：磁铁耳朵（r 红色 N 极、s 银色 S 极）、黄色身体、橙色脸蛋
local BASE = {
  "...r........s...", "...k........k...", "..kkk......kkk..", "..kkkkkkkkkkkk..", ".kyyyyyyyyyyyyk.", ".kywyyyyyyyyyyk.",
  "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk", "kyyyyyyyyyyyyyyk",
  ".kyyyyyyyyyyyyk.", "..kkkkkkkkkkkk..", "...kk......kk...", "................" }
local PAL = { k = INK, y = YEL, w = "#FFFFFF", p = "#F08A5A", r = "#E5482F", s = "#BFC4C8", o = "#F2643C" }
local function rows(mood, blink, gx, talk)
  local g = {}
  for i, r in ipairs(BASE) do g[i] = {} for c in r:gmatch(".") do g[i][#g[i] + 1] = c end end
  local function set(x, y, c) if g[y + 1] and x >= 0 and x < 16 then g[y + 1][x + 1] = c end end
  local dx = gx > 4 and 1 or gx < -4 and -1 or 0
  local function put(list, c) for _, p in ipairs(list) do set(p[1], p[2], c or "k") end end
  if mood == "happy" then put({ { 4, 8 }, { 5, 7 }, { 6, 8 }, { 9, 8 }, { 10, 7 }, { 11, 8 }, { 6, 10 }, { 7, 11 }, { 8, 11 }, { 9, 10 } })
  elseif mood == "sleepy" then put({ { 4, 8 }, { 5, 8 }, { 6, 8 }, { 9, 8 }, { 10, 8 }, { 11, 8 }, { 7, 11 }, { 8, 11 } })
  elseif mood == "sad" then put({ { 4, 7 }, { 5, 8 }, { 10, 8 }, { 11, 7 }, { 6, 11 }, { 7, 10 }, { 8, 10 }, { 9, 11 } })
  elseif mood == "listen" then put({ { 4, 7 }, { 5, 7 }, { 4, 8 }, { 5, 8 }, { 10, 7 }, { 11, 7 }, { 10, 8 }, { 11, 8 }, { 7, 11 }, { 8, 11 } })
  elseif mood == "think" then put({ { 5, 6 }, { 5, 7 }, { 11, 6 }, { 11, 7 }, { 8, 11 }, { 9, 11 } })
  else
    if blink then put({ { 4 + dx, 8 }, { 5 + dx, 8 }, { 10 + dx, 8 }, { 11 + dx, 8 } }) else put({ { 4 + dx, 7 }, { 5 + dx, 7 }, { 4 + dx, 8 }, { 5 + dx, 8 }, { 10 + dx, 7 }, { 11 + dx, 7 }, { 10 + dx, 8 }, { 11 + dx, 8 } }) end
    if mood == "talk" and talk then put({ { 7, 10 }, { 8, 10 } }); put({ { 7, 11 }, { 8, 11 } }, "o") else put({ { 7, 10 }, { 8, 10 } }) end
  end
  put({ { 2, 10 }, { 3, 10 }, { 12, 10 }, { 13, 10 } }, "p") -- 脸蛋
  local out = {}
  for i, r in ipairs(g) do out[i] = table.concat(r) end
  return out
end
T.rows = rows

-- 像素描边块：粗黑边 + 硬投影
local function pbox(ui, parent, x, y, w, h, fill, shadow, bw)
  bw = bw or 3
  if shadow and shadow > 0 then ui.plain(parent, x + shadow, y + shadow, w, h, INK) end
  return ui.plain(parent, x, y, w, h, fill, { border = bw, border_color = INK })
end
-- 房间里的小家具（几块色块），按应用 id 区分
local function furniture(ui, room, id, w, h)
  local x, y = w - 64, h - 60
  local F = {
    workbuddy = { { 0, 30, 56, 6, WOOD }, { 10, 4, 36, 26, INK }, { 14, 8, 28, 18, "#1E3B2F" } },
    ac = { { 0, 4, 56, 20, "#FFFFFF" }, { 4, 16, 48, 3, "#8A8A8A" }, { 10, 30, 8, 4, BLUE }, { 26, 34, 8, 4, BLUE }, { 42, 30, 8, 4, BLUE } },
    pomodoro = { { 12, 14, 32, 26, "#F2643C" }, { 22, 8, 12, 6, GRASS }, { 0, 40, 56, 6, WOOD } },
    snake = { { 4, 10, 48, 30, INK }, { 8, 14, 40, 22, "#3A6E46" }, { 14, 22, 18, 4, YEL } },
    water = { { 16, 0, 24, 18, "#BFE3F5" }, { 12, 18, 32, 28, "#FFFFFF" }, { 24, 26, 8, 6, "#F2643C" } },
    clock = { { 12, 4, 32, 32, "#FFFFFF" }, { 27, 10, 3, 14, INK }, { 27, 21, 10, 3, INK } },
  }
  local box = ui.plain(room, x, y, 56, 48)
  for _, r in ipairs(F[id] or {}) do ui.plain(box, r[1], r[2], r[3], r[4], r[5]) end
  return box
end
local function arrow(ui, parent, x, y, color) -- 用色块画 ▶，避免依赖字体里的符号
  local a = ui.plain(parent, x, y, 12, 16)
  for i = 0, 3 do ui.plain(a, i * 3, i * 2, 3, 16 - i * 4, color) end
  return a
end

---------------------------------------------------------------- v2：形象与场景（「我的」页用）
-- 形象：px 为像素倍数（10 → 160×160）。set(mood, now) 自己处理眨眼、视线和两帧呼吸
function T.mascot(ctx, parent, x, y, px)
  local sprite = ctx.ui.sprite(parent, x, y, px, rows("idle", false, 0), PAL)
  local key, blink_at, bob = "", 0, -1
  local self = { box = sprite.box, w = 16 * px, h = 16 * px }
  function self.set(mood, now)
    if now > blink_at + 3800 then blink_at = now end
    local blink = mood == "idle" and now - blink_at < 300
    local gx = math.floor(6 * math.sin(now / 1500))
    local talk = (now // 250) % 2 == 0
    local k = mood .. tostring(blink) .. (gx > 4 and 1 or gx < -4 and -1 or 0) .. (mood == "talk" and tostring(talk) or "")
    if k ~= key then key = k; sprite.draw(rows(mood, blink, gx, talk)) end
    local b = ((now // 500) % 2 == 0 and 0 or px // 2) + (mood == "happy" and px * 2 or 0) -- 两帧呼吸，开心时跳一下
    if b ~= bob then bob = b; sprite.move(x, y - b) end
  end
  return self
end

-- 场景：天空、地板、窗户（设计稿的房间一角）。返回地面高度，形象站在上面
function T.scene(ctx, parent, w, h)
  local ui = ctx.ui
  local floor = math.floor(h * 0.68)
  ui.plain(parent, 0, 0, w, floor, "#CDE8F2")
  ui.plain(parent, 0, floor, w, 3, "#C79E62")
  ui.plain(parent, 0, floor + 3, w, h - floor - 3, "#DDB57C")
  for i = 1, 3 do ui.plain(parent, 0, floor + 3 + i * ((h - floor) // 4), w, 2, "#D2A86E") end -- 木地板缝
  local win = ui.plain(parent, 26, 26, 92, 76, "#A9D8EE", { border = 3, border_color = INK })
  ui.plain(win, 0, 0, 86, 30, "#C6E6F4")
  ui.plain(win, 41, 0, 3, 70, INK)
  return { ground = h - 8 } -- 小磁的脚踩的位置（站在木地板上）
end

---------------------------------------------------------------- v1：主题外壳接口（小家页只用 hub）
function T.build(ctx, f)
  local ui, C, W = ctx.ui, ctx.C, ctx.W
  local view = {}
  if not f.hub_only then
    -- 主题外壳独立运行时的状态栏、软键栏、首页与控制中心；花园里由系统外框负责，这里不画
    local sb = ui.plain(f.screen, 0, 0, W, T.status_h, C.bg)
    local time = ui.text(sb, 18, 14, "", INK)
    view.status = { update = function(m) ui.set_text(time, m.time) end }
    local kb = ui.plain(f.screen, 0, f.H - T.keybar_h, W, T.keybar_h, C.bg)
    local la = ui.text(kb, 40, 30, "", INK)
    local lb = ui.text(kb, W // 2 + 40, 30, "", INK)
    view.keybar = { A = ui.catcher(kb, W // 2, T.keybar_h), B = ui.catcher(kb, W // 2, T.keybar_h, W // 2, 0), set = function(l) la:set_text(l[1] or ""); lb:set_text(l[2] or "") end }
    view.home = { update = function() end }
    view.control = { update = function() end, taps = {} }
  end

  -- 中枢：房子剖面，每个房间一个应用
  local hub = f.tiles.hub
  for i = 0, 5 do ui.plain(hub, 40 + i * 34, 52 - i * 9, W - 80 - i * 68, 9, i % 2 == 0 and "#D93E27" or "#E5482F") end
  ui.plain(hub, W - 120, 0, 22, 40, INK) -- 烟囱
  local house = ui.plain(hub, 12, 60, W - 24, f.h - 64, "#FFF1D2", { border = 4, border_color = INK })
  local rects = ui.grid(#f.entries, 2, 10, 10, W - 48, f.h - 92, 10)
  local rooms, parts = {}, {}
  for i, r in ipairs(rects) do
    local e = f.entries[i]
    local room = ui.plain(house, r.x, r.y, r.w, r.h, WALL[e.id] or C.surface, { border = 3, border_color = INK })
    ui.plain(room, 0, r.h - 12, r.w, 6, WOOD)
    local tag = ui.plain(room, 8, 8, math.min(r.w - 16, ui.text_w(e.name) + 16), 28, T.tokens.accent, { border = 2, border_color = INK })
    ui.text(tag, 6, 0, e.name, INK)
    local val = ui.text(room, 12, 44, "", INK)
    local lab = ui.text(room, 12, 72, "", C.textMuted, { w = r.w - 80 })
    local fur = furniture(ui, room, e.id, r.w, r.h - 6)
    local slot = ui.plain(room, 6, 40, r.w - 12, r.h - 56)
    rooms[i] = ui.catcher(room, r.w, r.h)
    parts[i] = { labw = r.w - 80, room = room, val = val, lab = lab, fur = fur, slot = slot, w = r.w - 12, h = r.h - 56, x = r.x, y = r.y, rw = r.w, rh = r.h }
  end
  local cursor = arrow(ui, house, -40, 0, "#F2643C")
  local mini = ui.sprite(house, -100, 0, 4, rows("idle", false, 0), PAL)
  local mini_tap = ui.catcher(mini.box, 64, 64) -- 小磁站在房间里，点它也算点这个房间
  local focus = 0
  view.hub = {
    rooms = rooms,
    focus_tap = mini_tap,
    -- 焦点房间里小磁站的位置（核心用协程让它一格一格走过去）
    walker = { box = mini.box, spot = function(i) local p = parts[i]; return p.x + p.rw - 70, p.y + p.rh - 78 end },
    focus = function(i)
      if focus > 0 then parts[focus].room:set_style({ border_color = INK, border_width = 3 }) end
      focus = i
      local p = parts[i]
      p.room:set_style({ border_color = "#F2643C", border_width = 5 })
      cursor:set_pos(p.x + p.rw - 20, p.y + 10)
      for j, q in ipairs(parts) do ui.show(q.fur, j ~= i) end -- 小磁站在家具的位置，家具先藏起来
      if not ctx.walk then mini.move(p.x + p.rw - 70, p.y + p.rh - 78) end
    end,
    slot = function(i) return parts[i].slot, parts[i].w, parts[i].h end,
    summary = function(i, v, l) ui.set_text(parts[i].val, v); ui.set_text(parts[i].lab, ui.fit(l, parts[i].labw)) end,
    live = function(i, on)
      for _, o in ipairs({ parts[i].val, parts[i].lab }) do ui.show(o, not on) end
      ui.show(mini.box, not on)
    end,
    play = function(i, on) parts[i].room:set_style({ border_color = on and BLUE or "#F2643C" }) end,
  }
  return view
end

function T.app_frame(ctx, overlay, app, w, h)
  local ui = ctx.ui
  local bar = pbox(ui, overlay, 14, 10, w - 28, 50, YEL, 5, 4)
  ui.text(bar, 16, 10, app.name, INK)
  local body = ui.plain(overlay, 0, 74, w, h - 74)
  return { body = body, w = w, h = h - 74 }
end

return T
