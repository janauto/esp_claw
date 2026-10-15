-- 我的 · 花园：核心状态机。负责页面栈、按键路由、主题装载、落盘节流、快照、主循环与性能记录。
-- 分层：
--   core/   本目录：与具体页面无关的能力（外框、输入、调度、沙箱、存储、小磁、主题注册表、花园模型）
--   pages/  页面：我的 · 小磁 / 小家 / 花园 / 种子库 / 植物 / 房间 / 个性化 / 一句话生成
--   themes/ 主题：只管“怎么画”；rooms/ 小家的房间应用；plants/ 内置种子（与用户植物走同一个沙箱）
-- 同一时刻只挂载一个页面的界面对象（离开页面就销毁，状态留在页面栈里，回来时按状态重建）。
local M = {}

local PAGES = { "mine", "house", "garden", "seeds", "plant", "room", "custom", "vibe" }

local function default_data()
  return { v = 1, pet = {}, settings = { theme = "house", colors = {} }, rooms = {}, plots = nil, next_uid = 1 }
end

function M.run(env)
  local lvgl = env.lvgl
  local app = { env = env, lvgl = lvgl, running = true }

  ---------------------------------------------------------------- 时间与系统信息（仿真里缺的接口全部 pcall 兜底）
  app.now = env.now
  function app.time()
    local ok, t = pcall(env.time)
    return (ok and tonumber(t)) or os.time()
  end
  function app.clock_ok() return app.time() > 1704067200 end -- 2024-01-01 之前视为没对时
  function app.date(f) local ok, v = pcall(env.date, f); return ok and v or "" end
  function app.online()
    local ok, ip = pcall(env.ip)
    return ok and type(ip) == "string" and ip ~= "" and ip ~= "0.0.0.0"
  end
  app.log = env.log

  ---------------------------------------------------------------- 模块
  app.ui = require("core.ui").bind(lvgl)
  app.store = require("core.store").new(env)
  app.input = require("core.input").new(env)
  app.sched = require("core.sched").new(env)
  local Themes = require("core.themes")
  local ui = app.ui

  ---------------------------------------------------------------- 数据（garden.json）
  local data, status = app.store.load_garden()
  local fresh = data == nil
  data = data or default_data()
  data.pet = type(data.pet) == "table" and data.pet or {}
  data.settings = type(data.settings) == "table" and data.settings or { theme = "house" }
  data.settings.colors = type(data.settings.colors) == "table" and data.settings.colors or {}
  data.rooms = type(data.rooms) == "table" and data.rooms or {}
  data.next_uid = tonumber(data.next_uid) or 1
  app.data = data
  env.log("GD_DATA " .. app.store.paths.garden .. " status=" .. status)

  -- 落盘节流：改动先记脏，最多每 1.5 s 写一次；关键操作调用 save_now() 立刻写。
  -- 原因：系统底部上滑会直接停止 Lua（之后每 100 条指令就抛错），来不及做退出清理，只能平时勤快地写。
  local dirty, last_save, last_snap = {}, 0, ""
  function app.mark_dirty(what) dirty[what or "data"] = true end
  function app.save_now()
    local ok, err = app.store.save_garden(data)
    if not ok then env.log("GD_WARN save failed " .. tostring(err)) end
    last_save = app.now()
    for k in pairs(dirty) do dirty[k] = nil end
    return ok
  end

  ---------------------------------------------------------------- 小磁
  app.pet = require("core.pet").new(app, data.pet)

  ---------------------------------------------------------------- 主题（颜色用元表继承：用户配色 → 主题 → 基础）
  app.themes = Themes.new(app)
  -- 上次选的是 AI 主题（DATA/garden/themes）：先扫描再套用，否则重启后会悄悄退回极简
  if not app.themes.by_id[data.settings.theme] then app.themes.scan() end
  function app.apply_theme()
    local e = app.themes.get(data.settings.theme)
    app.theme, app.T = e, e.mod
    app.C = Themes.palette(e.mod, data.settings.colors[e.id])
    app.S = Themes.style(e.mod, app.C, data.settings.colors[e.id])
  end
  app.apply_theme()

  -- 主题风格的卡片：硬投影（像素风）+ 描边 + 圆角，全部由 style 决定
  function app.card(parent, x, y, w, h, opts)
    opts = opts or {}
    local S, C = app.S, app.C
    local sh = opts.shadow or S.shadow
    if sh and sh > 0 then ui.plain(parent, x + sh, y + sh, w, h, S.ink, { radius = opts.radius or S.radius }) end
    return ui.plain(parent, x, y, w, h, opts.fill or C.surface, { radius = opts.radius or S.radius, border = opts.border or S.border,
      border_color = opts.border_color or S.ink })
  end

  -- 小标签（强调色胶囊）：返回对象与宽度
  function app.chip(parent, x, y, text, opts)
    opts = opts or {}
    local w = ui.text_w(text) + 20
    local o = ui.plain(parent, x, y, w, 28, opts.fill or app.S.chip, { radius = 14 })
    ui.text(o, 10, 2, text, opts.color or app.S.chip_text)
    return o, w
  end
  -- 焦点框：一个透明描边对象，焦点变化时只移动它（不重画卡片）
  function app.ring(parent)
    local r = ui.plain(parent, -100, -100, 10, 10, nil, { border = 4, border_color = app.S.focus, radius = app.S.radius + 4 })
    local self = { obj = r }
    function self.to(x, y, w, h, radius)
      r:set_pos(x - 6, y - 6); r:set_size(w + 12, h + 12)
      if radius then r:set_style({ radius = radius + 6 }) end
    end
    function self.hide() r:set_pos(-200, -200) end
    return self
  end

  ---------------------------------------------------------------- 屏幕与外框
  local scr = lvgl.create_screen()
  scr:set_style({ bg_color = "#101010", pad = 0, border_width = 0 })
  scr:set_scroll({ dir = "none", scrollbar = "off" })
  scr:load()
  app.scr = scr
  app.W, app.H = scr:get_size()
  app.frame = require("core.frame").new(app)
  local F = app.frame

  ---------------------------------------------------------------- 花园模型（植物装载、沙箱调用、热重载）
  app.garden = require("core.garden").new(app, fresh)

  ---------------------------------------------------------------- 小家的房间（主题外壳的房间应用，状态存在 garden.json 的 rooms 里）
  app.rooms = {}
  for _, name in ipairs(require("rooms.registry")) do
    local ok, a = pcall(require, "rooms." .. name)
    if ok and type(a) == "table" and a.id then
      data.rooms[a.id] = type(data.rooms[a.id]) == "table" and data.rooms[a.id] or {}
      local oki, err = pcall(a.init, data.rooms[a.id])
      if not oki then env.log("GD_ERR room.init " .. a.id .. " " .. tostring(err)) end
      app.rooms[#app.rooms + 1] = a
    else
      env.log("GD_ERR room " .. name .. " " .. tostring(a))
    end
  end
  function app.room_state(a) return data.rooms[a.id] end
  function app.room_ctx()
    return { ui = ui, C = app.C, digits = app.T.digits or "block", radius = app.T.radius or 0, date = app.date, now = app.now,
      W = app.W, H = app.H, log = env.log, pet = app.pet }
  end

  ---------------------------------------------------------------- 字体：只有可左右滑的分页（tileview）需要；后台协程准备
  local fonts = require("core.fonts")
  app.font_path = fonts.existing(env, app.store)
  local function font_ready(path)
    app.font = fonts.load(env, path, 20)
    env.log("GD_FONT ready " .. tostring(path) .. " handle=" .. tostring(app.font ~= nil))
  end
  if app.font_path then font_ready(app.font_path)
  elseif not env.no_font_copy then
    app.sched.spawn("font", fonts.copy_task(env, app.store, function(p, err)
      if p then app.font_path = p; font_ready(p) else env.log("GD_FONT unavailable: " .. tostring(err) .. "（分页改用按键/按钮翻页）") end
    end))
  end

  ---------------------------------------------------------------- 页面栈
  app.pages = {}
  for _, id in ipairs(PAGES) do app.pages[id] = require("pages." .. id) end
  app.stack = {}
  local cur, page_objs, frame_objs = nil, 0, 0
  local last_state = ""

  function app.log_state(reason)
    local ids = {}
    for i, e in ipairs(app.stack) do ids[i] = e.id end
    local s = string.format('{"page":"%s","stack":"%s","theme":"%s","focus":%s,"mode":"%s","sheet":%s,"reason":"%s"}',
      ids[#ids] or "?", table.concat(ids, ">"), app.theme.id, tostring(cur and cur.focus_id and cur.focus_id() or "null"),
      cur and cur.mode and cur.mode() or "", tostring(F.sheet_ ~= nil), reason or "")
    if s ~= last_state then last_state = s; env.log("GD_STATE " .. s) end
  end

  local last_labels = ""
  function app.refresh_keys()
    local l
    if F.sheet_ then l = F.sheet_labels()
    else
      l = (cur and cur.labels and app.safe("labels", cur.labels)) or {}
      if l.back == nil then l.back = #app.stack <= 1 and "首页" or "返回" end
    end
    local sig = table.concat({ l.prev or "", l.ok or "", l.next or "", l.back or "" }, "|")
    if sig ~= last_labels then last_labels = sig; F.keys(l) end
  end

  -- 页面回调出错：记 GD_ERR，不让整个应用崩
  function app.safe(tag, fn, ...)
    if type(fn) ~= "function" then return nil end
    local r = table.pack(pcall(fn, ...))
    if not r[1] then env.log("GD_ERR " .. tag .. " " .. tostring(r[2])); return nil end
    return table.unpack(r, 2, r.n)
  end

  local function destroy_cur()
    if cur then app.safe("page.destroy", cur.destroy) end
    cur = nil
    F.close_sheet()
    F.content:clean()
  end

  local function build_top(reason)
    destroy_cur()
    local e = app.stack[#app.stack]
    local t0, o0 = app.now(), ui.count
    local ok, page = pcall(app.pages[e.id].new, app, F.content, e.params or {}, e.state)
    if not ok or type(page) ~= "table" then
      env.log("GD_ERR page.build " .. e.id .. " " .. tostring(page))
      F.content:clean()
      if #app.stack > 1 then table.remove(app.stack); return build_top("fallback") end
      error(page)
    end
    cur = page
    page_objs = ui.count - o0
    env.log(string.format("GD_PAGE %s objs=%d build_ms=%d", e.id, page_objs, app.now() - t0))
    app.refresh_keys()
    app.log_state(reason)
  end

  function app.open(id, params)
    local top = app.stack[#app.stack]
    if top and cur and cur.save then app.safe("page.save", cur.save) end
    app.stack[#app.stack + 1] = { id = id, params = params or {}, state = {} }
    build_top("open")
  end
  function app.back()
    if #app.stack <= 1 then app.quit("back"); return end
    table.remove(app.stack)
    build_top("back")
  end
  function app.rebuild(reason) build_top(reason or "rebuild") end
  function app.page() return cur end
  function app.quit(why) env.log("GD_QUIT " .. tostring(why)); app.running = false end

  -- 换主题：保存设置，用同一份状态重建外框底色与当前页（植物、房间数据都不丢）
  function app.set_theme(id)
    data.settings.theme = id
    app.apply_theme()
    F.retheme()
    app.save_now()
    app.rebuild("theme")
    app.snapshot(true)
    F.toast("已换上：" .. app.theme.name .. "（只作用于我的和小家）")
  end

  ---------------------------------------------------------------- 快照（给原生首页「我的」小组件，契约 1.2）
  function app.say(now, stable)
    return app.pet.say(now or app.now(), { thirsty = app.garden.thirsty_count(), theme_say = app.T.say }, stable)
  end
  function app.snapshot(force)
    local snap = app.pet.snapshot(app.theme, app.garden.count(), app.say(nil, true))
    local updated = snap.updated
    snap.updated = 0
    local json = require("core.json")
    local sig = json.encode(snap)
    if not force and sig == last_snap then return end
    snap.updated = updated
    local ok, err = app.store.write_atomic(app.store.paths.snapshot, json.encode(snap))
    if ok then last_snap = sig; env.log("GD_SNAPSHOT " .. json.encode(snap)) else env.log("GD_WARN snapshot " .. tostring(err)) end
  end

  ---------------------------------------------------------------- 输入路由
  local function handle(ev)
    if ev.fn then ev.fn(); app.refresh_keys(); return end
    if ev.mark then env.log("GD_MARK " .. tostring(ev.mark)); return end
    if ev.cmd then
      local c = tostring(ev.cmd)
      local th = c:match("^theme:([%w_]+)$")
      if th then app.set_theme(th)
      elseif c == "quit" then app.quit("cmd")
      elseif c == "rescan" then app.garden.scan(true); app.themes.scan(); if cur and cur.rescan then cur.rescan() end
      elseif c == "dump" then env.log("GD_DUMP " .. require("core.json").encode(data)) end
      return
    end
    local k, kind = ev.key, ev.kind
    if F.sheet_ then F.sheet_key(k, kind); app.refresh_keys(); app.log_state("sheet-key"); return end
    if cur and cur.key and app.safe("page.key", cur.key, k, kind) then app.refresh_keys(); app.log_state("key"); return end
    if k == "back" and (kind == "short" or kind == "long") then app.back(); return end
  end

  ---------------------------------------------------------------- 启动
  frame_objs = ui.count
  F.status(app.now())
  local first = tostring(env.args.page or "mine")
  app.stack = { { id = "mine", params = {}, state = {} } }
  if first == "house" or first == "garden" then
    app.stack[2] = { id = first, params = {}, state = {} } -- 直达小家 / 花园：SET 仍先回到「我的」
  end
  build_top("start")
  env.log(string.format("GD_START page=%s theme=%s data=%s first_screen_ms=%d", first, app.theme.id, status, app.now() - (env.t_boot or app.now())))
  -- 首屏出来之后再做的事：扫描自定义主题、首次运行写一次数据与快照
  app.sched.spawn("boot", function(task)
    task.sleep(300)
    app.themes.scan()
    if fresh then app.save_now() end
    app.snapshot(true)
    if cur and cur.rescan then app.safe("page.rescan", cur.rescan) end
  end)

  ---------------------------------------------------------------- 主循环（性能预算）
  local t_last, t_upd, t_pet, t_tick, t_perf, t_snap, t_scan = app.now(), 0, 0, 0, app.now(), app.now(), app.now()
  local loops, worst, total = 0, 0, 0
  local deadline = env.max_ms and (app.now() + env.max_ms) or nil
  while app.running do
    lvgl.process_events(env.frame_ms or 30)
    local now = app.now()
    for _, ev in ipairs(app.input.poll(now)) do
      local ok, err = pcall(handle, ev)
      if not ok then
        env.log("GD_ERR handle " .. tostring(ev.key or ev.cmd or "fn") .. " " .. tostring(err))
        F.close_sheet()
        pcall(app.rebuild, "recover")
      end
      if not app.running then break end
    end
    if not app.running then break end
    app.sched.step(now, 8)
    F.tick(now)
    if cur and cur.anim then app.safe("page.anim", cur.anim, now) end
    if cur and cur.update and now - t_upd >= (cur.rate or 1000) then t_upd = now; app.safe("page.update", cur.update, now); app.refresh_keys() end
    if now - t_tick >= 500 then -- 房间（番茄钟、WorkBuddy 重写）与植物的数据推进：不在小家 / 花园里也照常走
      local dt = now - t_tick
      t_tick = now
      local rctx = app.room_ctx()
      for _, a in ipairs(app.rooms) do if a.tick then app.safe("room.tick." .. a.id, a.tick, data.rooms[a.id], dt, rctx) end end
      app.garden.tick(now)
    end
    if now - t_pet >= 10000 then t_pet = now; app.pet.tick() end
    if now - t_scan >= 3000 and cur and cur.watch then -- 热重载：花园相关页面每 3 s 看一眼 DATA/garden/plants
      t_scan = now
      if app.garden.scan(false) and cur.rescan then app.safe("page.rescan", cur.rescan) end
    end
    if next(dirty) and now - last_save >= 1500 then app.save_now() end
    if now - t_snap >= 2000 then t_snap = now; app.snapshot(false) end
    -- 性能记录：每 10 s 一行（仿真里没有 system.info 时 heap=?）
    local dt = now - t_last
    t_last = now; loops = loops + 1; total = total + dt; if dt > worst then worst = dt end
    if now - t_perf >= 10000 then
      local okh, info = pcall(env.info)
      local heap = (okh and type(info) == "table" and info.sram_free) and string.format("sram=%dk psram=%sk", info.sram_free // 1024,
        info.psram_free and tostring(info.psram_free // 1024) or "-") or "?"
      env.log(string.format("GD_PERF page=%s loops=%d avg_ms=%.1f worst_ms=%d objs=%d lua_kb=%d heap=%s", app.stack[#app.stack].id, loops,
        total / math.max(1, loops), worst, frame_objs + page_objs, math.floor(collectgarbage("count")), heap))
      loops, worst, total, t_perf = 0, 0, 0, now
    end
    if deadline and now > deadline then env.log("GD_TIMEOUT"); break end
  end

  ---------------------------------------------------------------- 退出：落盘、快照、释放按键
  destroy_cur()
  app.save_now()
  app.snapshot(true)
  app.input.close()
  env.log("GD_EXIT")
end

return M
