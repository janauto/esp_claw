-- 我的 · 花园：用户 / AI 代码的沙箱。
-- 植物（plants/*.lua）和自定义主题（themes/*.lua）都由这里装载与调用：
--   1. load(src, 名字, "t", env)：只收文本代码（拒绝预编译字节码），全局环境是专门造的白名单 _ENV，
--      没有 io / os.execute / require / load / debug / package / 固件模块，碰不到文件与系统能力；
--   2. guard()：每次调用都放进一个新协程，用 debug.sethook 按指令数计预算（死循环会被打断），
--      再用 coroutine.resume 的错误捕获兜底；出错只返回错误摘要，由调用方把这一块标成“枯萎”；
--   3. 植物自己开的协程也挂同一个预算钩子；沙箱里的 pcall 不能吞掉“预算用尽”，防止 pcall 套死循环逃逸；
--   4. 每次调用前后对比 Lua 内存，单次暴涨（例如 string.rep 造超大字符串）同样判为失败。
local M = {}

M.BUDGET = setmetatable({}, { __tostring = function() return "运行超时：超出指令预算" end }) -- 唯一的哨兵错误对象
M.STEP = 1000           -- 每 1000 条指令检查一次预算
M.MEM_LIMIT_KB = 1024   -- 单次调用允许新增的内存

local spent, limit = 0, math.huge
local function hook()
  spent = spent + M.STEP
  if spent > limit then error(M.BUDGET, 0) end
end
local function arm(co) debug.sethook(co, hook, "", M.STEP); return co end

local function copy(src, names)
  local t = {}
  for _, k in ipairs(names) do t[k] = src[k] end
  return t
end

-- 白名单环境。每个植物 / 主题各自一份，彼此的全局变量互不可见。
function M.env(extra)
  local E = {
    math = copy(math, { "abs", "ceil", "floor", "max", "min", "fmod", "huge", "pi", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan",
      "exp", "log", "random", "tointeger", "type", "modf", "ult", "maxinteger", "mininteger" }),
    string = copy(string, { "byte", "char", "find", "format", "gmatch", "gsub", "len", "lower", "match", "rep", "reverse", "sub", "upper" }),
    table = copy(table, { "concat", "insert", "remove", "sort", "unpack", "pack", "move" }),
    utf8 = copy(utf8, { "char", "charpattern", "codes", "codepoint", "len", "offset" }),
    -- 时间只读：os.time / os.date / os.clock / os.difftime；没有 execute、remove、rename、getenv、exit
    os = { time = os.time, date = function(f, t) return os.date(f, t) end, clock = os.clock, difftime = os.difftime },
    pairs = pairs, ipairs = ipairs, next = next, select = select, type = type, tostring = tostring, tonumber = tonumber,
    error = error, assert = assert, rawequal = rawequal, rawlen = rawlen, rawget = rawget, rawset = rawset,
    _VERSION = _VERSION,
  }
  -- 元表只对表开放；拿不到字符串的共享元表，也就改不了宿主的字符串方法
  E.setmetatable = function(t, mt)
    if type(t) ~= "table" then error("setmetatable 只能用于表", 2) end
    return setmetatable(t, mt)
  end
  E.getmetatable = function(t)
    if type(t) ~= "table" then return nil end
    return getmetatable(t)
  end
  -- pcall / xpcall：照常捕获错误，但“预算用尽”必须继续往外抛
  E.pcall = function(f, ...)
    local r = table.pack(pcall(f, ...))
    if not r[1] and r[2] == M.BUDGET then error(M.BUDGET, 0) end
    return table.unpack(r, 1, r.n)
  end
  E.xpcall = function(f, h, ...)
    local r = table.pack(xpcall(f, function(e) if e == M.BUDGET then return e end return h(e) end, ...))
    if not r[1] and r[2] == M.BUDGET then error(M.BUDGET, 0) end
    return table.unpack(r, 1, r.n)
  end
  -- 协程：植物可以用协程写动画，新协程同样挂上预算钩子
  E.coroutine = {
    create = function(f) return arm(coroutine.create(f)) end,
    wrap = function(f)
      local co = arm(coroutine.create(f))
      return function(...)
        local r = table.pack(coroutine.resume(co, ...))
        if not r[1] then error(r[2], 0) end
        return table.unpack(r, 2, r.n)
      end
    end,
    resume = function(co, ...)
      local r = table.pack(coroutine.resume(co, ...))
      if not r[1] and r[2] == M.BUDGET then error(M.BUDGET, 0) end
      return table.unpack(r, 1, r.n)
    end,
    yield = coroutine.yield, status = coroutine.status, isyieldable = coroutine.isyieldable, running = coroutine.running,
  }
  for k, v in pairs(extra or {}) do E[k] = v end
  E._G = E
  return E
end

-- 错误摘要：把 [string "plants/x.lua"]:12: msg 变成 “第 12 行：msg”，只留一行，方便在小地块上显示
function M.summary(err)
  if err == M.BUDGET then return tostring(M.BUDGET) end
  local s = tostring(err or "未知错误")
  s = s:gsub("^%[string \"[^\"]*\"%]:(%d+):%s*", "第 %1 行：")
  s = s:gsub("^[^:\n]+%.lua:(%d+):%s*", "第 %1 行：")
  s = s:gsub("\n.*", "")
  if #s > 160 then s = s:sub(1, 160) end
  return s
end

-- 受控调用：在新协程里执行 fn(...)，返回 true, 结果... 或 false, 错误摘要, 原始错误
function M.guard(fn, budget, ...)
  if type(fn) ~= "function" then return false, "不是函数" end
  local save_spent, save_limit = spent, limit
  spent, limit = 0, budget or 200000
  local kb0 = collectgarbage("count")
  local co = arm(coroutine.create(fn))
  local r = table.pack(coroutine.resume(co, ...))
  local grown = collectgarbage("count") - kb0
  spent, limit = save_spent, save_limit
  if not r[1] then return false, M.summary(r[2]), r[2] end
  if coroutine.status(co) ~= "dead" then return false, "回调里不能直接 yield（请在自己的协程里使用）" end
  if grown > M.MEM_LIMIT_KB then
    collectgarbage("collect")
    return false, string.format("单次占用内存过多（%d KB）", math.floor(grown))
  end
  return true, table.unpack(r, 2, r.n)
end

-- 装载一段源码为模块：返回 module 或 nil, 错误摘要
function M.load(src, chunkname, env, budget)
  if type(src) ~= "string" or #src == 0 then return nil, "文件是空的" end
  if src:byte(1) == 27 then return nil, "不接受预编译字节码" end
  local chunk, err = load(src, "=" .. chunkname, "t", env)
  if not chunk then return nil, M.summary(err) end
  local ok, mod = M.guard(chunk, budget or 500000)
  if not ok then return nil, mod end
  if type(mod) ~= "table" then return nil, "文件最后要 return 一个表" end
  return mod
end

return M
