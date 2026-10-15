-- 故意写成死循环的 AI 植物：验证指令预算（debug.sethook 计数）能打断它，界面不卡死
local P = { name = "转圈圈", desc = "故意死循环，演示指令预算" }
function P.init(s) end
function P.card(ui, s, ctx)
  local n = 0
  while true do n = n + 1 end
end
return P
