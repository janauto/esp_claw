-- 页面：种子库。内置种子（SYSTEM，随固件）+ 用户 / AI 植物（<DATA>/garden/plants/*.lua，带 AI 标记）。
-- 选一颗按 M 种下：种在花园最后一格，回到花园并聚焦到它。载入失败的种子画成“枯萎”并给出原因，种不下去。
-- 每 3 秒看一眼 DATA/garden/plants：手机或电脑推来新植物，种子库里马上多一颗（热重载）。
local Pager = require("core.pager")

local M = {}
local COLS, ROWS = 2, 3
local PER = COLS * ROWS

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local g = app.garden
  local page = { rate = 1000, watch = true }
  local list = g.seed_list()
  local n = #list
  state.focus = math.max(1, math.min(state.focus or 1, math.max(1, n)))

  local planted = {}
  for _, p in ipairs(g.plots()) do local k = g.key(p.src, p.seed); planted[k] = (planted[k] or 0) + 1 end
  local ai = 0
  for _, s in ipairs(list) do if s.src == "user" then ai = ai + 1 end end
  app.frame.header(body, { back = "花园", title = "种子库", chip = n .. " 颗  AI " .. ai })

  local GX, GY, GAP, PAD = 18, 64, 12, 8
  local GW, GH = W - 36, 3 * 184 + 2 * GAP
  local SW, SH = (GW - GAP) // COLS, (GH - (ROWS - 1) * GAP) // ROWS
  local npages = math.max(1, math.ceil(n / PER))
  local function page_of(i) return math.max(1, math.ceil(i / PER)) end
  local rings = {}
  local pager
  pager = Pager.new(app, body, GX - PAD, GY - PAD, GW + 2 * PAD, GH + 2 * PAD, npages, function(p)
    page.ensure(p)
    if page_of(state.focus) ~= p then page.focus(math.min(n, (p - 1) * PER + 1), true) end
    page.footer()
    app.log_state("swipe")
  end)
  local function xy(i)
    local q = (i - 1) % PER
    return PAD + (q % COLS) * (SW + GAP), PAD + (q // COLS) * (SH + GAP)
  end

  local b = S.border
  local built = {}
  -- 只建当前页和左右相邻页的卡片（AI 植物再多，对象数也有上限）；翻页时补建、远处的页清空
  local function draw_card(i)
    local s = list[i]
    local x, y = xy(i)
    local sh = math.min(S.shadow, 4)
    local cw, ch = SW - sh, SH - sh
    local card = app.card(pager.tiles[page_of(i)], x, y, cw, ch, { fill = s.err and C.surfaceAlt or C.surface })
    local iw = cw - 2 * b - 20
    -- 图标：植物自己画的像素图标（≤ 12×12），没有就画一颗种子
    local okicon = false
    if not s.err and type(s.icon) == "table" and type(s.pal) == "table" then
      local pal = {}
      for k, v in pairs(s.pal) do if type(k) == "string" and #k == 1 then pal[k] = (C[v] and require("core.themes").is_color(C[v]) and C[v]) or (require("core.themes").is_color(v) and v) or nil end end
      local rows = {}
      for r = 1, math.min(12, #s.icon) do if type(s.icon[r]) == "string" then rows[#rows + 1] = s.icon[r]:sub(1, 12) end end
      if #rows > 0 then okicon = pcall(ui.sprite, card, 10, 10, 4, rows, pal) end
    end
    if not okicon then
      ui.plain(card, 18, 18, 24, 30, s.err and C.line or "#B07A4A", { radius = 12 })
      ui.plain(card, 28, 8, 4, 12, s.err and C.line or "#5FA35A", { radius = 2 })
    end
    ui.text(card, 66, 10, ui.fit(s.name, iw - 60 - (s.src == "user" and 44 or 0)), C.text)
    if s.src == "user" then app.chip(card, cw - 2 * b - 44, 8, "AI", { fill = "#FF3B30", color = "#FFFFFF" }) end
    ui.text(card, 66, 38, s.src == "user" and "我的创作" or "内置", C.textMuted)
    if s.err then
      ui.text(card, 10, 70, "载入失败", C.danger)
      ui.text(card, 10, 98, ui.wrap(s.err, iw, 2), C.textMuted, { w = iw, h = 60 })
    else
      ui.text(card, 10, 70, ui.wrap(s.desc ~= "" and s.desc or "一株小植物", iw, 2), C.text, { w = iw, h = 60 })
      ui.text(card, 10, ch - 2 * b - 36, planted[s.key] and ("已种 " .. planted[s.key] .. " 株") or "还没种", C.textMuted)
    end
    app.input.bind_tap(ui.catcher(card, cw, ch), function() if i == state.focus then page.plant() else page.focus(i) end end)
  end
  function page.ensure(p)
    for q = 1, npages do
      local want = math.abs(q - p) <= 1
      if want and not built[q] then
        built[q] = true
        rings[q] = app.ring(pager.tiles[q]) -- 焦点框先建：压在卡片下面，只露出外圈，不挡点按
        for i = (q - 1) * PER + 1, math.min(n, q * PER) do draw_card(i) end
        app.log("GD_SEEDS_PAGE build " .. q)
      elseif not want and built[q] then
        built[q] = nil
        pager.tiles[q]:clean()
        rings[q] = nil
      end
    end
  end
  if n == 0 then ui.text(body, 40, 200, "没有找到种子", C.textMuted) end

  local footer = ui.plain(body, 0, GY + GH + 10, W, app.frame.CH - GY - GH - 10)
  function page.footer()
    footer:clean()
    local total = npages * 22
    for p = 1, npages do
      local on = p == pager.index
      ui.plain(footer, (W - total) // 2 + (p - 1) * 22, 6, on and 18 or 10, 10, on and S.chip or C.line, { radius = 5 })
      app.input.bind_tap(ui.catcher(footer, 22, 26, (W - total) // 2 + (p - 1) * 22 - 4, 0), function() page.focus(math.min(n, (p - 1) * PER + 1)) end)
    end
    ui.text(footer, 24, 22, ui.fit("AI 植物放在 DATA/garden/plants/，几秒内出现在这里", W - 48), C.textMuted, { w = W - 48 })
  end

  function page.focus(i, from_swipe)
    if n == 0 then return end
    state.focus = math.max(1, math.min(n, i))
    local p = page_of(state.focus)
    if not from_swipe and pager.index ~= p then pager.show(p) end
    page.ensure(p)
    for q, r in pairs(rings) do if q ~= p then r.hide() end end
    local x, y = xy(state.focus)
    local sh = math.min(S.shadow, 4)
    rings[p].to(x, y, SW - sh, SH - sh, S.radius)
    page.footer()
    app.refresh_keys()
    app.log_state("focus")
  end
  function page.focus_id() return '"' .. (list[state.focus] and list[state.focus].key or "") .. '"' end

  -- 种下：回到花园，焦点落在新的这一株上
  function page.plant()
    local s = list[state.focus]
    if not s then return end
    if s.err then app.frame.sheet("载入失败：" .. s.name, { { text = "原因", sub = s.err }, { text = "知道了" } }, { focus = 2 }); return end
    local idx, err = g.add(s.key)
    if not idx then app.frame.toast(err); return end
    local parent = app.stack[#app.stack - 1]
    if parent and parent.id == "garden" then parent.state.focus = idx; parent.state.page = nil end
    app.frame.toast("种下了：" .. s.name)
    app.back()
  end

  function page.key(k, kind)
    if kind ~= "short" then return k ~= "back" end
    if k == "prev" then page.focus(state.focus <= 1 and n or state.focus - 1); return true end
    if k == "next" then page.focus(state.focus >= n and 1 or state.focus + 1); return true end
    if k == "ok" then page.plant(); return true end
    return false
  end
  function page.labels()
    local s = list[state.focus]
    return { prev = "上一颗", ok = s and (s.err and "看原因" or "种下") or "", next = "下一颗" }
  end
  local count = n
  function page.rescan() if #g.seed_list() ~= count then app.rebuild("seeds") else app.rebuild("reload") end end

  pager.show(page_of(state.focus), false)
  page.ensure(page_of(state.focus))
  page.footer()
  page.focus(state.focus)
  return page
end

return M
