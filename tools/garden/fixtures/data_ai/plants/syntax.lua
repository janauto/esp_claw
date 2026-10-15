-- 故意有语法错误的 AI 植物：种子库里直接显示“载入失败”和行号，种不下去
local P = { name = "缺括号", desc = "语法错误"
function P.card(ui, s, ctx) return {} end
return P
