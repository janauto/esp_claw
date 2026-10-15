-- 时钟：读系统时间（实时）
local A = { id = "clock", name = "时钟", kind = "live" }
local function now_hm(ctx) local ok, v = pcall(ctx.date, "%H:%M"); return ok and v ~= "" and v or "--:--" end
function A.init(s) end
function A.summary(s, ctx) return ctx and now_hm(ctx) or "--:--", "本地时间" end
local function face(ctx, box, w, h, px)
  local d = ctx.ui.digits(box, (w - 5 * 6 * px) / 2, (h - 7 * px) / 2, 5, px, ctx.C.text, ctx.digits)
  local self = {}
  function self.update() d.set(now_hm(ctx)) end
  self.update()
  return self
end
function A.preview(ctx, box, w, h, s) return face(ctx, box, w, h, math.max(3, math.floor(w / 34))) end
function A.open(ctx, body, w, h, s)
  local self = face(ctx, body, w, h - 80, 14)
  local ok, d = pcall(ctx.date, "%Y-%m-%d")
  ctx.ui.text(body, 16, h - 60, ok and d or "", ctx.C.textMuted)
  function self.key() return false end
  function self.labels() return {} end
  return self
end
return A
