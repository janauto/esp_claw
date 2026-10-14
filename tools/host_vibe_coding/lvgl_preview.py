"""Lightweight preview for ESP-Claw Lua LVGL scripts.

This is intentionally not a full Lua or LVGL runtime.  It recognizes the Lua
binding shape documented by components/lua_modules/lua_module_lvgl/README.md:

    local obj = lvgl.label(parent, { text = "...", x = 10, y = 20 })
    obj:set_pos(10, 20)
    obj:set_size(120, 40)
    obj:set_text("...")
    obj:set_style({ bg_color = "#112233" })

The output is a best-effort object tree that the GUI can paint on a Tk Canvas.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from typing import Any


CONSTRUCTOR_RE = re.compile(
    r"(?:local\s+)?(?P<var>[A-Za-z_]\w*)\s*=\s*lvgl\.(?P<kind>[A-Za-z_]\w*)\s*"
    r"\(\s*(?P<parent>[A-Za-z_]\w*|nil)?\s*,?\s*(?P<opts>\{[\s\S]*?\})?\s*\)",
    re.MULTILINE,
)
SCREEN_RE = re.compile(r"(?:local\s+)?(?P<var>[A-Za-z_]\w*)\s*=\s*lvgl\.create_screen\s*\(")
METHOD_RE = re.compile(
    r"(?P<var>[A-Za-z_]\w*)\s*:\s*(?P<method>set_pos|set_size|set_text|set_style|align)\s*"
    r"\((?P<args>[^\n)]*)\)",
    re.MULTILINE,
)
NUMBER_RE = re.compile(r"-?\d+(?:\.\d+)?")

DEFAULT_WIDTH = 320
DEFAULT_HEIGHT = 240


@dataclass
class PreviewNode:
    var: str
    kind: str
    parent: str = "screen"
    opts: dict[str, Any] = field(default_factory=dict)
    children: list[str] = field(default_factory=list)
    events: dict[str, str] = field(default_factory=dict)

    def get_int(self, key: str, default: int) -> int:
        value = self.opts.get(key)
        if isinstance(value, (int, float)):
            return int(value)
        if isinstance(value, str):
            found = NUMBER_RE.search(value)
            if found:
                try:
                    return int(float(found.group(0)))
                except ValueError:
                    return default
        return default

    def get_text(self) -> str:
        value = self.opts.get("text")
        if isinstance(value, str):
            return value
        if self.kind == "button":
            return "Button"
        if self.kind in {"checkbox", "switch"}:
            return self.kind
        return ""


@dataclass
class PreviewModel:
    width: int = DEFAULT_WIDTH
    height: int = DEFAULT_HEIGHT
    nodes: dict[str, PreviewNode] = field(default_factory=dict)
    root: str = "screen"
    warnings: list[str] = field(default_factory=list)
    state: dict[str, Any] = field(default_factory=dict)
    logical_boxes: dict[str, tuple[int, int, int, int]] = field(default_factory=dict)
    render_transform: tuple[int, int, float] = (0, 0, 1.0)

    @property
    def drawable_nodes(self) -> list[PreviewNode]:
        ordered: list[PreviewNode] = []

        def visit(var: str) -> None:
            for child in self.nodes.get(var, PreviewNode(var, "object")).children:
                node = self.nodes.get(child)
                if node:
                    ordered.append(node)
                    visit(child)

        visit(self.root)
        return ordered


def _strip_comments(script: str) -> str:
    return re.sub(r"--[^\n]*", "", script or "")


def _split_top_level(text: str) -> list[str]:
    parts: list[str] = []
    buf: list[str] = []
    depth = 0
    quote = ""
    escape = False
    for char in text:
        if quote:
            buf.append(char)
            if escape:
                escape = False
            elif char == "\\":
                escape = True
            elif char == quote:
                quote = ""
            continue
        if char in {"'", '"'}:
            quote = char
            buf.append(char)
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth = max(0, depth - 1)
        if char == "," and depth == 0:
            part = "".join(buf).strip()
            if part:
                parts.append(part)
            buf = []
            continue
        buf.append(char)
    part = "".join(buf).strip()
    if part:
        parts.append(part)
    return parts


def _parse_value(raw: str) -> Any:
    value = raw.strip()
    if len(value) >= 2 and value[0] in {"'", '"'} and value[-1] == value[0]:
        return value[1:-1]
    if value in {"true", "false"}:
        return value == "true"
    if NUMBER_RE.fullmatch(value):
        return int(float(value))
    if value.startswith("{") and value.endswith("}"):
        items = _split_top_level(value[1:-1])
        return [_parse_value(item) for item in items]
    return value


def _value_to_string(value: Any, state: dict[str, Any]) -> str:
    if isinstance(value, str) and value in state:
        return str(state[value])
    return str(value)


def _eval_lua_expr(expr: str, state: dict[str, Any]) -> Any:
    """Evaluate a tiny Lua expression subset used in callbacks."""
    text = expr.strip()
    if ".." in text:
        return "".join(_value_to_string(_eval_lua_expr(part, state), state) for part in text.split(".."))
    add = re.fullmatch(r"([A-Za-z_]\w*)\s*([+-])\s*(\d+)", text)
    if add:
        base = state.get(add.group(1), 0)
        try:
            base_i = int(base)
        except (TypeError, ValueError):
            base_i = 0
        delta = int(add.group(3))
        return base_i + delta if add.group(2) == "+" else base_i - delta
    if text in state:
        return state[text]
    return _parse_value(text)


def parse_lua_table(table_text: str | None) -> dict[str, Any]:
    if not table_text:
        return {}
    text = table_text.strip()
    if text.startswith("{") and text.endswith("}"):
        text = text[1:-1]
    opts: dict[str, Any] = {}
    positional_index = 1
    for part in _split_top_level(text):
        if "=" in part:
            key, _, raw = part.partition("=")
            opts[key.strip()] = _parse_value(raw)
        else:
            opts[str(positional_index)] = _parse_value(part)
            positional_index += 1
    return opts


def _extract_display_size(device_info: str, script: str) -> tuple[int, int]:
    blob = f"{device_info}\n{script}"
    width = DEFAULT_WIDTH
    height = DEFAULT_HEIGHT
    for key, value in re.findall(r"\b(width|height)\b[\"']?\s*[:=]\s*(\d{2,4})", blob):
        if key == "width":
            width = int(value)
        else:
            height = int(value)
    if width <= 0 or height <= 0 or width > 1200 or height > 1200:
        return DEFAULT_WIDTH, DEFAULT_HEIGHT
    return width, height


def build_lvgl_preview(script: str, device_info: str = "") -> PreviewModel:
    body = _strip_comments(script)
    width, height = _extract_display_size(device_info, body)
    model = PreviewModel(width=width, height=height)
    model.nodes[model.root] = PreviewNode(model.root, "screen", "", {"w": width, "h": height})

    for name, raw in re.findall(r"(?:local\s+)?([A-Za-z_]\w*)\s*=\s*([\"'][^\"']*[\"']|-?\d+)\s*$", body, re.MULTILINE):
        model.state[name] = _parse_value(raw)

    screen_match = SCREEN_RE.search(body)
    if screen_match:
        screen_var = screen_match.group("var")
        model.nodes[screen_var] = PreviewNode(screen_var, "screen", model.root, {"w": width, "h": height})
        model.nodes[model.root].children.append(screen_var)
        model.root = screen_var

    for match in CONSTRUCTOR_RE.finditer(body):
        kind = match.group("kind")
        if kind in {"init", "deinit", "run", "process_events", "indev_register", "indev_unregister", "create_screen"}:
            continue
        var = match.group("var")
        parent = match.group("parent") or model.root
        if parent == "nil":
            parent = model.root
        opts = parse_lua_table(match.group("opts"))
        node = PreviewNode(var=var, kind=kind, parent=parent, opts=opts)
        model.nodes[var] = node
        if parent not in model.nodes:
            model.warnings.append(f"预览：父对象 `{parent}` 未识别，已挂到屏幕根节点")
            parent = model.root
            node.parent = parent
        model.nodes[parent].children.append(var)

    for match in METHOD_RE.finditer(body):
        var = match.group("var")
        method = match.group("method")
        node = model.nodes.get(var)
        if not node:
            continue
        args = _split_top_level(match.group("args"))
        if method == "set_pos" and len(args) >= 2:
            node.opts["x"] = _parse_value(args[0])
            node.opts["y"] = _parse_value(args[1])
        elif method == "set_size" and len(args) >= 2:
            node.opts["w"] = _parse_value(args[0])
            node.opts["h"] = _parse_value(args[1])
        elif method == "set_text" and args:
            node.opts["text"] = _parse_value(args[0])
        elif method == "set_style" and args:
            style = parse_lua_table(args[0])
            node.opts.update(style)
        elif method == "align" and args:
            node.opts["align"] = _parse_value(args[0])
            if len(args) >= 3:
                node.opts["x"] = _parse_value(args[1])
                node.opts["y"] = _parse_value(args[2])

    for var, event, callback_body in re.findall(
        r"([A-Za-z_]\w*)\s*:\s*on\s*\(\s*[\"'](\w+)[\"']\s*,\s*function\s*\(\s*\)\s*([\s\S]*?)\s*end\s*\)",
        body,
    ):
        node = model.nodes.get(var)
        if node:
            node.events[event] = callback_body

    if "lvgl" not in body:
        model.warnings.append("脚本没有 require(\"lvgl\")，无法生成 LVGL 结构预览")
    if not model.drawable_nodes:
        model.warnings.append("未识别到 lvgl.* 构造器，预览为空")
    return model


def run_preview_event(model: PreviewModel, node_var: str, event: str = "clicked") -> list[str]:
    """Run a small subset of Lua callback code and mutate the preview model."""
    node = model.nodes.get(node_var)
    if not node:
        return [f"预览：对象 `{node_var}` 不存在"]
    callback = node.events.get(event)
    if not callback:
        return [f"预览：`{node_var}` 没有 `{event}` 回调"]
    notes: list[str] = []
    for raw_stmt in re.split(r"\n|;", callback):
        stmt = raw_stmt.strip()
        if not stmt or stmt.startswith("--"):
            continue
        assign = re.fullmatch(r"([A-Za-z_]\w*)\s*=\s*(.+)", stmt)
        if assign and ":" not in assign.group(1):
            model.state[assign.group(1)] = _eval_lua_expr(assign.group(2), model.state)
            continue
        call = re.fullmatch(r"([A-Za-z_]\w*)\s*:\s*(set_text|set_value|set_pos|set_size|set_style)\s*\((.*)\)", stmt)
        if not call:
            if stmt.startswith("print"):
                notes.append("预览日志：" + stmt)
            else:
                notes.append(f"预览暂不支持回调语句：{stmt[:80]}")
            continue
        target = model.nodes.get(call.group(1))
        if not target:
            notes.append(f"预览：回调目标 `{call.group(1)}` 未识别")
            continue
        method = call.group(2)
        args = _split_top_level(call.group(3))
        if method == "set_text" and args:
            target.opts["text"] = str(_eval_lua_expr(args[0], model.state))
        elif method == "set_value" and args:
            target.opts["value"] = _eval_lua_expr(args[0], model.state)
        elif method == "set_pos" and len(args) >= 2:
            target.opts["x"] = _eval_lua_expr(args[0], model.state)
            target.opts["y"] = _eval_lua_expr(args[1], model.state)
        elif method == "set_size" and len(args) >= 2:
            target.opts["w"] = _eval_lua_expr(args[0], model.state)
            target.opts["h"] = _eval_lua_expr(args[1], model.state)
        elif method == "set_style" and args:
            target.opts.update(parse_lua_table(args[0]))
    return notes


def node_at_canvas_point(model: PreviewModel, canvas_x: int, canvas_y: int) -> str:
    ox, oy, scale = model.render_transform
    if scale <= 0:
        return ""
    lx = int((canvas_x - ox) / scale)
    ly = int((canvas_y - oy) / scale)
    for node in reversed(model.drawable_nodes):
        x, y, w, h = model.logical_boxes.get(node.var, (0, 0, 0, 0))
        if x <= lx <= x + w and y <= ly <= y + h:
            return node.var
    return ""


def _color(value: Any, default: str) -> str:
    if isinstance(value, int):
        return f"#{value & 0xFFFFFF:06x}"
    if isinstance(value, str):
        text = value.strip()
        if re.fullmatch(r"#[0-9a-fA-F]{6}", text):
            return text
        if re.fullmatch(r"0x[0-9a-fA-F]{6}", text):
            return "#" + text[2:]
    return default


def _rect_for(node: PreviewNode, parent_box: tuple[int, int, int, int], model: PreviewModel) -> tuple[int, int, int, int]:
    px, py, pw, ph = parent_box
    defaults = {
        "label": (140, 28),
        "button": (110, 42),
        "slider": (150, 24),
        "bar": (150, 20),
        "checkbox": (120, 28),
        "switch": (58, 32),
        "textarea": (160, 44),
        "container": (max(80, pw - 24), max(50, ph // 3)),
    }
    dw, dh = defaults.get(node.kind, (96, 36))
    w = node.get_int("w", dw)
    h = node.get_int("h", dh)
    x = node.get_int("x", 0)
    y = node.get_int("y", 0)
    align = str(node.opts.get("align") or "").lower()
    if align in {"center", "mid"}:
        x += (pw - w) // 2
        y += (ph - h) // 2
    elif align in {"top_mid", "top_center"}:
        x += (pw - w) // 2
    elif align in {"bottom_mid", "bottom_center"}:
        x += (pw - w) // 2
        y += ph - h
    elif align in {"left_mid"}:
        y += (ph - h) // 2
    elif align in {"right_mid"}:
        x += pw - w
        y += (ph - h) // 2
    elif align in {"bottom_left"}:
        y += ph - h
    elif align in {"bottom_right"}:
        x += pw - w
        y += ph - h
    elif align in {"top_right"}:
        x += pw - w
    return px + x, py + y, max(4, w), max(4, h)


def render_lvgl_preview(canvas: Any, model: PreviewModel) -> None:
    canvas.delete("all")
    canvas.update_idletasks()
    cw = max(240, int(canvas.winfo_width() or 320))
    ch = max(180, int(canvas.winfo_height() or 240))
    scale = min((cw - 24) / model.width, (ch - 24) / model.height)
    ox = int((cw - model.width * scale) / 2)
    oy = int((ch - model.height * scale) / 2)
    model.render_transform = (ox, oy, scale)
    model.logical_boxes.clear()

    def sx(value: int) -> int:
        return int(ox + value * scale)

    def sy(value: int) -> int:
        return int(oy + value * scale)

    boxes: dict[str, tuple[int, int, int, int]] = {model.root: (0, 0, model.width, model.height)}
    canvas.create_rectangle(sx(0), sy(0), sx(model.width), sy(model.height), fill="#101820", outline="#334155")

    for node in model.drawable_nodes:
        parent_box = boxes.get(node.parent, (0, 0, model.width, model.height))
        x, y, w, h = _rect_for(node, parent_box, model)
        boxes[node.var] = (x, y, w, h)
        model.logical_boxes[node.var] = (x, y, w, h)
        fill = _color(node.opts.get("bg_color"), "#1f2937")
        outline = _color(node.opts.get("border_color"), "#64748b")
        text_color = _color(node.opts.get("text_color"), "#f8fafc")
        x1, y1, x2, y2 = sx(x), sy(y), sx(x + w), sy(y + h)

        if node.kind == "label":
            text = node.get_text() or node.var
            canvas.create_text(x1 + 4, y1 + max(10, int(12 * scale)), anchor="w", fill=text_color, text=text[:36])
        elif node.kind in {"button", "checkbox", "switch"}:
            canvas.create_rectangle(x1, y1, x2, y2, fill=fill if fill != "#1f2937" else "#2563eb", outline=outline)
            canvas.create_text((x1 + x2) // 2, (y1 + y2) // 2, fill=text_color, text=node.get_text()[:24])
        elif node.kind in {"slider", "bar"}:
            canvas.create_rectangle(x1, y1, x2, y2, fill="#0f172a", outline=outline)
            value = max(0, min(100, node.get_int("value", 50)))
            canvas.create_rectangle(x1, y1, x1 + int((x2 - x1) * value / 100), y2, fill="#22c55e", outline="")
        else:
            canvas.create_rectangle(x1, y1, x2, y2, fill=fill, outline=outline)
            label = node.kind if node.kind != "container" else ""
            if label:
                canvas.create_text(x1 + 4, y1 + 12, anchor="w", fill="#cbd5e1", text=label)

    if model.warnings:
        canvas.create_text(8, ch - 8, anchor="sw", fill="#f59e0b", text=model.warnings[0][:80])
