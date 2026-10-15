-- 示例 AI 植物（按 PLANT_API.md 生成）：起来走走。每坐满 50 分钟提醒一次，站起来按 M 记一次。
local P = {
  name = "起来走走",
  desc = "坐久了提醒你起来活动",
  icon = { "...kk...", "...kk...", "..kkkk..", ".k.kk.k.", "...kk...", "..k..k..", ".k....k.", "........" },
  pal = { k = "accent" },
}
local SIT = 50 * 60

function P.init(s) s.sat = 0; s.stands = 0; s.day = "" end
function P.tick(s, dt, ctx)
  local today = ctx.date("%Y-%m-%d")
  if s.day ~= today then s.day = today; s.stands = 0 end
  s.sat = math.min(SIT, s.sat + dt / 1000)
end
function P.summary(s) return s.sat >= SIT and "该起来了" or (math.floor((SIT - s.sat) / 60) .. " 分钟后提醒"), "今天站起 " .. s.stands .. " 次" end

function P.card(ui, s, ctx)
  local t = ui.text(0, 2, "", "text")
  local sub = ui.text(0, 34, "", "textMuted")
  local bar = ui.bar(0, ui.H - 14, ui.W, 10, 0, "accent")
  local function paint()
    local v, l = P.summary(s)
    t:text(v); sub:text(l); bar:set(s.sat / SIT)
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local big = ui.text(0, 20, "", "text", { w = ui.W })
  local bar = ui.bar(0, 70, ui.W, 14, 0, "accent")
  local info = ui.text(0, 110, "", "textMuted", { w = ui.W })
  local self = {}
  function self.update()
    local v, l = P.summary(s)
    big:text(v); bar:set(s.sat / SIT); info:text(l)
  end
  function self.key(k)
    if k == "ok" then s.sat = 0; s.stands = s.stands + 1; ctx.pet.cheer("play"); ctx.toast("走一走，真棒"); self.update(); return true end
    return false
  end
  function self.labels() return { ok = "站起来了" } end
  self.update()
  return self
end

return P
