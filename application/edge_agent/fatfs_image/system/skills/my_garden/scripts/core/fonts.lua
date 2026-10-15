-- 我的 · 花园：字体（沿用主题外壳 core/fonts.lua 的做法，样机已验证）。
-- 问题：固件以西文字体初始化 LVGL 默认主题，tileview 会带上自己的西文字体样式，里面的中文变方框；
--       Lua 绑定拿不到默认字体句柄，font_load 又只认 DATA 下的路径。
-- 办法：把系统分区的中文字体流式复制到 DATA（只做一次，按大小校验），再 font_load 显式挂到 tileview。
-- 与主题外壳的区别：复制写成协程，每帧只拷几块，首屏不用等 2～3 秒；拷完之前翻页用按键/触摸按钮，
-- 拷完之后新建的分页才换成可以左右滑的 tileview。主题外壳已经拷过的字体（xiaojia/ui_font.ttf）直接复用。
local M = {}

local SOURCES = { "/system/fonts/NotoSansSC-Regular-sub.ttf" }
local CHUNK = 16384

local function size_of(path)
  local f = io and io.open(path, "rb")
  if not f then return nil end
  local n = f:seek("end")
  f:close()
  return n
end

local function source()
  for _, p in ipairs(SOURCES) do
    local n = size_of(p)
    if n and n > 0 then return p, n end
  end
end

-- 已有可用的 DATA 副本就直接返回路径（大小与系统字体一致才算）
function M.existing(env, store)
  if not io or not io.open then return nil end
  local _, total = source()
  if not total then return nil end
  for _, p in ipairs({ store.join(store.base, "ui_font.ttf"), store.join(store.root, "xiaojia", "ui_font.ttf") }) do
    if size_of(p) == total then return p end
  end
end

-- 协程任务：分块复制，每块之间让出一帧。完成后调用 done(path)；失败调用 done(nil, 原因)
function M.copy_task(env, store, done)
  return function(task)
    if not io or not io.open then return done(nil, "没有 io 库") end
    local src_path, total = source()
    if not src_path then return done(nil, "找不到系统字体") end
    local dst = store.join(store.base, "ui_font.ttf")
    local tmp = dst .. ".part"
    store.ensure_dirs()
    local src = io.open(src_path, "rb")
    local out = src and io.open(tmp, "wb")
    if not out then if src then src:close() end; return done(nil, "不能写 " .. tmp) end
    local copied, t0, slice = 0, env.now(), env.now()
    while true do
      local chunk = src:read(CHUNK)
      if not chunk then break end
      if not out:write(chunk) then out:close(); src:close(); os.remove(tmp); return done(nil, "写入失败（DATA 满了？）") end
      copied = copied + #chunk
      -- 每帧最多占用约 6 ms，超出就把控制权还给主循环：界面照常响应
      if env.now() - slice >= 6 then task.yield(); slice = env.now() end
    end
    src:close(); out:close()
    if copied ~= total then os.remove(tmp); return done(nil, "复制不完整") end
    os.remove(dst)
    if not os.rename(tmp, dst) then return done(nil, "改名失败") end
    env.log(string.format("GD_FONT copied %s -> %s %d bytes in %d ms（后台协程）", src_path, dst, copied, env.now() - t0))
    return done(dst)
  end
end

-- 加载指定字号；同一路径按字号缓存
local cache = {}
function M.load(env, path, size)
  local key = path .. "@" .. size
  if cache[key] ~= nil then return cache[key] or nil end
  -- font_load 只认“相对 LVGL 数据根”或“数据根下的绝对路径”，各固件/仿真的数据根写法不同：依次尝试
  local okr, root = pcall(env.storage.get_root_dir)
  local tries = { path }
  if okr and root and path:sub(1, #root) == root then tries[#tries + 1] = path:sub(#root + 2) end
  tries[#tries + 1] = (path:gsub("^/+", ""))
  local last
  for _, p in ipairs(tries) do
    local ok, f = pcall(env.lvgl.font_load, p, { size = size })
    if ok and f then cache[key] = f; return f end
    last = f
  end
  cache[key] = false
  env.log("GD_FONT load failed " .. tostring(last))
  return nil
end

function M.reset() cache = {} end

return M
