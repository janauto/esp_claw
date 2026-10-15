-- 我的 · 花园：输入层（沿用主题外壳 core/input.lua 的实体键与 0.6 s 长按做法，已在样机验证）。
-- 把实体键、屏幕软键、触摸回调、仿真时间轴统一成一个事件队列，由主循环在 LVGL 回调之外处理
-- （在 LVGL 事件回调里直接删改界面对象有风险，所以触摸只“投递”，不当场执行）。
-- 逻辑键：
--   prev  VOLUME_DOWN  上一个
--   ok    MODE         确认；长按 0.6 s = 拿起 / 上下文操作
--   next  VOLUME_UP    下一个
--   back  SET          返回上一级；顶层退出
-- 事件种类：short（短按）、long（按住满 0.6 s 时立刻发出）、long_up（长按后松开）。
local M = {}

local KEYMAP = { MODE = "ok", VOLUME_UP = "next", VOLUME_DOWN = "prev", SET = "back" }
M.LONG_MS = 600 -- 板级 YAML 的长按是 2 s，太慢；只用按下/抬起事件自己计时

function M.new(env)
  local self = { queue = {}, button = nil, group = nil, held = {}, down_at = {}, autoplay = env.autoplay, auto_i = 1, t0 = env.now() }

  local function push(ev) self.queue[#self.queue + 1] = ev end
  function self.key(key, kind) push({ key = key, kind = kind or "short" }) end
  function self.post(fn) push({ fn = fn }) end -- 触摸回调：把要做的事排进主循环

  -- 实体键：板级按键组。仿真里 button 模块是桩（没有 open_group/on），自动跳过。
  local ok, button = pcall(require, "button")
  if ok and type(button) == "table" and type(button.open_group) == "function" and type(button.on) == "function" then
    local okg, group = pcall(button.open_group, "adc_button_group")
    if okg and group then
      self.button, self.group = button, group
      local function key_of(evt) return KEYMAP[(type(evt) == "table" and evt.label) or ""] end
      button.on(group, "press_down", function(evt) local k = key_of(evt); if k then self.down_at[k] = env.now() end end)
      button.on(group, "press_up", function(evt)
        local k = key_of(evt); if not k then return end
        if self.held[k] then self.held[k] = nil; push({ key = k, kind = "long_up" })
        elseif self.down_at[k] then push({ key = k, kind = "short" }) end
        self.down_at[k] = nil
      end)
      env.log("GD_INPUT keys=adc_button_group")
    end
  end
  if not self.group then env.log("GD_INPUT keys=none（仿真或没有按键组的板子）") end
  if self.autoplay then env.log("GD_INPUT autoplay steps=" .. #self.autoplay) end

  -- 屏幕软键 / 可长按的触摸对象：clicked = 短按，long_pressed = 长按。
  -- LVGL 9 在长按松开后仍会补发 clicked，这里记一笔，避免一次长按被当成“长按 + 短按”两次。
  local after_long = setmetatable({}, { __mode = "k" })
  function self.bind_soft(obj, key)
    if not obj then return end
    obj:on("long_pressed", function() after_long[obj] = true; self.held[key] = true; push({ key = key, kind = "long" }) end)
    obj:on("released", function() if self.held[key] then self.held[key] = nil; push({ key = key, kind = "long_up" }) end end)
    obj:on("clicked", function()
      if after_long[obj] then after_long[obj] = nil; return end
      push({ key = key, kind = "short" })
    end)
  end
  -- 普通可点对象：点按 / 长按各自投递一个函数
  function self.bind_tap(obj, on_tap, on_long)
    if not obj then return end
    if on_long then obj:on("long_pressed", function() after_long[obj] = true; push({ fn = on_long }) end) end
    obj:on("clicked", function()
      if after_long[obj] then after_long[obj] = nil; return end
      if on_tap then push({ fn = on_tap }) end
    end)
  end

  function self.poll(now)
    if self.button then pcall(self.button.dispatch) end
    for k, t in pairs(self.down_at) do
      if not self.held[k] and now - t >= M.LONG_MS then self.held[k] = true; push({ key = k, kind = "long" }) end
    end
    -- 仿真时间轴：{t, key, kind} 注入按键；{t, mark} 截图点；{t, cmd} 调试命令
    if self.autoplay then
      local s = self.autoplay[self.auto_i]
      while s and now - self.t0 >= (s.t or 0) do
        if s.key then push({ key = s.key, kind = s.kind or "short" }) end
        if s.mark then push({ mark = s.mark }) end
        if s.cmd then push({ cmd = s.cmd }) end
        -- 截图点：后续步骤整体顺延，给外部截图留出时间（默认 700 ms）
        if s.mark then self.t0 = self.t0 + (s.hold or 700) end
        self.auto_i = self.auto_i + 1
        s = self.autoplay[self.auto_i]
      end
    end
    local q = self.queue
    self.queue = {}
    return q
  end

  function self.close()
    if self.button and self.group then pcall(self.button.close, self.group) end
  end
  return self
end

return M
