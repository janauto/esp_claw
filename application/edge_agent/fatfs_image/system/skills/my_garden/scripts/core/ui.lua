-- 我的 · 花园：绘图助手（沿用主题外壳 core/ui.lua，已在样机上验证；本文件只做增补）。
-- 设计取舍（硬件性能）：
--   * lv_obj 自带默认主题样式（背景、边框、圆角、内边距、可滚动），这里统一清掉，避免多余绘制。
--   * 设备只有一种字号的默认字体（3680 字子集，连 ! 和 ^ 都没有），大数字用 5×7 色块/点阵绘制。
--   * 精灵按行合并成色块，只在表情变化时重建；数字按点位差分更新，只改变化的点。
--   * 文字没变就不下发（真机上每次 set_text 都要重排、重绘、重新光栅化 TTF 字形）。
local M = { count = 0 } -- count：本助手创建的对象累计数，用来核算各页面的对象预算

local lvgl

function M.bind(lv)
  lvgl = lv
  return M
end

-- 透明或纯色的“平面”对象：去掉默认主题的边框/圆角/内边距，关闭滚动
function M.plain(parent, x, y, w, h, color, opts)
  opts = opts or {}
  M.count = M.count + 1
  local o = lvgl.object(parent, {
    x = math.floor(x), y = math.floor(y), w = math.max(1, math.floor(w)), h = math.max(1, math.floor(h)),
    bg_color = color or "#000000", bg_opa = color and (opts.opa or 255) or 0,
    border_width = opts.border or 0, border_color = opts.border_color or "#000000",
    radius = opts.radius or 0, pad = 0,
  })
  o:set_scroll({ dir = "none", scrollbar = "off" })
  return o
end

-- 估算文字宽度（默认字号 20）：中日韩字符约 1 个字号宽，其余约半个
function M.text_w(str, size)
  size = size or 20
  local w = 0
  for _, c in utf8.codes(str or "") do w = w + (c > 0x2E80 and size or size * 0.55) end
  return math.floor(w)
end

-- 文字没变就不下发：真机上每次 set_text 都会让 LVGL 重排并重绘这块区域（TTF 字形还要重新光栅化）
local last_text = setmetatable({}, { __mode = "k" }) -- 弱键表：对象被回收后缓存自动消失
function M.set_text(o, str)
  str = str or ""
  if last_text[o] == str then return end
  last_text[o] = str
  o:set_text(str)
end

-- 按估算宽度截成一行，放不下就以 ... 结尾（Lua 绑定没有标签省略模式，长说明折行会被切掉半行）
function M.fit(str, w, size)
  str = tostring(str or "")
  if M.text_w(str, size) <= w then return str end
  local out, acc, dots = {}, 0, M.text_w("...", size)
  for _, c in utf8.codes(str) do
    local cw = c > 0x2E80 and (size or 20) or (size or 20) * 0.55
    if acc + cw + dots > w then break end
    out[#out + 1] = utf8.char(c); acc = acc + cw
  end
  return table.concat(out) .. "..."
end

-- 按宽度折成最多 lines 行（自己断行，最后一行放不下以 ... 结尾）；返回带换行的字符串
function M.wrap(str, w, lines, size)
  str = tostring(str or ""):gsub("\n", " ")
  size = size or 20
  local rows, cur, acc, cut = {}, {}, 0, false
  for _, c in utf8.codes(str) do
    local cw = c > 0x2E80 and size or size * 0.55
    if acc + cw > w and #cur > 0 then
      rows[#rows + 1] = table.concat(cur)
      cur, acc = {}, 0
      if #rows >= lines then cut = true; break end
    end
    cur[#cur + 1] = utf8.char(c); acc = acc + cw
  end
  if not cut and #cur > 0 then rows[#rows + 1] = table.concat(cur) end
  if cut then rows[lines] = M.fit(rows[lines] .. "...", w, size) end
  return table.concat(rows, "\n")
end

function M.text(parent, x, y, str, color, opts)
  opts = opts or {}
  M.count = M.count + 1
  local l = lvgl.label(parent, { x = math.floor(x), y = math.floor(y), text = str or "", text_color = color or "#FFFFFF" })
  if opts.w then l:set_size(math.floor(opts.w), opts.h or 32) end
  last_text[l] = str or ""
  return l
end

-- 点按承接层：LVGL 里普通对象默认可点击且点击不冒泡，装饰子对象会截走点按。
-- 在容器最上层放一个透明、不可滚动的对象统一接收点按；拖动仍沿滚动链交给父级（例如 tileview）。
function M.catcher(parent, w, h, x, y)
  return M.plain(parent, x or 0, y or 0, w, h)
end

-- 隐藏/显示：绑定里没有 hidden 标志，用整体透明度实现（透明对象仍可点击，需要时另行移出屏幕）
function M.show(o, on)
  o:set_style({ opa = on and 255 or 0 })
end

-- 像素精灵：rows 为等长字符串数组，pal 把字符映射成颜色，"." 透明；px 为放大倍数
function M.sprite(parent, x, y, px, rows, pal)
  local box = M.plain(parent, x, y, #rows[1] * px, #rows * px)
  local self = { box = box, px = px }
  function self.draw(r)
    box:clean()
    for j, row in ipairs(r) do
      local i = 1
      while i <= #row do
        local c = row:sub(i, i)
        local n = 1
        while i + n <= #row and row:sub(i + n, i + n) == c do n = n + 1 end
        if c ~= "." and pal[c] then M.plain(box, (i - 1) * px, (j - 1) * px, n * px, px, pal[c]) end
        i = i + n
      end
    end
  end
  function self.move(nx, ny) box:set_pos(math.floor(nx), math.floor(ny)) end
  self.draw(rows)
  return self
end

-- 网格：把区域切成 n 个等大格子（cols 列），返回 {x,y,w,h} 列表
function M.grid(n, cols, x, y, w, h, gap)
  local rows = math.ceil(n / cols)
  local cw, ch = (w - gap * (cols - 1)) / cols, (h - gap * (rows - 1)) / rows
  local out = {}
  for i = 1, n do
    local c, r = (i - 1) % cols, (i - 1) // cols
    out[i] = { x = math.floor(x + c * (cw + gap)), y = math.floor(y + r * (ch + gap)), w = math.floor(cw), h = math.floor(ch) }
  end
  return out
end

-- 5×7 字模（只收界面会用到的字符；° 等设备字库没有的符号在这里用点阵画）
M.FONT = {
  ["0"] = { ".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###." }, ["1"] = { "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###." },
  ["2"] = { ".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####" }, ["3"] = { "#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###." },
  ["4"] = { "...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#." }, ["5"] = { "#####", "#....", "####.", "....#", "....#", "#...#", ".###." },
  ["6"] = { "..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###." }, ["7"] = { "#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..." },
  ["8"] = { ".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###." }, ["9"] = { ".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.." },
  [":"] = { ".....", "..#..", "..#..", ".....", "..#..", "..#..", "....." }, ["/"] = { "....#", "...#.", "...#.", "..#..", ".#...", ".#...", "#...." },
  ["%"] = { "##..#", "##.#.", "...#.", "..#..", ".#...", ".#.##", "#..##" }, ["+"] = { ".....", "..#..", "..#..", "#####", "..#..", "..#..", "....." },
  ["-"] = { ".....", ".....", ".....", "#####", ".....", ".....", "....." }, ["d"] = { ".##..", "#..#.", ".##..", ".....", ".....", ".....", "....." }, -- d = 度号（点阵画，不走字体）
  ["."] = { ".....", ".....", ".....", ".....", ".....", ".##..", ".##.." },
  [" "] = { ".....", ".....", ".....", ".....", ".....", ".....", "....." },
}
M.FONT["\u{00B0}"] = M.FONT["d"] -- xj:glyph-ok 度号由点阵绘制

-- 大数字：每个字符位预建 5×7 个点，set() 只改变化的点
-- style = "block"（像素方块）或 "dot"（圆点）
function M.digits(parent, x, y, slots, px, color, style, off_color)
  local box = M.plain(parent, x, y, slots * 6 * px, 7 * px)
  local cells, shown = {}, {}
  local gap = style == "dot" and math.max(1, math.floor(px * 0.18)) or 0
  for s = 1, slots do
    cells[s] = {}
    for j = 1, 7 do
      for i = 1, 5 do
        local o = M.plain(box, ((s - 1) * 6 + i - 1) * px + gap, (j - 1) * px + gap, px - gap * 2, px - gap * 2,
          off_color or color, { radius = style == "dot" and px or 0, opa = off_color and 255 or 0 })
        cells[s][(j - 1) * 5 + i] = o
        shown[o] = false
      end
    end
  end
  local self = { box = box, text = "" }
  function self.set(str)
    str = tostring(str)
    if str == self.text then return end
    self.text = str
    local chars = {}
    for _, c in utf8.codes(str) do chars[#chars + 1] = utf8.char(c) end
    for s = 1, slots do
      local ch = chars[s] or ""
      local g = M.FONT[ch ~= "" and ch or " "] or M.FONT[" "]
      for j = 1, 7 do
        local row = g[j]
        for i = 1, 5 do
          local o = cells[s][(j - 1) * 5 + i]
          local on = row:sub(i, i) == "#"
          if shown[o] ~= on then
            shown[o] = on
            if off_color then o:set_style({ bg_color = on and color or off_color })
            else o:set_style({ bg_opa = on and 255 or 0 }) end
          end
        end
      end
    end
  end
  return self
end

---------------------------------------------------------------- 花园增补
-- 用色块画的箭头（设备字库没有 ◀ ▶ ← →；‹ › 设备有但仿真像素字体没有，统一画出来）
-- dir = "left" | "right" | "up" | "down"；size 为箭头高度
function M.arrow(parent, x, y, dir, size, color)
  size = size or 14
  local half = math.max(2, size // 2)
  local box = M.plain(parent, x, y, (dir == "up" or dir == "down") and size or half, (dir == "up" or dir == "down") and half or size)
  local step = math.max(1, size // 7)
  for i = 0, half - 1, step do
    local len = size - i * 2
    if len <= 0 then break end
    if dir == "right" then M.plain(box, i, i, step, len, color)
    elseif dir == "left" then M.plain(box, half - i - step, i, step, len, color)
    elseif dir == "down" then M.plain(box, i, i, len, step, color)
    else M.plain(box, i, half - i - step, len, step, color) end
  end
  return box
end

-- 水滴图标（口渴提示）：几块色块拼出来
function M.drop(parent, x, y, color)
  local box = M.plain(parent, x, y, 14, 18)
  M.plain(box, 5, 0, 4, 5, color, { radius = 2 }); M.plain(box, 3, 4, 8, 5, color, { radius = 2 })
  M.plain(box, 1, 7, 12, 11, color, { radius = 6 })
  return box
end

-- 10 格分段条（心情/能量/陪伴）：set(n) 只改变化的格子
function M.seg(parent, x, y, w, h, n, on_color, off_color, radius)
  n = n or 10
  local gap = 3
  local cw = (w - gap * (n - 1)) / n
  local cells, lit = {}, {}
  for i = 1, n do
    cells[i] = M.plain(parent, x + (i - 1) * (cw + gap), y, cw, h, off_color, { radius = radius or 0 })
    lit[i] = false
  end
  local self = {}
  function self.set(k)
    for i = 1, n do
      local on = i <= k
      if lit[i] ~= on then lit[i] = on; cells[i]:set_style({ bg_color = on and on_color or off_color }) end
    end
  end
  return self
end

return M
