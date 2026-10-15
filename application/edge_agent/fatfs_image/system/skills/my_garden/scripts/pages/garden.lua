-- 页面：花园。地块网格 2 列 × 3 行一页（可翻页），每块种着一株植物 = 一个沙箱里的小 Lua 模块。
--   按键：◀▶ 换地块（跨页自动翻）；M 打开植物；长按 M 拿起 → ◀▶ 移动 → M 或 SET 放下；
--         焦点在页头“全部浇水”上按 M = 给口渴的植物都浇水（小磁会开心）；最后一格“种植物”进种子库。
--   触摸：点地块第一下聚焦、第二下打开；长按地块 = 拿起，拿起后点别的地块 = 挪过去，底部按钮 前移 / 放下 / 后移；
--         左右滑翻页（字体准备好后用原生 tileview；之前点页码翻页）。
--   性能：只有焦点地块 1 秒刷新一次（“活”的那一株），同页其他地块 5 秒一次，别的页不刷新。
--   隔离：每张卡片都在沙箱预算里画；某一株出错只把它画成“枯萎 + 原因”，别的地块照常。
local Pager = require("core.pager")

local M = {}
local COLS, ROWS = 2, 3
local PER = COLS * ROWS

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local g = app.garden
  local page = { rate = 1000, watch = true }
  local plots = g.plots()
  local nslots = math.min(#plots + 1, g.MAX_SLOTS or 12) -- 最后一格是“种植物”
  if #plots >= require("core.garden").MAX then nslots = #plots end -- 满了就不显示“种植物”
  local npages = math.max(1, math.ceil(nslots / PER))
  state.focus = math.max(0, math.min(state.focus or 1, nslots))
  local thirsty = g.thirsty_count()
  if state.focus == 0 and thirsty == 0 then state.focus = 1 end

  local chip_ring = app.ring(body) -- 先建焦点框：压在页头标签下面，不挡点按
  app.frame.header(body, { back = "我的", title = "花园",
    chip = thirsty > 0 and ("全部浇水 " .. thirsty) or ("种子 " .. #g.seed_list()),
    on_chip = function() if thirsty > 0 then page.water_all() else app.open("seeds") end end })

  ---------------------------------------------------------------- 网格与分页
  local GX, GY, GW, GH, GAP = 18, 64, W - 36, PER // COLS * 184 + (ROWS - 1) * 12, 12
  local SW, SH = (GW - GAP) // COLS, (GH - (ROWS - 1) * GAP) // ROWS
  local pager
  local slots, views, rings, tasks = {}, {}, {}, {}
  local function page_of(i) return math.max(1, math.ceil(i / PER)) end
  local PAD = 8 -- 分页容器比网格大一圈，焦点框画在格子外面不会被裁掉
  local function slot_xy(i)
    local p = (i - 1) % PER
    return PAD + (p % COLS) * (SW + GAP), PAD + (p // COLS) * (SH + GAP)
  end

  local footer = ui.plain(body, 0, GY + GH + 10, W, app.frame.CH - GY - GH - 10)

  ---------------------------------------------------------------- 画一个地块
  local function draw_dead(box, w, h, err)
    ui.text(box, 0, 0, "枯萎", C.danger)
    ui.text(box, 0, 30, ui.wrap(err or "", w, 2), C.textMuted, { w = w, h = 60 })
    ui.text(box, 0, h - 28, ui.fit("打开看原因和修法", w), C.textMuted)
  end

  local draw_slot -- 前置声明（点按回调里要用）
  local function tap_slot(i)
    if state.moving then page.drop_at(i); return end
    if i == state.focus then page.activate() else page.focus(i) end
  end
  local function long_slot(i)
    page.focus(i)
    if plots[i] and not state.moving then page.pick() end
  end

  function draw_slot(i)
    local box = slots[i]
    if not box then return end
    box:clean()
    views[i] = nil
    local sh = math.min(S.shadow, 4)
    local cw, ch = SW - sh, SH - sh
    local plot = plots[i]
    if not plot then -- “种植物”空位
      local card = app.card(box, 0, 0, cw, ch, { fill = C.bg, border_color = C.line, shadow = 0 })
      ui.plain(card, cw // 2 - 4 - S.border, 34, 8, 44, S.chip, { radius = 3 })
      ui.plain(card, cw // 2 - 22 - S.border, 52, 44, 8, S.chip, { radius = 3 })
      ui.text(card, cw // 2 - 30 - S.border, 92, "种植物", C.text)
      local sub = ui.fit("种子库 " .. #g.seed_list() .. " 颗", cw - 20)
      ui.text(card, (cw - 2 * S.border - ui.text_w(sub)) // 2, 124, sub, C.textMuted)
      app.input.bind_tap(ui.catcher(card, cw, ch), function() tap_slot(i) end)
      return
    end
    local st, err = g.status(plot)
    local moving = state.moving == plot.uid
    local card = app.card(box, 0, 0, cw, ch, { fill = st == "ok" and C.surface or C.surfaceAlt,
      border_color = moving and S.focus or nil, border = moving and math.max(4, S.border + 2) or nil, shadow = moving and 0 or nil })
    local seed = g.seed_of(plot)
    local name = seed and seed.name or plot.seed
    app.chip(card, 8, 8, ui.fit(name, cw - 70), st == "ok" and { fill = S.tag, color = S.tag_text } or { fill = C.line, color = C.surface })
    if seed and seed.src == "user" then app.chip(card, cw - 2 * S.border - (g.thirsty(plot) and 66 or 42), 8, "AI", { fill = "#FF3B30", color = "#FFFFFF" }) end
    if g.thirsty(plot) and st == "ok" then ui.drop(card, cw - 2 * S.border - 24, 13, "#3D8BD9") end
    local iw, ih = cw - 2 * S.border - 20, ch - 2 * S.border - 50
    local inner = ui.plain(card, 10, 44, iw, ih)
    if st ~= "ok" then
      draw_dead(inner, iw, ih, err)
    else
      local v, e = g.mount(plot, inner, iw, ih, "card")
      if not v then inner:clean(); card:set_style({ bg_color = C.surfaceAlt }); draw_dead(inner, iw, ih, e)
      else views[i] = v end
    end
    if moving then app.chip(card, cw - 2 * S.border - 72, ch - 2 * S.border - 32, "移动中") end
    app.input.bind_tap(ui.catcher(card, cw, ch), function() tap_slot(i) end, function() long_slot(i) end)
  end

  local function build_grid()
    pager = Pager.new(app, body, GX - PAD, GY - PAD, GW + 2 * PAD, GH + 2 * PAD, npages, function(p, how)
      state.page = p
      if page_of(state.focus) ~= p then -- 滑到新的一页：焦点落在这一页同样的位置
        local pos = (state.focus >= 1 and (state.focus - 1) % PER or 0)
        page.focus(math.min(nslots, (p - 1) * PER + pos + 1), true)
      end
      page.paint_footer()
      app.log_state(how)
    end)
    for p = 1, npages do rings[p] = app.ring(pager.tiles[p]) end
    for i = 1, nslots do
      local x, y = slot_xy(i)
      slots[i] = ui.plain(pager.tiles[page_of(i)], x, y, SW, SH)
      draw_slot(i)
    end
  end

  ---------------------------------------------------------------- 底部：页码 / 提示，拿起时换成 前移 · 放下 · 后移
  local dots
  function page.paint_footer()
    footer:clean()
    if state.moving then
      local labels = { { "前移", -1 }, { "放下", 0 }, { "后移", 1 } }
      local bw = (W - 36 - 20) // 3
      for k, it in ipairs(labels) do
        local b = app.card(footer, 18 + (k - 1) * (bw + 10), 4, bw, 44, { radius = 22, fill = k == 2 and S.chip or C.surface, shadow = 0 })
        ui.text(b, (bw - ui.text_w(it[1])) // 2 - S.border, 7, it[1], k == 2 and S.chip_text or C.text)
        app.input.bind_tap(ui.catcher(b, bw, 44), function() if it[2] == 0 then page.drop() else page.shift(it[2]) end end)
      end
      return
    end
    dots = {}
    local total = npages * 22
    for p = 1, npages do
      local on = p == (pager and pager.index or 1)
      local d = ui.plain(footer, (W - total) // 2 + (p - 1) * 22, 6, on and 18 or 10, 10, on and S.chip or C.line, { radius = 5 })
      dots[p] = d
      app.input.bind_tap(ui.catcher(footer, 22, 26, (W - total) // 2 + (p - 1) * 22 - 4, 0), function() page.focus(math.min(nslots, (p - 1) * PER + 1)) end)
    end
    ui.text(footer, 24, 22, pager and pager.swipe and "左右滑翻页    长按 M 或长按地块：拿起换位置" or "长按 M 或长按地块：拿起换位置", C.textMuted, { w = W - 48 })
  end

  ---------------------------------------------------------------- 焦点
  function page.focus(i, from_swipe)
    state.focus = math.max(0, math.min(nslots, i))
    if state.focus == 0 and thirsty == 0 then state.focus = 1 end
    for p, r in ipairs(rings) do if p ~= page_of(state.focus) or state.focus == 0 then r.hide() end end
    if state.focus == 0 then
      chip_ring.to(W - 18 - (ui.text_w("全部浇水 " .. thirsty) + 20), 15, ui.text_w("全部浇水 " .. thirsty) + 20, 30, 15)
    else
      chip_ring.hide()
      local x, y = slot_xy(state.focus)
      local sh = math.min(S.shadow, 4)
      rings[page_of(state.focus)].to(x, y, SW - sh, SH - sh, S.radius)
      local p = page_of(state.focus)
      if not from_swipe and pager.index ~= p then pager.show(p); state.page = p; page.paint_footer() end
    end
    app.refresh_keys()
    app.log_state("focus")
  end
  function page.focus_id() return state.focus end
  function page.mode() return state.moving and "move" or "" end

  function page.activate()
    if state.focus == 0 then page.water_all(); return end
    local plot = plots[state.focus]
    if not plot then app.open("seeds"); return end
    app.open("plant", { uid = plot.uid })
  end

  ---------------------------------------------------------------- 拿起 / 移动 / 放下
  function page.pick()
    local plot = plots[state.focus]
    if not plot then return end
    state.moving = plot.uid
    draw_slot(state.focus)
    page.paint_footer()
    app.frame.toast("拿起了：" .. (g.seed_of(plot) and g.seed_of(plot).name or plot.seed))
    app.log_state("pick")
  end
  function page.shift(d)
    local i = g.index_of(state.moving)
    if not i then state.moving = nil; return end
    local j = g.move(i, d)
    if j == i then app.frame.toast(d < 0 and "已经在最前面了" or "已经在最后面了"); return end
    draw_slot(i); draw_slot(j)
    page.focus(j)
    app.log_state("move")
  end
  function page.drop_at(i)
    local from = g.index_of(state.moving)
    if not from then state.moving = nil; return end
    local j = g.move_to(from, math.min(i, #plots))
    for k = math.min(from, j), math.max(from, j) do draw_slot(k) end
    page.focus(j)
    page.drop()
  end
  function page.drop()
    local i = g.index_of(state.moving)
    state.moving = nil
    if i then draw_slot(i) end
    page.paint_footer()
    app.save_now()
    app.frame.toast("放好了")
    app.log_state("drop")
  end

  ---------------------------------------------------------------- 浇水（协程：水滴从地块上方落下）
  function page.water_all()
    if state.moving then page.drop() end -- 拿着植物时点“全部浇水”：先放下
    local n = 0
    for i, p in ipairs(plots) do
      if g.water(p) then
        n = n + 1
        if slots[i] and page_of(i) == pager.index then
          local box = slots[i]
          tasks[#tasks + 1] = "drops" .. i
          app.sched.spawn("drops" .. i, function(task)
            local drops = {}
            for k = 1, 3 do drops[k] = ui.plain(box, 40 + k * 36, 10, 10, 16, "#3D8BD9", { radius = 5 }) end
            for step = 1, 6 do
              for k, d in ipairs(drops) do d:set_pos(40 + k * 36, 10 + step * 22 + (k % 2) * 10) end
              task.sleep(70)
            end
            draw_slot(i)
          end)
        else
          draw_slot(i)
        end
      end
    end
    app.save_now()
    app.frame.toast(n > 0 and ("浇了 " .. n .. " 株，" .. app.pet.name() .. "很开心") or "今天都浇过水了")
    thirsty = g.thirsty_count()
    if state.focus == 0 then state.focus = 1 end
    tasks[#tasks + 1] = "water-refresh"
    app.sched.spawn("water-refresh", function(task) task.sleep(600); app.rebuild("watered") end) -- 页头的浇水数字跟着变
  end

  ---------------------------------------------------------------- 刷新：焦点地块 1 Hz，同页其他地块 5 s 一次
  local tick_n = 0
  function page.update(now)
    tick_n = tick_n + 1
    local p = pager.index
    for i = (p - 1) * PER + 1, math.min(nslots, p * PER) do
      local v, plot = views[i], plots[i]
      if v and plot and (i == state.focus or tick_n % 5 == 0) then g.view_call(plot, v, "update", 60000, now) end
    end
  end
  -- 某一株在运行中出错：只重画它自己
  function page.plant_failed(plot)
    local i = g.index_of(plot.uid)
    if i then app.input.post(function() draw_slot(i) end) end
  end
  function page.rescan() app.rebuild("reload") end
  function page.destroy() for _, t in ipairs(tasks) do if t ~= "water-refresh" then app.sched.kill(t) end end end

  ---------------------------------------------------------------- 按键
  function page.key(k, kind)
    if state.moving then
      if kind == "short" and k == "prev" then page.shift(-1)
      elseif kind == "short" and k == "next" then page.shift(1)
      elseif (k == "ok" and (kind == "short" or kind == "long")) or (k == "back" and kind == "short") then page.drop() end
      return true -- 拿起状态下 SET = 放下（不是返回）
    end
    if kind == "long" and k == "ok" then
      if plots[state.focus] then page.pick() end
      return true
    end
    if kind ~= "short" then return k ~= "back" end
    local lo = thirsty > 0 and 0 or 1
    if k == "prev" then page.focus(state.focus <= lo and nslots or state.focus - 1); return true end
    if k == "next" then page.focus(state.focus >= nslots and lo or state.focus + 1); return true end
    if k == "ok" then page.activate(); return true end
    return false
  end
  function page.labels()
    if state.moving then return { prev = "前移", ok = "放下", next = "后移", back = "放下" } end
    local ok = state.focus == 0 and "浇水" or plots[state.focus] and "打开" or "种植物"
    return { prev = "上一株", ok = ok, next = "下一株" }
  end

  build_grid()
  local p0 = state.page or page_of(state.focus)
  if pager.show then pager.show(page_of(math.max(1, state.focus)) or p0, false) end
  page.paint_footer()
  page.focus(state.focus)
  return page
end

return M
