-- 我的 · 花园：持久化。所有写入都在 <DATA>/garden/ 下（SYSTEM 分区只读）：
--   garden.json    布局、植物状态、小磁、设置（契约 1.1/1.2）
--   snapshot.json  给原生首页「我的」小组件的快照（契约 1.2）
--   plants/*.lua   用户 / AI 植物；themes/*.lua 自定义主题
-- 写法：先写 .tmp，再删旧文件，再 rename（FAT 不能覆盖式 rename）。读取时如果正式文件缺失或损坏，
-- 回退到 .tmp（覆盖过程中被强制停止的情况），再不行就用默认值并把坏文件另存为 .bad，不悄悄丢数据。
local json = require("core.json")

local M = {}

function M.new(env)
  local st = env.storage
  local self = {}
  local okr, r = pcall(st.get_root_dir)
  self.root = (okr and type(r) == "string" and r ~= "") and r or "/sdcard"
  local function join(...) return st.join_path(...) end
  self.base = join(self.root, "garden")
  self.paths = {
    garden = join(self.base, "garden.json"),
    snapshot = join(self.base, "snapshot.json"),
    plants = join(self.base, "plants"),
    themes = join(self.base, "themes"),
  }
  self.join = join

  local function exists(p) local ok, v = pcall(st.exists, p); return ok and v end
  local function mkdir(p) if not exists(p) then pcall(st.mkdir, p) end end
  function self.ensure_dirs()
    mkdir(self.base); mkdir(self.paths.plants); mkdir(self.paths.themes)
  end

  function self.read(p)
    local ok, v = pcall(st.read_file, p)
    if ok and type(v) == "string" then return v end
    return nil
  end

  -- 原子写：.tmp → 删旧 → rename；返回 true 或 false, 原因
  function self.write_atomic(p, text)
    local tmp = p .. ".tmp"
    local ok, err = pcall(st.write_file, tmp, text)
    if not ok then return false, "写入失败：" .. tostring(err) end
    if self.read(tmp) ~= text then return false, "写入后回读不一致（DATA 满了？）" end
    if exists(p) then pcall(st.remove, p) end
    local okm, errm = pcall(st.rename, tmp, p)
    if not okm then return false, "改名失败：" .. tostring(errm) end
    return true
  end

  function self.read_json(p)
    local raw = self.read(p)
    if not raw or #raw == 0 then return nil, "missing" end
    local v, err = json.decode(raw)
    if type(v) ~= "table" then return nil, err or "not object", raw end
    return v
  end

  -- 读取 garden.json：返回 数据表, 状态（new / ok / recovered / reset）
  function self.load_garden()
    local v, err, raw = self.read_json(self.paths.garden)
    if v then return v, "ok" end
    local t = self.read_json(self.paths.garden .. ".tmp")
    if t then env.log("GD_WARN garden.json 不可用（" .. tostring(err) .. "），用 .tmp 恢复"); return t, "recovered" end
    if err ~= "missing" then
      env.log("GD_WARN garden.json 损坏（" .. tostring(err) .. "），另存为 .bad 后重建")
      if raw then pcall(st.write_file, self.paths.garden .. ".bad", raw) end
      return nil, "reset"
    end
    return nil, "new"
  end

  function self.save_garden(data)
    self.ensure_dirs()
    return self.write_atomic(self.paths.garden, json.encode(data))
  end

  -- 列目录：返回按名字排序的 {name, path, size, mtime}（只要指定后缀）
  function self.list(dir, suffix)
    local ok, list = pcall(st.listdir, dir)
    local out = {}
    if not ok or type(list) ~= "table" then return out end
    for _, e in ipairs(list) do
      local name = type(e) == "table" and e.name or e
      if type(name) == "string" and (not suffix or name:sub(-#suffix) == suffix) and (type(e) ~= "table" or e.type ~= "dir") then
        local size, mtime = type(e) == "table" and e.size or nil, type(e) == "table" and e.mtime or nil
        if size == nil then -- 仿真的 listdir 不给大小：补一次 stat
          local oks, s = pcall(st.stat, join(dir, name))
          if oks and type(s) == "table" then size, mtime = s.size, s.mtime end
        end
        out[#out + 1] = { name = name, path = join(dir, name), size = size, mtime = mtime }
      end
    end
    table.sort(out, function(a, b) return a.name < b.name end)
    return out
  end

  return self
end

return M
