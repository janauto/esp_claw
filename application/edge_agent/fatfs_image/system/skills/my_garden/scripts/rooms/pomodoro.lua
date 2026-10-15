-- 番茄钟（真实计时，离线可用）；完成一次专注会让小磁开心
local A = { id = "pomodoro", name = "番茄钟", kind = "live" }
local function mmss(sec) sec = math.max(0, math.floor(sec)); return string.format("%02d:%02d", sec // 60, sec % 60) end
function A.init(s) s.left = s.left or 25 * 60; s.on = s.on or false end
function A.tick(s, dt, ctx)
  if s.on then
    s.left = s.left - dt / 1000
    if s.left <= 0 then s.left = 0; s.on = false; if ctx and ctx.pet then ctx.pet.cheer("focus") end end
  end
end
function A.summary(s) return mmss(s.left), s.on and "专注中" or (s.left <= 0 and "完成" or "未开始") end
local function page(ctx, body, w, h, s, px)
  local ui, C = ctx.ui, ctx.C
  local d = ui.digits(body, (w - 5 * 6 * px) / 2, math.max(8, (h - 7 * px) / 2 - 20), 5, px, C.text, ctx.digits)
  local bar = ui.plain(body, 16, h - 34, w - 32, 10, C.surfaceAlt, { radius = 5 })
  local fill = ui.plain(bar, 0, 0, 1, 10, C.accent, { radius = 5 })
  local self = {}
  function self.update()
    d.set(mmss(s.left))
    fill:set_size(math.max(1, math.floor((w - 32) * (1 - s.left / 1500))), 10)
  end
  self.update()
  return self
end
function A.preview(ctx, box, w, h, s) return page(ctx, box, w, h, s, math.max(3, math.floor(w / 34))) end
function A.open(ctx, body, w, h, s)
  local self = page(ctx, body, w, h - 60, s, 14)
  function self.key(k)
    if k == "ok" then if s.left <= 0 then s.left = 25 * 60 end; s.on = not s.on; return true end
    if k == "next" then s.on = false; s.left = 25 * 60; self.update(); return true end
    return false
  end
  function self.labels() return { ok = s.on and "暂停" or "开始", next = "重置" } end
  return self
end
return A
