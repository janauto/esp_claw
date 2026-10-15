"""Shared app installation dialog for the Linux/macOS and Windows frontends."""
from __future__ import annotations

import tkinter as tk
import json
import tempfile
from pathlib import Path
from tkinter import filedialog, messagebox, simpledialog, ttk

from app_package import app_request, build_package, download_app, install_app


class AppManagerMixin:
    def _open_app_manager(self) -> None:
        if getattr(self, "app_dialog", None) is not None and self.app_dialog.winfo_exists():
            self.app_dialog.lift()
            return
        window = self.app_dialog = tk.Toplevel(self)
        window.title("SD 用户应用管理")
        window.geometry("720x650")
        window.columnconfigure(1, weight=1)
        self.app_fields = {key: tk.StringVar(value=value) for key, value in (
            ("app_id", "my_app"), ("title", "My App"), ("selected", ""), ("unselected", ""), ("color", "EDC22E"), ("pill_color", ""))}
        for row, (key, label) in enumerate((("app_id", "应用 ID"), ("title", "显示名称"),
                                           ("selected", "选中图片"), ("unselected", "未选中图片"), ("color", "边框颜色 RGB"),
                                           ("pill_color", "标签颜色 RGB（可选）"))):
            ttk.Label(window, text=label).grid(row=row, column=0, sticky="w", padx=12, pady=6)
            ttk.Entry(window, textvariable=self.app_fields[key]).grid(row=row, column=1, sticky="ew", pady=6)
            if key in {"selected", "unselected"}:
                ttk.Button(window, text="浏览…", command=lambda k=key: self._pick_app_icon(k)).grid(row=row, column=2, padx=12)
        ttk.Label(window, text="系统应用由固件提供；这里管理 SD 卡 apps 目录中的用户应用。\n图片留空使用默认图标；只选一张时自动生成暗色状态。",
                  wraplength=640).grid(row=6, column=0, columnspan=3, sticky="w", padx=12, pady=8)
        actions = ttk.Frame(window)
        actions.grid(row=7, column=0, columnspan=3, sticky="w", padx=12, pady=8)
        self.app_action_buttons = []
        for index, (label, command) in enumerate((("新增当前 Lua", self._install_current_app), ("导出到 SD 文件夹", self._export_current_app),
                               ("刷新列表", self._list_apps), ("获取应用信息", self._get_app_info),
                               ("读取代码和图标", self._download_selected_app), ("删除选中应用", self._remove_app), ("重命名选中应用", self._rename_app))):
            button = ttk.Button(actions, text=label, command=command)
            button.grid(row=index // 3, column=index % 3, sticky="ew", padx=(0, 6), pady=3)
            self.app_action_buttons.append(button)
        self.app_tree = ttk.Treeview(window, columns=("id", "title"), show="headings", height=7, selectmode="browse")
        self.app_tree.heading("id", text="应用 ID")
        self.app_tree.heading("title", text="显示名称")
        self.app_tree.grid(row=8, column=0, columnspan=3, sticky="nsew", padx=12)
        window.rowconfigure(8, weight=1)
        self.app_details = tk.Text(window, height=6, wrap="word", state="disabled")
        self.app_details.grid(row=9, column=0, columnspan=3, sticky="nsew", padx=12, pady=6)
        self.app_status = tk.StringVar(value="应用按组存储在 SD 卡上。同名应用需删除后重新安装。")
        ttk.Label(window, textvariable=self.app_status, wraplength=680).grid(row=10, column=0, columnspan=3, sticky="w", padx=12, pady=8)
        self._set_apps_busy(getattr(self, "_apps_busy", False))
        if self.bridge and not getattr(self, "_apps_busy", False):
            self._list_apps()

    def _pick_app_icon(self, key):
        path = filedialog.askopenfilename(parent=self.app_dialog, filetypes=[("图标", "*.png *.jpg *.jpeg *.bmp *.bin"), ("全部文件", "*")])
        if path:
            self.app_fields[key].set(path)

    def _current_app_package(self):
        from host_vibe import validate_generated_lua
        script = self.script_text.get("1.0", "end").strip()
        if not script:
            raise ValueError("请先生成或加载 Lua 脚本")
        validate_generated_lua(script)
        fields = {key: value.get().strip() for key, value in self.app_fields.items()}
        return build_package(script, fields["app_id"], fields["title"],
                             selected=Path(fields["selected"]) if fields["selected"] else None,
                             unselected=Path(fields["unselected"]) if fields["unselected"] else None,
                             frame_color=int(fields["color"].removeprefix("#"), 16),
                             pill_color=int(fields["pill_color"].removeprefix("#"), 16) if fields["pill_color"] else None)

    def _set_apps_busy(self, busy):
        self._apps_busy = busy
        if getattr(self, "app_dialog", None) is not None and self.app_dialog.winfo_exists():
            for button in self.app_action_buttons:
                button.configure(state="disabled" if busy else "normal")

    def _submit_app_job(self, name, job):
        if self._apps_busy:
            return
        if not self.bridge:
            messagebox.showwarning("未连接", "请先连接设备", parent=self.app_dialog)
            return
        self._set_apps_busy(True)
        self.app_status.set("正在处理… 图片通过 BLE 上传可能需要几分钟。")
        self.worker.submit(name, job, replace=False)

    def _install_current_app(self):
        try:
            package = self._current_app_package()
        except Exception as error:
            messagebox.showerror("应用信息有误", str(error), parent=self.app_dialog)
            return
        bridge = self.bridge
        async def job():
            return await install_app(bridge, package, lambda text: self.result_queue.put(("log", text, None)))
        self._submit_app_job("apps_install", job)

    def _export_current_app(self):
        try:
            package = self._current_app_package()
            directory = filedialog.askdirectory(title="选择 SD 卡的 apps 文件夹", parent=self.app_dialog)
            if directory:
                target = package.export(Path(directory))
                self.app_status.set(f"已导出到 {target}；重新插卡并重启设备即可加载。")
        except Exception as error:
            messagebox.showerror("导出失败", str(error), parent=self.app_dialog)

    def _list_apps(self):
        bridge = self.bridge
        async def job():
            return await app_request(bridge, "app_list")
        self._submit_app_job("apps_list", job)

    def _selected_app_id(self):
        selection = self.app_tree.selection()
        if not selection:
            self.app_status.set("请先从列表中选择一个用户应用。")
            return None
        return selection[0]

    def _get_app_info(self):
        app_id = self._selected_app_id()
        if app_id is None:
            return
        bridge = self.bridge
        async def job():
            return await app_request(bridge, "app_info", app_id=app_id)
        self._submit_app_job("apps_info", job)

    def _download_selected_app(self):
        app_id = self._selected_app_id()
        if app_id is None:
            return
        if self.script_text.get("1.0", "end").strip() and not messagebox.askyesno(
                "载入设备代码", "设备应用的代码将替换当前编辑器内容，继续？", parent=self.app_dialog):
            return
        bridge = self.bridge
        async def job():
            return await download_app(bridge, app_id, lambda text: self.result_queue.put(("log", text, None)))
        self._submit_app_job("apps_download", job)

    def _show_app_details(self, info):
        self.app_details.configure(state="normal")
        self.app_details.delete("1.0", "end")
        self.app_details.insert("1.0", json.dumps(info, ensure_ascii=False, indent=2))
        self.app_details.configure(state="disabled")

    def _load_downloaded_app(self, package):
        script = package.files["main.lua"].decode("utf-8")
        directory = tempfile.TemporaryDirectory(prefix="esp-claw-app-")
        try:
            target = package.export(Path(directory.name))
        except Exception:
            directory.cleanup()
            raise
        previous = getattr(self, "_app_download_dir", None)
        self._app_download_dir = directory
        if previous is not None:
            previous.cleanup()
        self.script_text.delete("1.0", "end")
        self.script_text.insert("1.0", script)
        self.current_prompt = ""
        self.plan = None
        if hasattr(self, "current_script_mode"):
            self.current_script_mode = "release"
        for key in ("app_id", "title"):
            self.app_fields[key].set(package.manifest[key])
        self.app_fields["color"].set(f"{package.manifest['frame_color']:06X}")
        self.app_fields["pill_color"].set(f"{package.manifest['pill_color']:06X}")
        for key in ("selected", "unselected"):
            filename = f"icon_{key}.bin"
            self.app_fields[key].set(str(target / filename) if filename in package.files else "")
        self._show_app_details(package.manifest)
        if hasattr(self, "_refresh_preview"):
            self._refresh_preview()
        self.app_status.set("已读取代码、名称和图标；可导出到本地，修改后用新 ID 新增或删除旧版后重新安装。")
        self.status_var.set("设备应用已载入 Lua 编辑器")

    def destroy(self):
        directory = getattr(self, "_app_download_dir", None)
        if directory is not None:
            directory.cleanup()
            self._app_download_dir = None
        super().destroy()

    def _remove_app(self):
        selection = self.app_tree.selection()
        if not selection:
            return
        app_id = selection[0]
        if not messagebox.askyesno("删除应用", f"从设备删除 {app_id} 及其脚本、图标？", parent=self.app_dialog):
            return
        bridge = self.bridge
        async def job():
            return await app_request(bridge, "app_remove", app_id=app_id)
        self._submit_app_job("apps_remove", job)

    def _handle_apps_result(self, name, result, error=None):
        self._set_apps_busy(False)
        if getattr(self, "app_dialog", None) is None or not self.app_dialog.winfo_exists():
            return
        if error:
            self.app_status.set(str(error))
        elif name == "apps_info":
            self._show_app_details(result)
            self.app_status.set("已获取用户应用的配置、脚本和图标路径及文件信息。")
        elif name == "apps_download":
            try:
                self._load_downloaded_app(result)
            except (OSError, UnicodeError, ValueError) as error:
                self.app_status.set(f"载入应用失败：{error}")
                messagebox.showerror("载入应用失败", str(error), parent=self.app_dialog)
        elif name == "apps_list":
            self.app_tree.delete(*self.app_tree.get_children())
            for app in result or []:
                self.app_tree.insert("", "end", iid=app["app_id"], values=(app["app_id"], app["title"]))
            self.app_status.set(f"已安装 {len(result or [])} 个应用；选中后可删除。")
        else:
            message = {"apps_install": "应用已安装", "apps_rename": "应用已重命名"}.get(name, "应用已删除")
            message += "；请重启设备刷新主页。" if result.get("refresh") == "restart_required" else "；返回设备主页后刷新。"
            self.status_var.set(message)
            self.app_status.set(message)
            messagebox.showinfo("设备应用栏", message, parent=self.app_dialog)
            self._list_apps()

    def _rename_app(self):
        app_id = self._selected_app_id()
        if app_id is None:
            return
        title = simpledialog.askstring("重命名", "新的显示名称（最多95个UTF-8字节）", parent=self.app_dialog)
        if title is None:
            return
        from app_package import validate_identity
        try:
            validate_identity(app_id, title)
        except ValueError as error:
            messagebox.showerror("名称无效", str(error), parent=self.app_dialog)
            return
        bridge = self.bridge
        async def job():
            return await app_request(bridge, "app_rename", app_id=app_id, title=title)
        self._submit_app_job("apps_rename", job)
