-- 我的 · 花园：协程调度器。
-- 动画、后台字体准备、小磁走路、浇水水滴这些“分很多帧完成”的事，都写成一段顺序代码的协程：
--   sched.spawn("walk", function(task) for ... do move(); task.sleep(60) end end)
-- 主循环每帧调用 step(now, budget_ms)：只恢复到点的任务；总耗时超过预算就留到下一帧，界面不卡。
-- 任务出错只结束它自己（记 GD_ERR），不影响主循环。
local M = {}

function M.new(env)
  local S = { tasks = {}, order = {} }

  -- fn(task, ...)：task.sleep(ms) 让出并在 ms 后继续；task.yield() 让到下一帧；task.alive 外部可查
  function S.spawn(name, fn, ...)
    S.kill(name) -- 同名任务只保留最新的一个（例如连续换焦点时，小磁只朝最新的房间走）
    local task = { name = name, wake = 0, alive = true }
    function task.sleep(ms) coroutine.yield(ms or 0) end
    function task.yield() coroutine.yield(0) end
    local args = table.pack(...)
    task.co = coroutine.create(function() return fn(task, table.unpack(args, 1, args.n)) end)
    S.tasks[name] = task
    local listed = false
    for _, n in ipairs(S.order) do if n == name then listed = true; break end end
    if not listed then S.order[#S.order + 1] = name end
    return task
  end

  function S.kill(name)
    local t = S.tasks[name]
    if t then t.alive = false; S.tasks[name] = nil end
  end

  function S.alive(name) return S.tasks[name] ~= nil end

  function S.step(now, budget_ms)
    local t0 = env.now()
    local keep = {}
    for _, name in ipairs(S.order) do
      local t = S.tasks[name]
      if t and t.alive then
        keep[#keep + 1] = name
        if now >= t.wake and env.now() - t0 <= (budget_ms or 8) then
          local ok, ms = coroutine.resume(t.co)
          if not ok then
            env.log("GD_ERR task " .. name .. " " .. tostring(ms))
            t.alive = false; S.tasks[name] = nil
          elseif coroutine.status(t.co) == "dead" then
            t.alive = false; S.tasks[name] = nil
          else
            t.wake = now + (tonumber(ms) or 0)
          end
        end
      end
    end
    -- 清理已结束的任务名（保持启动顺序，先启动的先执行）
    local order = {}
    for _, name in ipairs(keep) do if S.tasks[name] then order[#order + 1] = name end end
    S.order = order
  end

  return S
end

return M
