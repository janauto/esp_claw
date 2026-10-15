-- 内置种子：像素画板（8×8）。◀▶ 移动光标、M 换颜色；触摸直接点格子。
-- 卡片上显示缩略图：按行合并成色块，超出卡片的对象预算（ui.left()）就退成文字，展示“按预算降级”的写法。
local P = {
  name = "像素画板",
  desc = "8 乘 8 的小画板，画个表情吧",
  icon = { "rrrr....", "rrrr....", "rrrr....", "....bbbb", "....bbbb", "gggg....", "gggg....", "........" },
  pal = { r = "#F2643C", b = "#3D8BD9", g = "#5FA35A" },
}
local N = 8
local ORDER = { ".", "a", "b", "c" }
local NEXT = { ["."] = "a", a = "b", b = "c", c = "." }
local PAL = { a = "#F2643C", b = "#3D8BD9", c = "#5FA35A" }
local START = "........" .. "..a..a.." .. "..a..a.." .. "........" .. ".b....b." .. "..bbbb.." .. "........" .. "........"

local function rows(s)
  local r = {}
  for y = 1, N do r[y] = s.px:sub((y - 1) * N + 1, y * N) end
  return r
end
-- 按行合并后的色块数（= sprite 要建的对象数）。注意 Lua 模式不支持“反向引用 + 量词”，所以逐字符数
local function runs(s)
  local n = 0
  for _, row in ipairs(rows(s)) do
    local prev
    for c in row:gmatch(".") do
      if c ~= prev and c ~= "." then n = n + 1 end
      prev = c
    end
  end
  return n
end

function P.init(s) s.px = START; s.cur = 1 end
function P.summary(s)
  local n = 0
  for _ in s.px:gmatch("[abc]") do n = n + 1 end
  return "画了 " .. n .. " 格", "像素画板"
end

function P.card(ui, s, ctx)
  local size = math.min(ui.H - 4, 96)
  ui.box(0, 0, size, size, "surfaceAlt", { radius = 4 })
  local t = ui.text(size + 10, 4, "", "textMuted", { w = ui.W - size - 10, h = 60 })
  -- 按预算降级：缩略图要的色块数超过卡片剩下的对象预算，就只显示文字（不让整株枯萎）
  local art, art_runs
  local need = runs(s)
  if need + 1 <= ui.left() then art = ui.sprite(0, 0, size // N, rows(s), PAL); art_runs = need
  else ui.text(4, 4, "图太花", "textMuted") end
  local last = s.px
  local function paint()
    t:text(ui.wrap(select(1, P.summary(s)), ui.W - size - 10, 2))
    if art and s.px ~= last then
      local n = runs(s)
      if n <= ui.left() + art_runs then last = s.px; art:draw(rows(s)); art_runs = n end -- 重画前的色块会删掉：可用 = 剩余 + 旧图
    end
  end
  paint()
  return { update = paint }
end

function P.open(ui, s, ctx)
  local cell = math.min((ui.W - 20) // N, (ui.H - 120) // N)
  local ox = (ui.W - cell * N) // 2
  local cells = {}
  ui.box(ox - 2, -2, cell * N + 4, cell * N + 4, "line", { radius = 4 })
  for i = 1, N * N do
    local x, y = (i - 1) % N, (i - 1) // N
    local c = ui.box(ox + x * cell + 1, y * cell + 1, cell - 2, cell - 2, "surface")
    cells[i] = c
    c:on_tap(function() s.cur = i; s.px = s.px:sub(1, i - 1) .. NEXT[s.px:sub(i, i)] .. s.px:sub(i + 1) end)
  end
  local cursor = ui.box(0, 0, cell, cell, nil, { border = 3, border_color = "accent" })
  local tip = ui.text(0, cell * N + 16, "", "textMuted", { w = ui.W })
  local self = { rate = 300 }
  function self.update()
    for i, c in ipairs(cells) do
      local ch = s.px:sub(i, i)
      c:fill(ch == "." and "surface" or PAL[ch])
    end
    cursor:pos(ox + ((s.cur - 1) % N) * cell, ((s.cur - 1) // N) * cell)
    tip:text("第 " .. ((s.cur - 1) // N + 1) .. " 行第 " .. ((s.cur - 1) % N + 1) .. " 格    M 换颜色")
  end
  function self.key(k)
    if k == "prev" then s.cur = (s.cur - 2) % (N * N) + 1; self.update(); return true end
    if k == "next" then s.cur = s.cur % (N * N) + 1; self.update(); return true end
    if k == "ok" then
      local i = s.cur
      s.px = s.px:sub(1, i - 1) .. NEXT[s.px:sub(i, i)] .. s.px:sub(i + 1)
      self.update(); return true
    end
    return false
  end
  function self.labels() return { prev = "左一格", ok = "换颜色", next = "右一格" } end
  self.update()
  return self
end

return P
