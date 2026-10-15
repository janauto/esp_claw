-- 内置种子：心情日记。每天记一下心情（5 档），卡片上看最近 7 天的颜色；记一次 = ctx.pet.cheer("mood")。
local P = {
  name = "心情日记",
  desc = "每天记一下心情，看看这一周",
  icon = { ".yyyyyy.", "yyyyyyyy", "yykyykyy", "yyyyyyyy", "ykyyyyky", "yykkkkyy", ".yyyyyy.", "........" },
  pal = { y = "#F6C445", k = "#2B2118" },
}
local MOODS = { { "开心", "#F6C445" }, { "不错", "#9CCB5A" }, { "平静", "#7FB7F0" }, { "有点闷", "#B9A7D9" }, { "难过", "#9A9A9A" } }

local function day_str(t) return os.date("%Y-%m-%d", t) end
local function last_days(ctx, n)
  local out, now = {}, ctx.time()
  for i = n - 1, 0, -1 do out[#out + 1] = day_str(now - i * 86400) end
  return out
end

function P.init(s) s.log = {}; s.sel = 1 end
function P.summary(s, ctx)
  local m = ctx.clock_ok() and s.log[ctx.date("%Y-%m-%d")]
  return m and MOODS[m][1] or "还没记", "今天的心情"
end

function P.card(ui, s, ctx)
  local dot = ui.box(0, 8, 22, 22, "line", { radius = 11 })
  local t = ui.text(32, 2, "", "text")
  local cells = {}
  local cw = (ui.W - 6 * 6) // 7
  for i = 1, 7 do cells[i] = ui.box((i - 1) * (cw + 6), ui.H - 40, cw, 32, "surfaceAlt", { radius = 6 }) end
  ui.text(0, 40, "最近 7 天", "textMuted")
  local function paint()
    local m = ctx.clock_ok() and s.log[ctx.date("%Y-%m-%d")]
    t:text(m and ("今天" .. MOODS[m][1]) or "今天还没记")
    dot:fill(m and MOODS[m][2] or "line")
    if ctx.clock_ok() then
      for i, d in ipairs(last_days(ctx, 7)) do local v = s.log[d]; cells[i]:fill(v and MOODS[v][2] or "surfaceAlt") end
    end
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local self = {}
  local rows = {}
  for i, m in ipairs(MOODS) do
    local r = ui.box(0, (i - 1) * 62, ui.W, 52, "surfaceAlt", { radius = 10 })
    local dot = ui.box(16, (i - 1) * 62 + 15, 22, 22, m[2], { radius = 11 })
    local t = ui.text(52, (i - 1) * 62 + 11, m[1], "text")
    rows[i] = { r = r, t = t }
    r:on_tap(function() s.sel = i; self.update() end)
  end
  ui.text(0, 5 * 62 + 10, "最近 14 天", "textMuted")
  local cells = {}
  local cw = (ui.W - 13 * 6) // 14
  for i = 1, 14 do cells[i] = ui.box((i - 1) * (cw + 6), 5 * 62 + 44, cw, 40, "surfaceAlt", { radius = 6 }) end
  local note = ui.text(0, 5 * 62 + 100, "", "textMuted", { w = ui.W })
  function self.update()
    for i, row in ipairs(rows) do
      row.r:fill(i == s.sel and "accent" or "surfaceAlt")
      row.t:color(i == s.sel and "onAccent" or "text")
    end
    if not ctx.clock_ok() then note:text("设备还没对时，先不记日期"); return end
    for i, d in ipairs(last_days(ctx, 14)) do local v = s.log[d]; cells[i]:fill(v and MOODS[v][2] or "surfaceAlt") end
    local m = s.log[ctx.date("%Y-%m-%d")]
    note:text(m and ("今天记的是：" .. MOODS[m][1]) or "今天还没记，选一个按 M")
  end
  function self.key(k)
    if k == "prev" then s.sel = (s.sel - 2) % #MOODS + 1; self.update(); return true end
    if k == "next" then s.sel = s.sel % #MOODS + 1; self.update(); return true end
    if k == "ok" then
      if not ctx.clock_ok() then ctx.toast("设备还没对时"); return true end
      s.log[ctx.date("%Y-%m-%d")] = s.sel
      local keep = {} -- 只留最近 30 天
      for _, d in ipairs(last_days(ctx, 30)) do keep[d] = s.log[d] end
      s.log = keep
      ctx.pet.cheer("mood", #MOODS - s.sel + 1)
      self.update(); return true
    end
    return false
  end
  function self.labels() return { prev = "上一个", ok = "记下", next = "下一个" } end
  self.update()
  return self
end

return P
