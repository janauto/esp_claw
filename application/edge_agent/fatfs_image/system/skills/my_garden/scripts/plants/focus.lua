-- 内置种子：番茄钟（专注计时）。真实计时，离开花园也在走（tick 由核心每 0.5 秒推进）；
-- 完成一次专注 = ctx.pet.cheer("focus")，小磁会说“专注完成，休息一下吧”。
local P = {
  name = "番茄钟",
  desc = "25 分钟专注，完成后小磁会夸你",
  icon = { "...gg...", "..rrrr..", ".rrrrrr.", ".rrwrrr.", ".rrrwrr.", ".rrrrrr.", "..rrrr..", "........" },
  pal = { g = "#5FA35A", r = "#E5533B", w = "#FFFFFF" },
}
local function mmss(sec) sec = math.max(0, math.floor(sec)); return string.format("%02d:%02d", sec // 60, sec % 60) end

function P.init(s) s.mins = 25; s.left = 25 * 60; s.on = false; s.done = 0 end
function P.tick(s, dt, ctx)
  if not s.on then return end
  s.left = s.left - dt / 1000
  if s.left <= 0 then
    s.left, s.on, s.done = 0, false, (s.done or 0) + 1
    ctx.pet.cheer("focus")
    ctx.toast("专注完成，休息一下吧")
    ctx.save()
  end
end
function P.summary(s) return mmss(s.left), s.on and "专注中" or (s.left <= 0 and "完成" or "未开始") end

function P.card(ui, s, ctx)
  local t = ui.text(0, 2, "", "text")
  local st = ui.text(0, 34, "", "textMuted")
  local bar = ui.bar(0, ui.H - 16, ui.W, 10, 0, "accent")
  local function paint()
    t:text(mmss(s.left))
    st:text(s.on and "专注中" or (s.left <= 0 and "完成啦" or "未开始") .. (s.done > 0 and ("  已完成 " .. s.done) or ""))
    bar:set(1 - s.left / (s.mins * 60))
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local d = ui.digits((ui.W - 5 * 6 * 12) // 2, 40, 5, 12, "text")
  local bar = ui.bar(10, 160, ui.W - 20, 12, 0, "accent")
  local st = ui.text(0, 196, "", "text", { w = ui.W })
  local tip = ui.text(0, 232, "", "textMuted", { w = ui.W })
  local self = { rate = 500 }
  function self.update()
    d:set(mmss(s.left))
    bar:set(1 - s.left / (s.mins * 60))
    st:text(s.on and "专注中，别分心" or (s.left <= 0 and "完成啦，休息一下" or ("准备好了：" .. s.mins .. " 分钟")))
    tip:text("今天完成 " .. (s.done or 0) .. " 次")
  end
  function self.key(k)
    if k == "ok" then
      if s.left <= 0 then s.left = s.mins * 60 end
      s.on = not s.on
      self.update(); return true
    end
    if (k == "prev" or k == "next") and not s.on then
      s.mins = math.max(5, math.min(60, s.mins + (k == "next" and 5 or -5)))
      s.left = s.mins * 60
      self.update(); return true
    end
    return false
  end
  function self.labels()
    if s.on then return { ok = "暂停" } end
    return { prev = "少 5 分", ok = s.left < s.mins * 60 and s.left > 0 and "继续" or "开始", next = "多 5 分" }
  end
  self.update()
  return self
end

return P
