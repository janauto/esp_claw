-- 我的 · 花园：花园模型。负责种子（植物类型）的装载与热重载、地块（种下的植物实例）的增删换位与浇水，
-- 以及“受控调用”植物代码：每次调用都在沙箱预算内执行，出错只让这一株枯萎，并显示错误摘要。
--   内置种子：<SYSTEM>/skills/my_garden/scripts/plants/*.lua（与用户植物走同一个沙箱，证明接口够用）
--   用户 / AI 植物：<DATA>/garden/plants/*.lua（每 3 秒看一眼文件有没有变化，变了就热重载，地块状态保留）
-- 地块数据在 garden.json 的 plots 数组里，数组顺序就是花园里的位置（2 列 × 3 行一页）。
local sandbox = require("core.sandbox")
local plantui = require("core.plantui")
local Themes = require("core.themes")

local M = {}
M.MAX = 11          -- 最多 11 株：加上“种植物”空位正好两页（2×3）；按每张卡片 32 个对象的上限，整页仍在 600 个对象预算内
M.CARD_BUDGET = 32  -- 每张地块卡片里植物自己能建的对象数（11 张 × (32 + 核心约 12) + 外框 ≈ 560 < 600）
M.PAGE_BUDGET = 240 -- 植物全屏页能建的对象数（全屏页时花园页已销毁）
M.DEFAULT = { "sunflower", "water", "focus", "quote" } -- 第一次打开花园时已经种好的几株
local CHEER_OK = { water = true, unwater = true, focus = true, play = true, mood = true }

-- 文件签名：设备上用 大小 + 修改时间；仿真的 stat 没有修改时间，就再加首尾片段（够发现推送了新文件）
local function signature(f, src)
  local s = tostring(f.size or (src and #src) or 0)
  if f.mtime then return s .. "@" .. f.mtime end
  if src then return s .. "#" .. src:sub(1, 64) .. src:sub(-64) end
  return s
end

function M.new(app, fresh)
  local data, store, env = app.data, app.store, app.env
  local g = { seeds = {}, order = {}, withered = {}, files = {} }
  local sys_dir = env.script_dir .. "/plants"

  ---------------------------------------------------------------- 种子
  local function key(src, id) return src .. "/" .. id end
  g.key = key

  local function load_seed(src_kind, id, path, src, sig)
    local name = (src_kind == "sys" and "plants/" or "garden/plants/") .. id .. ".lua"
    local mod, err = sandbox.load(src or "", name, sandbox.env(), 600000)
    if mod and (type(mod.name) ~= "string" or mod.name == "") then mod, err = nil, "缺少 name（植物显示名）" end
    if mod and type(mod.card) ~= "function" and type(mod.open) ~= "function" and type(mod.summary) ~= "function" then
      mod, err = nil, "至少要有 card / open / summary 其中一个函数"
    end
    local k = key(src_kind, id)
    local old = g.seeds[k]
    local seed = { key = k, id = id, src = src_kind, path = path, sig = sig, mod = mod, err = err,
      name = mod and mod.name or (old and old.name) or id, desc = mod and tostring(mod.desc or "") or "", icon = mod and mod.icon, pal = mod and mod.pal }
    g.seeds[k] = seed
    if not old then g.order[#g.order + 1] = k end
    if err then env.log("GD_PLANT_FAIL " .. k .. " " .. err) else env.log("GD_PLANT_LOAD " .. k .. " " .. seed.name) end
    -- 换了新代码：这个种子的枯萎地块都给一次复活的机会
    for _, p in ipairs(data.plots) do if key(p.src, p.seed) == k then g.withered[p.uid] = nil end end
    return seed, old ~= nil
  end

  -- 扫描种子目录。force=true 时连内置种子一起重读；返回是否有变化（页面据此重建）
  function g.scan(force)
    local changed = false
    local function walk(src_kind, dir)
      local seen = {}
      for _, f in ipairs(store.list(dir, ".lua")) do
        local id = f.name:sub(1, -5)
        if id:match("^[a-z][a-z0-9_]*$") then
          seen[id] = true
          local k = key(src_kind, id)
          -- 设备上有修改时间：签名不用读文件；仿真没有：读一下文件再算签名（植物文件很小）
          local src = (force or not f.mtime) and store.read(f.path) or nil
          local sig = signature(f, src)
          if force or g.files[k] ~= sig then
            src = src or store.read(f.path)
            g.files[k] = sig
            local _, existed = load_seed(src_kind, id, f.path, src, sig)
            if existed then env.log("GD_RELOAD " .. k) end
            changed = true
          end
        end
      end
      -- 文件被删掉：种子标成不可用，已种下的地块显示枯萎（数据保留，文件放回来就复活）
      for k2, s in pairs(g.seeds) do
        if s.src == src_kind and not seen[s.id] and not s.err then
          s.err, s.mod = "植物文件已删除", nil
          g.files[k2] = nil
          changed = true
        end
      end
    end
    if force or not g.sys_loaded then walk("sys", sys_dir); g.sys_loaded = true end
    walk("user", store.paths.plants)
    return changed
  end

  function g.seed_of(plot) return g.seeds[key(plot.src, plot.seed)] end
  function g.seed_list()
    local out = {}
    for _, k in ipairs(g.order) do out[#out + 1] = g.seeds[k] end
    return out
  end

  ---------------------------------------------------------------- 地块
  local function valid_plot(p)
    return type(p) == "table" and math.type(tonumber(p.uid)) == "integer" and type(p.seed) == "string" and (p.src == "sys" or p.src == "user")
  end
  local plots = {}
  for _, p in ipairs(type(data.plots) == "table" and data.plots or {}) do
    if valid_plot(p) then p.state = type(p.state) == "table" and p.state or {}; p.watered = p.watered or ""; plots[#plots + 1] = p end
  end
  data.plots = plots
  for _, p in ipairs(plots) do if p.uid >= data.next_uid then data.next_uid = p.uid + 1 end end

  function g.plots() return data.plots end
  function g.count() return #data.plots end
  function g.index_of(uid) for i, p in ipairs(data.plots) do if p.uid == uid then return i end end end
  local function today() return app.clock_ok() and app.date("%Y-%m-%d") or nil end
  function g.thirsty(plot)
    local d = today()
    local seed = g.seed_of(plot)
    if not d or (seed and seed.mod and seed.mod.care == false) then return false end
    return plot.watered ~= d
  end
  function g.thirsty_count()
    local n = 0
    for _, p in ipairs(data.plots) do if g.thirsty(p) then n = n + 1 end end
    return n
  end

  ---------------------------------------------------------------- 植物的 ctx（只读信息 + 少量受控能力）
  function g.ctx(plot)
    return {
      uid = plot.uid,
      now = function() return app.now() end,
      time = function() return app.clock_ok() and app.time() or 0 end,
      date = function(f) return app.date(f or "%Y-%m-%d") end,
      clock_ok = function() return app.clock_ok() end,
      toast = function(m) app.frame.toast(tostring(m)) end,
      log = function(m) env.log("GD_PLANT " .. plot.seed .. " " .. tostring(m)) end,
      save = function() app.mark_dirty("plants") end,
      theme = { id = app.theme.id, name = app.theme.name, digits = app.T.digits or "block" },
      C = Themes.readonly(app.C),
      pet = {
        name = app.pet.name, mood = app.pet.mood, energy = app.pet.energy, days = app.pet.days, today = app.pet.today,
        mood_text = app.pet.mood_text,
        cheer = function(kind, amount)
          if not CHEER_OK[kind] then error("cheer 只接受 water / unwater / focus / play / mood", 2) end
          return app.pet.cheer(kind, amount)
        end,
      },
    }
  end

  -- 枯萎：记录错误摘要（只在内存里，不写进 garden.json；修好文件或重载后复活）
  function g.wither(plot, err)
    if g.withered[plot.uid] then return end
    g.withered[plot.uid] = tostring(err)
    env.log("GD_WITHER " .. plot.seed .. "#" .. plot.uid .. " " .. tostring(err))
    local page = app.page and app.page()
    if page and page.plant_failed then app.safe("page.plant_failed", page.plant_failed, plot) end
  end
  function g.status(plot)
    local seed = g.seed_of(plot)
    if not seed then return "dead", "种子不见了（" .. plot.src .. "/" .. plot.seed .. "）" end
    if seed.err then return "dead", seed.err end
    if g.withered[plot.uid] then return "dead", g.withered[plot.uid] end
    return "ok"
  end

  -- 受控调用植物函数：缺省的可选函数视为成功；出错让这一株枯萎
  function g.call(plot, fname, budget, ...)
    local st, err = g.status(plot)
    if st ~= "ok" then return false, err end
    local f = g.seed_of(plot).mod[fname]
    if f == nil then return true end
    local r = table.pack(sandbox.guard(f, budget, ...))
    if not r[1] then g.wither(plot, r[2]) end
    return table.unpack(r, 1, r.n)
  end

  -- 植物的点按回调：同样在预算内执行
  local function tapper(plot)
    return function(obj, fn)
      app.input.bind_tap(obj, function()
        if g.status(plot) ~= "ok" then return end
        local ok, err = sandbox.guard(fn, 100000)
        if not ok then g.wither(plot, err) else app.mark_dirty("plants") end
      end)
    end
  end

  -- 画地块卡片 / 植物页：返回植物给的视图（{update, key, labels}）或 nil, 错误
  function g.mount(plot, parent, w, h, which)
    local pui = plantui.new(app, parent, w, h, which == "open" and M.PAGE_BUDGET or M.CARD_BUDGET, tapper(plot))
    local seed = g.seed_of(plot)
    local fname = which
    if seed and seed.mod and type(seed.mod[which]) ~= "function" then fname = nil end
    if not fname then -- 没写 card/open：用 summary 画一个默认的样子
      local ok, v, l = g.call(plot, "summary", 50000, plot.state, g.ctx(plot))
      if not ok then return nil, v end
      pui.text(0, which == "open" and 20 or 4, tostring(v or seed.name), "text")
      if l then pui.text(0, which == "open" and 56 or 36, pui.fit(l, w), "textMuted") end
      return {}
    end
    local ok, view = g.call(plot, fname, 300000, pui, plot.state, g.ctx(plot))
    if not ok then return nil, view end
    return type(view) == "table" and view or {}
  end

  -- 视图方法（update / key / labels）也走受控调用
  function g.view_call(plot, view, fname, budget, ...)
    if not view or type(view[fname]) ~= "function" or g.status(plot) ~= "ok" then return false end
    local r = table.pack(sandbox.guard(view[fname], budget or 60000, ...))
    if not r[1] then g.wither(plot, r[2]); return false end
    return table.unpack(r, 1, r.n)
  end

  ---------------------------------------------------------------- 种、拔、换位、浇水
  function g.add(seed_key)
    if #data.plots >= M.MAX then return nil, "花园满了（最多 " .. M.MAX .. " 株），先移除一株" end
    local seed = g.seeds[seed_key]
    if not seed then return nil, "没有这颗种子" end
    if seed.err then return nil, "这颗种子载入失败：" .. seed.err end
    local plot = { uid = data.next_uid, seed = seed.id, src = seed.src, state = {}, watered = "", planted = app.clock_ok() and app.time() or 0 }
    data.next_uid = data.next_uid + 1
    data.plots[#data.plots + 1] = plot
    g.call(plot, "init", 200000, plot.state, g.ctx(plot)) -- 初始化出错也种下去：在花园里显示枯萎和原因，而不是悄悄失败
    app.save_now()
    env.log("GD_ADD " .. seed.key .. "#" .. plot.uid .. " at=" .. #data.plots)
    return #data.plots
  end
  function g.remove(i)
    local p = table.remove(data.plots, i)
    if p then g.withered[p.uid] = nil; app.save_now(); env.log("GD_REMOVE " .. p.seed .. "#" .. p.uid) end
    return p
  end
  function g.move(i, d)
    local j = i + d
    if j < 1 or j > #data.plots then return i end
    data.plots[i], data.plots[j] = data.plots[j], data.plots[i]
    app.mark_dirty("plants")
    return j
  end
  -- 直接挪到第 j 个位置（触摸：拿起后点目标地块）
  function g.move_to(i, j)
    j = math.max(1, math.min(#data.plots, j))
    if i == j or not data.plots[i] then return i end
    local p = table.remove(data.plots, i)
    table.insert(data.plots, j, p)
    app.mark_dirty("plants")
    return j
  end
  function g.water(plot)
    if not g.thirsty(plot) then return false end
    plot.watered = today()
    app.pet.cheer("care")
    g.call(plot, "on_water", 100000, plot.state, g.ctx(plot))
    app.mark_dirty("plants")
    env.log("GD_WATER " .. plot.seed .. "#" .. plot.uid)
    return true
  end
  -- 手动重新加载：强制重读这颗种子的文件，复活它的枯萎地块
  function g.reload(plot)
    local seed = g.seed_of(plot)
    if seed and seed.src == "user" then g.files[seed.key] = nil; g.scan(false)
    else g.scan(true) end
    g.withered[plot.uid] = nil
  end

  -- 约 1 Hz 的数据推进（不画图）
  local last_tick = app.now()
  function g.tick(now)
    local dt = now - last_tick
    last_tick = now
    for _, p in ipairs(data.plots) do
      local seed = g.seed_of(p)
      if seed and seed.mod and seed.mod.tick and g.status(p) == "ok" then g.call(p, "tick", 50000, p.state, dt, g.ctx(p)) end
    end
  end

  ---------------------------------------------------------------- 启动：装载种子，新花园先种上几株
  g.scan(true)
  if fresh and #data.plots == 0 then
    for _, id in ipairs(M.DEFAULT) do if g.seeds[key("sys", id)] then g.add(key("sys", id)) end end
  end
  return g
end

return M
