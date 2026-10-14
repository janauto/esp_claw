#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
"""ESP-Claw 上位机 Lua 模块目录。

两阶段上下文策略：
1. 先把这里的紧凑清单（每个模块一行描述）交给 LLM，让它判断要用设备的哪些能力；
2. 再按选中的模块名，按需加载 components/lua_modules/*/README.md 作为生成上下文。

全部 README 合计约 135 KB，一次性塞进请求既浪费 token 又会淹没关键 API 约束；
按需加载可以让复杂任务（比如小游戏）拿到真正需要的 display / lvgl / 输入模块文档。
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
LUA_MODULES_DIR = REPO_ROOT / "components" / "lua_modules"

DEFAULT_PER_DOC_LIMIT = 12000
DEFAULT_TOTAL_LIMIT = 26000


@dataclass(frozen=True)
class LuaModuleInfo:
    """一个可 require 的 Lua 模块。

    require   脚本里 require() 用的名字
    component components/lua_modules 下的组件目录（README 来源，可被多个模块共享）
    tier      粗分类，便于按场景过滤
    summary   一行描述，进入 LLM 的模块清单
    keywords  中英文关键词，用于 LLM 不可用时的兜底选择
    """

    require: str
    component: str
    tier: str
    summary: str
    keywords: tuple[str, ...] = ()

    @property
    def doc_path(self) -> Path:
        return LUA_MODULES_DIR / self.component / "README.md"


CATALOG: tuple[LuaModuleInfo, ...] = (
    # ---- 基础设施：几乎所有脚本都可能用到 ----
    LuaModuleInfo(
        "board_manager",
        "lua_module_board_manager",
        "core",
        "取板级外设句柄（屏幕、触摸、I2C 总线等），display/lvgl/lcd_touch 初始化的前置依赖。",
        ("board", "板级", "句柄", "handle", "外设", "初始化"),
    ),
    LuaModuleInfo(
        "delay",
        "lua_module_delay",
        "core",
        "delay.delay_ms(n) 毫秒延时；主循环、动画节拍、保持画面都要用。",
        ("delay", "延时", "睡眠", "sleep", "等待", "循环", "loop", "动画", "帧", "fps", "节拍"),
    ),
    LuaModuleInfo(
        "system",
        "lua_module_system",
        "core",
        "时间、运行时长、IP、内存/堆、任务栈、Wi-Fi 状态查询。",
        ("system", "系统", "内存", "memory", "heap", "堆", "uptime", "时间", "ip", "wifi", "网络", "状态"),
    ),
    LuaModuleInfo(
        "storage",
        "lua_module_storage",
        "core",
        "文件系统读写：读写文件、列目录，用于存档、配置、最高分。",
        ("storage", "文件", "file", "保存", "存档", "读写", "目录", "配置", "最高分", "排行"),
    ),
    LuaModuleInfo(
        "json",
        "lua_module_json",
        "core",
        "JSON 编解码辅助库。",
        ("json", "编码", "解码", "序列化", "parse"),
    ),
    LuaModuleInfo(
        "thread",
        "lua_module_thread",
        "core",
        "Lua 任务管理与命名队列/信号量/事件组，用于多任务协作。",
        ("thread", "线程", "任务", "task", "并发", "队列", "queue", "信号量", "semaphore", "事件组"),
    ),
    LuaModuleInfo(
        "capability",
        "lua_module_call_capability",
        "core",
        "在 Lua 里直接调用已注册的 Capability，访问模块未覆盖的设备能力。",
        ("capability", "能力", "调用", "cap", "capability.call"),
    ),
    LuaModuleInfo(
        "arg_schema",
        "lua_module_system",
        "core",
        "纯 Lua 参数归一化助手。",
        ("arg_schema", "参数校验", "参数归一"),
    ),
    # ---- 显示：display 与 lvgl 二选一，不要在同一脚本混用 ----
    LuaModuleInfo(
        "display",
        "lua_module_display",
        "ui",
        "低层绘制：文字、直线、矩形、圆、像素缓冲、帧生命周期。仅用于静态显示或简单动画。",
        (
            "display", "显示", "屏幕", "画面", "绘制", "画", "draw", "文字", "text", "矩形",
            "圆", "像素", "帧", "frame", "静态", "简单动画", "hello", "刷新", "clear",
        ),
    ),
    LuaModuleInfo(
        "lvgl",
        "lua_module_lvgl",
        "ui",
        "LVGL 控件框架：按钮、标签、事件回调、触摸游戏。游戏/交互界面首选（设备端推荐）。",
        (
            "lvgl", "控件", "widget", "按钮", "button", "标签", "label", "列表", "list",
            "滑块", "slider", "开关", "switch", "复选", "菜单", "menu", "界面框架",
            "交互界面", "ui框架", "表单", "键盘", "图表", "chart", "进度条", "arc", "roller",
            # 游戏相关关键词（优先级高）
            "游戏", "game", "点击", "触摸", "touch", "tap", "跳跃", "jump", "贪吃蛇", "snake",
            "俄罗斯方块", "tetris", "乒乓", "pong", "分数", "score", "障碍物", "obstacle",
            "交互", "interact", "事件", "event", "clicked", "pressed",
        ),
    ),
    LuaModuleInfo(
        "lcd",
        "lua_module_lcd",
        "ui",
        "LCD 面板初始化、背光与显示辅助（比 display 更贴近面板层）。",
        ("lcd", "面板", "背光", "backlight", "panel", "亮度"),
    ),
    LuaModuleInfo(
        "image",
        "lua_module_image",
        "ui",
        "图像帧类型、格式转换、缩放、文件读写；中文/Unicode 文字需先渲染成图再绘制。",
        ("image", "图片", "图像", "jpeg", "jpg", "png", "缩放", "转换", "位图", "中文", "unicode", "图标"),
    ),
    LuaModuleInfo(
        "ssd1306",
        "lua_driver_i2c",
        "ui",
        "纯 Lua SSD1306 I2C OLED 驱动。",
        ("ssd1306", "oled", "小屏"),
    ),
    # ---- 输入 ----
    LuaModuleInfo(
        "lcd_touch",
        "lua_module_lcd_touch",
        "input",
        "读屏幕触摸坐标；进入轮询前 sync 一次，主循环用 poll 采样，触屏交互和触屏游戏操作要用。",
        ("lcd_touch", "触摸", "触屏", "touch", "点击", "坐标", "滑动", "手势", "按下"),
    ),
    LuaModuleInfo(
        "button",
        "lua_module_button",
        "input",
        "物理按键事件与回调（单击、长按等）；也支持板级按键组（多个按键共用一路 ADC），回调里用 label 区分按的是哪个键。",
        (
            "button", "按键", "按钮事件", "长按", "单击", "双击", "实体按键",
            "按键组", "多按键", "组合键", "音量键", "adc按键", "板级按键",
            "volume_up", "volume_down",
        ),
    ),
    LuaModuleInfo(
        "knob",
        "lua_module_knob",
        "input",
        "旋转编码器 / 旋钮事件。",
        ("knob", "旋钮", "编码器", "encoder", "旋转"),
    ),
    LuaModuleInfo(
        "touch",
        "lua_driver_touch",
        "input",
        "芯片电容触摸按键读取（TouchPad 外设，非屏幕触摸）。",
        ("touchpad", "电容触摸", "触摸按键"),
    ),
    LuaModuleInfo(
        "lib_si12t_touch",
        "lua_driver_i2c",
        "input",
        "纯 Lua Si12T I2C 电容触摸驱动。",
        ("si12t",),
    ),
    # ---- 传感器 ----
    LuaModuleInfo(
        "imu",
        "lua_module_imu",
        "sensor",
        "IMU 加速度计与陀螺仪读取，可做体感/倾斜控制。",
        ("imu", "加速度", "陀螺仪", "姿态", "体感", "倾斜", "摇晃", "重力", "accel", "gyro"),
    ),
    LuaModuleInfo(
        "environmental_sensor",
        "lua_module_environmental_sensor",
        "sensor",
        "BME690 温度、湿度、气压、气体数据。",
        ("环境", "温度", "湿度", "气压", "空气", "bme690", "temperature", "humidity"),
    ),
    LuaModuleInfo(
        "magnetometer",
        "lua_module_magnetometer",
        "sensor",
        "磁力计 / 电子罗盘读取。",
        ("磁力计", "罗盘", "指南针", "compass", "magnet"),
    ),
    LuaModuleInfo(
        "lib_fuel_gauge",
        "lua_module_fuel_gauge",
        "sensor",
        "纯 Lua 电量计库（BQ27220 / MAX17048 等）。",
        ("电量", "电池", "battery", "剩余电量", "fuel gauge", "充电"),
    ),
    LuaModuleInfo(
        "sci",
        "lua_module_sci",
        "sensor",
        "DFRobot SCI 采集模块（DFR0999）I2C 读取。",
        ("sci", "dfrobot", "dfr0999"),
    ),
    # ---- 通用外设 IO ----
    LuaModuleInfo(
        "gpio",
        "lua_driver_gpio",
        "io",
        "GPIO 电平读写与方向配置。",
        ("gpio", "引脚", "电平", "高低", "输入输出", "pin", "开关灯"),
    ),
    LuaModuleInfo(
        "adc",
        "lua_driver_adc",
        "io",
        "ADC 单次采样，返回校准后的毫伏值（电位器、模拟传感器）。",
        ("adc", "模拟", "采样", "电位器", "电压", "毫伏", "analog", "摇杆"),
    ),
    LuaModuleInfo(
        "i2c",
        "lua_driver_i2c",
        "io",
        "I2C 总线扫描与逐设备读写。",
        ("i2c", "总线", "扫描", "从机", "寄存器"),
    ),
    LuaModuleInfo(
        "uart",
        "lua_driver_uart",
        "io",
        "UART 串口收发（轮询读写）。",
        ("uart", "串口", "serial", "波特率", "收发"),
    ),
    LuaModuleInfo(
        "mcpwm",
        "lua_driver_mcpwm",
        "io",
        "通用 PWM 输出（频率/占空比），可驱动电机、舵机。",
        ("mcpwm", "pwm", "占空比", "电机", "舵机", "servo", "motor", "调速"),
    ),
    LuaModuleInfo(
        "ledc",
        "lua_module_ledc",
        "io",
        "LEDC PWM 通道，常用于 LED 调光与背光。",
        ("ledc", "调光", "亮度", "呼吸灯", "pwm led"),
    ),
    LuaModuleInfo(
        "pcnt",
        "lua_driver_pcnt",
        "io",
        "PCNT 脉冲计数 / 编码器计数读取。",
        ("pcnt", "脉冲", "计数", "转速"),
    ),
    LuaModuleInfo(
        "rmt",
        "lua_driver_rmt",
        "io",
        "RMT 收发红外/自定义时序波形。",
        ("rmt", "波形", "时序", "载波"),
    ),
    LuaModuleInfo(
        "led_strip",
        "lua_module_led_strip",
        "io",
        "可寻址灯带（WS2812 等）颜色控制。",
        ("led_strip", "灯带", "ws2812", "彩灯", "rgb灯", "流水灯", "氛围灯"),
    ),
    LuaModuleInfo(
        "ir",
        "lua_module_ir",
        "io",
        "红外接收、学习与发射。",
        ("ir", "红外", "遥控", "学习码"),
    ),
    # ---- 多媒体 ----
    LuaModuleInfo(
        "audio",
        "lua_module_audio",
        "media",
        "PCM/WAV 播放与录音、频谱分析；游戏音效、提示音要用。",
        ("audio", "音频", "声音", "播放", "录音", "喇叭", "音效", "wav", "pcm", "频谱", "提示音", "蜂鸣"),
    ),
    LuaModuleInfo(
        "camera",
        "lua_module_camera",
        "media",
        "摄像头拍照与取流。",
        ("camera", "摄像头", "拍照", "拍摄", "取流", "画面采集"),
    ),
    LuaModuleInfo(
        "motion_detect",
        "lua_module_vision",
        "media",
        "基于 image.frame 的运动检测。",
        ("运动检测", "motion", "移动侦测", "视觉"),
    ),
    # ---- 通信 / 网络 ----
    LuaModuleInfo(
        "ble",
        "lua_module_ble",
        "net",
        "BLE 通用收发（GATT 服务/客户端）。",
        ("ble", "蓝牙", "gatt", "广播", "从机", "主机"),
    ),
    LuaModuleInfo(
        "ble_hid",
        "lua_module_ble_hid",
        "net",
        "BLE HID 复合设备（媒体控制、键盘、鼠标）。",
        ("hid", "键盘", "鼠标", "媒体控制", "手柄"),
    ),
    LuaModuleInfo(
        "http_server",
        "lua_module_http_server",
        "net",
        "在已有 HTTP server 上挂静态文件和 HTTP 回调。",
        ("http", "web", "网页", "接口", "服务器", "url", "路由"),
    ),
    LuaModuleInfo(
        "event_publisher",
        "lua_module_event_publisher",
        "net",
        "从 Lua 往 Event Router 发事件。",
        ("event", "事件", "发布", "router", "上报"),
    ),
)

BY_REQUIRE: dict[str, LuaModuleInfo] = {item.require: item for item in CATALOG}

TIER_LABELS: dict[str, str] = {
    "core": "基础设施",
    "ui": "显示与图形",
    "input": "输入",
    "sensor": "传感器",
    "io": "通用外设 IO",
    "media": "多媒体",
    "net": "通信与网络",
}

#: 选择显示模块时必须一起带上的依赖，否则 LLM 容易漏掉初始化前置条件。
IMPLIED_MODULES: dict[str, tuple[str, ...]] = {
    "display": ("board_manager", "delay"),
    "lvgl": ("board_manager",),
    "lcd_touch": ("board_manager",),
    "lcd": ("board_manager",),
    "motion_detect": ("image",),
}


def catalog_overview() -> str:
    """返回按分类分组的紧凑清单，每个模块一行，用作第一阶段路由上下文。"""
    lines: list[str] = []
    for tier, label in TIER_LABELS.items():
        members = [item for item in CATALOG if item.tier == tier]
        if not members:
            continue
        lines.append(f"### {label}")
        for item in members:
            lines.append(f"- {item.require}: {item.summary}")
    return "\n".join(lines)


def known_requires() -> tuple[str, ...]:
    return tuple(item.require for item in CATALOG)


#: 判定「该走 display 全屏重绘」的关键词。命中则在 lvgl/display 互斥中选 display。
def select_by_keywords(prompt: str) -> list[str]:
    """关键词兜底选择：LLM 路由失败时使用。"""
    haystack = prompt.lower()
    hits = [item.require for item in CATALOG
            if any(keyword.lower() in haystack for keyword in item.keywords)]

    # 互斥规则：lvgl 和 display 不能同时使用，一律优先 lvgl。
    # 上层对象接口（object/label/set_pos/set_style + on 事件）兼容性明显好于
    # display 的逐帧自绘，游戏和动画同样适用；lcd_touch 要保留，因为触摸是靠
    # board_manager.get_lcd_touch_handle 取到 handle 再 lvgl.indev_register 注册的。
    if 'lvgl' in hits and 'display' in hits:
        hits.remove('display')

    return expand_with_implied(hits)


def expand_with_implied(requires: list[str]) -> list[str]:
    """补齐隐含依赖，保持调用方给出的顺序。"""
    out: list[str] = []
    for name in requires:
        for implied in IMPLIED_MODULES.get(name, ()):
            if implied not in out:
                out.append(implied)
        if name not in out:
            out.append(name)
    return out


def normalize_requires(raw: object) -> list[str]:
    """把 LLM 返回的模块名清洗成目录里真实存在的 require 名。"""
    if not isinstance(raw, list):
        return []
    out: list[str] = []
    for entry in raw:
        if not isinstance(entry, str):
            continue
        name = entry.strip().strip("\"'").removeprefix("require(").rstrip(")")
        name = name.strip().strip("\"'")
        if name in BY_REQUIRE and name not in out:
            out.append(name)
    return out


def resolve_docs(requires: list[str]) -> list[tuple[str, Path]]:
    """把模块名映射为 (模块名列表, README 路径)，共享同一 README 的模块会合并。"""
    ordered: list[tuple[str, Path]] = []
    seen: dict[Path, int] = {}
    for name in requires:
        info = BY_REQUIRE.get(name)
        if not info:
            continue
        path = info.doc_path
        if path in seen:
            index = seen[path]
            merged = f"{ordered[index][0]}, {name}"
            ordered[index] = (merged, path)
            continue
        seen[path] = len(ordered)
        ordered.append((name, path))
    return ordered


def load_module_docs(
    requires: list[str],
    *,
    per_doc_limit: int = DEFAULT_PER_DOC_LIMIT,
    total_limit: int = DEFAULT_TOTAL_LIMIT,
) -> tuple[str, list[str]]:
    """读取选中模块的 README，返回 (上下文文本, 实际载入的 README 相对路径)。"""
    sections: list[str] = []
    loaded: list[str] = []
    used = 0

    for label, path in resolve_docs(requires):
        if not path.exists():
            continue
        if used >= total_limit:
            break
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        budget = min(per_doc_limit, total_limit - used)
        if len(text) > budget:
            text = text[:budget].rstrip() + "\n...[README 截断，未显示的部分请勿凭猜测使用]"
        try:
            rel = path.relative_to(REPO_ROOT).as_posix()
        except ValueError:
            rel = path.as_posix()
        sections.append(f"## require(\"{label}\") -> {rel}\n{text}")
        loaded.append(rel)
        used += len(text)

    return "\n\n".join(sections), loaded
