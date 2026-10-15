-- 我的 · 小磁（my_garden）入口。固件 1.0 的「我的」：用户可客制化的花园。
-- 启动（契约 1.1）：原生首页以 cap_lua_run_script_async("<SYSTEM>/skills/my_garden/scripts/main.lua", args_json, 0,
--   "my_garden", "display", true, ...) 独占显示启动；args_json = {"page":"mine"|"house"|"garden"}，固件注入为全局 args。
-- 退出：主循环结束即回原生首页（SET 逐级返回，顶层再按 SET 退出；系统底部上滑会直接停止本应用）。
-- 模块按脚本所在目录解析：固件（cap_lua_runtime.c）把脚本目录放在 package.path 最前面，所以 require("core.app")
--   找的是 <SYSTEM>/skills/my_garden/scripts/core/app.lua；SYSTEM 只读，所有写入去 <DATA>/garden/。
local t_boot = nil
local lvgl = require("lvgl")
local system = require("system")
local storage = require("storage")
local okt, t0 = pcall(system.millis)
t_boot = okt and t0 or nil

local script_dir = (package.path or ""):match("^([^;]-)/%?%.lua") or "."

local function read(p)
  local ok, v = pcall(storage.read_file, p)
  if ok and type(v) == "string" and #v > 0 then return v end
end
local json = require("core.json")

-- args：固件注入全局 args；仿真不注入，回退到脚本目录的 sim_args.json（只在仿真里挂载，设备 SYSTEM 里没有这个文件）
local A = {}
if type(args) == "table" then for k, v in pairs(args) do A[k] = v end end
local sim_args = read(script_dir .. "/sim_args.json")
if sim_args then
  local t = json.decode(sim_args)
  if type(t) == "table" then for k, v in pairs(t) do if A[k] == nil then A[k] = v end end end
end
-- 仿真按键时间轴（同样只在仿真里挂载）
local autoplay
local raw = read(script_dir .. "/autoplay.json")
if raw then autoplay = json.decode(raw) end

local env = {
  lvgl = lvgl, storage = storage, script_dir = script_dir, args = A, autoplay = type(autoplay) == "table" and autoplay or nil,
  log = function(s) print(s) end,
  now = function() return system.millis() end,
  date = function(f) return system.date(f) end,
  time = function() return system.time() end,
  ip = function() return system.ip() end,
  info = function() return system.info() end,
  max_ms = tonumber(A.max_ms), frame_ms = 30, t_boot = t_boot, no_font_copy = A.no_font_copy == true,
}

local owned, touch = false, false
local ok, err = pcall(function()
  assert(lvgl.init({ font_size = 20, buffer_lines = 40 }))
  owned = true
  -- 触摸：Lua 应用必须自己把 lcd_touch 注册成 LVGL 输入设备，否则点按、滑动都无效
  pcall(function()
    local bm = require("board_manager")
    local h = bm.get_lcd_touch_handle("lcd_touch")
    if h and lvgl.indev_register("touch", h) then touch = true end
  end)
  print(touch and "GD_INPUT touch=lcd_touch" or "GD_INPUT touch=none（只用实体键）")
  require("core.app").run(env)
end)
if touch then pcall(lvgl.indev_unregister, "touch") end
if owned then pcall(lvgl.deinit) end
if not ok then print("GD_FATAL " .. tostring(err)); error(err) end
print("GD_DONE")
