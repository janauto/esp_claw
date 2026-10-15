-- 主题 D · 极简：黑底白字、点阵小磁、红色只留给 AI 标记。也是花园的降级兜底主题（任何主题载入失败都回到这里）。
-- 沿用主题外壳 themes/minimal.lua（样机已验证），加 v2 扩展：mascot / scene / style / say；f.hub_only 时只画中枢。
-- 主题接口（三个内置主题一致；自定义主题照此实现，见 PLANT_API.md 末尾的“自定义主题”）：
--   v1 字段：id, name, tokens, digits("block"|"dot"), radius, build(ctx, f) -> view（view.hub 用于小家页）, app_frame
--   v2 可选：tag（卡片小字）、style{border,shadow,radius,ink,chip,chip_text,focus,seg_radius}、say（小磁台词）、
--            mascot(ctx, parent, x, y, px) -> {box, w, h, set(mood, now)}、scene(ctx, parent, w, h) -> {ground}
local T = {
  id = "minimal", name = "极简", tag = "点阵", digits = "dot", radius = 20, status_h = 48, keybar_h = 88, budget = { fps = 6 },
  tokens = { bg = "#000000", surface = "#0A0A0A", surfaceAlt = "#1C1C1C", line = "#3A3A3A", text = "#F4F4F4", textMuted = "#8C8C8C",
    accent = "#F4F4F4", onAccent = "#000000", danger = "#E5352B", success = "#4FD27F" },
  style = { border = 2, shadow = 0, radius = 16, ink = "#F4F4F4", seg_radius = 4, chip = "#FF3B30", chip_text = "#FFFFFF", focus = "#FF3B30",
    tag = "#F4F4F4", tag_text = "#000000" },
  say = "安静模式，有事叫我",
}

-- 15×13 点阵公仔：身体亮点、眼嘴为熄灭点；只改变化的点
local COLS, ROWS = 15, 13
local function mask(x, y) local nx, ny = (x - 7) / 6.9, (y - 7.3) / 5.6; return nx * nx + ny * ny <= 1 end
local function holes(mood, blink, gx)
  local h, d = {}, (gx or 0) > 4 and 1 or (gx or 0) < -4 and -1 or 0
  local function add(x, y) h[y * 32 + x] = true end
  if mood == "happy" then for _, p in ipairs({ { 3, 6 }, { 4, 5 }, { 5, 6 }, { 9, 6 }, { 10, 5 }, { 11, 6 }, { 5, 9 }, { 6, 10 }, { 7, 10 }, { 8, 10 }, { 9, 9 } }) do add(p[1], p[2]) end
  elseif mood == "sleepy" then for _, p in ipairs({ { 3, 7 }, { 4, 7 }, { 5, 7 }, { 9, 7 }, { 10, 7 }, { 11, 7 }, { 7, 10 } }) do add(p[1], p[2]) end
  elseif mood == "sad" then for _, p in ipairs({ { 4, 6 }, { 5, 7 }, { 9, 7 }, { 10, 6 }, { 5, 10 }, { 6, 9 }, { 7, 9 }, { 8, 9 }, { 9, 10 } }) do add(p[1], p[2]) end
  else
    local rows = blink and { 7 } or mood == "listen" and { 4, 5, 6, 7 } or mood == "think" and { 4, 5 } or { 5, 6, 7 }
    local cols = mood == "think" and { 5, 6, 10, 11 } or { 4 + d, 5 + d, 9 + d, 10 + d }
    for _, y in ipairs(rows) do for _, x in ipairs(cols) do add(x, y) end end
    if mood == "talk" then for _, p in ipairs({ { 6, 9 }, { 7, 9 }, { 8, 9 }, { 6, 10 }, { 7, 10 }, { 8, 10 } }) do add(p[1], p[2]) end
    else for _, p in ipairs({ { 6, 10 }, { 7, 10 }, { 8, 10 } }) do add(p[1], p[2]) end end
  end
  return h
end
local function dot_mascot(ui, parent, x, y, pitch, C)
  local box = ui.plain(parent, x, y, COLS * pitch, (ROWS + 2) * pitch)
  local dots, lit = {}, {}
  local r = math.floor(pitch * 0.72)
  local tip = ui.plain(box, 7 * pitch + (pitch - r) // 2, 0, r, r, "#FF3B30", { radius = r }) -- 头顶红点 = AI
  ui.plain(box, 7 * pitch + (pitch - r) // 2, pitch, r, r, C.textMuted, { radius = r })
  for yy = 0, ROWS - 1 do
    for xx = 0, COLS - 1 do
      if mask(xx, yy) then
        local o = ui.plain(box, xx * pitch + (pitch - r) // 2, (yy + 2) * pitch + (pitch - r) // 2, r, r, C.text, { radius = r })
        dots[yy * 32 + xx] = o; lit[yy * 32 + xx] = true
      end
    end
  end
  local self = { box = box, key = "" }
  function self.set(mood, blink, gx, glow)
    local k = mood .. tostring(blink) .. tostring(gx)
    if k ~= self.key then
      self.key = k
      local h = holes(mood, blink, gx)
      for id, o in pairs(dots) do
        local on = not h[id]
        if lit[id] ~= on then lit[id] = on; o:set_style({ bg_color = on and C.text or C.surfaceAlt }) end
      end
    end
    tip:set_style({ bg_opa = glow and 255 or 160 })
  end
  return self
end

---------------------------------------------------------------- v2：形象与场景
function T.mascot(ctx, parent, x, y, px)
  local pitch = math.max(4, math.floor(px * 16 / COLS)) -- 与像素小磁占同样大的位置
  local m = dot_mascot(ctx.ui, parent, x + (16 * px - COLS * pitch) // 2, y + 16 * px - (ROWS + 2) * pitch, pitch, ctx.C)
  local blink_at = 0
  local self = { box = m.box, w = 16 * px, h = 16 * px }
  function self.set(mood, now)
    if now > blink_at + 4200 then blink_at = now end
    local gx = math.floor(6 * math.sin(now / 1400))
    m.set(mood, now - blink_at < 160, gx > 4 and 5 or gx < -4 and -5 or 0, (now // 700) % 2 == 0)
  end
  return self
end

function T.scene(ctx, parent, w, h)
  local ui = ctx.ui
  local line = h - 20
  for i = 0, 17 do ui.plain(parent, 14 + i * ((w - 28) // 17), line, 4, 4, "#5A5A5A", { radius = 2 }) end -- 一排点阵地平线
  return { ground = line - 8 }
end

---------------------------------------------------------------- v1：主题外壳接口（小家页只用 hub）
function T.build(ctx, f)
  local ui, C, W = ctx.ui, ctx.C, ctx.W
  local view = {}
  if not f.hub_only then
    local sb = ui.plain(f.screen, 0, 0, W, T.status_h, C.bg)
    local time = ui.text(sb, 20, 12, "", C.text)
    view.status = { update = function(m) ui.set_text(time, m.time) end }
    local kb = ui.plain(f.screen, 0, f.H - T.keybar_h, W, T.keybar_h, C.bg)
    local la, lb = ui.text(kb, 40, 30, "", C.text), ui.text(kb, W // 2 + 40, 30, "", C.text)
    view.keybar = { A = ui.catcher(kb, W // 2, T.keybar_h), B = ui.catcher(kb, W // 2, T.keybar_h, W // 2, 0), set = function(l) la:set_text(l[1] or ""); lb:set_text(l[2] or "") end }
    view.home = { update = function() end }
    view.control = { update = function() end, taps = {} }
  end
  -- 中枢：2×3 描边卡片，焦点反白
  local hub = f.tiles.hub
  ui.text(hub, 24, 10, "小家", C.text)
  local rects = ui.grid(#f.entries, 2, 16, 48, W - 32, f.h - 60, 12)
  local rooms, parts = {}, {}
  for i, r in ipairs(rects) do
    local card = ui.plain(hub, r.x, r.y, r.w, r.h, C.bg, { border = 2, border_color = C.line, radius = T.radius })
    local name = ui.text(card, 18, 12, f.entries[i].name, C.textMuted)
    local val = ui.text(card, 18, r.h - 70, "", C.text)
    local lab = ui.text(card, 18, r.h - 40, "", C.textMuted, { w = r.w - 30 })
    local slot = ui.plain(card, 10, 40, r.w - 20, r.h - 50)
    rooms[i] = ui.catcher(card, r.w, r.h); parts[i] = { labw = r.w - 30, card = card, name = name, val = val, lab = lab, slot = slot, w = r.w - 20, h = r.h - 50 }
  end
  local focus = 0
  local function paint(i, on)
    local p = parts[i]
    p.card:set_style({ bg_color = on and C.accent or C.bg, border_color = on and C.accent or C.line })
    for _, o in ipairs({ p.name, p.val, p.lab }) do o:set_style({ text_color = on and C.onAccent or (o == p.val and C.text or C.textMuted) }) end
  end
  view.hub = {
    rooms = rooms,
    focus = function(i) if focus > 0 then paint(focus, false) end; focus = i; paint(i, true) end,
    -- 焦点卡片反白：给房间里的应用内容一套反色，应用本身不用知道
    slot = function(i) return parts[i].slot, parts[i].w, parts[i].h, i == focus and { text = C.onAccent, textMuted = C.onAccent, surface = C.accent, surfaceAlt = "#D0D0D0", accent = C.onAccent } or nil end,
    summary = function(i, v, l) ui.set_text(parts[i].val, v); ui.set_text(parts[i].lab, ui.fit(l, parts[i].labw)) end,
    live = function(i, on) ui.show(parts[i].val, not on); ui.show(parts[i].lab, not on) end,
    play = function(i, on) parts[i].card:set_style({ border_color = on and C.danger or C.accent }) end,
  }
  return view
end

function T.app_frame(ctx, overlay, app, w, h)
  local ui, C = ctx.ui, ctx.C
  ui.text(overlay, 20, 10, app.name, C.text)
  ui.plain(overlay, 20, 44, w - 40, 1, C.line)
  local body = ui.plain(overlay, 0, 52, w, h - 52)
  return { body = body, w = w, h = h - 52 }
end

return T
