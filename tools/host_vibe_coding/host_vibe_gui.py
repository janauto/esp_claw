#!/usr/bin/env python3
"""ESP-Claw 电脑端图形上位机。"""

from __future__ import annotations

import asyncio
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

from host_vibe import (
    DEFAULT_CONFIG_PATH,
    DEFAULT_DEVICE_PREFIX,
    DEFAULT_HISTORY_PATH,
    HOST_LLM_PRESETS,
    SERVICE_UUID,
    BleBridge,
    HostLlmConfig,
    apply_provider_preset,
    append_history,
    build_messages,
    call_openai_compatible,
    extract_lua,
    load_config,
    provider_key_from_label,
    provider_label,
    provider_values,
    read_recent_history,
    request_id,
    save_config,
    validate_generated_lua,
)


class AsyncWorker:
    """在后台线程里串行执行异步任务，避免阻塞 Tk 主线程。"""

    def __init__(self, on_result: Callable[[str, Any, Exception | None], None]) -> None:
        self.on_result = on_result
        self.jobs: queue.Queue[tuple[str, Callable[[], Any]]] = queue.Queue()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def submit(self, name: str, job: Callable[[], Any]) -> None:
        self.jobs.put((name, job))

    def _run(self) -> None:
        asyncio.run(self._loop())

    async def _loop(self) -> None:
        while True:
            name, job = await asyncio.to_thread(self.jobs.get)
            try:
                result = await job()
                self.on_result(name, result, None)
            except Exception as err:
                self.on_result(name, None, err)


class HostVibeApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ESP-Claw 电脑端 Vibe Coding")
        self.geometry("1180x820")
        self.minsize(980, 680)

        self.config_path = DEFAULT_CONFIG_PATH
        self.history_path = DEFAULT_HISTORY_PATH
        self.config = load_config(self.config_path)
        self.device_map: dict[str, Any] = {}
        self.result_queue: queue.Queue[tuple[str, Any, Exception | None]] = queue.Queue()
        self.worker = AsyncWorker(self._worker_result)

        self._build_ui()
        self._load_config_into_form()
        self._load_recent_history()
        self.after(120, self._poll_results)
        self.protocol("WM_DELETE_WINDOW", self._close)

    def _build_ui(self) -> None:
        self.columnconfigure(0, weight=1)
        self.rowconfigure(1, weight=1)

        header = ttk.Frame(self, padding=(18, 14))
        header.grid(row=0, column=0, sticky="ew")
        header.columnconfigure(1, weight=1)
        ttk.Label(header, text="ESP-Claw 电脑端 Vibe Coding", font=("TkDefaultFont", 16, "bold")).grid(
            row=0, column=0, sticky="w"
        )
        self.status_var = tk.StringVar(value="未连接设备")
        ttk.Label(header, textvariable=self.status_var).grid(row=0, column=1, sticky="e")

        body = ttk.Frame(self, padding=(18, 0, 18, 18))
        body.grid(row=1, column=0, sticky="nsew")
        body.columnconfigure(0, weight=1)
        body.columnconfigure(1, weight=2)
        body.rowconfigure(2, weight=1)

        self._build_llm_panel(body)
        self._build_ble_panel(body)
        self._build_chat_panel(body)
        self._build_script_panel(body)

    def _build_llm_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="电脑端 LLM 配置（保存到本地）", padding=10)
        frame.grid(row=0, column=0, columnspan=2, sticky="ew", pady=(0, 10))
        frame.columnconfigure(1, weight=1)
        frame.columnconfigure(3, weight=1)

        self.provider_var = tk.StringVar()
        ttk.Label(frame, text="LLM 提供商").grid(row=0, column=0, sticky="w", padx=(0, 6), pady=3)
        self.provider = ttk.Combobox(frame, textvariable=self.provider_var, values=provider_values(), state="readonly")
        self.provider.grid(row=0, column=1, sticky="ew", padx=(0, 12), pady=3)
        self.provider.bind("<<ComboboxSelected>>", self._provider_changed)
        ttk.Label(frame, text="选择预设后通常只需填写 API Key 和模型。", foreground="#666").grid(
            row=0, column=2, columnspan=2, sticky="w", pady=3
        )

        self.base_url = self._entry(frame, "Base URL", 1, 0)
        self.model = self._entry(frame, "模型", 1, 2)
        self.api_key = self._entry(frame, "API Key", 2, 0, show="*")
        self.max_tokens = self._entry(frame, "最大 Token", 2, 2)
        ttk.Label(frame, text="系统提示词").grid(row=3, column=0, sticky="w", padx=(0, 6), pady=3)
        self.system_prompt = ttk.Entry(frame)
        self.system_prompt.grid(row=3, column=1, columnspan=3, sticky="ew", pady=3)

        actions = ttk.Frame(frame)
        actions.grid(row=4, column=0, columnspan=4, sticky="w", pady=(8, 0))
        ttk.Button(actions, text="保存 LLM 配置", command=self._save_llm).pack(side="left", padx=(0, 8))
        ttk.Button(actions, text="重新加载本地配置", command=self._reload_llm).pack(side="left")

    def _provider_changed(self, _event: object | None = None) -> None:
        provider = provider_key_from_label(self.provider_var.get())
        preset = HOST_LLM_PRESETS.get(provider)
        if not preset or provider == "custom":
            return

        try:
            max_tokens = int(self.max_tokens.get().strip() or preset.max_tokens)
        except ValueError:
            max_tokens = preset.max_tokens

        snapshot = HostLlmConfig(
            provider=provider,
            base_url=self.base_url.get().strip(),
            model=self.model.get().strip(),
            api_key=self.api_key.get(),
            auth_type=preset.auth_type,
            max_tokens=max_tokens,
            system_prompt=self.system_prompt.get().strip(),
        )
        merged = apply_provider_preset(snapshot, provider)
        for entry, value in (
            (self.base_url, merged.base_url),
            (self.model, merged.model),
            (self.max_tokens, str(merged.max_tokens)),
        ):
            entry.delete(0, "end")
            entry.insert(0, value)
        self.status_var.set(f"已应用 {preset.label} 预设")

    def _build_ble_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="设备扫描与 BLE 连接", padding=10)
        frame.grid(row=1, column=0, sticky="nsew", padx=(0, 8))
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(4, weight=1)

        row = ttk.Frame(frame)
        row.grid(row=0, column=0, sticky="ew")
        row.columnconfigure(1, weight=1)
        self.name_prefix = tk.StringVar(value=DEFAULT_DEVICE_PREFIX)
        ttk.Label(row, text="设备名前缀").grid(row=0, column=0, sticky="w", padx=(0, 6))
        ttk.Entry(row, textvariable=self.name_prefix, width=18).grid(row=0, column=1, sticky="ew", padx=(0, 6))
        ttk.Button(row, text="扫描设备", command=self._scan).grid(row=0, column=2, sticky="e")

        self.devices = ttk.Combobox(frame, state="readonly")
        self.devices.grid(row=1, column=0, sticky="ew", pady=(8, 8))

        actions = ttk.Frame(frame)
        actions.grid(row=2, column=0, sticky="ew")
        ttk.Button(actions, text="连接选中设备", command=self._connect).pack(fill="x", pady=2)
        ttk.Button(actions, text="断开连接", command=self._disconnect).pack(fill="x", pady=2)
        ttk.Button(actions, text="获取设备信息", command=self._get_info).pack(fill="x", pady=2)
        ttk.Button(actions, text="查询 job 状态", command=self._job_status).pack(fill="x", pady=2)
        ttk.Button(actions, text="停止 job", command=self._job_stop).pack(fill="x", pady=2)

        self.info_text = self._text(frame, height=18)
        self.info_text.grid(row=4, column=0, sticky="nsew", pady=(8, 0))

    def _build_chat_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="LLM 对话与 Lua 生成", padding=10)
        frame.grid(row=1, column=1, sticky="nsew")
        frame.columnconfigure(0, weight=1)
        frame.rowconfigure(0, weight=1)

        self.chat_text = self._text(frame, height=18)
        self.chat_text.grid(row=0, column=0, sticky="nsew")

        self.prompt = tk.Text(frame, height=5, wrap="word")
        self.prompt.grid(row=1, column=0, sticky="ew", pady=(8, 6))
        self.prompt.insert("1.0", "读取设备信息，生成一个 Lua 脚本打印当前内存和网络状态。")

        actions = ttk.Frame(frame)
        actions.grid(row=2, column=0, sticky="w")
        ttk.Button(actions, text="生成 Lua", command=self._generate).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="上传并执行", command=self._upload_run).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="清空对话", command=self._clear_chat).pack(side="left")

    def _build_script_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Lua 脚本与执行日志", padding=10)
        frame.grid(row=2, column=0, columnspan=2, sticky="nsew", pady=(10, 0))
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)
        frame.rowconfigure(1, weight=1)

        ttk.Label(frame, text="Lua 脚本").grid(row=0, column=0, sticky="w")
        ttk.Label(frame, text="执行日志").grid(row=0, column=1, sticky="w")

        self.script_text = self._text(frame, height=11)
        self.script_text.grid(row=1, column=0, sticky="nsew", padx=(0, 6))

        self.log_text = self._text(frame, height=11)
        self.log_text.grid(row=1, column=1, sticky="nsew", padx=(6, 0))

        actions = ttk.Frame(frame)
        actions.grid(row=2, column=0, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Button(actions, text="打开本地 Lua", command=self._open_lua).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="保存 Lua 到本地", command=self._save_lua).pack(side="left")

    @staticmethod
    def _entry(parent: ttk.Frame, label: str, row: int, col: int, show: str | None = None) -> tk.Entry:
        ttk.Label(parent, text=label).grid(row=row, column=col, sticky="w", padx=(0, 6), pady=3)
        entry = ttk.Entry(parent, show=show or "")
        entry.grid(row=row, column=col + 1, sticky="ew", padx=(0, 12), pady=3)
        return entry

    @staticmethod
    def _text(parent: ttk.Frame, height: int) -> tk.Text:
        return tk.Text(parent, height=height, wrap="word", state="normal", font=("TkFixedFont", 10))

    def _load_config_into_form(self) -> None:
        self.provider_var.set(provider_label(self.config.provider))
        values = (
            self.config.base_url,
            self.config.model,
            self.config.api_key,
            str(self.config.max_tokens),
            self.config.system_prompt,
        )
        for entry, value in zip(
            (self.base_url, self.model, self.api_key, self.max_tokens, self.system_prompt), values
        ):
            entry.delete(0, "end")
            entry.insert(0, value)

    def _reload_llm(self) -> None:
        self.config = load_config(self.config_path)
        self._load_config_into_form()
        self.status_var.set("已重新加载本地 LLM 配置")

    def _save_llm(self) -> None:
        try:
            provider = provider_key_from_label(self.provider_var.get())
            preset = HOST_LLM_PRESETS.get(provider)
            base_url = self.base_url.get().strip()
            model = self.model.get().strip()
            max_tokens_text = self.max_tokens.get().strip()
            if provider != "custom" and preset:
                base_url = base_url or preset.base_url
                model = model or preset.model
            self.config = HostLlmConfig(
                provider=provider,
                base_url=base_url,
                model=model,
                api_key=self.api_key.get(),
                auth_type=preset.auth_type if preset else "bearer",
                max_tokens=int(max_tokens_text or (preset.max_tokens if preset else 4096)),
                system_prompt=self.system_prompt.get().strip(),
            )
            save_config(self.config_path, self.config)
            self.status_var.set(f"LLM 配置已保存到 {self.config_path}")
        except ValueError:
            messagebox.showerror("配置错误", "最大 Token 必须是整数")

    def _load_recent_history(self) -> None:
        history = read_recent_history(self.history_path)
        if not history:
            self.chat_text.insert("end", "尚无本地历史记录。\n")
            return

        self.chat_text.insert("end", "最近历史记录：\n")
        for item in history:
            prompt = item.get("prompt")
            result = item.get("result")
            if isinstance(prompt, str):
                self.chat_text.insert("end", f"- 用户：{prompt}\n")
            if isinstance(result, dict):
                self.chat_text.insert("end", f"- 结果：{result.get('ok', result)}\n")
        self.chat_text.insert("end", "\n")

    def _scan(self) -> None:
        prefix = self.name_prefix.get().strip() or DEFAULT_DEVICE_PREFIX
        self.status_var.set(f"正在扫描 BLE 设备（前缀：{prefix}）…")

        async def job() -> Any:
            from bleak import BleakScanner

            devices = await BleakScanner.discover(timeout=8)
            return [item for item in devices if (item.name or "").startswith(prefix)]

        self.worker.submit("scan", job)

    def _connect(self) -> None:
        selected = self.devices.get()
        if not selected:
            messagebox.showwarning("未选择设备", "请先扫描并选择一个设备")
            return

        target = self.device_map.get(selected)
        if target is None:
            target = selected.split("\t", 1)[0]

        prefix = self.name_prefix.get().strip() or DEFAULT_DEVICE_PREFIX
        self.status_var.set("正在连接 BLE…")

        async def job() -> Any:
            bridge = BleBridge(target, prefix, 15)
            await bridge.connect()
            return bridge

        self.worker.submit("connect", job)

    def _disconnect(self) -> None:
        if not self.bridge:
            self.status_var.set("当前没有已连接设备")
            return
        self.status_var.set("正在断开 BLE 连接…")
        self.worker.submit("disconnect", self.bridge.disconnect)

    def _get_info(self) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先扫描并连接设备")
            return

        async def job() -> Any:
            return await self.bridge.request({"id": request_id("info"), "op": "device_info"}, timeout=20)

        self.worker.submit("info", job)

    def _job_status(self) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        async def job() -> Any:
            return await self.bridge.request({"id": request_id("job"), "op": "job_status"}, timeout=20)

        self.worker.submit("job_status", job)

    def _job_stop(self) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        async def job() -> Any:
            return await self.bridge.request({"id": request_id("stop"), "op": "job_stop"}, timeout=20)

        self.worker.submit("job_stop", job)

    def _generate(self) -> None:
        try:
            self._save_llm()
        except Exception:
            return

        prompt = self.prompt.get("1.0", "end").strip()
        if not prompt:
            messagebox.showwarning("空提示词", "请输入自然语言需求")
            return

        self.chat_text.insert("end", f"\n你：{prompt}\n")
        self.status_var.set("LLM 正在生成 Lua…")

        async def job() -> Any:
            history = read_recent_history(self.history_path)
            messages = build_messages(prompt, self.device_info, history, self.config.system_prompt)
            raw = await asyncio.to_thread(call_openai_compatible, self.config, messages)
            script = extract_lua(raw)
            validate_generated_lua(script)
            return {"raw": raw, "script": script, "prompt": prompt}

        self.worker.submit("generate", job)

    def _upload_run(self) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        script = self.script_text.get("1.0", "end").strip()
        if not script:
            messagebox.showwarning("脚本为空", "请先生成或加载 Lua 脚本")
            return
        try:
            validate_generated_lua(script)
        except RuntimeError as err:
            messagebox.showerror("Lua API 校验失败", str(err))
            return

        self.status_var.set("正在上传并执行 Lua…")

        async def job() -> Any:
            put = await self.bridge.request(
                {"id": request_id("lua"), "op": "put_lua", "source": script},
                timeout=20,
            )
            if not put.get("ok"):
                raise RuntimeError(str(put.get("error") or put))

            path = str(put.get("data") or "")
            run = await self.bridge.request(
                {"id": request_id("run"), "op": "run_lua", "path": path, "timeout_ms": 60000},
                timeout=20,
            )
            return {"put": put, "run": run, "script": script}

        self.worker.submit("run", job)

    def _open_lua(self) -> None:
        path = filedialog.askopenfilename(filetypes=[("Lua files", "*.lua"), ("All files", "*")])
        if not path:
            return
        self._load_file_into(self.script_text, Path(path))
        self.status_var.set(f"已加载 Lua 文件：{path}")

    def _save_lua(self) -> None:
        path = filedialog.asksaveasfilename(
            defaultextension=".lua",
            filetypes=[("Lua files", "*.lua"), ("All files", "*")],
        )
        if not path:
            return
        Path(path).write_text(self.script_text.get("1.0", "end").strip() + "\n", encoding="utf-8")
        self.status_var.set(f"Lua 已保存到 {path}")

    def _load_file_into(self, widget: tk.Text, path: Path) -> None:
        widget.delete("1.0", "end")
        widget.insert("1.0", path.read_text(encoding="utf-8"))

    def _format_error(self, error: Exception) -> str:
        text = str(error).strip()
        if text:
            return text
        return f"{error.__class__.__name__}"

    def _worker_result(self, name: str, result: Any, error: Exception | None) -> None:
        self.result_queue.put((name, result, error))

    def _poll_results(self) -> None:
        while not self.result_queue.empty():
            name, result, error = self.result_queue.get()
            if error:
                self.status_var.set("操作失败")
                self.after(0, lambda err=error: messagebox.showerror("上位机错误", self._format_error(err)))
                continue

            if name == "scan":
                devices = result or []
                values = []
                self.device_map = {}
                for item in devices:
                    label = f"{item.name or '(unnamed)'}\t{item.address}"
                    values.append(label)
                    self.device_map[label] = item
                self.devices["values"] = values
                if values and not self.devices.get():
                    self.devices.current(0)
                self.status_var.set(f"扫描完成，找到 {len(values)} 个设备")
            elif name == "connect":
                self.bridge = result
                self.status_var.set("BLE 已连接")
            elif name == "disconnect":
                if self.bridge:
                    self.bridge = None
                self.status_var.set("BLE 已断开")
            elif name == "info":
                self.device_info = str(result.get("data") or result.get("error") or "")
                self.info_text.delete("1.0", "end")
                self.info_text.insert("1.0", self.device_info)
                self.status_var.set("设备信息已获取")
            elif name == "job_status":
                text = str(result.get("data") or result.get("error") or result)
                self.log_text.insert("end", f"job_status: {text}\n")
                self.log_text.see("end")
                self.status_var.set("已查询 job 状态")
            elif name == "job_stop":
                text = str(result.get("data") or result.get("error") or result)
                self.log_text.insert("end", f"job_stop: {text}\n")
                self.log_text.see("end")
                self.status_var.set("已请求停止 job")
            elif name == "generate":
                payload = result or {}
                raw = str(payload.get("raw") or "")
                script = str(payload.get("script") or "")
                prompt = str(payload.get("prompt") or "")
                if prompt:
                    self.chat_text.insert("end", f"上位机：\n{prompt}\n")
                if raw:
                    self.chat_text.insert("end", f"LLM：\n{raw}\n")
                self.script_text.delete("1.0", "end")
                self.script_text.insert("1.0", script)
                self.status_var.set("Lua 已生成")
            elif name == "run":
                payload = result or {}
                put = payload.get("put")
                run = payload.get("run")
                script = str(payload.get("script") or "")
                self.log_text.insert("end", f"put_lua: {put}\nrun_lua: {run}\n")
                self.log_text.see("end")
                append_history(
                    self.history_path,
                    {
                        "prompt": self.prompt.get("1.0", "end").strip(),
                        "script": script,
                        "result": run,
                    },
                )
                self.status_var.set("Lua 执行完成")

        self.after(120, self._poll_results)

    def _clear_chat(self) -> None:
        self.chat_text.delete("1.0", "end")
        self.chat_text.insert("1.0", "对话已清空。\n")
        self.status_var.set("已清空对话区")

    def _close(self) -> None:
        if self.bridge:
            self.worker.submit("disconnect", self.bridge.disconnect)
        self.destroy()


if __name__ == "__main__":
    HostVibeApp().mainloop()