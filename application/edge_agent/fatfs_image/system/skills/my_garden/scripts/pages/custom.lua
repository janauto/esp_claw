-- 页面：个性化（形象与主题）。设备只有四颗键、没有中文输入：名字从预设里挑，配色从一组强调色里挑。
--   ◀▶ 选一行，M 换下一个值（立刻生效、自动保存）；触摸：点一行 = 换下一个值，点行两端的小箭头 = 往回 / 往前。
-- 配色只覆盖当前主题的强调色（主题 token 的第一层元表），其余颜色沿用主题；强调色上的文字颜色按对比度自动选黑或白。
local Themes = require("core.themes")

local M = {}
M.NAMES = { "小磁", "磁磁", "阿磁", "小夹", "豆豆", "团子", "小橘", "波波", "汤圆", "布丁" }
M.COLORS = { { "主题默认", nil }, { "橘子", "#F08A24" }, { "番茄", "#E5533B" }, { "天空", "#3D8BD9" }, { "薄荷", "#1FA67A" },
  { "葡萄", "#8B5CF6" }, { "樱花", "#E86A9E" }, { "柠檬", "#E3B505" } }

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local data = app.data
  local page = {}
  state.focus = state.focus or 1
  app.frame.header(body, { back = "我的", title = "个性化", chip = "主题 " .. app.theme.name })

  -- 预览：当前主题的小磁 + 名字
  local pv = app.card(body, 18, 64, W - 36, 210, { fill = S.scene })
  local ctx = { ui = ui, C = C, W = W, H = app.H, date = app.date, now = app.now }
  local mascot_fn = app.T.mascot or require("themes.house").mascot
  local mascot = app.safe("theme.mascot", mascot_fn, ctx, pv, 40, 30, 8)
  ui.text(pv, 230, 50, "名字", C.textMuted)
  local name_chip = app.chip(pv, 230, 82, app.pet.name())
  ui.text(pv, 230, 130, ui.fit("配色只作用于当前主题", W - 36 - 250), C.textMuted, { w = W - 36 - 250, h = 60 })

  local function color_index()
    local cur = (data.settings.colors[app.theme.id] or {}).accent
    for i, c in ipairs(M.COLORS) do if c[2] == cur then return i end end
    return 1
  end
  local function name_index()
    for i, n in ipairs(M.NAMES) do if n == app.pet.name() then return i end end
    return 1
  end
  local rows = {
    { title = "名字", value = function() return app.pet.name() end,
      step = function(d) data.pet.name = M.NAMES[(name_index() - 1 + d) % #M.NAMES + 1] end },
    { title = "配色", value = function() return M.COLORS[color_index()][1] end,
      step = function(d)
        local c = M.COLORS[(color_index() - 1 + d) % #M.COLORS + 1]
        data.settings.colors[app.theme.id] = c[2] and { accent = c[2] } or nil
      end },
    { title = "恢复默认", value = function() return "名字和配色" end,
      step = function() data.pet.name = "小磁"; data.settings.colors[app.theme.id] = nil end },
  }
  local ring = app.ring(body)
  local RY, RH = 292, 72
  for i, r in ipairs(rows) do
    local y = RY + (i - 1) * (RH + 14)
    local c = app.card(body, 18, y, W - 36, RH)
    ui.text(c, 20, (RH - 2 * S.border - 28) // 2, r.title, C.text)
    local v = ui.text(c, 150, (RH - 2 * S.border - 28) // 2, r.value(), C.textMuted, { w = 200 })
    if i == 2 then
      local cur = (data.settings.colors[app.theme.id] or {}).accent or app.C.accent
      ui.plain(c, W - 36 - 2 * S.border - 140, (RH - 2 * S.border - 24) // 2, 24, 24, cur, { radius = 12, border = 2, border_color = C.line })
    end
    r.y, r.label = y, v
    app.input.bind_tap(ui.catcher(c, W - 36, RH), function() page.focus(i); page.change(1) end)
    if i < 3 then
      for k, dir in ipairs({ "left", "right" }) do
        local bx = W - 36 - 2 * S.border - (k == 1 and 96 or 46)
        local btn = ui.plain(c, bx, (RH - 2 * S.border - 36) // 2, 38, 36, C.surfaceAlt, { radius = 18 })
        ui.arrow(btn, dir == "left" and 12 or 16, 11, dir, 14, C.text)
        app.input.bind_tap(ui.catcher(btn, 38, 36), function() page.focus(i); page.change(dir == "left" and -1 or 1) end)
      end
    end
  end
  ui.text(body, 24, RY + 3 * (RH + 14) + 6, ui.wrap("名字会出现在「我的」页头、气泡和系统首页的小组件里；更多形象和主题可以在手机 App 或电脑上用一句话生成。", W - 48, 3), C.textMuted, { w = W - 48, h = 96 })

  function page.focus(i)
    state.focus = (i - 1) % #rows + 1
    ring.to(18, rows[state.focus].y, W - 36, RH, S.radius)
    app.refresh_keys()
    app.log_state("focus")
  end
  -- 改一个值：保存、重新套主题颜色、写快照，然后用同一状态重建本页（预览立刻变）
  function page.change(d)
    rows[state.focus].step(d)
    app.apply_theme()
    app.frame.retheme()
    app.save_now()
    app.snapshot(true)
    app.frame.toast(state.focus == 3 and "已恢复默认" or (rows[state.focus].title .. "：" .. rows[state.focus].value()))
    app.rebuild("custom")
  end
  function page.key(k, kind)
    if kind ~= "short" then return k ~= "back" end
    if k == "prev" then page.focus(state.focus - 1); return true end
    if k == "next" then page.focus(state.focus + 1); return true end
    if k == "ok" then page.change(1); return true end
    return false
  end
  function page.labels() return { prev = "上一项", ok = state.focus == 3 and "恢复" or "换一个", next = "下一项" } end
  function page.focus_id() return state.focus end
  local t_anim = 0
  function page.anim(now)
    if mascot and now - t_anim >= 125 then t_anim = now; mascot.set(app.pet.face(now), now) end
  end
  page.focus(state.focus)
  return page
end

return M
