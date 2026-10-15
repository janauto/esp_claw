-- 故意写错的 AI 植物：卡片里访问了不存在的配置表（运行时错误）。用来验证“枯萎”隔离：
-- 只有这一株显示枯萎和错误原因，别的地块照常；修好文件（fixtures/broken_fixed.lua）后热重载复活。
local P = { name = "坏掉的花", desc = "故意写错，用来演示枯萎隔离" }
function P.init(s) s.n = 0 end
function P.card(ui, s, ctx)
  local limit = s.cfg.limit -- s.cfg 不存在：attempt to index a nil value
  ui.text(0, 0, "上限 " .. limit, "text")
  return {}
end
return P
