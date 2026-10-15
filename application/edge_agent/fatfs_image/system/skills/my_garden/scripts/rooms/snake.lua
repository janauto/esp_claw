-- 贪吃蛇：焦点房间里自动演示；按 M 直接在房间里玩（◀ 左转、▶ 右转，相对转向），SET 退出游玩。
-- 沿用主题外壳 apps/snake.lua（样机已验证）；一局玩完会让小磁开心一下（ctx.pet）。
local A = { id = "snake", name = "贪吃蛇", kind = "play" }
local DIRS = { { 1, 0 }, { 0, 1 }, { -1, 0 }, { 0, -1 } }
function A.init(s) s.best = s.best or 0 end
function A.summary(s) return "最高 " .. s.best, "房间里就能玩" end
local function game(ctx, box, w, h, s, playing)
  local ui, C = ctx.ui, ctx.C
  local cell = math.max(8, math.floor(math.min(w / 12, h / 6))) -- 房间里格子也要一眼看清
  local cols, rows = math.floor(w / cell), math.floor(h / cell)
  local ox, oy = math.floor((w - cols * cell) / 2), math.floor((h - rows * cell) / 2)
  local field = ui.plain(box, ox, oy, cols * cell, rows * cell, C.surface, { radius = 2 })
  local pool, food = {}, ui.plain(field, 0, 0, cell - 1, cell - 1, C.danger, { radius = cell })
  -- 结束提示带底板：盖住下面的蛇身，任何主题（含极简反白卡片）都看得清
  local plate = ui.plain(box, ox + 4, oy + 4, cols * cell - 8, 34, C.surfaceAlt, { radius = 4 })
  local note = ui.text(plate, 8, 2, "", C.text, { w = cols * cell - 24 })
  ui.show(plate, false)
  local g = { body = { { 3, 2 }, { 2, 2 }, { 1, 2 } }, dir = 1, score = 0, alive = true, auto = not playing, acc = 0 }
  local function place_food()
    g.fx, g.fy = math.random(0, cols - 1), math.random(0, rows - 1)
    food:set_pos(g.fx * cell, g.fy * cell)
  end
  place_food()
  local self = { game = g }
  local function draw()
    for i, p in ipairs(g.body) do
      local o = pool[i]
      if not o then o = ui.plain(field, 0, 0, cell - 1, cell - 1, C.accent); pool[i] = o end
      o:set_pos(p[1] * cell, p[2] * cell)
      o:set_style({ opa = 255 })
    end
    for i = #g.body + 1, #pool do pool[i]:set_style({ opa = 0 }) end
  end
  function self.turn(d) g.dir = (g.dir - 1 + d) % 4 + 1 end
  local function restart() g.body = { { 3, 2 }, { 2, 2 }, { 1, 2 } }; g.dir = 1; g.alive = true; g.score = 0; ui.set_text(note, ""); ui.show(plate, false) end
  function self.step()
    if not g.alive then
      if not g.auto then return end -- 游玩中撞到了：停下等玩家决定
      restart()
    end
    if g.auto then -- 演示：朝食物拐弯，避免出界
      local hx, hy = g.body[1][1], g.body[1][2]
      if (g.dir == 1 or g.dir == 3) and hx == g.fx then g.dir = g.fy > hy and 2 or 4 end
      if (g.dir == 2 or g.dir == 4) and hy == g.fy then g.dir = g.fx > hx and 1 or 3 end
    end
    local d = DIRS[g.dir]
    local nx, ny = g.body[1][1] + d[1], g.body[1][2] + d[2]
    if g.auto then nx, ny = nx % cols, ny % rows end
    local function die()
      g.alive = false
      if not g.auto then
        ui.set_text(note, "游戏结束 " .. g.score .. " 分"); ui.show(plate, true)
        if g.score > 0 and ctx.pet then ctx.pet.cheer("play") end
      end
    end
    if nx < 0 or ny < 0 or nx >= cols or ny >= rows then die(); return end
    for _, p in ipairs(g.body) do if p[1] == nx and p[2] == ny then die(); return end end
    table.insert(g.body, 1, { nx, ny })
    if nx == g.fx and ny == g.fy then g.score = g.score + 1; if not g.auto then s.best = math.max(s.best, g.score) end; place_food()
    else table.remove(g.body) end
    if #g.body > 40 then table.remove(g.body) end
    draw()
  end
  function self.update(dt) g.acc = g.acc + (dt or 250); if g.acc >= 250 then g.acc = 0; self.step() end end
  function self.key(k)
    if not g.alive and not g.auto then if k == "ok" then restart(); draw() end; return true end
    if k == "prev" then self.turn(-1); return true end
    if k == "next" then self.turn(1); return true end
    return k == "ok" -- 游玩中 M 不做别的事（避免误触打开房间）
  end
  function self.labels() return g.alive and { prev = "左转", ok = "", next = "右转", back = "退出" } or { ok = "再来一局", back = "退出" } end
  function self.set_play(on) g.auto = not on; restart() end
  draw()
  return self
end
function A.preview(ctx, box, w, h, s) return game(ctx, box, w, h, s, false) end
function A.open(ctx, body, w, h, s) return game(ctx, body, w, h - 20, s, true) end
return A
