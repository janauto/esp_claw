-- 页面：一株植物的全屏页。植物自己的界面（open）在沙箱里画、在沙箱里响应按键；
-- 核心负责“照料”：浇水、重新加载、移除（长按 M 弹出，触摸用底部三个按钮）。
-- 植物出错：整页换成“枯萎”说明（原因、文件位置、怎么修），别的植物不受影响；修好文件后自动热重载。
local M = {}

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local g = app.garden
  local i = g.index_of(params.uid)
  if not i then error("这株植物已经不在了") end
  local plot = g.plots()[i]
  local seed = g.seed_of(plot)
  local name = seed and seed.name or plot.seed
  local page = { rate = 1000, watch = true }
  local st, err = g.status(plot)

  app.frame.header(body, { back = "花园", title = ui.fit(name, 200),
    chip = st ~= "ok" and "枯萎" or (g.thirsty(plot) and "口渴" or "今天浇过水"),
    on_chip = function() page.water() end })

  local AY, AH = 64, app.frame.CH - 64 - 70
  local b = S.border
  local card = app.card(body, 18, AY, W - 36, AH, { fill = st == "ok" and C.surface or C.surfaceAlt })
  local iw, ih = W - 36 - 2 * b - 24, AH - 2 * b - 24
  local inner = ui.plain(card, 12, 12, iw, ih)
  local view
  if st == "ok" then
    view, err = g.mount(plot, inner, iw, ih, "open")
    if not view then inner:clean(); card:set_style({ bg_color = C.surfaceAlt }); st = "dead" end
  end
  if st ~= "ok" then
    ui.text(inner, 0, 0, "枯萎了", C.danger)
    ui.text(inner, 0, 34, ui.wrap("这株植物的代码出错了，别的植物不受影响。", iw, 2), C.text, { w = iw, h = 60 })
    ui.text(inner, 0, 104, "原因", C.textMuted)
    ui.text(inner, 0, 134, ui.wrap(err or "", iw, 5), C.text, { w = iw, h = 150 })
    ui.text(inner, 0, 292, "文件", C.textMuted)
    ui.text(inner, 0, 322, ui.wrap(seed and seed.path or (plot.src .. "/" .. plot.seed), iw, 2), C.text, { w = iw, h = 60 })
    ui.text(inner, 0, ih - 60, ui.wrap("修好文件后会自动热重载；也可以长按 M 选「重新加载」。", iw, 2), C.textMuted, { w = iw, h = 60 })
  end

  ---------------------------------------------------------------- 照料：浇水 / 重新加载 / 移除
  function page.water()
    if st ~= "ok" then app.frame.toast("先把它救活（重新加载）再浇水吧"); return end
    if g.water(plot) then
      app.save_now()
      app.frame.toast("浇好啦，" .. app.pet.name() .. "很开心")
      app.sched.spawn("plant-refresh", function(task) task.sleep(300); app.rebuild("watered") end)
    else
      app.frame.toast(app.clock_ok() and "今天已经浇过水了" or "设备还没对时，先不记浇水")
    end
  end
  function page.reload()
    g.reload(plot)
    app.frame.toast(g.status(plot) == "ok" and ("已重新加载：" .. name) or "重新加载了，但还是有错")
    app.rebuild("reload")
  end
  function page.remove()
    app.frame.sheet("把「" .. name .. "」从花园移除？它的记录也会删掉", {
      { text = "移除", sub = "不能撤销", act = function()
        g.remove(g.index_of(plot.uid) or i)
        app.frame.toast("已移除：" .. name)
        app.back()
      end },
      { text = "取消" },
    }, { focus = 2 })
  end
  function page.more()
    app.frame.sheet(name, {
      { text = "浇水", sub = st ~= "ok" and "先救活" or (g.thirsty(plot) and "口渴了" or "今天浇过了"), act = page.water },
      { text = "重新加载", sub = "重读植物文件", act = page.reload },
      { text = "移除", sub = "从花园拔掉", act = page.remove },
      { text = "取消" },
    })
  end

  local bar_y = app.frame.CH - 60
  local bw = (W - 36 - 20) // 3
  for k, it in ipairs({ { "浇水", page.water }, { "重新加载", page.reload }, { "移除", page.remove } }) do
    local btn = app.card(body, 18 + (k - 1) * (bw + 10), bar_y, bw, 46, { radius = 23, shadow = 0, fill = k == 1 and S.chip or C.surface })
    ui.text(btn, (bw - ui.text_w(it[1])) // 2 - b, 8, it[1], k == 1 and S.chip_text or C.text)
    app.input.bind_tap(ui.catcher(btn, bw, 46), it[2])
  end

  ---------------------------------------------------------------- 刷新与按键（植物的回调都在沙箱预算里）
  if view and tonumber(view.rate) then page.rate = math.max(200, tonumber(view.rate)) end
  function page.update(now) if view then g.view_call(plot, view, "update", 60000, now) end end
  function page.key(k, kind)
    if k == "ok" and kind == "long" then page.more(); return true end
    if kind ~= "short" then return k ~= "back" end
    if k == "back" then return false end
    if view and view.key then
      local ok, handled = g.view_call(plot, view, "key", 80000, k)
      if ok and handled then app.mark_dirty("plants"); return true end
    end
    if k == "ok" then -- 植物自己不用 M 时：口渴就浇水，否则弹出“更多”
      if st == "ok" and g.thirsty(plot) then page.water() else page.more() end
    end
    return true
  end
  function page.labels()
    local l = {}
    if view and view.labels then
      local ok, r = g.view_call(plot, view, "labels", 20000)
      if ok and type(r) == "table" then l = r end
    end
    local ok_label = type(l.ok) == "string" and l.ok ~= "" and l.ok or (st == "ok" and g.thirsty(plot) and "浇水" or "更多")
    return { prev = type(l.prev) == "string" and l.prev or "", ok = ok_label, next = type(l.next) == "string" and l.next or "" }
  end
  function page.plant_failed() app.input.post(function() app.rebuild("withered") end) end
  function page.destroy() app.sched.kill("plant-refresh") end
  function page.rescan() app.rebuild("reload") end
  function page.focus_id() return plot.uid end
  function page.mode() return st end
  return page
end

return M
