-- 故意越权的 AI 植物：想用 io / os.execute / require。沙箱里这些全局变量根本不存在，只会报错枯萎
local P = { name = "越狱草", desc = "故意越权，演示沙箱" }
function P.init(s) end
function P.card(ui, s, ctx)
  local ok = pcall(function() return require("storage") end)
  ui.text(0, 0, ok and "拿到了 storage" or "require 不存在", "text")
  local f = io.open("/sdcard/garden/garden.json", "w") -- io 是 nil：attempt to index a nil value (global 'io')
  return {}
end
return P
