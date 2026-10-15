-- 我的 · 花园：纯 Lua JSON 编解码。
-- 为什么不用固件的 json 模块：主题外壳实测工作台仿真里 json.encode 编码较大的表会写坏内存；
-- 另外纯 Lua 实现可以在宿主 Lua 里直接做单元测试，设备、仿真、宿主三处行为完全一致。
-- 编码时对象的键按字典序输出，同样的数据得到同样的文本（便于比较是否需要重写快照）。
local J = {}

J.null = setmetatable({}, { __tostring = function() return "null" end, __name = "json.null" })

local ESC = { ['"'] = '\\"', ["\\"] = "\\\\", ["\b"] = "\\b", ["\f"] = "\\f", ["\n"] = "\\n", ["\r"] = "\\r", ["\t"] = "\\t" }

local function enc_str(s)
  return '"' .. s:gsub('[%c"\\]', function(c) return ESC[c] or string.format("\\u%04x", c:byte()) end) .. '"'
end

local function is_array(t)
  local n = #t
  if n == 0 then return false end
  local count = 0
  for k in pairs(t) do
    if math.type(k) ~= "integer" or k < 1 or k > n then return false end
    count = count + 1
  end
  return count == n
end

local function enc(v, depth)
  if depth > 32 then error("json: 嵌套太深") end
  local t = type(v)
  if v == nil or v == J.null then return "null" end
  if t == "string" then return enc_str(v) end
  if t == "boolean" then return v and "true" or "false" end
  if t == "number" then
    if v ~= v or v == math.huge or v == -math.huge then return "null" end
    if math.type(v) == "integer" then return string.format("%d", v) end
    if v == math.floor(v) and math.abs(v) < 2 ^ 53 then return string.format("%d", math.floor(v)) end
    return string.format("%.14g", v)
  end
  if t ~= "table" then return "null" end -- 函数、userdata 等不可序列化的值写成 null
  if is_array(v) then
    local out = {}
    for i = 1, #v do out[i] = enc(v[i], depth + 1) end
    return "[" .. table.concat(out, ",") .. "]"
  end
  local keys = {}
  for k, x in pairs(v) do
    if type(k) == "string" and type(x) ~= "function" and type(x) ~= "userdata" and type(x) ~= "thread" then keys[#keys + 1] = k end
  end
  table.sort(keys)
  local out = {}
  for i, k in ipairs(keys) do out[i] = enc_str(k) .. ":" .. enc(v[k], depth + 1) end
  return "{" .. table.concat(out, ",") .. "}"
end

function J.encode(v) return enc(v, 0) end

---------------------------------------------------------------- 解码
local function utf8_of(cp)
  if cp < 0x80 then return string.char(cp) end
  return utf8.char(cp)
end

function J.decode(s)
  if type(s) ~= "string" then return nil, "json: 输入不是字符串" end
  local pos = 1
  local function fail(msg) error({ json = true, msg = msg .. " @" .. pos }, 0) end
  local function ws()
    pos = s:find("[^ \t\r\n]", pos) or (#s + 1)
  end
  local value
  local function str()
    pos = pos + 1
    local buf, i = {}, pos
    while true do
      local c = s:sub(i, i)
      if c == "" then fail("字符串没有结束") end
      if c == '"' then buf[#buf + 1] = s:sub(pos, i - 1); pos = i + 1; break end
      if c == "\\" then
        buf[#buf + 1] = s:sub(pos, i - 1)
        local e = s:sub(i + 1, i + 1)
        local map = { ['"'] = '"', ["\\"] = "\\", ["/"] = "/", b = "\b", f = "\f", n = "\n", r = "\r", t = "\t" }
        if map[e] then buf[#buf + 1] = map[e]; i = i + 2
        elseif e == "u" then
          local hex = s:sub(i + 2, i + 5)
          if not hex:match("^%x%x%x%x$") then fail("\\u 转义不完整") end
          local cp = tonumber(hex, 16)
          i = i + 6
          if cp >= 0xD800 and cp <= 0xDBFF and s:sub(i, i + 1) == "\\u" then -- 代理对
            local lo = tonumber(s:sub(i + 2, i + 5), 16)
            if lo and lo >= 0xDC00 and lo <= 0xDFFF then cp = 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00); i = i + 6 end
          end
          buf[#buf + 1] = utf8_of(cp)
        else fail("未知转义") end
        pos = i
      else
        i = i + 1
      end
    end
    return table.concat(buf)
  end
  local function num()
    local a, b = s:find("^-?%d+%.?%d*[eE]?[-+]?%d*", pos)
    if not a then fail("数字格式错误") end
    local lit = s:sub(a, b)
    pos = b + 1
    local n = math.tointeger(tonumber(lit)) or tonumber(lit)
    if n == nil then fail("数字格式错误") end
    return n
  end
  function value(depth)
    if depth > 64 then fail("嵌套太深") end
    ws()
    local c = s:sub(pos, pos)
    if c == "{" then
      pos = pos + 1
      local t = {}
      ws()
      if s:sub(pos, pos) == "}" then pos = pos + 1; return t end
      while true do
        ws()
        if s:sub(pos, pos) ~= '"' then fail("对象的键必须是字符串") end
        local k = str()
        ws()
        if s:sub(pos, pos) ~= ":" then fail("缺少冒号") end
        pos = pos + 1
        local v = value(depth + 1)
        if v ~= J.null then t[k] = v end
        ws()
        local d = s:sub(pos, pos)
        pos = pos + 1
        if d == "}" then return t end
        if d ~= "," then fail("对象里缺少逗号") end
      end
    elseif c == "[" then
      pos = pos + 1
      local t, n = {}, 0
      ws()
      if s:sub(pos, pos) == "]" then pos = pos + 1; return t end
      while true do
        local v = value(depth + 1)
        n = n + 1
        if v ~= J.null then t[n] = v end
        ws()
        local d = s:sub(pos, pos)
        pos = pos + 1
        if d == "]" then return t end
        if d ~= "," then fail("数组里缺少逗号") end
      end
    elseif c == '"' then return str()
    elseif c == "t" and s:sub(pos, pos + 3) == "true" then pos = pos + 4; return true
    elseif c == "f" and s:sub(pos, pos + 4) == "false" then pos = pos + 5; return false
    elseif c == "n" and s:sub(pos, pos + 3) == "null" then pos = pos + 4; return J.null
    elseif c:match("[-%d]") then return num()
    end
    fail("意外的字符 " .. (c == "" and "（文件结束）" or c))
  end
  local ok, res = pcall(function()
    local v = value(0)
    ws()
    if pos <= #s then fail("多余的内容") end
    return v
  end)
  if ok then return res == J.null and nil or res end
  if type(res) == "table" and res.json then return nil, "json: " .. res.msg end
  return nil, "json: " .. tostring(res)
end

return J
