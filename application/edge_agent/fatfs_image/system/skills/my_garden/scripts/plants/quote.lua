-- 内置种子：每日一句。按日期每天换一句，M 换一句。句子都是本项目自己写的短句。
local P = {
  name = "每日一句",
  desc = "每天一句小提醒",
  icon = { "kkkkkkkk", "kwwwwwwk", "kwkkkkwk", "kwwwwwwk", "kwkkkwwk", "kwwwwwwk", "kkkkkkkk", "........" },
  pal = { k = "#6E5E49", w = "#FFF7E3" },
}
local Q = {
  "慢慢来，比较快", "今天也要好好喝水", "先完成，再完美", "休息是为了走更远的路", "把手机放下，看看窗外",
  "认真吃饭也是一件大事", "一次只做一件事", "你已经做得很好了", "给自己泡杯热茶吧", "深呼吸三次再出发",
  "小步快跑，别怕慢", "早睡是最便宜的补品", "今天的烦恼留给今天", "整理桌面，也整理心情", "记得夸夸自己",
  "走一走，想法会变多", "多喝水，少熬夜", "把大事拆成小事", "笑一笑，运气会变好", "做完这件就休息",
}
local function pick(s, ctx)
  local yday = 1
  if ctx.clock_ok() then yday = os.date("*t", ctx.time()).yday or 1 end
  return Q[(yday + (s.off or 0)) % #Q + 1]
end

function P.init(s) s.off = 0 end
function P.summary(s, ctx) return pick(s, ctx), "每日一句" end

function P.card(ui, s, ctx)
  local t = ui.text(0, 2, "", "text", { w = ui.W, h = ui.H - 4 })
  local last
  local function paint()
    local q = pick(s, ctx)
    if q ~= last then last = q; t:text(ui.wrap(q, ui.W, 3)) end
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  ui.box(0, 0, 8, 200, "accent", { radius = 4 })
  local t = ui.text(28, 10, "", "text", { w = ui.W - 28, h = 200 })
  local d = ui.text(28, 230, "", "textMuted")
  local self = {}
  function self.update()
    t:text(ui.wrap(pick(s, ctx), ui.W - 40, 5))
    d:text(ctx.clock_ok() and ctx.date("%Y-%m-%d") or "")
  end
  function self.key(k)
    if k == "ok" or k == "next" then s.off = (s.off or 0) + 1; self.update(); return true end
    if k == "prev" then s.off = (s.off or 0) - 1; self.update(); return true end
    return false
  end
  function self.labels() return { prev = "上一句", ok = "换一句", next = "下一句" } end
  self.update()
  return self
end

return P
