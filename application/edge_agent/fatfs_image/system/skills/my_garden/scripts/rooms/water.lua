-- 喝水提醒：杯数就是小磁今天的喝水计数（与花园里的“喝水”植物是同一个数），喝一杯小磁会开心
local A = { id = "water", name = "喝水提醒", kind = "static", generated = true }
local GOAL = 8
function A.init(s) end
local function cups(ctx) return (ctx and ctx.pet and ctx.pet.today().cups) or 0 end
function A.summary(s, ctx) return cups(ctx) .. "/" .. GOAL, "杯" end
function A.open(ctx, body, w, h, s)
  local ui, C = ctx.ui, ctx.C
  local d = ui.digits(body, (w - 3 * 6 * 14) / 2, 30, 3, 14, C.text, ctx.digits)
  local row = {}
  for i = 1, GOAL do row[i] = ui.plain(body, 20 + (i - 1) * ((w - 40) / GOAL), 170, (w - 40) / GOAL - 8, 48, C.surfaceAlt, { radius = 4 }) end
  ui.text(body, 16, 240, "每天 8 杯；花园里的喝水植物记的是同一个数", C.textMuted, { w = w - 32, h = 60 })
  local self = {}
  function self.update()
    local n = cups(ctx)
    d.set(math.min(n, 9) .. "/" .. GOAL)
    for i, c in ipairs(row) do c:set_style({ bg_color = i <= n and C.accent or C.surfaceAlt }) end
  end
  function self.key(k)
    if k == "ok" then ctx.pet.cheer("water"); self.update(); return true end
    if k == "prev" then ctx.pet.cheer("unwater"); self.update(); return true end
    return false
  end
  function self.labels() return { prev = "撤销", ok = "喝一杯" } end
  self.update()
  return self
end
return A
