-- 我的 · 花园：主题注册表。主题只作用于「我的」、小家和花园内容区；状态栏、软键栏是系统外框，不随主题变。
--   内置：themes/house（小家 · 像素）、themes/minimal（极简 · 点阵，兜底）、themes/hub（中枢 · 玻璃）
--   自定义：<DATA>/garden/themes/*.lua，经沙箱装载（沿用主题外壳的主题接口 v1；v2 扩展字段可选）
-- 颜色用元表一层层继承：用户配色覆盖 → 主题 token → 基础 token。用户只改了强调色，其余颜色自动沿用主题的。
local sandbox = require("core.sandbox")

local M = {}

M.BUILTIN = { "house", "minimal", "hub" }
M.FALLBACK = "minimal"
M.ROLES = { "bg", "surface", "surfaceAlt", "line", "text", "textMuted", "accent", "onAccent", "danger", "success" }

-- 基础 token：任何主题缺的颜色都从这里来（等于极简主题的配色）
M.BASE = { bg = "#000000", surface = "#0E0E0E", surfaceAlt = "#1C1C1C", line = "#3A3A3A", text = "#F4F4F4", textMuted = "#8C8C8C",
  accent = "#F4F4F4", onAccent = "#000000", danger = "#E5352B", success = "#4FD27F" }

local function is_color(v) return type(v) == "string" and v:match("^#%x%x%x%x%x%x$") ~= nil end
M.is_color = is_color

-- WCAG 相对亮度与对比度（用户换强调色时，自动给强调色上的文字选黑或白）
local function lum(hex)
  local function ch(i)
    local v = tonumber(hex:sub(i, i + 1), 16) / 255
    return v <= 0.03928 and v / 12.92 or ((v + 0.055) / 1.055) ^ 2.4
  end
  return 0.2126 * ch(2) + 0.7152 * ch(4) + 0.0722 * ch(6)
end
function M.contrast(a, b)
  local la, lb = lum(a), lum(b)
  if la < lb then la, lb = lb, la end
  return (la + 0.05) / (lb + 0.05)
end
function M.on_color(bg)
  return M.contrast(bg, "#141414") >= M.contrast(bg, "#FFFFFF") and "#141414" or "#FFFFFF"
end

-- 颜色表：over（用户覆盖）→ theme.tokens → BASE，三层元表继承
function M.palette(theme, over)
  local tokens = setmetatable({}, { __index = M.BASE })
  for k, v in pairs(theme.tokens or {}) do if is_color(v) then tokens[k] = v end end
  local top = {}
  for k, v in pairs(over or {}) do if is_color(v) then top[k] = v end end
  if top.accent and not top.onAccent then top.onAccent = M.on_color(top.accent) end
  return setmetatable(top, { __index = tokens })
end

-- 给植物的只读视图：能读，不能改（改主题颜色的权力不在植物手里）
function M.readonly(t)
  return setmetatable({}, { __index = t, __newindex = function() error("颜色表是只读的", 2) end, __metatable = false })
end

-- 卡片风格：主题 v2 扩展 style 字段；没写的项由 token 和 radius 推出来（同样走元表）
function M.style(theme, C, over)
  local defaults = { border = 2, shadow = 0, radius = theme.radius or 12, ink = C.line, chip = C.accent, chip_text = C.onAccent,
    seg_radius = 2, scene = C.surface, focus = C.accent }
  local s = setmetatable({}, { __index = defaults })
  for k, v in pairs(type(theme.style) == "table" and theme.style or {}) do s[k] = v end
  if over and is_color(over.accent) then s.chip, s.chip_text, s.focus = C.accent, C.onAccent, C.accent end -- 用户换了强调色：标签与焦点框跟着换
  -- 地块名牌：默认与标签同色；主题可以单独指定（极简用白底黑字，红色只留给 AI 标记）
  if rawget(s, "tag") == nil or (over and is_color(over.accent)) then s.tag, s.tag_text = s.chip, s.chip_text end
  return s
end

---------------------------------------------------------------- 注册表
function M.new(app)
  local env = app.env
  local R = { list = {}, by_id = {}, failed = {} }

  local function add(t, src, path)
    if R.by_id[t.id] then return end
    local e = { id = t.id, name = t.name or t.id, tag = t.tag or (src == "user" and "AI" or ""), src = src, mod = t, path = path }
    R.by_id[t.id] = e
    R.list[#R.list + 1] = e
  end

  for _, id in ipairs(M.BUILTIN) do
    local ok, t = pcall(require, "themes." .. id)
    if ok and type(t) == "table" and t.id == id then add(t, "sys")
    else env.log("GD_THEME_FAIL " .. id .. " " .. tostring(t)) end
  end

  -- 校验自定义主题：形状对、颜色合法；能力缺失的部分由核心兜底
  local function validate(t, id)
    if type(t) ~= "table" then return "不是表" end
    if t.id ~= id then return "主题 id 必须与文件名一致（" .. id .. "）" end
    if type(t.name) ~= "string" or t.name == "" then return "缺少 name" end
    if type(t.tokens) ~= "table" then return "缺少 tokens" end
    for _, k in ipairs(M.ROLES) do if not is_color(t.tokens[k]) then return "token " .. k .. " 不是 #RRGGBB" end end
    if M.contrast(t.tokens.text, t.tokens.bg) < 4.5 then return "正文与底色对比度不足 4.5" end
    return nil
  end

  -- 扫描 <DATA>/garden/themes/*.lua：坏主题不进列表，原因记在 failed 里（「一句话生成」页可以看到）
  function R.scan()
    R.failed = {}
    for _, f in ipairs(app.store.list(app.store.paths.themes, ".lua")) do
      local id = f.name:sub(1, -5)
      if not id:match("^[a-z][a-z0-9_]*$") then
        R.failed[#R.failed + 1] = { id = id, err = "文件名只能用小写字母、数字和下划线" }
      elseif not (R.by_id[id] and R.by_id[id].src == "sys") then
        local src = app.store.read(f.path)
        local t, err = sandbox.load(src or "", "themes/" .. f.name, sandbox.env(), 800000)
        err = err or validate(t, id)
        if err then
          R.failed[#R.failed + 1] = { id = id, err = err }
          env.log("GD_THEME_FAIL " .. id .. " " .. err)
          if R.by_id[id] then R.by_id[id].broken = err end
        elseif R.by_id[id] then -- 热重载：同 id 主题换成新模块
          R.by_id[id].mod, R.by_id[id].name, R.by_id[id].broken = t, t.name, nil
        else
          add(t, "user", f.path)
          env.log("GD_THEME_ADD " .. id .. " " .. t.name)
        end
      end
    end
  end

  function R.get(id)
    local e = R.by_id[id]
    if e and not e.broken then return e end
    return R.by_id[M.FALLBACK] or R.list[1]
  end

  return R
end

return M
