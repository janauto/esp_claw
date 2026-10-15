-- 宿主 Lua 单测的公共部分：假 LVGL（所有方法都返回 true，只数对象）、按 FAT 语义的假 storage（rename 不能覆盖已有文件）、
-- 假 env，以及极简断言。被测代码与设备上是同一份（package.path 指向 skills/my_garden/scripts）。
local H = {}
local scripts = arg and arg[1] or "../../../application/edge_agent/fatfs_image/system/skills/my_garden/scripts"
package.path = scripts .. "/?.lua;" .. package.path
H.scripts = scripts

---------------------------------------------------------------- 断言
H.passed, H.failed = 0, 0
local current = "?"
function H.case(name, fn)
  current = name
  local ok, err = pcall(fn)
  if ok then H.passed = H.passed + 1 else H.failed = H.failed + 1; print("  不通过 " .. name .. "：" .. tostring(err)) end
end
function H.ok(v, msg) if not v then error((msg or "期望为真") .. "（" .. current .. "）", 2) end end
function H.eq(a, b, msg) if a ~= b then error((msg or "不相等") .. "：期望 " .. tostring(b) .. "，实际 " .. tostring(a), 2) end end
function H.finish(title)
  print(string.format("%s：%d 通过，%d 不通过", title, H.passed, H.failed))
  os.exit(H.failed == 0 and 0 or 1)
end

---------------------------------------------------------------- 假 LVGL
local Obj = {}
Obj.__index = function(_, k) return Obj[k] or function() return true end end
function Obj.get_size() return 480, 800 end
function Obj.get_pos() return 0, 0 end
H.objects = 0
local function new_obj() H.objects = H.objects + 1; return setmetatable({}, Obj) end
H.lvgl = setmetatable({ object = new_obj, label = new_obj, tileview = new_obj, create_screen = new_obj,
  process_events = function() return 0 end, font_load = function() return nil end }, { __index = function() return function() return true end end })

---------------------------------------------------------------- 假 storage（FAT 语义）
function H.tmpdir()
  local p = os.tmpname()
  os.remove(p)
  os.execute("mkdir -p '" .. p .. "'")
  return p
end
local function exists(p) local ok = os.rename(p, p); return ok and true or false end
function H.storage(root)
  local S = {}
  function S.get_root_dir() return root end
  function S.join_path(...)
    local parts = {}
    for i, v in ipairs({ ... }) do parts[#parts + 1] = (i == 1 and v:gsub("/+$", "") or v:gsub("^/+", ""):gsub("/+$", "")) end
    return table.concat(parts, "/")
  end
  S.exists = exists
  function S.mkdir(p) os.execute("mkdir -p '" .. p .. "'"); return true end
  function S.write_file(p, s) local f = assert(io.open(p, "wb")); f:write(s); f:close(); return true end
  function S.read_file(p) local f = io.open(p, "rb"); if not f then return nil, "open failed: " .. p end; local s = f:read("a"); f:close(); return s end
  function S.remove(p) return os.remove(p) end
  function S.rename(a, b)
    if exists(b) then error("rename failed from " .. a .. " to " .. b .. ": File exists（FAT 不能覆盖）") end
    return assert(os.rename(a, b))
  end
  function S.stat(p)
    local f = io.open(p, "rb")
    if not f then return nil, "missing" end
    local n = f:seek("end"); f:close()
    return { type = "file", size = n }
  end
  function S.listdir(p)
    local out = {}
    local h = io.popen("ls -1 '" .. p .. "' 2>/dev/null")
    for name in h:lines() do out[#out + 1] = { name = name, type = "file" } end -- 和仿真一样：不给 size / mtime
    h:close()
    return out
  end
  return S
end

---------------------------------------------------------------- 假 env 与 app（花园模型需要的那部分）
function H.env(root, opts)
  opts = opts or {}
  local logs = {}
  local clock = { ms = 0, unix = opts.unix or 1792080000 } -- 2026-10-15 前后
  local env = {
    lvgl = H.lvgl, storage = H.storage(root), script_dir = scripts, args = {}, logs = logs, clock = clock,
    log = function(s) logs[#logs + 1] = s; if os.getenv("GD_VERBOSE") then print(s) end end,
    now = function() return clock.ms end,
    time = function() return clock.unix end,
    date = function(f) return os.date(f, clock.unix) end,
    ip = function() return nil end,
  }
  return env
end
function H.has_log(env, pat) for _, l in ipairs(env.logs) do if l:find(pat) then return true end end return false end

-- 只装配花园模型需要的部分（不建屏幕）：ui / store / pet / 主题颜色 / 落盘
function H.app(env, data)
  local app = { env = env, lvgl = H.lvgl, log = env.log }
  app.now = env.now
  app.time = env.time
  app.clock_ok = function() return env.time() > 1704067200 end
  app.date = function(f) return env.date(f) end
  app.ui = require("core.ui").bind(H.lvgl)
  app.store = require("core.store").new(env)
  app.data = data or { pet = {}, settings = { theme = "house", colors = {} }, rooms = {}, next_uid = 1 }
  app.saves = 0
  app.mark_dirty = function() end
  app.save_now = function() app.saves = app.saves + 1; return app.store.save_garden(app.data) end
  app.input = { bind_tap = function() end, post = function(fn) fn() end }
  app.frame = { toast = function(t) env.log("TOAST " .. t) end }
  app.safe = function(_, fn, ...) return fn(...) end
  app.pet = require("core.pet").new(app, app.data.pet)
  local Themes = require("core.themes")
  app.T = require("themes.house")
  app.theme = { id = "house", name = "小家", mod = app.T }
  app.C = Themes.palette(app.T, nil)
  app.S = Themes.style(app.T, app.C)
  return app
end

return H
