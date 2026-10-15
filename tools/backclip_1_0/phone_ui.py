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


def open_tab(name):
    """切到顶部某个标签页；标签条可横向滚动，看不到就先往对应方向拖。"""
    order = ["设备", "创作", "Lua", "预览", "应用", "日志", "商城", "取景与对话"]
    for _ in range(4):
        hit = find(name + " | Tab")
        if hit:
            adb("shell", "input", "tap", str(hit[0]), str(hit[1]))
            time.sleep(1.5)
            return True
        left = order.index(name) < 4 if name in order else True
        if left:
            adb("shell", "input", "swipe", "200", "380", "1000", "380", "250")
        else:
            adb("shell", "input", "swipe", "1000", "380", "200", "380", "250")
        time.sleep(1)
    return False


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
