-- 内置种子：向日葵。每天浇一次水长高一阶（种子 → 发芽 → 小苗 → 长叶 → 花苞 → 开花），连着浇会记连续天数。
-- 只用《植物接口》（PLANT_API.md）里的能力写成，和用户 / AI 植物走同一个沙箱：它就是给 AI 看的范例之一。
local P = {
  name = "向日葵",
  desc = "每天浇一次水，它就长高一点",
  icon = { "...yy...", "..yooy..", "...yy...", "....g...", "..llg...", "....gll.", "....g...", "ssssssss" },
  pal = { y = "#F6C445", o = "#8A5A35", g = "#3F8A3A", l = "#5FA35A", s = "#8A5A35" },
}
local STAGES = { "种子", "发芽", "小苗", "长叶", "花苞", "开花" }
local PAL = { y = "#F6C445", o = "#8A5A35", g = "#3F8A3A", l = "#6DB35F", b = "#9CCB5A", s = "#A8774A", d = "#6B4423", e = "#C9A27A" }
local SOIL = { "ssssssssssss", "dddddddddddd" }
local ART = {
  [0] = { "............", "............", "............", "............", "............", "............", "............", "............", "............", "............", ".....ee.....", "....eeee....", SOIL[1], SOIL[2] },
  [1] = { "............", "............", "............", "............", "............", "............", "............", "............", "............", "....l..l....", ".....ll.....", ".....g......", SOIL[1], SOIL[2] },
  [2] = { "............", "............", "............", "............", "............", "............", "............", "...ll.......", "....llg.ll..", "......gll...", "......g.....", "......g.....", SOIL[1], SOIL[2] },
  [3] = { "............", "............", "............", "............", ".....g......", ".....g.ll...", "..ll.gll....", "...llg......", ".....g.ll...", ".....gll....", ".....g......", ".....g......", SOIL[1], SOIL[2] },
  [4] = { "............", "............", ".....bb.....", "....bbbb....", ".....bb.....", ".....g......", "..ll.g.ll...", "...llgll....", ".....g......", "...llg......", ".....g.ll...", ".....g......", SOIL[1], SOIL[2] },
  [5] = { "...yyyyyy...", "..yyooooyy..", ".yyooooooyy.", ".yyooooooyy.", "..yyooooyy..", "...yyyyyy...", ".....g......", "..ll.g.ll...", "...llgll....", ".....g......", "...llg......", ".....g......", SOIL[1], SOIL[2] },
}

local function yesterday(day)
  local y, m, d = day:match("^(%d+)-(%d+)-(%d+)$")
  if not y then return "" end
  return os.date("%Y-%m-%d", os.time({ year = tonumber(y), month = tonumber(m), day = tonumber(d), hour = 12 }) - 86400)
end

function P.init(s) s.stage = 0; s.streak = 0; s.total = 0; s.last = "" end

function P.on_water(s, ctx)
  local today = ctx.date("%Y-%m-%d")
  s.streak = (s.last == yesterday(today)) and (s.streak + 1) or 1
  s.last, s.total = today, s.total + 1
  s.stage = math.min(5, s.stage + 1)
  ctx.toast(s.stage == 5 and "向日葵开花了" or ("向日葵长到了：" .. STAGES[s.stage + 1]))
end

function P.summary(s) return STAGES[s.stage + 1], "连浇 " .. s.streak .. " 天" end

function P.card(ui, s, ctx)
  local art = ui.sprite(4, ui.H - 14 * 7 - 2, 7, ART[s.stage], PAL)
  local t1 = ui.text(100, 8, STAGES[s.stage + 1], "text")
  local t2 = ui.text(100, 40, "连浇 " .. s.streak .. " 天", "textMuted")
  local t3 = ui.text(100, 72, "共 " .. s.total .. " 次", "textMuted")
  local shown = s.stage
  return { update = function()
    if shown ~= s.stage then shown = s.stage; art:draw(ART[s.stage]) end
    t1:text(STAGES[s.stage + 1]); t2:text("连浇 " .. s.streak .. " 天"); t3:text("共 " .. s.total .. " 次")
  end }
end

-- 全屏页：大图 + 成长相册（◀▶ 翻看长过的每个阶段）；M 留给核心的“浇水 / 更多”
function P.open(ui, s, ctx)
  local view_stage = s.stage
  local art = ui.sprite((ui.W - 12 * 18) // 2, 24, 18, ART[view_stage], PAL)
  local title = ui.text(0, 300, "", "text", { w = ui.W })
  local info = ui.text(0, 336, "", "textMuted", { w = ui.W })
  ui.text(0, ui.H - 64, ui.wrap("每天浇一次水，它就长高一阶；连着浇，小磁也会更开心。", ui.W, 2), "textMuted", { w = ui.W, h = 60 })
  local self = {}
  local function paint()
    art:draw(ART[view_stage])
    title:text(view_stage == s.stage and ("现在：" .. STAGES[s.stage + 1]) or ("相册：" .. STAGES[view_stage + 1]))
    info:text("连浇 " .. s.streak .. " 天    共浇 " .. s.total .. " 次")
  end
  function self.update() if view_stage == s.stage then paint() end end
  function self.key(k)
    if k == "prev" and view_stage > 0 then view_stage = view_stage - 1; paint(); return true end
    if k == "next" and view_stage < s.stage then view_stage = view_stage + 1; paint(); return true end
    return false
  end
  function self.labels() return { prev = view_stage > 0 and "前一阶" or "", next = view_stage < s.stage and "后一阶" or "" } end
  paint()
  return self
end

return P
