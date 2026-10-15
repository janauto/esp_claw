-- WorkBuddy 审批（演示数据：未接入真实 Agent 服务）
local STEPS = { "收集资料", "生成初稿", "校对格式", "发送给 3 位同事？", "归档" }
local A = { id = "workbuddy", name = "WorkBuddy", kind = "static", demo = true }
function A.init(s) s.step = s.step or 4; s.note = s.note or "等你确认" end
-- 演示：驳回后 3 秒 Agent 重写完成，回到待确认
function A.tick(s, dt)
  if s.rewrite then s.rewrite = s.rewrite - dt; if s.rewrite <= 0 then s.rewrite = nil; s.step = 4; s.note = "请再确认" end end
end
function A.summary(s) return (s.step > 5 and "完成" or (math.min(s.step, 5) .. "/5")), s.note end
function A.open(ctx, body, w, h, s)
  local ui, C = ctx.ui, ctx.C
  local rows = {}
  local title = ui.text(body, 16, 8, "", C.text, { w = w - 32 })
  for i, name in ipairs(STEPS) do
    local y = 48 + (i - 1) * 56
    rows[i] = { bg = ui.plain(body, 8, y, w - 16, 48, C.surfaceAlt, { radius = math.min(12, ctx.radius) }), mark = ui.text(body, 22, y + 12, "", C.text), name = ui.text(body, 60, y + 12, name, C.text) }
  end
  local note = ui.text(body, 16, 48 + 5 * 56 + 8, "", C.textMuted, { w = w - 32, h = 64 }) -- 两行说明，留足高度
  local self = {}
  function self.update()
    ui.set_text(title, "整理周报  " .. (s.step > 5 and "已完成" or s.step == 4 and "等你确认" or "Agent 重写中"))
    for i, r in ipairs(rows) do
      local cur, done = i == s.step, i < s.step
      r.bg:set_style({ bg_color = cur and C.accent or C.surfaceAlt })
      ui.set_text(r.mark, done and "OK" or cur and "！" or "-")
      r.mark:set_style({ text_color = cur and C.onAccent or C.text })
      r.name:set_style({ text_color = cur and C.onAccent or done and C.textMuted or C.text })
    end
    ui.set_text(note, s.step > 5 and "任务完成，已归档（演示数据）。M 重新演示" or s.step == 4 and "M 批准后继续执行，下一个键驳回让 Agent 重写（演示数据）" or "已驳回，Agent 正在重写，约 3 秒后回来")
  end
  function self.key(k)
    if k == "ok" and s.step == 4 then s.step = 6; s.note = "已批准"; self.update(); return true end
    if k == "next" and s.step == 4 then s.step = 3; s.note = "重写中"; s.rewrite = 3000; self.update(); return true end
    if k == "ok" and s.step > 5 then s.step = 4; s.note = "等你确认"; s.rewrite = nil; self.update(); return true end
    return false
  end
  function self.labels() return s.step == 4 and { ok = "批准", next = "驳回" } or s.step > 5 and { ok = "重新演示" } or {} end
  self.update()
  return self
end
return A
