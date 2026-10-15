-- 内置种子：倒数日。离周末、月底、新年还有几天；也可以自己定一个日子（◀▶ 一天一天调）。
-- 日期计算只用沙箱里允许的 os.time / os.date；设备没对时（ctx.clock_ok() 为假）就不瞎算。
local P = {
  name = "倒数日",
  desc = "离周末、月底、新年还有几天",
  icon = { "rrrrrrrr", "rwwwwwwr", "rwkwwkwr", "rwwwwwwr", "rwkkkkwr", "rwwwwwwr", "rrrrrrrr", "........" },
  pal = { r = "#E5533B", w = "#FFF7E3", k = "#2B2118" },
}
local EVENTS = { "周末", "月底", "新年", "自定日子" }

local function day0(t) local d = os.date("*t", t); return os.time({ year = d.year, month = d.month, day = d.day, hour = 12 }) end
local function days_left(s, now)
  local today = day0(now)
  local d = os.date("*t", today)
  local target
  if s.ev == 1 then
    local wd = d.wday -- 1 = 周日，7 = 周六
    if wd == 7 or wd == 1 then return 0 end
    target = today + (7 - wd) * 86400
  elseif s.ev == 2 then
    target = os.time({ year = d.month == 12 and d.year + 1 or d.year, month = d.month % 12 + 1, day = 1, hour = 12 }) - 86400
  elseif s.ev == 3 then
    target = os.time({ year = d.year + 1, month = 1, day = 1, hour = 12 })
  else
    target = today + (s.custom or 30) * 86400
  end
  return math.max(0, math.floor((target - today) / 86400 + 0.5)), target
end

function P.init(s) s.ev = 1; s.custom = 30 end
function P.summary(s, ctx)
  if not ctx.clock_ok() then return "--", "还没对时" end
  local n = days_left(s, ctx.time())
  return n == 0 and "今天" or tostring(n), "距" .. EVENTS[s.ev]
end

function P.card(ui, s, ctx)
  local big = ui.text(0, 4, "", "text")
  local sub = ui.text(0, 40, "", "textMuted")
  local strip = ui.box(0, ui.H - 12, ui.W, 8, "accent", { radius = 4 })
  local function paint()
    local v, l = P.summary(s, ctx)
    big:text(v == "今天" and "就是今天" or (v .. " 天"))
    sub:text(l)
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local d = ui.digits((ui.W - 3 * 6 * 14) // 2, 30, 3, 14, "text")
  local name = ui.text(0, 150, "", "text", { w = ui.W })
  local when = ui.text(0, 186, "", "textMuted", { w = ui.W })
  ui.text(0, ui.H - 64, ui.wrap("M 换一个日子；选「自定日子」时用上一个 / 下一个键一天一天调。", ui.W, 2), "textMuted", { w = ui.W, h = 60 })
  local self = {}
  function self.update()
    if not ctx.clock_ok() then d:set("--"); name:text("设备还没对时"); when:text("联网对时后自动显示"); return end
    local n, target = days_left(s, ctx.time())
    d:set(tostring(math.min(n, 999)))
    name:text(n == 0 and ("今天就是" .. EVENTS[s.ev]) or ("距" .. EVENTS[s.ev] .. "还有 " .. n .. " 天"))
    when:text(target and os.date("%Y-%m-%d", target) or "")
  end
  function self.key(k)
    if k == "ok" then s.ev = s.ev % #EVENTS + 1; self.update(); return true end
    if s.ev == 4 and (k == "prev" or k == "next") then
      s.custom = math.max(1, math.min(999, (s.custom or 30) + (k == "next" and 1 or -1)))
      self.update(); return true
    end
    return false
  end
  function self.labels()
    if s.ev == 4 then return { prev = "早一天", ok = "换一个", next = "晚一天" } end
    return { ok = "换一个" }
  end
  self.update()
  return self
end

return P
