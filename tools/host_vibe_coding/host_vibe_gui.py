#!/usr/bin/env python3
"""ESP-Claw 电脑端图形上位机。"""

from __future__ import annotations

import asyncio
import base64
import queue
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from typing import Any, Callable

import lua_module_catalog as catalog
from lvgl_preview import build_lvgl_preview, node_at_canvas_point, render_lvgl_preview, run_preview_event
from web_lvgl_preview import WebLvglPreviewServer, build_instructions, find_web_lvgl_sim
from host_vibe import (
    API_STYLES,
    DEFAULT_CONFIG_PATH,
    DEFAULT_DEVICE_PREFIX,
    DEFAULT_HISTORY_PATH,
    DEFAULT_MAX_ATTEMPTS,
    DEFAULT_MAX_TOKENS,
    HOST_LLM_PRESETS,
    JOB_POLL_TIMEOUT,
    SERVICE_UUID,
    BleBridge,
    Cancelled,
    HostLlmConfig,
    apply_provider_preset,
    append_history,
    build_messages,
    call_llm,
    check_connection,
    extract_lua,
    generation_system_prompt,
    iterate_lua,
    load_config,
    make_progress_reporter,
    plan_for,
    provider_key_from_label,
    provider_label,
    provider_values,
    read_recent_history,
    request_id,
    resolved_endpoint,
    run_lua_and_collect,
    save_config,
    simulated_device_preview,
    stop_job_and_wait,
    strip_debug_instrumentation,
    summarize_attempts,
    validate_generated_lua,
    Attempt,
)


class AsyncWorker:
    """在后台线程的事件循环里并发执行异步任务。

    原来是一条串行队列，代价很具体：生成 Lua 期间点"停止 job"，那个请求会排在
    生成后面，界面上就是按钮完全没反应——而这时候用户想停的恰恰是设备上正在
    跑的东西。BLE 请求按 req_id 分发、写入有锁，本来就支持并发。

    刻意只用**一个**事件循环：BleBridge 的 pending future 和写锁都绑在创建它们
    的循环上，多开循环会让跨循环的等待直接报错。
    """

    def __init__(self, on_result: Callable[[str, Any, Exception | None], None]) -> None:
        self.on_result = on_result
        self.loop = asyncio.new_event_loop()
        self._ready = threading.Event()
        self._tasks: dict[str, asyncio.Task[Any]] = {}
        self._lock = threading.Lock()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()
        # 等循环真的起来：submit 可能在窗口刚建好时就被调用。
        self._ready.wait(timeout=5.0)

    def _run(self) -> None:
        asyncio.set_event_loop(self.loop)
        self.loop.call_soon(self._ready.set)
        self.loop.run_forever()

    def submit(self, name: str, job: Callable[[], Any], *, replace: bool = True) -> None:
        """把一个协程工厂丢到后台循环上跑。

        replace=True（默认）时，同名任务如果还在跑就先取消它：连续点两次
        "生成"应该是"重新生成"，而不是攒出两个互相抢结果的请求。
        """
        if not self.loop.is_running():
            self.on_result(name, None, RuntimeError("后台任务循环已停止"))
            return

        async def runner() -> None:
            try:
                result = await job()
                self.on_result(name, result, None)
            except asyncio.CancelledError:
                # 必须报成 Cancelled 而不是"成功但结果为空"：后者会被结果分发
                # 当成一次正常生成，把编辑器里的脚本清空、状态写成"已生成"。
                # Cancelled 在 GUI 侧只记一行日志，不弹错误框。
                self.on_result(name, None, Cancelled("已取消"))
                raise
            except Exception as err:  # noqa: BLE001
                self.on_result(name, None, err)
            finally:
                with self._lock:
                    self._tasks.pop(name, None)

        def schedule() -> None:
            if replace:
                existing = self._tasks.get(name)
                if existing and not existing.done():
                    existing.cancel()
            task = self.loop.create_task(runner())
            with self._lock:
                self._tasks[name] = task

        self.loop.call_soon_threadsafe(schedule)

    def cancel(self, name: str) -> bool:
        """取消一个在跑的任务，返回它当时是否还活着。"""
        with self._lock:
            task = self._tasks.get(name)
        if task is None or task.done():
            return False
        self.loop.call_soon_threadsafe(task.cancel)
        return True

    def running(self, name: str) -> bool:
        with self._lock:
            task = self._tasks.get(name)
        return task is not None and not task.done()

    def stop(self, timeout: float = 3.0) -> None:
        """关窗口时收尾。

        asyncio.to_thread 用的线程池不是 daemon 线程，解释器退出时会等它，
        所以必须让循环自己停下来，否则关窗口后进程会挂住不退。
        """
        def shutdown() -> None:
            for task in list(self._tasks.values()):
                task.cancel()
            self.loop.stop()

        if self.loop.is_running():
            self.loop.call_soon_threadsafe(shutdown)
        self.thread.join(timeout=timeout)


class WorkBuddyAudioRuntime:
    sample_rate = 16000
    channels = 1
    frame_ms = 60
    frame_samples = sample_rate * frame_ms // 1000

    def __init__(self, log_fn: Callable[[str], None]) -> None:
        self.log_fn = log_fn
        self.rx_queue: queue.Queue[bytes] = queue.Queue(maxsize=16)
        self.stop_event = threading.Event()
        self.play_thread: threading.Thread | None = None
        self.playing = False

    def _imports(self) -> tuple[Any, Any]:
        try:
            import opuslib  # type: ignore[import-not-found]
            import sounddevice as sd  # type: ignore[import-not-found]
        except ImportError as exc:
            raise RuntimeError("缺少音频依赖，请安装：pip install sounddevice opuslib") from exc
        return opuslib, sd

    def start_playback(self) -> None:
        if self.playing:
            return
        opuslib, sd = self._imports()
        decoder = opuslib.Decoder(self.sample_rate, self.channels)
        self.stop_event.clear()

        def run() -> None:
            try:
                with sd.RawOutputStream(
                    samplerate=self.sample_rate,
                    channels=self.channels,
                    dtype="int16",
                    blocksize=self.frame_samples,
                ) as stream:
                    self.playing = True
                    self.log_fn("WorkBuddy 音频播放已启动")
                    while not self.stop_event.is_set():
                        try:
                            opus = self.rx_queue.get(timeout=0.2)
                        except queue.Empty:
                            continue
                        pcm = decoder.decode(opus, self.frame_samples, decode_fec=False)
                        stream.write(pcm)
            except Exception as exc:  # noqa: BLE001
                self.log_fn(f"WorkBuddy 音频播放失败: {exc}")
            finally:
                self.playing = False

        self.play_thread = threading.Thread(target=run, daemon=True)
        self.play_thread.start()

    def handle_rx_frame(self, frame: dict[str, Any]) -> None:
        data = frame.get("data")
        if not isinstance(data, str) or not data:
            return
        try:
            opus = base64.b64decode(data)
        except Exception:
            return
        try:
            self.start_playback()
            self.rx_queue.put_nowait(opus)
        except queue.Full:
            pass

    async def stream_host_mic_to_device(self, bridge: BleBridge) -> None:
        opuslib, sd = self._imports()
        encoder = opuslib.Encoder(self.sample_rate, self.channels, opuslib.APPLICATION_VOIP)
        encoder.bitrate = 24000
        self.stop_event.clear()
        await bridge.request(
            {
                "id": request_id("wbaudio"),
                "op": "workbuddy_audio_control",
                "payload": {"action": "start_playback"},
            },
            timeout=10,
        )
        self.log_fn("WorkBuddy 上位机麦克风传输已启动")
        try:
            with sd.RawInputStream(
                samplerate=self.sample_rate,
                channels=self.channels,
                dtype="int16",
                blocksize=self.frame_samples,
            ) as stream:
                seq = 0
                while not self.stop_event.is_set():
                    pcm, _overflowed = await asyncio.to_thread(stream.read, self.frame_samples)
                    opus = encoder.encode(bytes(pcm), self.frame_samples)
                    seq += 1
                    await bridge.send_nowait(
                        {
                            "id": request_id("wbaudio"),
                            "op": "workbuddy_audio_opus",
                            "payload": {
                                "seq": seq,
                                "sample_rate": self.sample_rate,
                                "channels": self.channels,
                                "bits": 16,
                                "frame_ms": self.frame_ms,
                                "data": base64.b64encode(opus).decode("ascii"),
                            },
                        }
                    )
        finally:
            try:
                await bridge.request(
                    {
                        "id": request_id("wbaudio"),
                        "op": "workbuddy_audio_control",
                        "payload": {"action": "stop"},
                    },
                    timeout=5,
                )
            except Exception:
                pass
            self.log_fn("WorkBuddy 上位机麦克风传输已停止")

    def stop(self) -> None:
        self.stop_event.set()


class HostVibeApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ESP-Claw 电脑端图形上位机")
        self.geometry("1180x820")
        self.minsize(980, 680)

        self.config_path = DEFAULT_CONFIG_PATH
        self.history_path = DEFAULT_HISTORY_PATH
        # 不要叫 self.config：那会覆盖 tk.Tk.config 方法。
        self.llm_config = load_config(self.config_path)
        self.device_map: dict[str, Any] = {}
        # 这几个状态必须先初始化，否则未连接时点按钮会直接 AttributeError。
        self.bridge: BleBridge | None = None
        self.device_info: str = ""
        self.plan: Any = None
        self.current_prompt: str = ""
        self.current_script_mode = "debug"
        self.feedback_attempts: list[Attempt] = []
        self.generation_round = 0
        self.preview_model: Any = None
        self.web_preview_server: WebLvglPreviewServer | None = None
        self.web_preview_check = tk.BooleanVar(value=False)
        self.workbuddy_audio = WorkBuddyAudioRuntime(lambda text: self.result_queue.put(("log", text, None)))
        self.workbuddy_audio_tx_running = False
        self.workbuddy_vars: dict[str, tk.StringVar] = {
            "online": tk.StringVar(value="1"),
            "tasks": tk.StringVar(value="3"),
            "credits": tk.StringVar(value="20"),
            "battery": tk.StringVar(value="70"),
            "record": tk.StringVar(value="0"),
            "reply_status": tk.StringVar(value="回复："),
            "reply_text": tk.StringVar(value="Hello WorkBuddy"),
            "workbuddy_status": tk.StringVar(value="已连接"),
        }
        self.result_queue: queue.Queue[tuple[str, Any, Exception | None]] = queue.Queue()
        #: 生成类任务的取消令牌。llm_client 会在 HTTP 读取、退避等待、
        #: SSE 循环里检查它，所以点了"停止生成"能当场断开，不用等请求跑完。
        self.generate_cancel = threading.Event()
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
        ttk.Label(header, text="ESP-Claw 电脑端图形上位机", font=("TkDefaultFont", 16, "bold")).grid(
            row=0, column=0, sticky="w"
        )
        self.status_var = tk.StringVar(value="未连接设备")
        ttk.Label(header, textvariable=self.status_var).grid(row=0, column=1, sticky="e")

        tabs = ttk.Notebook(self)
        tabs.grid(row=1, column=0, sticky="nsew", padx=18, pady=(0, 18))

        vibe = ttk.Frame(tabs, padding=(0, 10, 0, 0))
        vibe.columnconfigure(0, weight=1)
        vibe.columnconfigure(1, weight=2)
        vibe.rowconfigure(2, weight=1)
        tabs.add(vibe, text="Vibe Coding")

        workbuddy = ttk.Frame(tabs, padding=(0, 10, 0, 0))
        workbuddy.columnconfigure(0, weight=1)
        workbuddy.rowconfigure(1, weight=1)
        tabs.add(workbuddy, text="WorkBuddy")

        self._build_llm_panel(vibe)
        self._build_ble_panel(vibe)
        self._build_chat_panel(vibe)
        self._build_script_panel(vibe)
        self._build_workbuddy_page(workbuddy)

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

        # 接口风格必须能手选。OpenAI 兼容和 Anthropic 的请求体、鉴权头、SSE
        # 事件形状完全不同，靠 base_url 猜会在自建网关上猜错，症状是一律 400。
        ttk.Label(frame, text="接口风格").grid(row=3, column=0, sticky="w", padx=(0, 6), pady=3)
        self.api_style_var = tk.StringVar(value=API_STYLES[0])
        self.api_style = ttk.Combobox(
            frame,
            textvariable=self.api_style_var,
            values=list(API_STYLES),
            state="readonly",
        )
        self.api_style.grid(row=3, column=1, sticky="ew", padx=(0, 12), pady=3)
        self.api_style.bind("<<ComboboxSelected>>", lambda _e: self._refresh_endpoint_hint())
        ttk.Label(
            frame,
            text="openai_chat = /chat/completions；anthropic = /v1/messages",
            foreground="#666",
        ).grid(row=3, column=2, columnspan=2, sticky="w", pady=3)

        ttk.Label(frame, text="系统提示词").grid(row=4, column=0, sticky="w", padx=(0, 6), pady=3)
        self.system_prompt = ttk.Entry(frame)
        self.system_prompt.grid(row=4, column=1, columnspan=3, sticky="ew", pady=3)

        # 把归一后的地址显示出来。Base URL 填法不统一（带不带 /v1、有没有粘上
        # 完整 endpoint）是 404 的头号原因，而光看 Base URL 是看不出来的。
        self.endpoint_var = tk.StringVar(value="")
        ttk.Label(frame, textvariable=self.endpoint_var, foreground="#0a7").grid(
            row=5, column=0, columnspan=4, sticky="w", pady=(4, 0)
        )
        for entry in (self.base_url, self.model):
            entry.bind("<KeyRelease>", lambda _e: self._refresh_endpoint_hint())

        actions = ttk.Frame(frame)
        actions.grid(row=6, column=0, columnspan=4, sticky="w", pady=(8, 0))
        ttk.Button(actions, text="保存 LLM 配置", command=self._save_llm).pack(side="left", padx=(0, 8))
        ttk.Button(actions, text="重新加载本地配置", command=self._reload_llm).pack(side="left", padx=(0, 8))
        # 探活按钮：一次点击分清地址错 / Key 错 / 模型名错 / 网络不通，
        # 不必再拿一次完整生成去试。
        ttk.Button(actions, text="测试连接", command=self._test_connection).pack(side="left")

    def _refresh_endpoint_hint(self) -> None:
        """回显这次配置实际会 POST 到哪个地址。"""
        probe = HostLlmConfig(
            base_url=self.base_url.get().strip(),
            model=self.model.get().strip(),
            api_key="x",
            api_style=self.api_style_var.get().strip() or API_STYLES[0],
        )
        self.endpoint_var.set(f"实际请求地址：{resolved_endpoint(probe)}")

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
            api_style=self.api_style_var.get().strip() or API_STYLES[0],
        )
        merged = apply_provider_preset(snapshot, provider)
        for entry, value in (
            (self.base_url, merged.base_url),
            (self.model, merged.model),
            (self.max_tokens, str(merged.max_tokens)),
        ):
            entry.delete(0, "end")
            entry.insert(0, value)
        # 预设要能改写接口风格：切到 Anthropic 却留着 openai_chat，
        # 请求会拿 OpenAI 的体去打 /v1/messages，必然 400。
        self.api_style_var.set(merged.api_style)
        self._refresh_endpoint_hint()
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

    def _build_workbuddy_page(self, parent: ttk.Frame) -> None:
        parent.columnconfigure(0, weight=1)

        status = ttk.LabelFrame(parent, text="连接", padding=10)
        status.grid(row=0, column=0, sticky="ew")
        status.columnconfigure(0, weight=1)
        ttk.Label(status, text="复用 Vibe Coding 页中的 BLE 连接。设备按钮录音会自动在这里播放。").grid(
            row=0, column=0, sticky="w"
        )
        ttk.Button(status, text="获取设备信息", command=self._get_info).grid(row=0, column=1, padx=(8, 0))

        controls = ttk.Frame(parent)
        controls.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        controls.columnconfigure(0, weight=1)
        self._build_workbuddy_panel(controls)

        audio = ttk.LabelFrame(controls, text="WorkBuddy 音频", padding=10)
        audio.grid(row=1, column=0, sticky="ew", pady=(10, 0))
        self.workbuddy_audio_button = ttk.Button(
            audio,
            text="开始传递电脑麦克风音频",
            command=self._toggle_workbuddy_audio_tx,
        )
        self.workbuddy_audio_button.pack(side="left")
        ttk.Button(audio, text="停止设备音频会话", command=self._stop_workbuddy_audio_session).pack(
            side="left", padx=(8, 0)
        )
        ttk.Label(
            audio,
            text="设备按钮：设备 mic -> Opus -> BLE -> 上位机播放；本按钮：电脑 mic -> Opus -> BLE -> 设备播放。",
            foreground="#555",
        ).pack(side="left", padx=(12, 0))

    def _build_workbuddy_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="WorkBuddy UI 控制", padding=8)
        frame.grid(row=0, column=0, sticky="ew")
        frame.columnconfigure(1, weight=1)

        fields = (
            ("online", "在线"),
            ("tasks", "任务"),
            ("credits", "积分"),
            ("battery", "电量"),
            ("record", "录音态"),
            ("reply_status", "回复前缀"),
            ("reply_text", "回复内容"),
            ("workbuddy_status", "状态"),
        )
        for row, (key, label) in enumerate(fields):
            ttk.Label(frame, text=label).grid(row=row, column=0, sticky="w", pady=2)
            ttk.Entry(frame, textvariable=self.workbuddy_vars[key], width=18).grid(
                row=row, column=1, sticky="ew", padx=(6, 6), pady=2
            )
            ttk.Button(frame, text="发送", command=lambda item=key: self._send_workbuddy_single(item)).grid(
                row=row, column=2, sticky="e", pady=2
            )

        actions = ttk.Frame(frame)
        actions.grid(row=len(fields), column=0, columnspan=3, sticky="ew", pady=(8, 0))
        ttk.Button(actions, text="发送全部", command=self._send_workbuddy_all).pack(side="left")
        ttk.Button(actions, text="Demo", command=self._reset_workbuddy_demo).pack(side="left", padx=(6, 0))

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
        ttk.Button(actions, text="查看模块选择", command=self._plan_only).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="生成 Debug Lua", command=self._generate).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="反馈并重生成", command=self._regenerate_with_feedback).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="转 Release", command=self._make_release).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="新脚本", command=self._new_lua_flow).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="上传并执行", command=self._upload_run).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="生成并自动迭代", command=self._generate_iterate).pack(side="left", padx=(0, 6))
        # 生成期间必须有出口。之前一次生成起步就只能等它跑完（大预算下可能几分钟），
        # 期间点任何按钮都排在它后面。
        self.stop_generate_button = ttk.Button(
            actions, text="停止生成", command=self._stop_generate, state="disabled"
        )
        self.stop_generate_button.pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="清空对话", command=self._clear_chat).pack(side="left")

        options = ttk.Frame(frame)
        options.grid(row=3, column=0, sticky="w", pady=(6, 0))
        ttk.Label(options, text="最多迭代轮数").pack(side="left", padx=(0, 6))
        self.max_attempts = tk.StringVar(value=str(DEFAULT_MAX_ATTEMPTS))
        ttk.Spinbox(options, from_=1, to=6, width=4, textvariable=self.max_attempts).pack(side="left", padx=(0, 12))
        ttk.Label(options, text="等待执行结果秒数").pack(side="left", padx=(0, 6))
        self.poll_timeout = tk.StringVar(value="15")  # 缩短默认值到 15 秒
        ttk.Spinbox(options, from_=5, to=600, width=5, textvariable=self.poll_timeout).pack(side="left")
        ttk.Checkbutton(
            options,
            text="上传前自动 Web 预览",
            variable=self.web_preview_check,
        ).pack(side="left", padx=(12, 0))

        feedback = ttk.Frame(frame)
        feedback.grid(row=4, column=0, sticky="ew", pady=(6, 0))
        feedback.columnconfigure(1, weight=1)
        ttk.Label(feedback, text="代码反馈").grid(row=0, column=0, sticky="w", padx=(0, 6))
        self.feedback_entry = ttk.Entry(feedback)
        self.feedback_entry.grid(row=0, column=1, sticky="ew")

    def _build_script_panel(self, parent: ttk.Frame) -> None:
        frame = ttk.LabelFrame(parent, text="Lua 脚本与执行日志", padding=10)
        frame.grid(row=2, column=0, columnspan=2, sticky="nsew", pady=(10, 0))
        frame.columnconfigure(0, weight=1)
        frame.columnconfigure(1, weight=1)
        frame.columnconfigure(2, weight=1)
        frame.rowconfigure(1, weight=1)

        ttk.Label(frame, text="Lua 脚本").grid(row=0, column=0, sticky="w")
        ttk.Label(frame, text="模拟设备端预览").grid(row=0, column=1, sticky="w")
        ttk.Label(frame, text="执行日志").grid(row=0, column=2, sticky="w")

        self.script_text = self._text(frame, height=11)
        self.script_text.grid(row=1, column=0, sticky="nsew", padx=(0, 6))

        preview_frame = ttk.Frame(frame)
        preview_frame.grid(row=1, column=1, sticky="nsew", padx=(6, 6))
        preview_frame.columnconfigure(0, weight=1)
        preview_frame.rowconfigure(0, weight=2)
        preview_frame.rowconfigure(1, weight=1)
        self.preview_canvas = tk.Canvas(preview_frame, height=190, bg="#0f172a", highlightthickness=1)
        self.preview_canvas.grid(row=0, column=0, sticky="nsew")
        self.preview_canvas.bind("<Configure>", lambda _event: self._refresh_preview())
        self.preview_canvas.bind("<Button-1>", self._preview_click)
        self.preview_text = self._text(preview_frame, height=5)
        self.preview_text.grid(row=1, column=0, sticky="nsew", pady=(6, 0))

        self.log_text = self._text(frame, height=11)
        self.log_text.grid(row=1, column=2, sticky="nsew", padx=(6, 0))

        actions = ttk.Frame(frame)
        actions.grid(row=2, column=0, columnspan=3, sticky="w", pady=(6, 0))
        ttk.Button(actions, text="打开本地 Lua", command=self._open_lua).pack(side="left", padx=(0, 6))
        ttk.Button(actions, text="保存 Lua 到本地", command=self._save_lua).pack(side="left")
        ttk.Button(actions, text="刷新预览", command=self._refresh_preview).pack(side="left", padx=(6, 0))
        ttk.Button(actions, text="真实 LVGL Web 预览", command=self._open_web_lvgl_preview).pack(side="left", padx=(6, 0))

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
        self.provider_var.set(provider_label(self.llm_config.provider))
        values = (
            self.llm_config.base_url,
            self.llm_config.model,
            self.llm_config.api_key,
            str(self.llm_config.max_tokens),
            self.llm_config.system_prompt,
        )
        for entry, value in zip(
            (self.base_url, self.model, self.api_key, self.max_tokens, self.system_prompt), values
        ):
            entry.delete(0, "end")
            entry.insert(0, value)
        self.api_style_var.set(self.llm_config.api_style or API_STYLES[0])
        self._refresh_endpoint_hint()

    def _reload_llm(self) -> None:
        self.llm_config = load_config(self.config_path)
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
            self.llm_config = HostLlmConfig(
                provider=provider,
                base_url=base_url,
                model=model,
                api_key=self.api_key.get(),
                # 没有预设时沿用当前值，别硬退回 bearer：那会把本地
                # vLLM/Ollama 显式配好的 auth_type=none 冲掉，之后一直报缺 Key。
                auth_type=preset.auth_type if preset else self.llm_config.auth_type,
                max_tokens=int(max_tokens_text or (preset.max_tokens if preset else DEFAULT_MAX_TOKENS)),
                system_prompt=self.system_prompt.get().strip(),
                api_style=self.api_style_var.get().strip() or API_STYLES[0],
                # 这两项只在配置文件里手改，界面不提供输入框，但必须原样保留。
                extra_headers=dict(self.llm_config.extra_headers),
                extra_params=dict(self.llm_config.extra_params),
            )
            save_config(self.config_path, self.llm_config)
            self._refresh_endpoint_hint()
            self.status_var.set(f"LLM 配置已保存到 {self.config_path}")
        except ValueError:
            messagebox.showerror("配置错误", "最大 Token 必须是整数")

    def _test_connection(self) -> None:
        """探活当前配置，把结论写进对话区。

        存在的理由：生成失败时原因可能在四个地方（地址、Key、模型名、网络），
        拿一次完整生成去试既慢又只会给一条含糊的报错。
        """
        self._save_llm()
        self.status_var.set("正在测试 LLM 连接…")

        async def job() -> Any:
            return await asyncio.to_thread(check_connection, self.llm_config)

        self.worker.submit("check", job)

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
            # 创建带事件回调的 bridge
            def on_event(event: dict) -> None:
                # 把设备事件放入结果队列，让主线程处理
                self.result_queue.put(("device_event", event, None))

            bridge = BleBridge(
                target,
                prefix,
                15,
                on_device_event=on_event,
                on_audio_frame=self.workbuddy_audio.handle_rx_frame,
            )
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

    def _send_workbuddy_payload(self, payload: dict[str, Any]) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        async def job() -> Any:
            assert self.bridge is not None
            return await self.bridge.request(
                {"id": request_id("workbuddy"), "op": "workbuddy_ui", "payload": payload},
                timeout=15,
            )

        self.status_var.set("正在更新 WorkBuddy UI…")
        self.worker.submit("workbuddy_ui", job)

    def _send_workbuddy_single(self, key: str) -> None:
        value = self.workbuddy_vars[key].get().strip()
        if not value:
            messagebox.showinfo("WorkBuddy UI", "字段为空，不发送")
            return
        self._send_workbuddy_payload({"fields": {key: value}})

    def _send_workbuddy_all(self) -> None:
        fields = {
            key: var.get().strip()
            for key, var in self.workbuddy_vars.items()
            if var.get().strip()
        }
        self._send_workbuddy_payload({"fields": fields})

    def _reset_workbuddy_demo(self) -> None:
        values = {
            "online": "1",
            "tasks": "3",
            "credits": "20",
            "battery": "70",
            "record": "0",
            "reply_status": "回复：",
            "reply_text": "Hello WorkBuddy",
            "workbuddy_status": "已连接",
        }
        for key, value in values.items():
            self.workbuddy_vars[key].set(value)
        self._send_workbuddy_payload({"fields": values})

    def _toggle_workbuddy_audio_tx(self) -> None:
        if self.workbuddy_audio_tx_running:
            self.workbuddy_audio.stop()
            self.workbuddy_audio_tx_running = False
            self.workbuddy_audio_button.configure(text="开始传递电脑麦克风音频")
            self.status_var.set("正在停止 WorkBuddy 音频传输…")
            return
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        async def job() -> Any:
            assert self.bridge is not None
            await self.workbuddy_audio.stream_host_mic_to_device(self.bridge)
            return {"stopped": True}

        self.workbuddy_audio_tx_running = True
        self.workbuddy_audio_button.configure(text="停止传递电脑麦克风音频")
        self.status_var.set("正在传递电脑麦克风音频…")
        self.worker.submit("workbuddy_audio_tx", job)

    def _stop_workbuddy_audio_session(self) -> None:
        self.workbuddy_audio.stop()
        self.workbuddy_audio_tx_running = False
        if hasattr(self, "workbuddy_audio_button"):
            self.workbuddy_audio_button.configure(text="开始传递电脑麦克风音频")
        if not self.bridge:
            return

        async def job() -> Any:
            assert self.bridge is not None
            return await self.bridge.request(
                {
                    "id": request_id("wbaudio"),
                    "op": "workbuddy_audio_control",
                    "payload": {"action": "stop"},
                },
                timeout=10,
            )

        self.worker.submit("workbuddy_audio_control", job)

    def _job_stop(self) -> None:
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备")
            return

        self.status_var.set("正在停止 job…")

        async def job() -> Any:
            # 使用新的快速停止函数，不做长时间轮询
            stop_resp, final_status = await stop_job_and_wait(
                self.bridge,
                job_id=None,  # 停止当前 job
                timeout=5,
                wait_after_stop=2.0,  # 只等待 2 秒
            )
            return {"stop": stop_resp, "status": final_status}

        self.worker.submit("job_stop", job)

    def _current_prompt(self) -> str:
        try:
            self._save_llm()
        except Exception:
            return ""
        prompt = self.prompt.get("1.0", "end").strip()
        if not prompt:
            messagebox.showwarning("空提示词", "请输入自然语言需求")
            return ""
        return prompt

    def _int_var(self, var: tk.StringVar, fallback: int) -> int:
        try:
            return max(1, int(var.get().strip()))
        except (ValueError, AttributeError):
            return fallback

    def _log(self, message: str) -> None:
        """从后台线程往日志区写一行（经队列，Tk 只能在主线程碰控件）。"""
        self.result_queue.put(("log", message, None))

    def _begin_generation(self) -> None:
        """开一次生成任务前的公共准备：清掉上一次的取消标记，放出停止按钮。"""
        self.generate_cancel.clear()
        self.stop_generate_button.configure(state="normal")

    def _end_generation(self) -> None:
        if not (self.worker.running("generate") or self.worker.running("iterate")
                or self.worker.running("plan")):
            self.stop_generate_button.configure(state="disabled")

    def _stop_generate(self) -> None:
        """停止在跑的生成。

        先置取消令牌（llm_client 会在 HTTP 读取和退避等待里检查它，能当场断开），
        再取消 asyncio 任务作为兜底。只 cancel 任务是不够的：阻塞在 socket 读上
        的 to_thread 不会因为任务被取消而立刻回来。
        """
        self.generate_cancel.set()
        stopped = [name for name in ("generate", "iterate", "plan") if self.worker.cancel(name)]
        if stopped:
            self.status_var.set("正在停止生成…")
            self._log("已请求停止生成")
        else:
            self.status_var.set("当前没有在跑的生成任务")
        self.stop_generate_button.configure(state="disabled")

    def _current_script(self) -> str:
        return self.script_text.get("1.0", "end").strip()

    def _set_script(self, script: str, *, mode: str, prompt: str = "", plan: Any = None) -> None:
        self.current_script_mode = mode
        if prompt:
            self.current_prompt = prompt
        if plan is not None:
            self.plan = plan
        self.script_text.delete("1.0", "end")
        self.script_text.insert("1.0", script)
        self._refresh_preview()

    def _refresh_preview(self) -> None:
        script = self._current_script()
        prompt = self.current_prompt or self._current_prompt()
        if not script:
            if hasattr(self, "preview_canvas"):
                self.preview_canvas.delete("all")
            self.preview_text.delete("1.0", "end")
            self.preview_text.insert("1.0", "暂无 Lua 脚本。生成 Debug Lua 后会在这里显示模拟设备端预览。")
            return
        model = build_lvgl_preview(script, self.device_info)
        self.preview_model = model
        if hasattr(self, "preview_canvas"):
            render_lvgl_preview(self.preview_canvas, model)
        preview = simulated_device_preview(
            prompt,
            self.plan,
            script,
            debug=self.current_script_mode != "release",
            device_info=self.device_info,
        )
        if model.warnings:
            preview += "\n\nLVGL 预览提示：\n" + "\n".join(f"- {item}" for item in model.warnings[:5])
        self.preview_text.delete("1.0", "end")
        self.preview_text.insert("1.0", preview)

    def _preview_click(self, event: tk.Event) -> None:
        model = self.preview_model
        if model is None:
            return
        node_var = node_at_canvas_point(model, int(event.x), int(event.y))
        if not node_var:
            return
        notes = run_preview_event(model, node_var, "clicked")
        render_lvgl_preview(self.preview_canvas, model)
        if notes:
            self.log_text.insert("end", "\n".join(notes) + "\n")
        else:
            self.log_text.insert("end", f"预览点击：{node_var}.clicked\n")
        self.log_text.see("end")

    def _new_lua_flow(self) -> None:
        self.current_prompt = ""
        self.current_script_mode = "debug"
        self.feedback_attempts.clear()
        self.generation_round = 0
        self.plan = None
        self.feedback_entry.delete(0, "end")
        self.script_text.delete("1.0", "end")
        self.preview_text.delete("1.0", "end")
        self.log_text.delete("1.0", "end")
        self.status_var.set("已进入新的 Lua 生成流程")

    def _make_release(self) -> None:
        script = self._current_script()
        if not script:
            messagebox.showwarning("脚本为空", "请先生成 Debug Lua")
            return
        release = strip_debug_instrumentation(script)
        try:
            validate_generated_lua(release)
        except RuntimeError as err:
            messagebox.showerror("Release 校验失败", str(err))
            return
        self._set_script(release, mode="release")
        self.status_var.set("已转为 Release：调试 dbg 输出已移除")

    def _open_web_lvgl_preview(self) -> None:
        script = self._current_script()
        if not script:
            messagebox.showwarning("脚本为空", "请先生成或加载 Lua 脚本")
            return
        artifacts = find_web_lvgl_sim()
        if artifacts is None:
            messagebox.showerror("缺少 Web LVGL 模拟器构建产物", build_instructions())
            return
        if self.web_preview_server is None or self.web_preview_server.artifacts != artifacts:
            if self.web_preview_server is not None:
                self.web_preview_server.stop()
            self.web_preview_server = WebLvglPreviewServer(artifacts)
        url = self.web_preview_server.open_script(script)
        self.status_var.set(f"已打开真实 LVGL Web 预览：{url}")

    def _plan_only(self) -> None:
        """只跑第一阶段路由，看看会导入哪些 README，不连设备也能用。"""
        prompt = self._current_prompt()
        if not prompt:
            return
        self.status_var.set("正在规划实现方案…")
        self._begin_generation()

        async def job() -> Any:
            plan = await asyncio.to_thread(
                plan_for,
                self.llm_config,
                prompt,
                self.device_info,
                on_note=self._log,
                cancel=self.generate_cancel,
            )
            context, docs = catalog.load_module_docs(plan.modules)
            plan.docs[:] = docs
            return {"plan": plan, "context_chars": len(context)}

        self.worker.submit("plan", job)

    def _generate(self) -> None:
        prompt = self._current_prompt()
        if not prompt:
            return

        self.chat_text.insert("end", f"\n你：{prompt}\n")
        self.status_var.set("正在规划并生成 Debug Lua…")
        self.current_prompt = prompt
        self.current_script_mode = "debug"
        self.feedback_attempts.clear()
        self.generation_round = 0
        self._begin_generation()

        async def job() -> Any:
            history = read_recent_history(self.history_path)
            plan = await asyncio.to_thread(
                plan_for,
                self.llm_config,
                prompt,
                self.device_info,
                on_note=self._log,
                cancel=self.generate_cancel,
            )
            system_prompt = generation_system_prompt(self.llm_config.system_prompt, debug=True)
            messages = build_messages(
                prompt, self.device_info, history, system_prompt, plan
            )
            raw = await asyncio.to_thread(
                call_llm,
                self.llm_config,
                messages,
                on_progress=make_progress_reporter(self._log),
                # 降级、重试、限流这些都要让用户看见：否则界面像卡住，
                # 而且配置里真正的问题永远不会被修。
                on_note=self._log,
                cancel=self.generate_cancel,
            )
            script = extract_lua(raw)
            validate_generated_lua(script)
            return {"raw": raw, "script": script, "prompt": prompt, "plan": plan, "mode": "debug"}

        self.worker.submit("generate", job)

    def _regenerate_with_feedback(self) -> None:
        prompt = self.current_prompt or self._current_prompt()
        script = self._current_script()
        feedback = self.feedback_entry.get().strip()
        if not prompt:
            return
        if not script:
            messagebox.showwarning("脚本为空", "请先生成一版 Debug Lua")
            return
        if not feedback:
            messagebox.showwarning("缺少反馈", "请先在“代码反馈”里描述哪里有问题或希望怎么改")
            return

        self.chat_text.insert("end", f"\n用户反馈：{feedback}\n")
        self.status_var.set("正在根据反馈重生成 Debug Lua…")
        self.current_prompt = prompt
        self.current_script_mode = "debug"
        self.generation_round += 1
        feedback_attempts = [
            *self.feedback_attempts,
            Attempt(
                index=max(1, self.generation_round),
                script=script,
                error=f"用户在上位机预览或设备运行后反馈：{feedback}。请保持 DEBUG 状态，必要时添加更有定位价值的 dbg()，并只修改相关代码。",
            ),
        ]
        self._begin_generation()

        async def job() -> Any:
            history = read_recent_history(self.history_path)
            plan = self.plan
            if plan is None:
                plan = await asyncio.to_thread(
                    plan_for,
                    self.llm_config,
                    prompt,
                    self.device_info,
                    on_note=self._log,
                    cancel=self.generate_cancel,
                )
            system_prompt = generation_system_prompt(self.llm_config.system_prompt, debug=True)
            messages = build_messages(
                prompt,
                self.device_info,
                history,
                system_prompt,
                plan,
                feedback_attempts,
            )
            raw = await asyncio.to_thread(
                call_llm,
                self.llm_config,
                messages,
                on_progress=make_progress_reporter(self._log),
                on_note=self._log,
                cancel=self.generate_cancel,
            )
            next_script = extract_lua(raw)
            validate_generated_lua(next_script)
            return {
                "raw": raw,
                "script": next_script,
                "prompt": prompt,
                "plan": plan,
                "mode": "debug",
                "feedback_attempts": feedback_attempts,
            }

        self.worker.submit("generate", job)

    def _generate_iterate(self) -> None:
        """生成 → 执行 → 按设备真实报错迭代，直到成功或判定重试无意义。"""
        if not self.bridge:
            messagebox.showwarning("未连接", "自动迭代需要先连接设备")
            return
        prompt = self._current_prompt()
        if not prompt:
            return

        max_attempts = self._int_var(self.max_attempts, DEFAULT_MAX_ATTEMPTS)
        poll_timeout = float(self._int_var(self.poll_timeout, int(JOB_POLL_TIMEOUT)))
        self.chat_text.insert("end", f"\n你：{prompt}\n")
        self.status_var.set("正在生成并迭代执行…")
        self._begin_generation()

        async def job() -> Any:
            history = read_recent_history(self.history_path)
            attempts, plan = await iterate_lua(
                self.bridge,
                self.llm_config,
                prompt,
                self.device_info,
                history,
                max_attempts=max_attempts,
                poll_timeout=poll_timeout,
                on_event=self._log,
                cancel=self.generate_cancel,
                web_preview_check=bool(self.web_preview_check.get()),
            )
            return {"attempts": attempts, "plan": plan, "prompt": prompt}

        self.worker.submit("iterate", job)

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
        poll_timeout = float(self._int_var(self.poll_timeout, int(JOB_POLL_TIMEOUT)))

        async def job() -> Any:
            outcome = await run_lua_and_collect(
                self.bridge,
                script,
                poll_timeout=poll_timeout,
                on_event=lambda message: self.result_queue.put(("log", message, None)),
            )
            return {"outcome": outcome, "script": script}

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

    def _show_errors(self, errors: list[tuple[str, Exception]]) -> None:
        """一轮里攒下的错误合并成一个弹窗。

        之前是每条错误 after(0, showerror) 一次，几个任务同时失败就叠出一摞
        模态框，得逐个点掉才能碰界面。
        """
        if not errors:
            return
        if len(errors) == 1:
            name, error = errors[0]
            messagebox.showerror("上位机错误", f"[{name}] {self._format_error(error)}")
            return
        body = "\n\n".join(f"[{name}] {self._format_error(error)}" for name, error in errors)
        messagebox.showerror(f"上位机错误（{len(errors)} 条）", body)

    def _poll_results(self) -> None:
        errors: list[tuple[str, Exception]] = []
        while not self.result_queue.empty():
            name, result, error = self.result_queue.get()
            if error:
                if name == "workbuddy_audio_tx":
                    self.workbuddy_audio_tx_running = False
                    if hasattr(self, "workbuddy_audio_button"):
                        self.workbuddy_audio_button.configure(text="开始传递电脑麦克风音频")
                if isinstance(error, Cancelled):
                    # 用户主动停的，不是故障：写一行日志就够，不要弹框。
                    self.log_text.insert("end", f"[{name}] 已取消\n")
                    self.log_text.see("end")
                    self.status_var.set("已取消")
                else:
                    self.status_var.set("操作失败")
                    errors.append((name, error))
                    # 失败的详情也留在日志区：弹框点掉之后还能回看。
                    self.log_text.insert("end", f"[{name}] 失败：{self._format_error(error)}\n")
                    self.log_text.see("end")
                continue

            if name == "check":
                self.chat_text.insert("end", f"\n--- LLM 连接自检 ---\n{result}\n")
                self.chat_text.see("end")
                self.status_var.set("连接自检完成")
            elif name == "scan":
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
                payload = result or {}
                stop_resp = payload.get("stop", {})
                final_status = payload.get("status")
                stop_text = str(stop_resp.get("data") or stop_resp.get("error") or stop_resp)
                self.log_text.insert("end", f"job_stop 响应: {stop_text}\n")
                if final_status:
                    self.log_text.insert("end", f"最终状态: {final_status.describe()}\n")
                self.log_text.see("end")
                status_label = final_status.status if final_status else "未知"
                self.status_var.set(f"Job 已停止 (状态: {status_label})")
            elif name == "workbuddy_ui":
                text = str(result.get("data") or result.get("error") or result)
                self.log_text.insert("end", f"workbuddy_ui: {text}\n")
                self.log_text.see("end")
                self.status_var.set("WorkBuddy UI 已更新")
            elif name == "workbuddy_audio_tx":
                self.workbuddy_audio_tx_running = False
                if hasattr(self, "workbuddy_audio_button"):
                    self.workbuddy_audio_button.configure(text="开始传递电脑麦克风音频")
                self.status_var.set("WorkBuddy 音频传输已停止")
            elif name == "workbuddy_audio_control":
                text = str(result.get("data") or result.get("error") or result)
                self.log_text.insert("end", f"workbuddy_audio_control: {text}\n")
                self.log_text.see("end")
                self.status_var.set("WorkBuddy 音频会话已停止")
            elif name == "device_event":
                # 处理设备主动推送的事件（如 job 结束）
                event = result or {}
                event_type = event.get("type", "")
                data = event.get("data", {})
                # data 可能是字符串或字典
                if isinstance(data, str):
                    self.log_text.insert("end", f"[设备事件] {event_type}: {data}\n")
                    self.log_text.see("end")
                elif event_type == "job_event" and isinstance(data, dict):
                    job_id = data.get("job_id", "")
                    status = data.get("status", "")
                    self.log_text.insert("end", f"[设备事件] job {job_id} 状态: {status}\n")
                    self.log_text.see("end")
                    if status in {"stopped", "done", "failed", "timeout"}:
                        self.status_var.set(f"Job 已{status}")
            elif name == "log":
                self.log_text.insert("end", f"{result}\n")
                self.log_text.see("end")
            elif name == "plan":
                payload = result or {}
                plan = payload.get("plan")
                if plan is not None:
                    self.plan = plan
                    self.chat_text.insert(
                        "end",
                        f"\n{plan.describe()}\nREADME 上下文大小：{payload.get('context_chars', 0)} 字符\n",
                    )
                    self.chat_text.see("end")
                self.status_var.set("模块选择完成")
            elif name == "generate":
                payload = result or {}
                raw = str(payload.get("raw") or "")
                script = str(payload.get("script") or "")
                plan = payload.get("plan")
                mode = str(payload.get("mode") or "debug")
                prompt = str(payload.get("prompt") or self.current_prompt)
                feedback_attempts = payload.get("feedback_attempts")
                if plan is not None:
                    self.plan = plan
                    self.chat_text.insert("end", f"\n{plan.describe()}\n")
                if raw:
                    self.chat_text.insert("end", f"LLM：\n{raw}\n")
                self.chat_text.see("end")
                if isinstance(feedback_attempts, list):
                    self.feedback_attempts = feedback_attempts
                    self.feedback_entry.delete(0, "end")
                else:
                    self.feedback_attempts.clear()
                self._set_script(script, mode=mode, prompt=prompt, plan=plan)
                self.status_var.set("Debug Lua 已生成，可先看模拟预览再上传执行" if mode != "release" else "Release Lua 已生成")
            elif name == "run":
                payload = result or {}
                outcome = payload.get("outcome")
                script = str(payload.get("script") or "")
                if outcome is not None:
                    self.log_text.insert("end", f"{outcome.describe()}\n")
                    self.log_text.see("end")
                    append_history(
                        self.history_path,
                        {
                            "prompt": self.prompt.get("1.0", "end").strip(),
                            "script": script,
                            "result": outcome.describe(),
                            "ok": outcome.ok,
                        },
                    )
                    if not outcome.ok:
                        self.feedback_attempts.append(
                            Attempt(
                                index=len(self.feedback_attempts) + 1,
                                script=script,
                                outcome=outcome,
                            )
                        )
                    self._refresh_preview()
                    self.status_var.set("执行成功" if outcome.ok else f"执行未成功：{outcome.status}")
            elif name == "iterate":
                payload = result or {}
                attempts = payload.get("attempts") or []
                plan = payload.get("plan")
                if plan is not None:
                    self.plan = plan
                if not attempts:
                    self.status_var.set("没有产生任何生成结果")
                    continue
                final = attempts[-1]
                for attempt in attempts:
                    self.chat_text.insert("end", f"\n第 {attempt.index} 轮生成：\n{attempt.script}\n")
                    if attempt.outcome:
                        self.chat_text.insert("end", f"设备结果：{attempt.outcome.describe()}\n")
                    elif attempt.error:
                        self.chat_text.insert("end", f"未执行：{attempt.error}\n")
                self.chat_text.see("end")
                self.script_text.delete("1.0", "end")
                self.script_text.insert("1.0", final.script)
                self.log_text.insert("end", f"{summarize_attempts(attempts)}\n")
                self.log_text.see("end")
                append_history(
                    self.history_path,
                    {
                        "prompt": str(payload.get("prompt") or ""),
                        "modules": plan.modules if plan else [],
                        "docs": plan.docs if plan else [],
                        "attempts": len(attempts),
                        "script": final.script,
                        "result": final.outcome.describe() if final.outcome else final.error,
                        "ok": final.ok,
                    },
                )
                self.status_var.set("迭代完成：成功" if final.ok else "迭代结束：未成功")

        # 放在循环外：一轮里所有错误合成一个弹窗，不再叠出一摞模态框。
        # 按钮状态也统一在这里收，省得每条成功分支各写一遍（"iterate 没产出"
        # 那条分支就是靠 continue 跳过去的，很容易漏）。
        self._end_generation()
        self._show_errors(errors)
        self.after(120, self._poll_results)

    def _clear_chat(self) -> None:
        self.chat_text.delete("1.0", "end")
        self.chat_text.insert("1.0", "对话已清空。\n")
        self.status_var.set("已清空对话区")

    def _close(self) -> None:
        if self.web_preview_server is not None:
            self.web_preview_server.stop()
            self.web_preview_server = None
        if self.bridge:
            self.worker.submit("disconnect", self.bridge.disconnect)
        self.worker.stop()
        self.destroy()


if __name__ == "__main__":
    HostVibeApp().mainloop()
