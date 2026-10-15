-- 我的 · 花园：小磁的状态（心情、能量、陪伴天数、今天的互动）与说的话。
-- 数据就是 garden.json 里的 pet 表（原地修改，由核心统一落盘）：
--   name, mood(0-100), energy(0-100), first(首次运行日期 YYYY-MM-DD), days, t(上次结算的时间戳),
--   daily = {date, cups, focus, cares, plays, pokes, diary}
-- 时间：设备没联网对时的时候系统时间可能停在 1970 年，这时不结算衰减、不换日，陪伴天数沿用上次的值。
local M = {}

M.MOOD_TEXT = { { 80, "开心" }, { 60, "不错" }, { 40, "平静" }, { 20, "有点闷" }, { 0, "难过" } }
local POKE_LINES = { "嘿嘿，好痒", "在呢在呢", "今天也要加油", "摸摸头就开心", "找我玩吗" }
-- 每天最多计入多少次（防止某个植物刷心情）
local CAP = { water = 8, care = 12, play = 6, poke = 10, focus = 8, mood = 3 }

local function clamp(v, a, b) return math.max(a, math.min(b, v)) end

function M.day_num(s)
  local y, m, d = tostring(s or ""):match("^(%d+)-(%d+)-(%d+)")
  if not y then return nil end
  return os.time({ year = tonumber(y), month = tonumber(m), day = tonumber(d), hour = 12 }) // 86400
end

function M.new(app, data)
  local env = app.env
  local pet = { data = data, event = nil }
  data.name = type(data.name) == "string" and data.name ~= "" and data.name or "小磁"
  data.mood = tonumber(data.mood) or 72
  data.energy = tonumber(data.energy) or 80
  data.days = tonumber(data.days) or 1
  data.daily = type(data.daily) == "table" and data.daily or {}

  local function today() return app.clock_ok() and app.date("%Y-%m-%d") or nil end
  local function hour() return tonumber(app.date("%H")) or 12 end

  -- 换日：今天的计数清零（没对时就不换）
  local function roll()
    local d = today()
    if d and data.daily.date ~= d then
      data.daily = { date = d, cups = 0, focus = 0, cares = 0, plays = 0, pokes = 0 }
      app.mark_dirty("pet")
    end
    if d then
      data.first = data.first or d
      local a, b = M.day_num(data.first), M.day_num(d)
      if a and b then data.days = math.max(1, b - a + 1) end
    end
  end
  roll()

  -- 随时间结算：心情慢慢回到 55；白天能量缓慢下降，夜里（23 点到 7 点）恢复
  function pet.tick()
    roll()
    if not app.clock_ok() then return end
    local now = app.time()
    local last = tonumber(data.t) or now
    local dt_h = clamp((now - last) / 3600, 0, 72)
    data.t = now
    if dt_h <= 0 then return end
    data.mood = clamp(data.mood + (55 - data.mood) * math.min(1, 0.05 * dt_h), 0, 100)
    local h = hour()
    if h >= 23 or h < 7 then data.energy = clamp(data.energy + 10 * dt_h, 5, 100)
    else data.energy = clamp(data.energy - 3 * dt_h, 5, 100) end
  end

  function pet.name() return data.name end
  function pet.days() return data.days end
  function pet.mood() return math.floor(data.mood + 0.5) end
  function pet.energy() return math.floor(data.energy + 0.5) end
  function pet.level() return clamp(math.floor(data.mood / 10 + 0.5), 0, 10) end
  function pet.mood_text()
    for _, m in ipairs(M.MOOD_TEXT) do if data.mood >= m[1] then return m[2] end end
    return "难过"
  end
  -- 只读的今日计数（给植物看）
  function pet.today()
    local d = data.daily
    return { date = d.date, cups = d.cups or 0, focus = d.focus or 0, cares = d.cares or 0, plays = d.plays or 0 }
  end

  local function react(text) pet.event = { text = text, t = app.now() } end

  -- 互动：kind = water / unwater / focus / care / play / poke / mood；返回是否计入
  function pet.cheer(kind, amount)
    roll()
    local d = data.daily
    if kind == "unwater" then
      if (d.cups or 0) > 0 then d.cups = d.cups - 1; data.mood = clamp(data.mood - 3, 0, 100); app.mark_dirty("pet") end
      return true
    end
    local field = ({ water = "cups", focus = "focus", care = "cares", play = "plays", poke = "pokes", mood = "diary" })[kind]
    if not field then return false end
    local n = (d[field] or 0)
    local counted = n < (CAP[kind] or 10)
    d[field] = n + 1
    if kind == "water" then
      if counted then data.mood = data.mood + 5; data.energy = data.energy + 3 end
      react("喝好啦，今天第 " .. d.cups .. " 杯水")
    elseif kind == "focus" then
      if counted then data.mood = data.mood + 8; data.energy = data.energy - 6 end
      react("专注完成，休息一下吧")
    elseif kind == "care" then
      if counted then data.mood = data.mood + 3; data.energy = data.energy + 1 end
      react("浇好啦，花园更精神了")
    elseif kind == "play" then
      if counted then data.mood = data.mood + 4; data.energy = data.energy - 3 end
      react("好玩，再来一局")
    elseif kind == "mood" then
      if counted then data.mood = data.mood + 3 end
      react((tonumber(amount) or 3) >= 3 and "记下啦，今天也不错" or "抱抱，有我陪着你")
    elseif kind == "poke" then
      if counted then data.mood = data.mood + 1 end
      react(POKE_LINES[(n % #POKE_LINES) + 1])
    end
    data.mood = clamp(data.mood, 0, 100)
    data.energy = clamp(data.energy, 5, 100)
    app.mark_dirty("pet")
    return counted
  end

  -- 当前表情（形象用）：刚互动过 = 开心；夜里或没电 = 困；心情低 = 难过
  function pet.face(now)
    if pet.event and now - pet.event.t < 2500 then return "happy" end
    local h = hour()
    if (h >= 23 or h < 6) or data.energy < 20 then return "sleepy" end
    if data.mood < 30 then return "sad" end
    return "idle"
  end

  -- 气泡里的话：刚发生的事优先，其次按时段 / 状态轮换（每 8 秒换一句）
  -- stable = true：不轮换，固定取第一句（写进快照的那句，避免快照每 8 秒重写一次）
  function pet.say(now, info, stable)
    info = info or {}
    if pet.event and app.now() - pet.event.t < 8000 then return pet.event.text end
    local d, h = data.daily, hour()
    local cups = d.cups or 0
    local list = {}
    if h >= 23 or h < 6 then list[#list + 1] = "夜深了，早点休息"
    elseif h < 10 then list[#list + 1] = cups == 0 and "早上好，先喝杯水吧" or "早上好，今天也元气满满"
    elseif h >= 11 and h < 13 then list[#list + 1] = "中午了，记得好好吃饭"
    elseif h >= 18 then list[#list + 1] = "晚上好，今天辛苦了" end
    if (info.thirsty or 0) > 0 and not (h >= 23 or h < 6) then list[#list + 1] = "花园里有 " .. info.thirsty .. " 株植物口渴了" end
    if h >= 14 and h < 21 and cups < 4 then list[#list + 1] = "今天才喝了 " .. cups .. " 杯水" end
    if data.mood < 40 then list[#list + 1] = "有点闷，陪我玩一会吧" end
    if data.energy < 25 then list[#list + 1] = "有点累了，想休息一下" end
    list[#list + 1] = info.theme_say or "欢迎回家，今天想玩什么"
    if stable then return list[1] end
    return list[(now // 8000) % #list + 1]
  end

  -- 给原生首页「我的」小组件的快照（契约 1.2）
  function pet.snapshot(theme, plants, say)
    return { v = 1, pet_name = data.name, mood = pet.mood_text(), mood_level = pet.level(), energy = pet.energy(), days = data.days,
      theme = theme.id, theme_name = theme.name, say = say, plants = plants, updated = app.clock_ok() and app.time() or 0 }
  end

  return pet
end

return M
