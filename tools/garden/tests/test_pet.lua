-- 小磁：每日计数与上限、换日清零、陪伴天数、心情文字、气泡优先级、时间衰减、没对时不乱算
local H = dofile("helper.lua")
local root = H.tmpdir()
local env = H.env(root, { unix = os.time({ year = 2026, month = 10, day = 15, hour = 15 }) })
local app = H.app(env)
local pet = app.pet

H.case("首次运行记下第一天，陪伴 1 天", function()
  H.eq(pet.data.first, "2026-10-15"); H.eq(pet.days(), 1)
end)
H.case("喝水：计数、加心情，超过 8 杯不再加心情", function()
  local m0 = pet.mood()
  pet.cheer("water")
  H.eq(pet.today().cups, 1); H.ok(pet.mood() > m0)
  for _ = 1, 10 do pet.cheer("water") end
  local m1 = pet.mood()
  pet.cheer("water")
  H.eq(pet.mood(), m1, "超过上限不再加")
  pet.cheer("unwater")
  H.eq(pet.today().cups, 11)
end)
H.case("刚互动过：表情开心，气泡说刚发生的事", function()
  pet.cheer("care")
  H.eq(pet.face(env.clock.ms), "happy")
  H.ok(pet.say(env.clock.ms, {}):find("花园"), pet.say(env.clock.ms, {}))
end)
H.case("换日：今天的计数清零，陪伴天数 +1", function()
  env.clock.unix = env.clock.unix + 86400
  env.clock.ms = env.clock.ms + 60000
  pet.tick()
  H.eq(pet.today().cups, 0); H.eq(pet.days(), 2)
end)
H.case("下午喝水少：气泡会提醒（按时段轮换）", function()
  local seen = false
  for i = 0, 6 do if pet.say(env.clock.ms + 20000 + i * 8000, { thirsty = 2 }):find("才喝了") then seen = true end end
  H.ok(seen)
end)
H.case("深夜：困了", function()
  env.clock.unix = os.time({ year = 2026, month = 10, day = 16, hour = 23, min = 30 })
  env.clock.ms = env.clock.ms + 60000
  H.eq(pet.face(env.clock.ms), "sleepy")
end)
H.case("长时间没打开：心情回到 55 附近，夜里能量恢复", function()
  pet.data.mood, pet.data.energy, pet.data.t = 100, 30, env.clock.unix
  env.clock.unix = env.clock.unix + 6 * 3600
  pet.tick()
  H.ok(pet.mood() < 100 and pet.mood() > 55, tostring(pet.mood()))
  H.ok(pet.energy() > 30, tostring(pet.energy()))
end)
H.case("心情文字分档，快照字段齐全（契约 1.2）", function()
  pet.data.mood = 85; H.eq(pet.mood_text(), "开心")
  pet.data.mood = 45; H.eq(pet.mood_text(), "平静")
  local s = pet.snapshot({ id = "house", name = "小家" }, 6, "欢迎回家")
  for _, k in ipairs({ "v", "pet_name", "mood", "mood_level", "energy", "days", "theme", "theme_name", "say", "plants", "updated" }) do H.ok(s[k] ~= nil, k) end
  H.eq(s.mood_level, 5)
end)
H.case("设备没对时（1970 年）：不换日、不结算、陪伴天数沿用", function()
  local days = pet.days()
  env.clock.unix = 100
  pet.tick()
  H.eq(pet.days(), days)
end)
os.execute("rm -rf '" .. root .. "'")
H.finish("小磁")
