-- 内置种子：计数器。数杯数、数俯卧撑、数什么都行；归零要连按两下（防误触）。
local P = {
  name = "计数器",
  desc = "数什么都行，M 加一",
  icon = { "........", ".kkkkkk.", ".k.kk.k.", ".kkkkkk.", ".k.kk.k.", ".kkkkkk.", "........", "........" },
  pal = { k = "#3D8BD9" },
  care = false, -- 计数器不用浇水
}
function P.init(s) s.n = 0 end
function P.summary(s) return tostring(s.n), "次" end

function P.card(ui, s, ctx)
  local t = ui.text(0, 2, "", "text")
  local sub = ui.text(0, 34, "M 加一，打开后可以减", "textMuted", { w = ui.W, h = 60 })
  local bar = ui.bar(0, ui.H - 14, ui.W, 10, 0, "accent")
  local function paint() t:text("已数 " .. s.n .. " 次"); bar:set((s.n % 10) / 10) end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local d = ui.digits((ui.W - 4 * 6 * 14) // 2, 40, 4, 14, "text")
  local note = ui.text(0, 160, "", "textMuted", { w = ui.W })
  local armed = 0
  local self = {}
  function self.update()
    d:set(string.format("%4d", math.min(s.n, 9999)))
    note:text(armed > 0 and ctx.now() - armed < 2000 and "再按一次下一个键就归零" or "上一个键减一，M 加一，下一个键归零（按两下）")
  end
  function self.key(k)
    if k == "ok" then s.n = s.n + 1; armed = 0; self.update(); return true end
    if k == "prev" then s.n = math.max(0, s.n - 1); armed = 0; self.update(); return true end
    if k == "next" then
      if armed > 0 and ctx.now() - armed < 2000 then s.n = 0; armed = 0; ctx.toast("归零了") else armed = ctx.now() end
      self.update(); return true
    end
    return false
  end
  function self.labels()
    return { prev = "减一", ok = "加一", next = armed > 0 and ctx.now() - armed < 2000 and "确认归零" or "归零" }
  end
  self.update()
  return self
end

return P
