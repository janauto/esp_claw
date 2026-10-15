-- 内置种子：喝水。杯数就是小磁今天的喝水计数（ctx.pet.today().cups），与小家的“喝水提醒”房间是同一个数；
-- 喝一杯 = ctx.pet.cheer("water")，小磁会开心，原生首页的小组件也会跟着更新。
local P = {
  name = "喝水",
  desc = "每天 8 杯，和小磁一起记",
  icon = { "..bbbb..", ".b....b.", ".bwwwwb.", ".bwwwwb.", ".bwwwwb.", ".bwwwwb.", "..bbbb..", "........" },
  pal = { b = "#3D8BD9", w = "#A9D8F5" },
}
local GOAL = 8
local function cups(ctx) return ctx.pet.today().cups or 0 end

function P.init(s) end
function P.summary(s, ctx) return cups(ctx) .. "/" .. GOAL, "杯" end

function P.card(ui, s, ctx)
  local t = ui.text(0, 4, "", "text")
  local left = ui.text(0, 36, "", "textMuted")
  local row = {}
  local cw = (ui.W - 7 * 4) // GOAL
  for i = 1, GOAL do row[i] = ui.box((i - 1) * (cw + 4), ui.H - 34, cw, 30, "surfaceAlt", { radius = 4 }) end
  local function paint()
    local n = cups(ctx)
    t:text("今天 " .. n .. " / " .. GOAL .. " 杯")
    left:text(n >= GOAL and "今天喝够啦" or ("还差 " .. (GOAL - n) .. " 杯"))
    for i, c in ipairs(row) do c:fill(i <= n and "accent" or "surfaceAlt") end
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local d = ui.digits((ui.W - 3 * 6 * 14) // 2, 20, 3, 14, "text")
  local row = {}
  local cw = (ui.W - 7 * 8) // GOAL
  for i = 1, GOAL do row[i] = ui.box((i - 1) * (cw + 8), 150, cw, 64, "surfaceAlt", { radius = 6 }) end
  local note = ui.text(0, 240, "", "text", { w = ui.W })
  ui.text(0, 280, ui.wrap("和小家里的「喝水提醒」是同一个数：在哪儿记都算数。喝一杯小磁会开心一点。", ui.W, 3), "textMuted", { w = ui.W, h = 96 })
  local self = {}
  function self.update()
    local n = cups(ctx)
    d:set(math.min(n, 9) .. "/" .. GOAL)
    for i, c in ipairs(row) do c:fill(i <= n and "accent" or "surfaceAlt") end
    note:text(n >= GOAL and "今天喝够啦" or ("还差 " .. (GOAL - n) .. " 杯"))
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

return P
