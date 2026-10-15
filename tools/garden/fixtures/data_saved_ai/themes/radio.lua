local T = {
  id = "radio",
  name = "复古电台",
  digits = "block",
  radius = 10,
  status_h = 56,
  keybar_h = 96,
  budget = { fps = 4 },
  tokens = {
    bg = "#F3E8D1",
    surface = "#FFF6E3",
    surfaceAlt = "#E5D4B3",
    line = "#67432C",
    text = "#302218",
    textMuted = "#66503C",
    accent = "#75451F",
    onAccent = "#FFF6E3",
    danger = "#B92D25",
    success = "#385C36",
  },
}

local function text_set(o, value)
  value = tostring(value or "")
  if o.value ~= value then
    o.value = value
    o.node:set_text(value)
  end
end

local function label(ui, parent, x, y, value, color, w, h)
  return {
    node = ui.text(parent, x, y, value, color, { w = w, h = h or 24 }),
    value = value,
  }
end

local function centered(ui, parent, y, value, color, w)
  return ui.text(parent, math.max(0, (w - ui.text_w(value)) // 2), y, value, color)
end

local function wood(ui, parent, x, y, w, h, C)
  local box = ui.plain(parent, x, y, w, h, C.line, { radius = T.radius })
  for i = 1, 4 do
    local yy = math.floor(h * i / 5)
    ui.plain(box, 8, yy, w - 16, 1, C.textMuted, { opa = 100 })
  end
  return box
end

local function scale(ui, parent, x, y, w, C, count)
  ui.plain(parent, x, y + 12, w, 1, C.line)
  for i = 0, count - 1 do
    local major = i % 2 == 0
    ui.plain(parent, x + math.floor(i * (w - 2) / (count - 1)),
      y + (major and 0 or 5), 2, major and 12 or 7, C.line)
  end
end

local function mascot(ui, parent, x, y, C)
  local box = ui.plain(parent, x, y, 288, 260)
  ui.plain(box, 78, 2, 132, 36, nil,
    { radius = 14, border = 7, border_color = C.line })
  ui.plain(box, 36, 232, 42, 15, C.line, { radius = 4 })
  ui.plain(box, 210, 232, 42, 15, C.line, { radius = 4 })

  local body = wood(ui, box, 0, 28, 288, 208, C)
  ui.plain(body, 8, 8, 272, 192, C.surface,
    { radius = 12, border = 2, border_color = C.textMuted })

  local rim = ui.plain(body, 20, 40, 150, 150, C.line, { radius = 75 })
  ui.plain(rim, 6, 6, 138, 138, C.surfaceAlt, { radius = 69 })
  for d = -48, 48, 12 do
    local half = math.floor(math.sqrt(60 * 60 - d * d))
    ui.plain(rim, 75 - half, 75 + d, half * 2, 2, C.textMuted)
    ui.plain(rim, 75 + d, 75 - half, 2, half * 2, C.textMuted)
  end

  local face = ui.plain(rim, 33, 43, 84, 65, C.surface, { radius = 26 })
  local left = ui.plain(face, 18, 16, 9, 16, C.text, { radius = 5 })
  local right = ui.plain(face, 57, 16, 9, 16, C.text, { radius = 5 })
  local mouth = ui.plain(face, 30, 44, 24, 4, C.text, { radius = 2 })
  local smile = ui.plain(face, 24, 34, 36, 20, nil,
    { radius = 10, border = 3, border_color = C.text })
  local cover = ui.plain(face, 23, 33, 38, 12, C.surface)
  ui.show(smile, false)
  ui.show(cover, false)

  ui.text(body, 190, 20, "FM", C.text)
  local dial = ui.plain(body, 182, 56, 82, 65, C.surfaceAlt,
    { radius = 4, border = 2, border_color = C.line })
  scale(ui, dial, 10, 12, 62, C, 7)
  local needle = ui.plain(dial, 38, 8, 3, 35, C.accent)
  ui.text(dial, 10, 39, "88", C.textMuted)
  ui.text(dial, 42, 39, "108", C.textMuted)

  for _, xx in ipairs({ 184, 230 }) do
    local knob = ui.plain(body, xx, 146, 36, 36, C.line, { radius = 18 })
    ui.plain(knob, 5, 5, 26, 26, C.surfaceAlt, { radius = 13 })
    ui.plain(knob, 16, 7, 3, 10, C.accent, { radius = 1 })
  end

  local poses = {
    idle = { 16, 9, 16, 30, 44, 24, 4 },
    listen = { 12, 11, 21, 32, 42, 20, 8 },
    think = { 13, 10, 12, 34, 46, 18, 3 },
    talk = { 16, 9, 16, 32, 39, 20, 14 },
    happy = { 18, 13, 5, 30, 44, 24, 4 },
    sleepy = { 23, 13, 3, 36, 46, 12, 3 },
  }
  local last_pose, last_step = "", -1

  return {
    update = function(mood, now, poke)
      local blink = mood ~= "sleepy" and mood ~= "happy" and now % 4800 < 250
      local speaking = mood == "talk" and (now // 250) % 2 == 0
      local key = mood .. tostring(blink) .. tostring(speaking)

      if key ~= last_pose then
        last_pose = key
        local p = poses[mood] or poses.idle
        local eh = blink and 3 or p[3]
        left:set_pos(mood == "think" and 21 or 18, blink and 23 or p[1])
        right:set_pos(mood == "think" and 60 or 57, blink and 23 or p[1])
        left:set_size(p[2], eh)
        right:set_size(p[2], eh)
        mouth:set_pos(p[4], p[5])
        mouth:set_size(p[6], speaking and 6 or p[7])
        ui.show(mouth, mood ~= "happy")
        ui.show(smile, mood == "happy")
        ui.show(cover, mood == "happy")
        rim:set_style({ bg_color = mood == "listen" and C.accent or C.line })
      end

      local step = now // 250
      if step ~= last_step then
        last_step = step
        local bob = mood == "sleepy" and 0 or math.floor(2 * math.sin(now / 900))
        if mood == "happy" then
          bob = math.floor(3 * math.sin(now / 350))
        end
        box:set_pos(x, y + bob)
        local nx = mood == "think" and 36 + math.floor(15 * math.sin(now / 900))
          or mood == "listen" and 50 or 36 + ((tonumber(poke) or 0) % 4) * 4
        needle:set_pos(nx, 8)
      end
    end,
  }
end

function T.build(ctx, f)
  local ui, C, W, H = ctx.ui, ctx.C, ctx.W, f.h
  local view = {}

  local sb = ui.plain(f.screen, 0, 0, W, T.status_h, C.surface)
  ui.plain(sb, 0, T.status_h - 3, W, 3, C.line)
  local time = label(ui, sb, 20, 16, "", C.text, 78)
  centered(ui, sb, 16, "背夹小家", C.textMuted, W)
  local classic = ui.plain(sb, W - 106, 8, 88, 38, C.surfaceAlt,
    { radius = 7, border = 2, border_color = C.line })
  centered(ui, classic, 7, "经典", C.text, 88)
  local kc = ui.catcher(classic, 88, 38)

  view.status = {
    update = function(model)
      text_set(time, model and model.time or "")
    end,
  }

  local kb = wood(ui, f.screen, 0, f.H - T.keybar_h, W, T.keybar_h, C)
  local key_w = math.floor((W - 52) / 2)
  local function key(x)
    local plate = ui.plain(kb, x, 16, key_w, 62, C.surface,
      { radius = 9, border = 2, border_color = C.textMuted })
    local knob = ui.plain(plate, 12, 17, 28, 28, C.line, { radius = 14 })
    ui.plain(knob, 5, 5, 18, 18, C.surfaceAlt, { radius = 9 })
    ui.plain(knob, 12, 6, 3, 9, C.accent)
    local text = label(ui, plate, 50, 19, "", C.text, key_w - 60)
    return ui.catcher(plate, key_w, 62), text
  end

  local ka, la = key(18)
  local kbb, lb = key(W - 18 - key_w)
  view.keybar = {
    A = ka,
    B = kbb,
    C = kc,
    set = function(labels)
      labels = labels or {}
      text_set(la, labels[1])
      text_set(lb, labels[2])
    end,
  }

  local home = f.tiles.home
  ui.plain(home, 0, 0, W, H, C.bg)
  ui.plain(home, 12, 14, 4, H - 28, C.line, { radius = 2 })
  ui.plain(home, W - 16, 14, 4, H - 28, C.line, { radius = 2 })
  centered(ui, home, 20, "小家电台", C.text, W)
  local radio = mascot(ui, home, (W - 288) // 2, 76, C)
  local mood_text = label(ui, home, (W - 160) // 2, 358, "", C.textMuted, 160)

  local card = ui.plain(home, 28, H - 222, W - 56, 148, C.surface,
    { radius = T.radius, border = 3, border_color = C.line })
  scale(ui, card, 18, 14, W - 92, C, 15)
  local title = label(ui, card, 18, 38, "", C.text, W - 92, 26)
  local sub = label(ui, card, 18, 78, "", C.textMuted, W - 92, 56)
  centered(ui, home, H - 46, "上滑 中枢    下拉 控制中心", C.textMuted, W)

  local moods = {
    idle = "陪你慢慢听",
    listen = "听你说话",
    think = "正在想一想",
    talk = "正在回答",
    happy = "今天真开心",
    sleepy = "安静休息",
  }
  view.home = {
    update = function(model, now)
      model = model or {}
      now = tonumber(now) or 0
      local mood = model.mood or "idle"
      radio.update(mood, now, model.poke)
      local value = moods[mood] or moods.idle
      text_set(mood_text, value)
      mood_text.node:set_pos((W - ui.text_w(value)) // 2, 358)
      text_set(title, model.title)
      text_set(sub, model.sub)
    end,
  }

  local hub = f.tiles.hub
  ui.plain(hub, 0, 0, W, H, C.bg)
  ui.text(hub, 22, 16, "调频中枢", C.text)
  ui.text(hub, W - 102, 16, "FM", C.textMuted)

  local n = #f.entries
  local compact = n > 8
  local grid_h = compact and H - 244 or H - 72
  local rects = n > 0 and ui.grid(n, 2, 20, 60, W - 40, grid_h, 10) or {}
  local rooms, parts = {}, {}
  local shared, shared_title, shared_w, shared_h

  if compact then
    local panel = ui.plain(hub, 20, H - 172, W - 40, 156, C.surface,
      { radius = T.radius, border = 3, border_color = C.line })
    shared_title = label(ui, panel, 18, 10, "", C.text, W - 76)
    shared_w, shared_h = W - 76, 104
    shared = ui.plain(panel, 18, 40, shared_w, shared_h, C.surface)
  end

  for i, r in ipairs(rects) do
    local room = ui.plain(hub, r.x, r.y, r.w, r.h, C.surface,
      { radius = 5, border = 2, border_color = C.line })
    local name = label(ui, room, 26, 7, f.entries[i].name or "", C.text, r.w - 38, 24)
    local needle = ui.plain(room, 10, 7, 3, r.h - 14, C.accent)
    local tip = ui.plain(room, 6, 7, 11, 5, C.accent)
    ui.show(needle, false)
    ui.show(tip, false)

    local ticks = compact and 0 or 7
    for j = 0, ticks - 1 do
      ui.plain(room, 26 + math.floor(j * (r.w - 40) / (ticks - 1)),
        34, 2, j % 2 == 0 and 7 or 4, C.line)
    end

    local sh = compact and shared_h or r.h - 54
    local sw = compact and shared_w or r.w - 40
    local slot = compact and shared or ui.plain(room, 26, 46, sw, sh, C.surface)
    local val = label(ui, room, 26, compact and 31 or 46, "", C.text, r.w - 40, 24)
    local lab = label(ui, room, 26, 76, "", C.textMuted, r.w - 40, 24)
    if compact then
      ui.show(lab.node, false)
    end

    parts[i] = {
      card = room,
      name = name,
      needle = needle,
      tip = tip,
      val = val,
      lab = lab,
      slot = slot,
      w = sw,
      h = sh,
      playing = false,
      live = false,
    }
    rooms[i] = ui.catcher(room, r.w, r.h)
  end

  local focus = 0
  local function paint(i)
    local p = parts[i]
    if not p then return end
    local on = focus == i
    local color = p.playing and C.danger or C.accent
    p.card:set_style({
      border_color = on and color or C.line,
      border_width = on and 3 or 2,
    })
    p.needle:set_style({ bg_color = color })
    p.tip:set_style({ bg_color = color })
    p.name.node:set_style({ text_color = p.playing and C.danger or C.text })
    ui.show(p.needle, on)
    ui.show(p.tip, on)
  end

  view.hub = {
    rooms = rooms,
    focus = function(i)
      local old = focus
      focus = parts[i] and i or 0
      paint(old)
      paint(focus)
      if shared_title then
        text_set(shared_title, parts[focus] and f.entries[focus].name or "")
      end
    end,
    slot = function(i)
      local p = parts[i]
      if p then
        return p.slot, p.w, p.h
      end
    end,
    summary = function(i, value, text)
      local p = parts[i]
      if not p then return end
      text_set(p.val, value)
      text_set(p.lab, text)
    end,
    live = function(i, on)
      local p = parts[i]
      if not p then return end
      p.live = not not on
      ui.show(p.val.node, not on)
      ui.show(p.lab.node, not on and not compact)
      if not on then
        p.playing = false
        paint(i)
      end
    end,
    play = function(i, on)
      local p = parts[i]
      if not p then return end
      p.playing = not not on
      paint(i)
    end,
  }

  local cc = f.tiles.control
  ui.plain(cc, 0, 0, W, H, C.bg)
  centered(ui, cc, 20, "控制中心", C.text, W)
  scale(ui, cc, 28, 56, W - 56, C, 19)

  local rows = {}
  for i = 1, 8 do
    local panel = ui.plain(cc, 24, 88, W - 48, 76, C.surface,
      { radius = T.radius, border = 2, border_color = C.line })
    local knob = ui.plain(panel, 14, 23, 28, 28, C.line, { radius = 14 })
    local knob_mark = ui.plain(knob, 12, 5, 3, 10, C.surface)
    local pointer = ui.plain(panel, 5, 10, 3, 56, C.accent)
    rows[i] = {
      panel = panel,
      mark = knob_mark,
      pointer = pointer,
      name = label(ui, panel, 54, 12, "", C.text, W - 126, 24),
      value = label(ui, panel, 54, 40, "", C.textMuted, W - 126, 24),
    }
    ui.show(panel, false)
  end

  view.control = {
    update = function(items, fi)
      items = items or {}
      local count = math.min(#items, #rows)
      local cols = count > 4 and 2 or 1
      local row_count = math.max(1, math.ceil(count / cols))
      local height = math.min(108, math.floor((H - 112) / row_count))
      local width = math.floor((W - 48 - (cols - 1) * 12) / cols)

      for i, r in ipairs(rows) do
        local it = items[i]
        if it and i <= count then
          local selected = i == fi
          r.panel:set_pos(24 + ((i - 1) % cols) * (width + 12),
            88 + ((i - 1) // cols) * height)
          r.panel:set_size(width, height - 10)
          r.name.node:set_size(width - 68, 24)
          r.value.node:set_size(width - 68, 24)
          r.pointer:set_size(3, height - 30)
          r.panel:set_style({
            bg_color = selected and C.surfaceAlt or C.surface,
            border_color = selected and C.accent or C.line,
            border_width = selected and 3 or 2,
          })
          r.mark:set_pos(it.on and 12 or 6, it.on and 5 or 13)
          r.mark:set_size(it.on and 3 or 10, it.on and 10 or 3)
          text_set(r.name, it.name)
          text_set(r.value, it.value)
          ui.show(r.pointer, selected)
          ui.show(r.panel, true)
        else
          ui.show(r.panel, false)
          r.panel:set_pos(W + 24, 88)
        end
      end
    end,
  }

  return view
end

function T.app_frame(ctx, overlay, app, w, h)
  local ui, C = ctx.ui, ctx.C
  local header = ui.plain(overlay, 12, 8, w - 24, 72, C.surface,
    { radius = T.radius, border = 2, border_color = C.line })
  ui.text(header, 16, 10, app.name or "", C.text, { w = w - 56, h = 24 })
  ui.text(header, 16, 38, "长按右键返回", C.textMuted)
  ui.plain(overlay, 12, 88, w - 24, h - 100, C.line, { radius = T.radius })
  local body = ui.plain(overlay, 16, 92, w - 32, h - 108, C.surface,
    { radius = T.radius })
  return { body = body, w = w - 32, h = h - 108 }
end

return T
