-- 我的 · 花园：分页容器（花园地块、种子库）。
-- 字体准备好时用 LVGL 原生 tileview：左右滑由 C 层处理（跟手、吸附），Lua 只在换页后收到一次 value_changed；
--   tileview 必须显式挂上从 DATA 加载的中文字体，否则里面的中文是方框（主题外壳样机实测的坑）。
-- 字体还没准备好（首次运行的头几秒、SD 卡写不进去）：降级为普通容器，按键或点页码翻页，功能不缺。
-- set_scroll 会把没给出的吸附参数重置为 none，所以每次都显式带上 snap_x / snap_y。
local M = {}

-- on_change(i, how)：页码变化（how = "swipe" | "key"）；回调排进主循环执行，不在 LVGL 回调里改界面
function M.new(app, parent, x, y, w, h, n, on_change)
  local ui, lvgl = app.ui, app.lvgl
  local pg = { n = math.max(1, n), index = 1, tiles = {}, swipe = false }
  if app.font and pg.n > 1 then
    local ok, tv = pcall(lvgl.tileview, parent, { x = x, y = y, w = w, h = h, bg_opa = 0, border_width = 0, pad = 0, radius = 0, font = app.font })
    if ok and tv then
      ui.count = ui.count + 1
      tv:set_scroll({ dir = "hor", scrollbar = "off", snap_x = "center", snap_y = "center" })
      for i = 1, pg.n do
        local dir = i == 1 and "right" or i == pg.n and "left" or "hor"
        local t = tv:add_tile(i, 1, dir)
        t:set_style({ bg_opa = 0, pad = 0, border_width = 0 })
        ui.count = ui.count + 1
        pg.tiles[i] = t
      end
      pg.tv, pg.swipe = tv, true
      tv:on("value_changed", function()
        local t = tv:get_active_tile()
        if not t then return end
        local tx = t:get_pos()
        local i = math.floor(tx / w + 0.5) + 1
        if i ~= pg.index then
          pg.index = i
          app.input.post(function() on_change(i, "swipe") end)
        end
      end)
      function pg.show(i, anim)
        i = math.max(1, math.min(pg.n, i))
        if i == pg.index and not anim then return end
        pg.index = i
        tv:set_tile_by_index(i, 1, anim ~= false)
      end
      return pg
    end
  end
  -- 降级：几页是同一位置的普通容器，只有当前页在屏幕内（其余移到屏幕外，避免透明页挡住点按）
  for i = 1, pg.n do pg.tiles[i] = ui.plain(parent, i == 1 and x or x + app.W * 2, y, w, h) end
  function pg.show(i)
    i = math.max(1, math.min(pg.n, i))
    pg.index = i
    for j, t in ipairs(pg.tiles) do t:set_pos(j == i and x or x + app.W * 2, y) end
  end
  return pg
end

return M
