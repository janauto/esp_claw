-- 页面：小家（主题外壳的房子剖面，设计稿 04 屏）。
-- 复用主题外壳的三套主题与房间应用：主题的 build() 只画中枢（f.hub_only），房间由核心驱动——
--   焦点房间实时预览（番茄钟、时钟 1 Hz；贪吃蛇 4 Hz 自动演示），按 M 直接在房间里玩贪吃蛇；
--   小磁（像素主题）用协程一格一格走到焦点房间；其他房间每 2 秒刷新一次文字概述。
-- 去掉了主题外壳里的“经典栏 / 回系统首页 / 控制中心 / 按住说话演示”：这些在 1.0 归系统首页负责。
-- 自定义主题（<DATA>/garden/themes，主题外壳接口 v1）没有 hub_only 也能用：多画的部分丢进屏幕外的容器。
local sandbox = require("core.sandbox")

local M = {}

function M.new(app, body, params, state)
  local ui, W = app.ui, app.W
  local T, C = app.T, app.C
  local entries = app.rooms
  local page = { rate = 1000 }
  state.focus = state.focus or 1
  state.play = false -- 重建（例如换主题）会结束房间里的这一局

  app.frame.header(body, { back = "我的", title = "小家", chip = "EPP 主题外壳" })
  local HUB_Y, HUB_H = 58, app.frame.CH - 58 - 36
  local hub = ui.plain(body, 0, HUB_Y, W, HUB_H)
  ui.text(body, 24, app.frame.CH - 32, "SET 返回我的    底部上滑直接回系统首页", C.textMuted, { w = W - 48 })

  -- 主题画中枢。v1 主题会把状态栏、首页、控制中心也画出来：给它们一个屏幕外的容器
  local ctx = app.room_ctx()
  ctx.walk = true -- 小磁由核心的协程带着走，主题不要瞬移它
  local function sink() return ui.plain(body, W * 3, 0, W, app.H) end
  local f = { screen = sink(), tiles = { home = sink(), hub = hub, control = sink() }, w = W, h = HUB_H, H = app.H, entries = entries, hub_only = true }
  local ok, view
  if app.theme.src == "user" then ok, view = sandbox.guard(T.build, 4000000, ctx, f)
  else ok, view = pcall(T.build, ctx, f) end
  if not ok or type(view) ~= "table" or type(view.hub) ~= "table" then
    app.log("GD_ERR theme.build " .. app.theme.id .. " " .. tostring(view))
    hub:clean()
    view = require("themes.minimal").build(ctx, f) -- 主题画不出小家：降级为极简，房间照常可用
    app.frame.toast("这个主题的小家画不出来，先用极简样式")
  end
  local hv = view.hub

  ---------------------------------------------------------------- 房间预览：同一时刻只跑焦点房间这一个
  local prev_h, prev_i
  local function stop_play(note)
    if not state.play then return end
    state.play = false
    if prev_h and prev_h.set_play then app.safe("play.set", prev_h.set_play, false) end
    app.safe("hub.play", hv.play, prev_i or state.focus, false)
    page.rate = 1000
    if note then app.frame.toast(note) end
  end
  local function unmount()
    stop_play()
    if prev_i then
      local box = app.safe("hub.slot", hv.slot, prev_i)
      if box then box:clean() end
      app.safe("hub.live", hv.live, prev_i, false)
    end
    prev_h, prev_i = nil, nil
  end
  local function mount()
    unmount()
    local a = entries[state.focus]
    page.rate = (a and a.kind == "play") and 250 or 1000
    if not a or type(a.preview) ~= "function" then return end
    local box, w, h, C2 = app.safe("hub.slot", hv.slot, state.focus)
    if not box then return end
    local pctx = ctx
    if type(C2) == "table" then pctx = setmetatable({ C = setmetatable(C2, { __index = C }) }, { __index = ctx }) end -- 反白卡片：只给预览换一套颜色
    prev_h = app.safe("room.preview." .. a.id, a.preview, pctx, box, w, h, app.room_state(a))
    if prev_h then prev_i = state.focus; app.safe("hub.live", hv.live, state.focus, true) end
  end
  local function summaries()
    for i, a in ipairs(entries) do
      local v, l = app.safe("room.summary." .. a.id, a.summary, app.room_state(a), ctx)
      app.safe("hub.summary", hv.summary, i, tostring(v or ""), tostring(l or ""))
    end
  end

  ---------------------------------------------------------------- 小磁走到焦点房间（协程：每 60 ms 走 8 像素，像素主题的动效性格）
  local walker = hv.walker
  local function walk_to(i, instant)
    if not walker then return end
    local tx, ty = walker.spot(i)
    if instant or not state.wx then
      state.wx, state.wy = tx, ty
      walker.box:set_pos(tx, ty)
      return
    end
    app.sched.spawn("walk", function(task)
      while state.wx ~= tx or state.wy ~= ty do
        local dx, dy = tx - state.wx, ty - state.wy
        state.wx = state.wx + (dx > 0 and math.min(8, dx) or math.max(-8, dx))
        state.wy = state.wy + (dy > 0 and math.min(8, dy) or math.max(-8, dy))
        walker.box:set_pos(state.wx, state.wy)
        task.sleep(60)
      end
    end)
  end

  local function focus(i, instant)
    state.focus = (i - 1) % #entries + 1
    app.safe("hub.focus", hv.focus, state.focus)
    mount()
    walk_to(state.focus, instant)
    app.log_state("room-focus")
  end

  local function activate()
    local a = entries[state.focus]
    if a.kind == "play" and prev_h and prev_h.set_play then
      state.play = true
      app.safe("play.set", prev_h.set_play, true)
      app.safe("hub.play", hv.play, state.focus, true)
      app.frame.toast("在房间里玩：上一个 / 下一个键转向，SET 退出", 3000)
      app.log_state("play")
    else
      app.open("room", { id = a.id })
    end
  end

  -- 触摸：点房间第一下聚焦，第二下进入（可以玩的房间直接开始游玩）；点站在房间里的小磁 = 点这个房间
  for i, room in ipairs(hv.rooms or {}) do
    app.input.bind_tap(room, function()
      if state.play then return end
      if i == state.focus then activate() else focus(i) end
    end)
  end
  if hv.focus_tap then app.input.bind_tap(hv.focus_tap, function() if not state.play then activate() end end) end

  ---------------------------------------------------------------- 刷新
  local t_sum, t_last = 0, app.now()
  function page.update(now)
    local dt = now - t_last
    t_last = now
    if prev_h and prev_h.update then app.safe("preview.update", prev_h.update, dt) end
    if now - t_sum >= 2000 then t_sum = now; summaries() end
  end

  ---------------------------------------------------------------- 按键
  function page.key(k, kind)
    if state.play then
      if k == "back" and (kind == "short" or kind == "long") then stop_play(); app.log_state("play-end"); return true end
      if kind == "short" and prev_h and prev_h.key then app.safe("play.key", prev_h.key, k, kind) end
      return true
    end
    if kind ~= "short" and not (k == "ok" and kind == "long") then return k ~= "back" and true or false end
    if k == "prev" then focus(state.focus - 1); return true end
    if k == "next" then focus(state.focus + 1); return true end
    if k == "ok" then activate(); return true end
    return false
  end
  function page.labels()
    if state.play and prev_h and prev_h.labels then
      local l = app.safe("play.labels", prev_h.labels) or {}
      return { prev = l.prev or "", ok = l.ok or "", next = l.next or "", back = l.back or "退出" }
    end
    local a = entries[state.focus]
    return { prev = "上一间", ok = a.kind == "play" and "玩一局" or "进入", next = "下一间" }
  end
  function page.mode() return state.play and "play" or "" end
  function page.focus_id() return '"' .. entries[state.focus].id .. '"' end
  function page.destroy() app.sched.kill("walk"); unmount() end

  summaries()
  focus(state.focus, true)
  return page
end

return M
