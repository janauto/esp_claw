-- 我的 · 花园：给植物用的绘图接口（沙箱里的“画笔”）。
-- 植物拿不到 LVGL 对象本身，只拿到“句柄”：句柄的方法由核心实现，原始对象藏在本文件的私有弱表里，
-- 植物没法删掉别人的界面、乱挂事件或者越过对象预算。句柄的元表是锁住的（__metatable），也改不了方法。
-- 坐标都相对植物自己的区域（地块卡片或植物页），超出区域的部分会被裁掉。
local Themes = require("core.themes")

local M = {}

local RAW = setmetatable({}, { __mode = "k" }) -- 句柄 → { obj, kind, extra }
local Handle = {}
local HMT = { __index = Handle, __metatable = "locked", __newindex = function() error("句柄是只读的", 2) end }

local function raw(h)
  local r = RAW[h]
  if not r then error("不是有效的绘图句柄", 3) end
  return r
end

-- 句柄方法（所有植物共用；颜色可以写 #RRGGBB 或主题 token 名，如 "accent"）
function Handle:text(s) local r = raw(self); r.ui.set_text(r.obj, tostring(s or "")); return self end
function Handle:color(c) local r = raw(self); r.obj:set_style({ text_color = r.col(c) }); return self end
function Handle:fill(c) local r = raw(self); r.obj:set_style({ bg_color = r.col(c), bg_opa = 255 }); return self end
function Handle:border(c, w) local r = raw(self); r.obj:set_style({ border_color = r.col(c), border_width = math.floor(tonumber(w) or 2) }); return self end
function Handle:pos(x, y) local r = raw(self); r.obj:set_pos(math.floor(tonumber(x) or 0), math.floor(tonumber(y) or 0)); return self end
function Handle:size(w, h) local r = raw(self); r.obj:set_size(math.max(1, math.floor(tonumber(w) or 1)), math.max(1, math.floor(tonumber(h) or 1))); return self end
function Handle:show(on) local r = raw(self); r.obj:set_style({ opa = on == false and 0 or 255 }); return self end
function Handle:set(v) -- 大数字：set("12:30")；进度条：set(0.6)
  local r = raw(self)
  if r.kind == "digits" then r.extra.set(tostring(v))
  elseif r.kind == "bar" then
    local ratio = math.max(0, math.min(1, tonumber(v) or 0))
    r.extra.fill:set_size(math.max(1, math.floor(r.extra.w * ratio)), r.extra.h)
  else error("只有 digits 和 bar 句柄支持 set", 2) end
  return self
end
function Handle:draw(rows) -- 像素图：换一组行字符串重画
  local r = raw(self)
  if r.kind ~= "sprite" then error("只有 sprite 句柄支持 draw", 2) end
  r.check_rows(rows)
  local c0 = r.ui.count
  r.extra.draw(rows)
  local n = r.ui.count - c0
  r.acct(n - (r.n or 0)) -- 重画前的色块已经删掉：只记净增量
  r.n = n
  return self
end
function Handle:move(x, y) local r = raw(self); if r.kind == "sprite" then r.extra.move(x, y) else r.obj:set_pos(math.floor(x), math.floor(y)) end; return self end
-- 点按：回调在主循环里、在沙箱预算内执行；出错同样让这株植物枯萎
function Handle:on_tap(fn)
  local r = raw(self)
  if type(fn) ~= "function" then error("on_tap 需要函数", 2) end
  r.tap(r.obj, fn)
  return self
end

-- 新建一支画笔：parent 为植物区域容器；budget 为对象上限；tap(obj, fn) 由花园模型提供（受控执行）
function M.new(app, parent, w, h, budget, tap)
  local ui, C = app.ui, app.C
  local used = 0
  local P = { W = w, H = h, C = Themes.readonly(C), digits_style = app.T.digits or "block" }
  local function col(c)
    if type(c) == "string" then
      if C[c] and Themes.is_color(C[c]) then return C[c] end
      if Themes.is_color(c) then return c end
    end
    error("颜色要写 #RRGGBB 或主题 token 名（bg/surface/surfaceAlt/line/text/textMuted/accent/onAccent/danger/success）", 3)
  end
  local function take(n)
    used = used + (n or 1)
    if used > budget then error("界面对象超出预算（上限 " .. budget .. " 个）", 3) end
  end
  local function check_rows(rows)
    if type(rows) ~= "table" or #rows == 0 or #rows > 32 then error("像素图要 1～32 行字符串", 3) end
    for _, s in ipairs(rows) do if type(s) ~= "string" or #s > 32 then error("像素图每行是不超过 32 个字符的字符串", 3) end end
  end
  local function wrap(obj, kind, extra)
    local hd = setmetatable({}, HMT)
    RAW[hd] = { obj = obj, kind = kind, extra = extra, ui = ui, col = col, check_rows = check_rows, tap = tap }
    return hd
  end
  local function num(v, d) return math.floor(tonumber(v) or d or 0) end

  function P.box(x, y, bw, bh, color, opts)
    take()
    opts = type(opts) == "table" and opts or {}
    local o = ui.plain(parent, num(x), num(y), num(bw, 1), num(bh, 1), color and col(color) or nil,
      { radius = num(opts.radius), border = num(opts.border), border_color = opts.border_color and col(opts.border_color) or nil, opa = opts.opa and num(opts.opa) or nil })
    return wrap(o, "box")
  end
  function P.text(x, y, s, color, opts)
    take()
    opts = type(opts) == "table" and opts or {}
    local o = ui.text(parent, num(x), num(y), tostring(s or ""), col(color or "text"), { w = opts.w and num(opts.w), h = opts.h and num(opts.h) })
    return wrap(o, "text")
  end
  function P.digits(x, y, slots, px, color, style)
    slots, px = math.max(1, math.min(6, num(slots, 2))), math.max(2, math.min(16, num(px, 4)))
    take(1 + slots * 35)
    local d = ui.digits(parent, num(x), num(y), slots, px, col(color or "text"), style or P.digits_style)
    return wrap(d.box, "digits", d)
  end
  function P.sprite(x, y, px, rows, pal)
    check_rows(rows)
    if type(pal) ~= "table" then error("sprite 需要调色板表", 2) end
    local p = {}
    for k, v in pairs(pal) do if type(k) == "string" and #k == 1 then p[k] = col(v) end end
    local c0 = ui.count
    local sp = ui.sprite(parent, num(x), num(y), math.max(1, math.min(16, num(px, 4))), rows, p)
    local n = ui.count - c0 -- 按行合并后的真实色块数
    take(n)
    local hd = wrap(sp.box, "sprite", sp)
    RAW[hd].n = n - 1
    RAW[hd].acct = function(delta) take(delta) end
    return hd
  end
  function P.bar(x, y, bw, bh, ratio, color, bg)
    take(2)
    bw, bh = num(bw, 10), num(bh, 6)
    local back = ui.plain(parent, num(x), num(y), bw, bh, col(bg or "surfaceAlt"), { radius = bh // 2 })
    local fill = ui.plain(back, 0, 0, 1, bh, col(color or "accent"), { radius = bh // 2 })
    local hd = wrap(back, "bar", { fill = fill, w = bw, h = bh })
    hd:set(ratio or 0)
    return hd
  end
  function P.fit(s, fw) return ui.fit(tostring(s or ""), num(fw, 100)) end
  function P.wrap(s, ww, lines) return ui.wrap(tostring(s or ""), num(ww, 100), math.max(1, num(lines, 2))) end
  function P.text_w(s) return ui.text_w(tostring(s or "")) end
  function P.used() return used end
  function P.left() return budget - used end -- 还能建多少个对象（复杂的图可以据此降级成文字）
  return P
end

return M
