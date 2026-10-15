-- 客厅空调（演示：未调用红外模块；接入后只能显示“已发送”，红外没有回执）
local A = { id = "ac", name = "客厅空调", kind = "static", demo = true }
local MODES = { "制冷", "除湿", "送风", "自动" }
function A.init(s) s.temp = s.temp or 26; s.mode = s.mode or 1; s.sent = s.sent or 0 end
function A.summary(s) return s.temp .. "度", MODES[s.mode] end
function A.open(ctx, body, w, h, s)
  local ui, C = ctx.ui, ctx.C
  local big = ui.digits(body, (w - 3 * 6 * 14) / 2, 40, 3, 14, C.text, ctx.digits)
  local chips = {}
  for i, m in ipairs(MODES) do
    local x = 12 + (i - 1) * ((w - 24) / 4)
    chips[i] = { bg = ui.plain(body, x, 170, (w - 24) / 4 - 8, 44, C.surfaceAlt, { radius = math.min(12, ctx.radius) }) }
    chips[i].t = ui.text(chips[i].bg, 14, 8, m, C.text)
  end
  local sent = ui.text(body, 16, 236, "", C.success, { w = w - 32 })
  ui.text(body, 16, h - 40, "演示数据：未接红外发射", C.textMuted, { w = w - 32 })
  local self = {}
  function self.update()
    big.set(string.format("%2d", s.temp) .. "\u{00B0}") -- 度号由点阵画（设备字库没有这个符号）
    for i, c in ipairs(chips) do c.bg:set_style({ bg_color = i == s.mode and C.accent or C.surfaceAlt }); c.t:set_style({ text_color = i == s.mode and C.onAccent or C.text }) end
    ui.set_text(sent, s.sent > 0 and ("已发送 " .. s.temp .. " 度（红外无回执）") or "")
  end
  function self.key(k)
    if k == "next" then s.temp = math.min(30, s.temp + 1); s.sent = 1; self.update(); return true end
    if k == "prev" then s.temp = math.max(16, s.temp - 1); s.sent = 1; self.update(); return true end
    if k == "ok" then s.mode = s.mode % 4 + 1; self.update(); return true end
    return false
  end
  function self.labels() return { prev = "降温", ok = "模式", next = "升温" } end
  self.update()
  return self
end
return A
