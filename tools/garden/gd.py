#!/usr/bin/env python3
"""「我的」花园（my_garden）调试命令行：检查 → 宿主单测 → 480×800 同源仿真 → 场景回归。

  python3 tools/garden/gd.py check                 语法（宿主 Lua 5.5，代码保持 5.4 兼容）+ 设备字形逐字检查 + 体积
  python3 tools/garden/gd.py unit                  宿主 Lua 跑纯逻辑单测（沙箱、JSON、持久化、布局移动、小磁）
  python3 tools/garden/gd.py plant-check <植物.lua>  第三方 / AI 植物静态检查 + 宿主沙箱试跑（不需要仿真）
  ~/.cache/backclip-web/venv/bin/python -I tools/garden/gd.py sim --play keys.json [--touch touch.json] [--page garden]
  ~/.cache/backclip-web/venv/bin/python -I tools/garden/gd.py scen tools/garden/scenarios <输出目录> [--only 名字]

仿真：固件仓库 tools/lua_lvgl_web_sim 的 WebAssembly 构建（真实 LVGL 9.5 + Lua 5.4 C 绑定），见 simrun.py。
花园脚本挂到与设备相同的路径 /system/skills/my_garden/scripts/，DATA 根在仿真里是 /storage。
按键时间轴写成 autoplay.json、启动参数写成 sim_args.json，只挂进仿真，不进固件镜像。
"""
import argparse
import json
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
SKILL = REPO / "application/edge_agent/fatfs_image/system/skills/my_garden"
SCRIPTS = SKILL / "scripts"
SYS_PATH = "/system/skills/my_garden/scripts"
DATA_ROOT = "/storage"
HOST_LUA = pathlib.Path(os.environ.get("GD_HOST_LUA", pathlib.Path.home() / ".cache/backclip-web/host-lua-5.5.0/lua"))
DEVICE_FONT = REPO / "application/edge_agent/fatfs_image/system/fonts/NotoSansSC-Regular-sub.ttf"
SIM_FONT = REPO / "tools/lua_lvgl_web_sim/sim_assets/fonts/fusion-pixel-12px.ttf"
ENTRY_LIMIT = 64 * 1024  # cap_lua：入口脚本不超过 64 KB（require 进来的模块不受此限）
# 用户明确要求不用的字符（即使设备字库里有）：° → · ● … 以及设备字库没有的 ! ^
POLICY_BANNED = set("°→·●…!^←◀▶•")
LUA_STR = re.compile(r'"((?:[^"\\\n]|\\.)*)"|\'((?:[^\'\\\n]|\\.)*)\'')


def lua_files(root=SCRIPTS):
    return sorted(p for p in root.rglob("*.lua"))


def strip_comment(line):
    """去掉行尾注释（不在字符串里的 --）。只用于字形检查，粗略即可。"""
    out, q, i = [], None, 0
    while i < len(line):
        c = line[i]
        if q:
            out.append(c)
            if c == "\\" and i + 1 < len(line):
                out.append(line[i + 1]); i += 2; continue
            if c == q:
                q = None
        else:
            if line.startswith("--", i):
                break
            if c in "\"'":
                q = c
            out.append(c)
        i += 1
    return "".join(out)


def font_cmap(path):
    from fontTools.ttLib import TTFont
    return set(TTFont(str(path)).getBestCmap().keys())


def glyph_scan(files, base):
    """扫描 Lua 字符串字面量里的字符：设备字库缺的、按约定禁用的都算错误；仿真像素字体缺的单独列出。"""
    try:
        cmap = font_cmap(DEVICE_FONT)
    except ImportError:
        print("（跳过字形检查：当前 Python 没有 fontTools，请用系统 python3 运行 check）")
        return 0, {}
    sim = font_cmap(SIM_FONT) if SIM_FONT.exists() else None
    missing, sim_missing = {}, set()
    for p in files:
        for no, line in enumerate(p.read_text("utf-8").splitlines(), 1):
            if "xj:glyph-ok" in line or "gd:glyph-ok" in line:
                continue
            for m in LUA_STR.finditer(strip_comment(line)):
                lit = m.group(1) if m.group(1) is not None else (m.group(2) or "")
                cjk = re.search(r"[一-鿿]", lit)
                if lit.startswith("^") or "[^" in lit or (re.search(r"%[a-zA-Z.%\-+*?()\[\]]", lit) and not cjk):
                    continue  # Lua 匹配模式 / 纯格式串，不是界面文字（带中文的格式串照样检查）
                for ch in lit:
                    if ord(ch) < 32:
                        continue
                    if ord(ch) not in cmap or ch in POLICY_BANNED:
                        missing.setdefault(ch, []).append(f"{p.relative_to(base)}:{no}")
                    elif sim is not None and ord(ch) not in sim:
                        sim_missing.add(ch)
    return missing, sim_missing


def cmd_check(_a):
    bad, total = 0, 0
    for p in lua_files():
        size = p.stat().st_size
        total += size
        r = subprocess.run([str(HOST_LUA), "-", str(p)], input="assert(loadfile(arg[1]))", capture_output=True, text=True)
        st = "ok" if r.returncode == 0 else "SYNTAX"
        warn = "  （入口超过 64 KB）" if p.name == "main.lua" and p.parent == SCRIPTS and size > ENTRY_LIMIT else ""
        print(f"{st:6} {size:6d} B  {p.relative_to(SCRIPTS)}{warn}")
        if r.returncode != 0:
            bad += 1
            print("       " + (r.stderr or r.stdout).strip().splitlines()[-1])
    print(f"共 {len(lua_files())} 个模块，{total} 字节；语法错误 {bad} 个")
    missing, sim_missing = glyph_scan(lua_files(), SCRIPTS)
    for ch, where in sorted(missing.items()):
        why = "约定禁用" if ch in POLICY_BANNED else "设备字库没有"
        print(f"缺字  {ch} (U+{ord(ch):04X}，{why})  {', '.join(where[:4])}{' ...' if len(where) > 4 else ''}")
    print(f"设备字体 {DEVICE_FONT.name}：界面字符串里设备不能显示 / 约定禁用的字符 {len(missing)} 个")
    if sim_missing:
        print(f"仅仿真缺字（设备正常，仿真截图里会显示方框）：{' '.join(sorted(sim_missing))}")
    skill_md = SKILL / "SKILL.md"
    if skill_md.exists():
        m = re.match(r"^---\n(.*?)\n---\n", skill_md.read_text("utf-8"), re.S)
        try:
            meta = json.loads(m.group(1)) if m else None
        except json.JSONDecodeError as e:
            meta = None
            print("SKILL.md frontmatter 不是合法 JSON：", e)
        ok = bool(meta) and meta.get("name") == "my_garden" and meta.get("description") and isinstance(meta.get("metadata"), dict) \
            and meta.get("execution", {}).get("entry") == "scripts/main.lua"
        print("SKILL.md：" + ("frontmatter 符合 claw_skill 解析要求" if ok else "frontmatter 不符合要求"))
        bad += 0 if ok else 1
    return 1 if bad or missing else 0


# ---------------------------------------------------------------- 宿主单测
def cmd_unit(_a):
    tests = sorted((HERE / "tests").glob("test_*.lua"))
    failed = 0
    for t in tests:
        r = subprocess.run([str(HOST_LUA), str(t), str(SCRIPTS)], capture_output=True, text=True, cwd=str(HERE / "tests"))
        out = (r.stdout + r.stderr).strip()
        print(out)
        if r.returncode != 0:
            failed += 1
    print(f"宿主单测：{len(tests) - failed}/{len(tests)} 个文件通过（{HOST_LUA}）")
    return 1 if failed else 0


# ---------------------------------------------------------------- 植物检查
FORBIDDEN = [(r"\brequire\s*\(", "require"), (r"\bio\.", "io"), (r"\bos\.(execute|remove|rename|exit|getenv|tmpname)", "os 系统函数"),
             (r"\bload\s*\(", "load"), (r"\bloadstring\b", "loadstring"), (r"\bdofile\b", "dofile"), (r"\bdebug\.", "debug"),
             (r"\bpackage\.", "package"), (r"\bstorage\b", "storage"), (r"\bboard_manager\b", "board_manager"), (r"\blvgl\b", "lvgl")]


def cmd_plant_check(a):
    path = pathlib.Path(a.file).resolve()
    src = path.read_text("utf-8")
    code = "\n".join(strip_comment(l) for l in src.splitlines())
    ok = True
    hits = sorted({name for pat, name in FORBIDDEN if re.search(pat, code)})
    print(("不通过 " if hits else "通过  ") + "静态规则（不碰系统能力）" + (f"：命中 {', '.join(hits)}（沙箱里这些本来就不存在，调用会报错枯萎）" if hits else ""))
    ok &= not hits
    stem_ok = re.fullmatch(r"[a-z][a-z0-9_]*", path.stem) is not None
    print(("通过  " if stem_ok else "不通过 ") + f"文件名 {path.name}（小写字母、数字、下划线）")
    ok &= stem_ok
    size_ok = len(src.encode()) <= 16384
    print(("通过  " if size_ok else "不通过 ") + f"体积 {len(src.encode())} B（≤ 16 KB）")
    ok &= size_ok
    missing, _ = glyph_scan([path], path.parent)
    print(("不通过 " if missing else "通过  ") + "设备字体能显示所有文字" + (f"：{''.join(sorted(missing))}" if missing else ""))
    ok &= not missing
    r = subprocess.run([str(HOST_LUA), str(HERE / "tests" / "plant_probe.lua"), str(SCRIPTS), str(path)], capture_output=True, text=True,
                       cwd=str(HERE / "tests"))
    print((r.stdout + r.stderr).strip())
    ok &= r.returncode == 0
    print("结论：" + ("通过，可以放进 <DATA>/garden/plants/" if ok else "未通过"))
    return 0 if ok else 1


# ---------------------------------------------------------------- 仿真
def build_files(play=None, sim_args=None, data_files=None):
    files = [{"path": f"{SYS_PATH}/{p.relative_to(SCRIPTS).as_posix()}", "text": p.read_text("utf-8")} for p in lua_files()]
    if sim_args is not None:
        files.append({"path": f"{SYS_PATH}/sim_args.json", "text": json.dumps(sim_args, ensure_ascii=False)})
    if play is not None:
        files.append({"path": f"{SYS_PATH}/autoplay.json", "text": json.dumps(play, ensure_ascii=False)})
    for rel, text in (data_files or {}).items():
        files.append({"path": f"{DATA_ROOT}/garden/{rel}", "text": text})
    return files


def load_data_dir(d):
    out = {}
    if not d:
        return out
    d = pathlib.Path(d)
    for p in sorted(d.rglob("*")):
        if p.is_file():
            out[p.relative_to(d).as_posix()] = p.read_text("utf-8")
    return out


def run_sim(keys, touches, out, page="mine", seconds=None, data_files=None, extra_args=None, record_ms=0):
    sys.path.insert(0, str(HERE))
    import simrun
    keys = list(keys or [])
    if not any(k.get("cmd") == "quit" for k in keys):
        last = max([k.get("t", 0) for k in keys] + [t.get("t", 0) for t in (touches or [])] + [0])
        keys.append({"t": last + 1500, "cmd": "quit"})
    marks = sum(1 for k in keys if "mark" in k)
    last = max([k.get("t", 0) for k in keys] + [t.get("t", 0) for t in (touches or [])] + [0])
    secs = seconds or (last / 1000 + marks * 0.7 + 8)
    args = {"page": page, "max_ms": int((secs + 4) * 1000)}
    args.update(extra_args or {})
    files = build_files(keys, args, data_files)
    return simrun.run(files, f"{SYS_PATH}/main.lua", out, seconds=secs, touches=touches, record_ms=record_ms)


def cmd_sim(a):
    keys = json.loads(pathlib.Path(a.play).read_text("utf-8")) if a.play else []
    touches = json.loads(pathlib.Path(a.touch).read_text("utf-8")) if a.touch else []
    out = pathlib.Path(a.out or "/tmp/garden-sim")
    s = run_sim(keys, touches, out, page=a.page, seconds=a.seconds, data_files=load_data_dir(a.data), record_ms=a.record)
    print(json.dumps(s, ensure_ascii=False, indent=2))
    return 1 if s["errors"] or s["fatal"] else 0


def cmd_scen(a):
    src, dst = pathlib.Path(a.dir), pathlib.Path(a.out)
    dst.mkdir(parents=True, exist_ok=True)
    files = [src] if src.is_file() else sorted(src.glob("*.json"))
    lines, failed = ["# 场景运行结果（仿真）", ""], []
    for f in files:
        if a.only and a.only not in f.stem:
            continue
        sc = json.loads(f.read_text("utf-8"))
        data = load_data_dir(HERE / sc["data"]) if sc.get("data") else {}
        touches = []
        for t in sc.get("touch", []):
            if t.get("do") == "write" and "file" in t:  # 运行中写入 DATA：内容取自 fixtures 文件
                t = dict(t, text=(HERE / t["file"]).read_text("utf-8"))
            touches.append(t)
        out = dst / f.stem
        s = run_sim(sc.get("keys", []), touches, out, page=sc.get("page", "mine"), data_files=data, extra_args=sc.get("args"),
                    record_ms=sc.get("record_ms", 0))
        log = (out / "log.txt").read_text("utf-8") if (out / "log.txt").exists() else ""
        states = [l.split("GD_STATE ", 1)[1] for l in log.splitlines() if "GD_STATE" in l]
        errs = [l for l in log.splitlines() if re.search(r"GD_ERR|GD_FATAL|\[err\]|lua error", l, re.I)]
        expect_errs = sc.get("expect_errors", [])  # 故意报错的植物：允许出现的 GD_ERR 片段
        real_errs = [e for e in errs if not any(x in e for x in expect_errs)]
        checks = []
        for c in sc.get("checks", []):  # 日志断言：{"log": 正则, "min": 次数}
            n = len(re.findall(c["log"], log))
            okc = n >= c.get("min", 0 if "max" in c else 1) and n <= c.get("max", 10 ** 9)
            checks.append((c["log"], n, okc))
        perf = [l.split("GD_PERF ", 1)[1] for l in log.splitlines() if "GD_PERF" in l]
        pages = [l.split("GD_PAGE ", 1)[1] for l in log.splitlines() if "GD_PAGE" in l]
        ok = not real_errs and all(c[2] for c in checks) and s.get("done")
        if not ok:
            failed.append(f.stem)
        lines += [f"## {f.stem}：{'通过' if ok else '未通过'}", "", f"- 说明：{sc.get('desc', '')}",
                  f"- 截图：{', '.join(s.get('shots', [])) or '无'}（{out}）", f"- 非预期报错：{'; '.join(real_errs[:5]) or '无'}",
                  f"- 预期内的植物报错：{len(errs) - len(real_errs)} 条", f"- GD_DONE：{s.get('done')}"]
        for pat, n, okc in checks:
            lines.append(f"- 断言 `{pat}`：{n} 次 {'通过' if okc else '不通过'}")
        lines += ["- 页面构建：", "```", *pages[-20:], "```", "- 性能：", "```", *perf, "```", "- 状态序列（末 25 条）：", "```", *states[-25:], "```", ""]
        print(f"{f.stem}: {'通过' if ok else '未通过'}  状态 {len(states)}  截图 {len(s.get('shots', []))}  非预期报错 {len(real_errs)}", flush=True)
    (dst / "RESULTS.md").write_text("\n".join(lines), "utf-8")
    print("汇总：" + str(dst / "RESULTS.md"))
    return 1 if failed else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("check")
    sub.add_parser("unit")
    pc = sub.add_parser("plant-check")
    pc.add_argument("file")
    s = sub.add_parser("sim")
    s.add_argument("--play")
    s.add_argument("--touch")
    s.add_argument("--out")
    s.add_argument("--page", default="mine")
    s.add_argument("--data", help="挂到 /storage/garden/ 的目录（例如 fixtures/data_xxx）")
    s.add_argument("--seconds", type=float)
    s.add_argument("--record", type=int, default=0)
    sc = sub.add_parser("scen")
    sc.add_argument("dir")
    sc.add_argument("out")
    sc.add_argument("--only", default="")
    a = ap.parse_args()
    fn = {"check": cmd_check, "unit": cmd_unit, "plant-check": cmd_plant_check, "sim": cmd_sim, "scen": cmd_scen}[a.cmd]
    sys.exit(fn(a))


if __name__ == "__main__":
    main()
