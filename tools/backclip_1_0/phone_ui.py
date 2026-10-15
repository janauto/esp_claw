#!/usr/bin/env python3
"""手机 App 自动操作小工具（adb + uiautomator），用于 1.0 实机验收。

  phone_ui.py dump                 列出可见文字/描述及坐标
  phone_ui.py tap <文字>           点第一个包含该文字的节点中心
  phone_ui.py tapxy <x> <y>        点坐标（手机物理像素）
  phone_ui.py swipe <x1> <y1> <x2> <y2> [ms]
  phone_ui.py shot <png>           截主屏（这台手机有背屏，需指定主屏 ID）
  phone_ui.py wait <文字> [秒]     等某段文字出现
  phone_ui.py connect              设备页：已连就跳过，否则扫描并连接背夹
  phone_ui.py tab <名称>           切到顶部标签页（设备/创作/Lua/预览/应用/日志…）
  phone_ui.py tapexact <文字>      只点文字完全相同且唯一的节点
  phone_ui.py field <当前值> <新值> 改输入框（ASCII）
  phone_ui.py appact <id> <按钮>   应用页：对某个应用点 在背夹上打开/重命名/详情/卸载
  phone_ui.py install <id> <名称>  Lua 页载入贪吃蛇示例 → 应用页“安装当前 Lua”
"""
import re
import subprocess
import sys
import time

PKG = "com.pisen.aiclip"


def adb(*args, **kw):
    # 这台手机有背屏（display 1），不指定时注入的点击偶尔落到背屏上被吞掉：一律打到主屏 0
    if args[:2] == ("shell", "input") and "-d" not in args:
        args = ("shell", "input", "-d", "0") + tuple(args[2:])
    return subprocess.run(["adb", *args], capture_output=True, text=kw.get("text", True), timeout=kw.get("timeout", 60))


def main_display():
    out = adb("shell", "dumpsys", "SurfaceFlinger", "--display-id").stdout
    m = re.search(r"Display (\d+)", out)
    return m.group(1) if m else None


def dump():
    adb("shell", "uiautomator", "dump", "/sdcard/ui.xml", timeout=30)
    xml = adb("exec-out", "cat", "/sdcard/ui.xml").stdout
    nodes = []
    for m in re.finditer(r'<node [^>]*?>', xml):
        tag = m.group(0)
        text = re.search(r' text="([^"]*)"', tag)
        desc = re.search(r' content-desc="([^"]*)"', tag)
        bounds = re.search(r' bounds="\[(\d+),(\d+)\]\[(\d+),(\d+)\]"', tag)
        label = (text.group(1) if text else "") or (desc.group(1) if desc else "")
        if label and bounds:
            x1, y1, x2, y2 = map(int, bounds.groups())
            nodes.append((label.replace("&#10;", " | "), (x1 + x2) // 2, (y1 + y2) // 2, (x1, y1, x2, y2)))
    return nodes


def find(text):
    for label, cx, cy, b in dump():
        if text in label:
            return cx, cy, label
    return None


TAB_MARKERS = {"设备": ("扫描设备", "断开"), "应用": ("设备应用",), "Lua": ("打开示例",), "创作": ("生成 Lua",)}


def on_tab(name):
    marks = TAB_MARKERS.get(name)
    if not marks:
        return True
    labels = [n[0] for n in dump()]
    return any(any(m in l for l in labels) for m in marks)


def open_tab(name):
    """切到顶部某个标签页；标签条可横向滚动，看不到就先往对应方向拖。点完核对页面真的换了，没换再点。"""
    order = ["设备", "创作", "Lua", "预览", "应用", "日志", "商城", "取景与对话"]
    for _ in range(6):
        hit = find(name + " | Tab")
        if hit:
            adb("shell", "input", "tap", str(hit[0]), str(hit[1]))
            time.sleep(1.5)
            if on_tab(name):
                return True
            continue
        left = order.index(name) < 4 if name in order else True
        if left:
            adb("shell", "input", "swipe", "200", "380", "1000", "380", "250")
        else:
            adb("shell", "input", "swipe", "1000", "380", "200", "380", "250")
        time.sleep(1)
    return False


def tap_exact(label):
    hits = [n for n in dump() if n[0] == label]
    if len(hits) != 1:
        raise SystemExit(f"TAP_EXACT_FAIL {label} 命中 {len(hits)} 个")
    adb("shell", "input", "tap", str(hits[0][1]), str(hits[0][2]))
    time.sleep(1.2)
    print("TAP", label)


def set_field(current, value):
    """点中当前显示为 current 的输入框，全选后换成 value（adb 只能输 ASCII）。"""
    hits = [n for n in dump() if n[0] == current]
    if not hits:
        raise SystemExit(f"FIELD_NOT_FOUND {current}")
    adb("shell", "input", "tap", str(hits[0][1]), str(hits[0][2]))
    time.sleep(0.6)
    # 全选组合键在这台手机上不生效：光标移到末尾后按删除键清空
    adb("shell", "input", "keyevent", "123", *(["67"] * (len(current) + 4)))
    time.sleep(0.3)
    adb("shell", "input", "text", value)
    time.sleep(0.6)
    print("FIELD", current, "->", value)


def scroll_top():
    for _ in range(8):
        if find("设备应用") or find("扫描设备"):
            return
        adb("shell", "input", "swipe", "560", "800", "560", "2300", "250")
        time.sleep(0.4)


def app_action(app_id, action):
    """应用页：滚到 id 那张卡片，点卡片里的按钮（在背夹上打开/重命名/详情/卸载）。"""
    open_tab("应用")
    scroll_top()
    for _ in range(14):
        nodes = dump()
        keys = [f"| {app_id} |"] if ":" in app_id else [f"| {pre}{app_id} |" for pre in ("", "app:", "skill:")]
        cards = [n for n in nodes if any(k in n[0] + " |" for k in keys)]
        if cards:
            x1, y1, x2, y2 = cards[0][3]
            btns = [n for n in nodes if n[0] == action and y1 <= n[2] <= y2 and n[3][3] - n[3][1] > 60]
            if btns:
                adb("shell", "input", "tap", str(btns[0][1]), str(btns[0][2]))
                time.sleep(1.5)
                print("APP_ACTION", app_id, action)
                return True
        adb("shell", "input", "swipe", "560", "1900", "560", "1100", "350")
        time.sleep(0.9)
    print("APP_NOT_FOUND", app_id, action)
    return False


def install_example(app_id, name, example="贪吃蛇"):
    open_tab("Lua")
    tap_exact("打开示例")
    hits = [n for n in dump() if n[0].startswith(example + " |")]
    if not hits:
        raise SystemExit("EXAMPLE_NOT_FOUND " + example)
    adb("shell", "input", "tap", str(hits[0][1]), str(hits[0][2]))
    time.sleep(1.5)
    open_tab("应用")
    scroll_top()
    tap_exact("安装当前 Lua")
    set_field("my_app", app_id)
    set_field("我的应用", name)
    adb("shell", "input", "keyevent", "4")      # 收起输入法
    time.sleep(0.8)
    tap_exact("安装")


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "dump"
    if cmd == "dump":
        for label, cx, cy, b in dump():
            print(f"{label[:60]}  @({cx},{cy})  {b}")
    elif cmd == "tap":
        hit = find(sys.argv[2])
        if not hit:
            print("NOT_FOUND", sys.argv[2])
            sys.exit(2)
        adb("shell", "input", "tap", str(hit[0]), str(hit[1]))
        print("TAP", hit[2][:60], hit[0], hit[1])
    elif cmd == "tapxy":
        adb("shell", "input", "tap", sys.argv[2], sys.argv[3])
    elif cmd == "swipe":
        ms = sys.argv[6] if len(sys.argv) > 6 else "300"
        adb("shell", "input", "swipe", *sys.argv[2:6], ms)
    elif cmd == "shot":
        disp = main_display()
        args = ["exec-out", "screencap", "-p"] + (["-d", disp] if disp else [])
        data = subprocess.run(["adb", *args], capture_output=True, timeout=60).stdout
        open(sys.argv[2], "wb").write(data)
        print("SHOT", sys.argv[2], len(data))
    elif cmd == "tapexact":
        tap_exact(sys.argv[2])
    elif cmd == "field":
        set_field(sys.argv[2], sys.argv[3])
    elif cmd == "top":
        scroll_top()
    elif cmd == "appact":
        if not app_action(sys.argv[2], sys.argv[3]):
            sys.exit(3)
    elif cmd == "install":
        install_example(sys.argv[2], sys.argv[3])
    elif cmd == "tab":
        if not open_tab(sys.argv[2]):
            print("TAB_NOT_FOUND", sys.argv[2])
            sys.exit(2)
        print("TAB", sys.argv[2])
    elif cmd == "connect":
        # 已连上就不动；否则扫描 → 点背夹那一行的“连接” → 等到出现“断开”
        open_tab("设备")
        if find("断开"):
            print("ALREADY_CONNECTED")
            return
        hit = find("扫描设备")
        if not hit:
            print("NO_SCAN_BUTTON（请先回到设备页）")
            sys.exit(2)
        adb("shell", "input", "tap", str(hit[0]), str(hit[1]))
        end = time.time() + 25
        row = None
        while time.time() < end and not row:
            time.sleep(2)
            nodes = dump()
            if any(n[0] == "断开" for n in nodes):
                print("CONNECTED（App 已自动重连）")
                return
            rows = [n for n in nodes if "esp-claw-host" in n[0]]
            if rows:
                ry = rows[0][2]
                btns = [n for n in nodes if n[0] == "连接" and abs(n[2] - ry) < 200]
                row = btns[0] if btns else None
        if not row:
            print("DEVICE_NOT_FOUND")
            sys.exit(3)
        adb("shell", "input", "tap", str(row[1]), str(row[2]))
        end = time.time() + 20
        while time.time() < end:
            time.sleep(1.5)
            if find("断开"):
                print("CONNECTED")
                return
        print("CONNECT_TIMEOUT")
        sys.exit(4)
    elif cmd == "wait":
        limit = float(sys.argv[3]) if len(sys.argv) > 3 else 20
        end = time.time() + limit
        while time.time() < end:
            hit = find(sys.argv[2])
            if hit:
                print("FOUND", hit[2][:80])
                return
            time.sleep(1.5)
        print("TIMEOUT", sys.argv[2])
        sys.exit(3)


if __name__ == "__main__":
    main()
