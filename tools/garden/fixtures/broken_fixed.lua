-- 修好的版本：热重载后同一株植物复活，状态保留
local P = { name = "修好的花", desc = "修好了，热重载后复活" }
function P.init(s) s.n = 0 end
function P.card(ui, s, ctx)
  s.cfg = s.cfg or { limit = 3 }
  ui.text(0, 0, "又活过来了", "text")
  ui.text(0, 32, "上限 " .. s.cfg.limit, "textMuted")
  return {}
end
return P
