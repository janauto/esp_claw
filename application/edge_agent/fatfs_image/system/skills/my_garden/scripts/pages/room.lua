-- 页面：房间全屏页（从小家进入某个房间）。房间应用只通过 ctx 拿颜色与绘图助手，不知道当前主题。
local M = {}

function M.new(app, body, params, state)
  local ui, W = app.ui, app.W
  local a
  for _, r in ipairs(app.rooms) do if r.id == params.id then a = r end end
  if not a then error("没有这个房间：" .. tostring(params.id)) end
  local page = { rate = a.kind == "play" and 250 or 1000 }
  app.frame.header(body, { back = "小家", title = a.name, chip = a.demo and "演示数据" or nil })
  local y = 64
  local h = app.frame.CH - y - 12
  local card = app.card(body, 18, y, W - 36, h, { fill = app.C.bg })
  local b = app.S.border
  local inner = ui.plain(card, b, b, W - 36 - 2 * b, h - 2 * b)
  local ctx = app.room_ctx()
  local view = a.open(ctx, inner, W - 36 - 2 * b, h - 2 * b, app.room_state(a))
  if type(view) ~= "table" then error("房间没有返回页面") end
  local t_last = app.now()
  function page.update(now)
    local dt = now - t_last
    t_last = now
    if view.update then view.update(dt) end
  end
  function page.key(k, kind)
    if k == "back" or kind ~= "short" then return false end
    if view.key and view.key(k, kind) then app.mark_dirty("rooms"); return true end
    return true
  end
  function page.labels()
    local l = (view.labels and view.labels()) or {}
    return { prev = l.prev or "", ok = l.ok or "", next = l.next or "", back = l.back }
  end
  function page.focus_id() return '"' .. a.id .. '"' end
  return page
end

return M
