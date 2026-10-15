-- 页面：一句话生成（说明 + 现状）。生成在手机 App 或电脑上完成，背夹只负责“装进来、跑起来、出错不连累别人”：
--   AI 按 PLANT_API.md 写出植物 / 主题的 Lua 文件 → 检查（gd.py plant-check）→ 同步到 <DATA>/garden/plants|themes/
--   → 花园几秒内热重载，种子库 / 主题卡里出现带 AI 标记的新项。
-- 本页列出已装的 AI 植物与主题，以及载入失败的文件和原因；M = 立即重新扫描。
local M = {}

function M.new(app, body, params, state)
  local ui, C, S, W = app.ui, app.C, app.S, app.W
  local g = app.garden
  local page = { watch = true }
  app.frame.header(body, { back = "我的", title = "一句话生成", chip = "AI" })
  local iw = W - 36 - 2 * S.border - 28

  local c1 = app.card(body, 18, 64, W - 36, 214)
  ui.text(c1, 14, 10, "怎么用", C.text)
  ui.text(c1, 14, 44, ui.wrap("在手机 App 或电脑上用一句话描述想要的植物或主题，例如：做一株提醒我每小时站起来活动的植物。AI 按植物接口写出 Lua 文件，检查通过后同步到背夹，几秒内出现在种子库或主题卡里。", iw, 5), C.textMuted, { w = iw, h = 160 })

  local ai_plants, broken = {}, {}
  for _, s in ipairs(g.seed_list()) do
    if s.src == "user" then
      if s.err then broken[#broken + 1] = s.name .. "：" .. s.err else ai_plants[#ai_plants + 1] = s.name end
    end
  end
  local ai_themes = {}
  for _, e in ipairs(app.themes.list) do if e.src == "user" and not e.broken then ai_themes[#ai_themes + 1] = e.name end end
  for _, f in ipairs(app.themes.failed) do broken[#broken + 1] = "主题 " .. f.id .. "：" .. f.err end

  local c2 = app.card(body, 18, 292, W - 36, 160)
  ui.text(c2, 14, 10, "已装的 AI 作品", C.text)
  ui.text(c2, 14, 44, ui.fit("植物 " .. #ai_plants .. " 株" .. (#ai_plants > 0 and ("：" .. table.concat(ai_plants, "、")) or ""), iw), C.textMuted)
  ui.text(c2, 14, 74, ui.fit("主题 " .. #ai_themes .. " 个" .. (#ai_themes > 0 and ("：" .. table.concat(ai_themes, "、")) or ""), iw), C.textMuted)
  ui.text(c2, 14, 104, ui.fit("位置 DATA/garden/plants  DATA/garden/themes", iw), C.textMuted)

  local c3 = app.card(body, 18, 466, W - 36, 176, { fill = #broken > 0 and C.surfaceAlt or C.surface })
  ui.text(c3, 14, 10, #broken > 0 and ("载入失败 " .. #broken .. " 个") or "没有载入失败的文件", #broken > 0 and C.danger or C.text)
  for i = 1, math.min(4, #broken) do ui.text(c3, 14, 14 + i * 30, ui.fit(broken[i], iw), C.textMuted) end

  function page.key(k, kind)
    if k == "ok" and kind == "short" then
      g.scan(true); app.themes.scan()
      app.frame.toast("重新扫描完成")
      app.rebuild("rescan")
      return true
    end
    return k ~= "back"
  end
  function page.labels() return { prev = "", ok = "重新扫描", next = "" } end
  function page.rescan() app.rebuild("rescan") end
  return page
end

return M
