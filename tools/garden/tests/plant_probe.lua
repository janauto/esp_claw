-- 植物试跑（gd.py plant-check 调用）：在宿主 Lua 里用和设备同一份沙箱装载植物，画卡片和全屏页、按键、推进时间，
-- 报告每一步是否通过、对象用量。用法：lua plant_probe.lua <scripts 目录> <植物.lua>
local H = dofile("helper.lua")
local path = arg[2]
local f = assert(io.open(path, "rb"))
local src = f:read("a")
f:close()
local root = H.tmpdir()
local env = H.env(root)
local app = H.app(env)
local store = app.store
store.ensure_dirs()
local id = path:match("([^/]+)%.lua$")
env.storage.write_file(store.paths.plants .. "/" .. id .. ".lua", src)
local G = require("core.garden")
local g = G.new(app, false)
app.garden = g
app.page = function() return nil end
local seed = g.seeds["user/" .. id]
local fail = 0
local function report(ok, what, detail)
  print((ok and "通过  " or "不通过 ") .. what .. (detail and ("：" .. detail) or ""))
  if not ok then fail = fail + 1 end
end
report(seed and not seed.err, "沙箱装载（name 与函数齐全）", seed and seed.err)
if seed and not seed.err then
  report(type(seed.mod.name) == "string", "显示名 " .. tostring(seed.mod.name))
  local idx, err = g.add(seed.key)
  report(idx ~= nil, "种下（init）", err)
  local plot = idx and g.plots()[idx]
  if plot then
    local o0 = app.ui.count
    local card, e1 = g.mount(plot, H.lvgl.object(), 186, 124, "card")
    report(card ~= nil, "地块卡片（预算 " .. G.CARD_BUDGET .. " 个对象）", card and ((app.ui.count - o0) .. " 个对象") or e1)
    if card and card.update then report(g.view_call(plot, card, "update", 60000, 1000) ~= false, "卡片 update", g.withered[plot.uid]) end
    o0 = app.ui.count
    local page, e2 = g.mount(plot, H.lvgl.object(), 414, 540, "open")
    report(page ~= nil, "全屏页（预算 " .. G.PAGE_BUDGET .. " 个对象）", page and ((app.ui.count - o0) .. " 个对象") or e2)
    if page then
      for _, k in ipairs({ "prev", "ok", "next", "ok" }) do
        if page.key then g.view_call(plot, page, "key", 80000, k) end
      end
      report(g.status(plot) == "ok", "按键 prev / ok / next", g.withered[plot.uid])
      if page.labels then
        local ok, l = g.view_call(plot, page, "labels", 20000)
        local long = {}
        for k, v in pairs(type(l) == "table" and l or {}) do if utf8.len(tostring(v)) > 4 then long[#long + 1] = k .. "=" .. tostring(v) end end
        report(ok and type(l) == "table", "软键文字（每个不超过 4 个字）" .. (#long > 0 and ("，过长会被截断：" .. table.concat(long, " ")) or ""))
      end
    end
    for i = 1, 30 do env.clock.ms = env.clock.ms + 1000; g.tick(env.clock.ms) end
    report(g.status(plot) == "ok", "推进 30 秒（tick）", g.withered[plot.uid])
    g.water(plot)
    report(g.status(plot) == "ok", "浇水（on_water）", g.withered[plot.uid])
    local enc = require("core.json").encode(plot.state)
    report(#enc < 8192, "状态可保存为 JSON（" .. #enc .. " 字节，建议 < 8 KB）")
  end
end
os.execute("rm -rf '" .. root .. "'")
os.exit(fail == 0 and 0 or 1)
