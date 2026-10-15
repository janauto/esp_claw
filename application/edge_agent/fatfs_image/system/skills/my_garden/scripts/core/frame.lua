-- 我的 · 花园：系统外框（状态栏、四键软键栏、提示条、操作面板）与页头。
-- 状态栏和软键栏是“系统外框”：固定深色、不随主题变（设计稿：主题只作用于「我的」和小家的内容区）。
-- 软键栏四格与实体键一一对应：◀ 上一个（VOL−）· M 确认（MODE，长按 0.6 s = 拿起/更多）· 下一个 ▶（VOL+）· SET 返回。
-- 触摸点软键等于按实体键；长按 M 格等于长按 MODE。
local M = {}

local SYS = { bg = "#101010", text = "#EDEDED", muted = "#8A8A8A", badge = "#6A6A6A", accent = "#F0C430", pill = "#262626" }
M.SYS = SYS
M.STATUS_H, M.KEYS_H, M.HEAD_H = 40, 56, 58
local WEEK = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" }

function M.new(app)
  local ui, env, scr = app.ui, app.env, app.scr
  local W, H = app.W, app.H
  local F = { labels = {}, toast_until = 0 }

  ---------------------------------------------------------------- 状态栏
  local sb = ui.plain(scr, 0, 0, W, M.STATUS_H, SYS.bg)
  local time = ui.text(sb, 18, 9, "--:--", SYS.text)
  local pill = ui.plain(sb, (W - 184) // 2, 7, 184, 26, SYS.pill, { radius = 13 })
  local date = ui.text(pill, 16, 0, "", SYS.text, { w = 168, h = 26 })
  local net_dot = ui.plain(sb, W - 92, 16, 8, 8, SYS.muted, { radius = 4 })
  local net = ui.text(sb, W - 78, 9, "", SYS.muted)
  local last_min, last_net = "", nil
  function F.status(now)
    local hm = app.date("%H:%M")
    if hm ~= last_min then
      last_min = hm
      ui.set_text(time, app.clock_ok() and hm or "--:--")
      local okw, wd = pcall(function() return WEEK[(tonumber(app.date("%w")) or 0) + 1] end)
      local m, d = app.date("%m"), app.date("%d")
      ui.set_text(date, app.clock_ok() and string.format("%d月%d日 %s", tonumber(m) or 0, tonumber(d) or 0, okw and wd or "") or "未对时")
    end
    local online = app.online()
    if online ~= last_net then
      last_net = online
      net_dot:set_style({ bg_color = online and "#4FD27F" or SYS.muted })
      ui.set_text(net, online and "在线" or "离线")
    end
  end

  ---------------------------------------------------------------- 软键栏
  local kb = ui.plain(scr, 0, H - M.KEYS_H, W, M.KEYS_H, SYS.bg)
  ui.plain(kb, 0, 0, W, 1, "#2A2A2A")
  local slots = {}
  local function badge(parent, x, w, txt, dir)
    local b = ui.plain(parent, x, 15, w, 26, nil, { radius = 6, border = 2, border_color = SYS.badge })
    if dir then ui.arrow(b, dir == "left" and 6 or w - 14, 5, dir, 12, SYS.text)
    else ui.text(b, (w - ui.text_w(txt)) // 2 - 2, 1, txt, SYS.text) end
    return b
  end
  -- 每格 120 像素：键帽 + 最多 4 个字的说明（SET 格 3 个字）
  local defs = {
    { key = "prev", badge = function(p) return badge(p, 8, 24, nil, "left") end, lx = 38 },
    { key = "ok", badge = function(p) return badge(p, 6, 26, "M") end, lx = 38 },
    { key = "next", badge = function(p) return badge(p, 120 - 30, 24, nil, "right") end, lx = 4, right = true },
    { key = "back", badge = function(p) return badge(p, 6, 44, "SET") end, lx = 54 },
  }
  for i, d in ipairs(defs) do
    local slot = ui.plain(kb, (i - 1) * 120, 1, 120, M.KEYS_H - 1)
    local b = d.badge(slot)
    local l = ui.text(slot, d.lx, 15, "", i == 2 and SYS.accent or SYS.text)
    local c = ui.catcher(slot, 120, M.KEYS_H - 1)
    app.input.bind_soft(c, d.key)
    slots[d.key] = { label = l, badge = b, right = d.right, lx = d.lx }
  end
  -- labels = {prev=, ok=, next=, back=}；空串的键整格变暗（按了也没反应）
  function F.keys(labels)
    for k, s in pairs(slots) do
      local t = labels[k] or ""
      local maxw = k == "back" and 64 or 82
      t = ui.fit(t, maxw)
      if F.labels[k] ~= t then
        F.labels[k] = t
        ui.set_text(s.label, t)
        if s.right then s.label:set_pos(math.max(2, 120 - 34 - ui.text_w(t)), 15) end
        s.badge:set_style({ opa = t == "" and 90 or 255 })
      end
    end
  end

  ---------------------------------------------------------------- 内容区（随主题换底色）
  F.content = ui.plain(scr, 0, M.STATUS_H, W, H - M.STATUS_H - M.KEYS_H, app.C.bg)
  F.CW, F.CH = W, H - M.STATUS_H - M.KEYS_H
  function F.retheme() F.content:set_style({ bg_color = app.C.bg }) end

  ---------------------------------------------------------------- 页头：‹ 返回 · 标题 · 右侧标签（主题风格）
  -- spec = {back = "首页"|nil, title = "…", chip = "…"|nil, on_chip = fn|nil}
  function F.header(parent, spec)
    local C, S = app.C, app.S
    local box = ui.plain(parent, 0, 0, W, M.HEAD_H)
    local x = 18
    if spec.back then
      local bw = 44 + ui.text_w(spec.back)
      local pill_ = app.card(box, x, 12, bw, 36, { radius = 18, shadow = math.min(S.shadow, 3) })
      ui.arrow(pill_, 14, 10, "left", 14, C.text)
      ui.text(pill_, 30, 4, spec.back, C.text)
      app.input.bind_soft(ui.catcher(pill_, bw, 36), "back")
      x = x + bw + 14
    end
    -- 标题可以是多段（例如 {"我的", "小磁"}），段与段之间画一个小圆点（不依赖字库里的“·”）
    local parts = type(spec.title) == "table" and spec.title or { spec.title or "" }
    local title
    for i, part in ipairs(parts) do
      if i > 1 then ui.plain(box, x + 4, 28, 6, 6, C.text, { radius = 3 }); x = x + 14 end
      title = ui.text(box, x, 18, part, C.text)
      x = x + ui.text_w(part)
    end
    local chip
    if spec.chip and spec.chip ~= "" then
      local cw = ui.text_w(spec.chip) + 24
      chip = ui.plain(box, W - 18 - cw, 15, cw, 30, S.chip, { radius = 15 })
      ui.text(chip, 12, 3, spec.chip, S.chip_text)
      if spec.on_chip then app.input.bind_tap(ui.catcher(chip, cw, 30), spec.on_chip) end
    end
    return { box = box, title = title, chip = chip }
  end

  ---------------------------------------------------------------- 提示条（2 秒自动消失，盖在最上层）
  function F.toast(text, ms)
    F.hide_toast()
    local w = math.min(W - 60, ui.text_w(text) + 48)
    F.toast_layer = ui.plain(scr, (W - w) // 2, H - M.KEYS_H - 78, w, 50, "#1A1A1A", { radius = 25, border = 2, border_color = "#3A3A3A", opa = 240 })
    ui.text(F.toast_layer, 24, 12, ui.fit(text, w - 40), "#FFFFFF")
    F.toast_until = app.now() + (ms or 2200)
    env.log("GD_TOAST " .. text)
  end
  function F.hide_toast()
    if F.toast_layer then F.toast_layer:delete(); F.toast_layer = nil end
  end
  function F.tick(now)
    if F.toast_layer and now > F.toast_until then F.hide_toast() end
    F.status(now)
  end

  ---------------------------------------------------------------- 操作面板（长按 M 的“更多”、确认移除等）
  -- items = {{text=, sub=, act=function}, ...}；按键：◀▶ 选择、M 确定、SET 关闭；触摸：点行 = 执行，点空白 = 关闭
  function F.sheet(title, items, opts)
    F.close_sheet()
    local C, S = app.C, app.S
    local sh = { items = items, i = (opts and opts.focus) or 1 }
    local top = M.STATUS_H
    local layer = ui.plain(scr, 0, top, W, F.CH, "#000000", { opa = 130 })
    app.input.bind_tap(ui.catcher(layer, W, F.CH), function() F.close_sheet() end)
    local rowh = 56
    local ph = 64 + #items * rowh + 16
    local panel = app.card(layer, 16, F.CH - ph - 12, W - 32, ph, { fill = C.surface })
    ui.text(panel, 20, 16, ui.fit(title, W - 80), C.text)
    sh.rows = {}
    for i, it in ipairs(items) do
      local r = ui.plain(panel, 12, 56 + (i - 1) * rowh, W - 56, rowh - 8, C.surfaceAlt, { radius = math.min(12, S.radius) })
      local t = ui.text(r, 16, 11, it.text, C.text)
      local s = it.sub and ui.text(r, 30 + ui.text_w(it.text), 11, ui.fit(it.sub, W - 120 - ui.text_w(it.text)), C.textMuted) or nil
      app.input.bind_tap(ui.catcher(r, W - 56, rowh - 8), function() sh.i = i; F.sheet_pick() end)
      sh.rows[i] = { r = r, t = t, s = s }
    end
    function sh.paint()
      for i, row in ipairs(sh.rows) do
        local on = i == sh.i
        row.r:set_style({ bg_color = on and C.accent or C.surfaceAlt })
        row.t:set_style({ text_color = on and C.onAccent or C.text })
        if row.s then row.s:set_style({ text_color = on and C.onAccent or C.textMuted }) end
      end
    end
    sh.paint()
    sh.layer = layer
    F.sheet_ = sh
    app.log_state("sheet")
    return sh
  end
  function F.close_sheet()
    if F.sheet_ then F.sheet_.layer:delete(); F.sheet_ = nil end
  end
  function F.sheet_pick()
    local sh = F.sheet_
    if not sh then return end
    local it = sh.items[sh.i]
    F.close_sheet()
    if it and it.act then it.act() end
  end
  function F.sheet_key(k, kind)
    local sh = F.sheet_
    if kind ~= "short" and kind ~= "long" then return true end
    if k == "prev" then sh.i = (sh.i - 2) % #sh.items + 1; sh.paint()
    elseif k == "next" then sh.i = sh.i % #sh.items + 1; sh.paint()
    elseif k == "ok" and kind == "short" then F.sheet_pick()
    elseif k == "back" then F.close_sheet() end
    return true
  end
  function F.sheet_labels() return { prev = "上一项", ok = "确定", next = "下一项", back = "关闭" } end

  return F
end

return M
