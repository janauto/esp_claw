-- 主题 B · 中枢：家庭中枢风格。设计稿里的玻璃质感在设备上降级为实色 + 细描边（Lua 绑定不开放渐变与模糊）。
-- 沿用主题外壳 themes/hub.lua（样机已验证），加 v2 扩展：mascot（发光小团子）/ scene / style / say；f.hub_only 时只画中枢。
local T = {
  id = "hub", name = "中枢", tag = "玻璃", digits = "block", radius = 18, status_h = 56, keybar_h = 96, budget = { fps = 6 },
  tokens = { bg = "#0E1B3D", surface = "#172852", surfaceAlt = "#223664", line = "#3A4F86", text = "#EAF2FF", textMuted = "#9FB3D9",
    accent = "#5CC8FF", onAccent = "#0A1530", danger = "#FF6A5E", success = "#6BE5A8" },
  style = { border = 1, shadow = 0, radius = 22, ink = "#3A4F86", seg_radius = 3, chip = "#5CC8FF", chip_text = "#0A1530", focus = "#5CC8FF" },
  say = "家里一切正常",
}

local function blob(ui, parent, x, y, s, C)
  local box = ui.plain(parent, x, y, 120 * s // 10, 120 * s // 10)
  local function k(v) return math.floor(v * s / 10) end
  ui.plain(box, k(4), k(26), k(112), k(92), "#1F4C86", { radius = k(46) }) -- 发光的外圈（实色模拟）
  local body = ui.plain(box, k(10), k(20), k(100), k(92), C.accent, { radius = k(46) })
  ui.plain(box, k(50), 0, k(20), k(8), C.accent, { radius = k(4) }) -- 头顶光环
  local eyes = { ui.plain(body, k(28), k(26), k(14), k(24), C.onAccent, { radius = k(7) }), ui.plain(body, k(58), k(26), k(14), k(24), C.onAccent, { radius = k(7) }) }
  local mouth = ui.plain(body, k(42), k(60), k(16), k(6), C.onAccent, { radius = k(3) })
  local self = { box = box, key = "" }
  function self.set(mood, blink, breathe)
    local key = mood .. tostring(blink) .. tostring(breathe)
    if key == self.key then return end
    self.key = key
    local eh = blink and 4 or mood == "listen" and 30 or mood == "think" and 16 or mood == "happy" and 8 or mood == "sleepy" and 4 or 24
    for i, e in ipairs(eyes) do e:set_size(k(14), k(eh)); e:set_pos(k(i == 1 and 28 or 58), k(mood == "think" and 18 or 26 + (24 - eh) // 2)) end
    mouth:set_size(k(mood == "talk" and 20 or mood == "sad" and 18 or 16), k(mood == "talk" and 12 or mood == "happy" and 10 or 6))
    body:set_size(k(breathe and 102 or 100), k(breathe and 90 or 92))
  end
  return self
end

---------------------------------------------------------------- v2：形象与场景
function T.mascot(ctx, parent, x, y, px)
  local s = math.max(6, math.floor(px * 16 / 12)) -- px=10 时约 160 像素宽
  local w = 120 * s // 10
  local b = blob(ctx.ui, parent, x + (16 * px - w) // 2, y + 16 * px - w, s, ctx.C)
  local blink_at = 0
  local self = { box = b.box, w = 16 * px, h = 16 * px }
  function self.set(mood, now)
    if now > blink_at + 4500 then blink_at = now end
    b.set(mood, now - blink_at < 180, (now // 1300) % 2 == 0)
  end
  return self
end

function T.scene(ctx, parent, w, h)
  local ui = ctx.ui
  ui.plain(parent, 0, 0, w, h // 2, "#132650") -- 上半段稍亮，模拟渐变
  ui.plain(parent, w * 0.62, -60, 220, 220, "#1E4B86", { radius = 110, opa = 150 }) -- 右上角的光晕
  ui.plain(parent, 30, h - 24, w - 60, 2, "#3A4F86")
  return { ground = h - 30 }
end

---------------------------------------------------------------- v1：主题外壳接口（小家页只用 hub）
function T.build(ctx, f)
  local ui, C, W = ctx.ui, ctx.C, ctx.W
  local view = {}
  if not f.hub_only then
    local sb = ui.plain(f.screen, 0, 0, W, T.status_h, C.bg)
    local time = ui.text(sb, 22, 15, "", C.text)
    view.status = { update = function(m) ui.set_text(time, m.time) end }
    local kb = ui.plain(f.screen, 0, f.H - T.keybar_h, W, T.keybar_h, C.bg)
    local la, lb = ui.text(kb, 40, 30, "", C.text), ui.text(kb, W // 2 + 40, 30, "", C.text)
    view.keybar = { A = ui.catcher(kb, W // 2, T.keybar_h), B = ui.catcher(kb, W // 2, T.keybar_h, W // 2, 0), set = function(l) la:set_text(l[1] or ""); lb:set_text(l[2] or "") end }
    view.home = { update = function() end }
    view.control = { update = function() end, taps = {} }
  end
  -- 中枢：设备 / 应用卡片
  local hub = f.tiles.hub
  ui.text(hub, 24, 8, "设备与应用", C.text)
  local rects = ui.grid(#f.entries, 2, 16, 44, W - 32, f.h - 52, 12)
  local rooms, parts = {}, {}
  for i, r in ipairs(rects) do
    local e = f.entries[i]
    local card = ui.plain(hub, r.x, r.y, r.w, r.h, C.surface, { radius = T.radius, border = 1, border_color = C.line })
    local icon = ui.plain(card, 14, 12, 34, 34, C.surfaceAlt, { radius = 17, border = 2, border_color = C.accent })
    local first = e.name:match("^[%z\1-\127\194-\244][\128-\191]*") or "?"
    ui.text(icon, 7, 3, first, C.accent)
    ui.text(card, 58, 16, e.name, C.textMuted, { w = r.w - 70 })
    local val = ui.text(card, 16, r.h - 70, "", C.text)
    local lab = ui.text(card, 16, r.h - 40, "", C.textMuted, { w = r.w - 30 })
    local slot = ui.plain(card, 8, 52, r.w - 16, r.h - 60)
    rooms[i] = ui.catcher(card, r.w, r.h)
    parts[i] = { labw = r.w - 30, card = card, val = val, lab = lab, slot = slot, w = r.w - 16, h = r.h - 60 }
  end
  local focus = 0
  view.hub = {
    rooms = rooms,
    focus = function(i)
      if focus > 0 then parts[focus].card:set_style({ bg_color = C.surface, border_color = C.line, border_width = 1 }) end
      focus = i
      parts[i].card:set_style({ bg_color = C.surfaceAlt, border_color = C.accent, border_width = 3 })
    end,
    slot = function(i) return parts[i].slot, parts[i].w, parts[i].h end,
    summary = function(i, v, l) ui.set_text(parts[i].val, v); ui.set_text(parts[i].lab, ui.fit(l, parts[i].labw)) end,
    live = function(i, on) ui.show(parts[i].val, not on); ui.show(parts[i].lab, not on) end,
    play = function(i, on) parts[i].card:set_style({ border_color = on and C.success or C.accent }) end,
  }
  return view
end

function T.app_frame(ctx, overlay, app, w, h)
  local ui, C = ctx.ui, ctx.C
  local bar = ui.plain(overlay, 16, 10, w - 32, 52, C.surface, { radius = 26, border = 1, border_color = C.line })
  ui.text(bar, 22, 12, app.name, C.text)
  local body = ui.plain(overlay, 0, 74, w, h - 74)
  return { body = body, w = w, h = h - 74 }
end

return T
