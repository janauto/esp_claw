# ESP-Claw 安卓端上位机

`tools/host_vibe_coding` 的安卓版：手机用自然语言生成设备 Lua，经 BLE bridge 上传执行，
读回设备端真实结果并按报错自动迭代。

电脑端那份代码一行没改，两边共用同一套上下文资产（模块目录、playbook、README），
由 `tool/sync_assets.py` 从 `tools/host_vibe_coding` 单向导出。

```text
安卓 App
  ├─ 本地 LLM 配置与历史（SharedPreferences，不写设备端 NVS）
  ├─ 第一阶段：LLM 判断需要哪些设备模块（assets/catalog.json）
  ├─ 第二阶段：按需载入对应 README 后生成 Lua
  └─ BLE GATT
       └─ ESP-Claw C ble_host_bridge
            ├─ 设备端 cap_lua 执行 Lua
            └─ job_status / job_tail 回传真实执行结果
```

## 打包 APK

需要 Flutter SDK（3.22+，含 Dart 3.4+）和 Android SDK。本仓库不包含 `android/`
平台目录，第一次构建要先生成。

### 1. 装 Flutter

```bash
# 官方安装文档：https://docs.flutter.dev/get-started/install/linux/android
flutter doctor          # 确认 Android toolchain 是绿的
```

`flutter doctor` 报 Android licenses 未接受时：

```bash
flutter doctor --android-licenses
```

### 2. 生成平台目录并装依赖

```bash
cd tools/mobile_vibe_coding
./tool/setup_android.sh
```

这个脚本做三件事：

1. 在**临时目录**里跑 `flutter create` 生成脚手架，只把 `android/` 拷过来。
   刻意不在本工程直接跑 `flutter create .`——那条命令会覆盖 `lib/main.dart`。
2. 跑 `tool/patch_android.py`，给清单补 BLE 权限、把 `minSdk` 抬到 21。
3. `flutter pub get`。

可重复执行：`android/` 已存在时只做权限补丁。

### 3. 同步上下文资产

```bash
python3 tool/sync_assets.py
```

从 `tools/host_vibe_coding` 导出 40 个模块条目、37 份 README、3 份 playbook 到
`assets/`（约 252 KB，随 APK 一起打包）。**仓库里已经有一份导出结果**，只有在
改了模块目录或 README 之后才需要重跑。

### 4. 打包

```bash
# 调试包，直接装到连着的手机上
flutter run

# Release APK（单个通用包，约 20 MB）
flutter build apk --release

# 按 ABI 拆包，每个约 8 MB，体积敏感时用
flutter build apk --release --split-per-abi
```

产物在：

```text
build/app/outputs/flutter-apk/app-release.apk
```

装到手机：

```bash
adb install -r build/app/outputs/flutter-apk/app-release.apk
```

### 签名

`flutter build apk --release` 默认用 debug key 签名，能装能用，但不能上应用商店，
且换电脑重新打包后签名不同、需要卸载重装。要正式签名就建 keystore：

```bash
keytool -genkey -v -keystore ~/esp-claw-vibe.jks \
  -keyalg RSA -keysize 2048 -validity 10000 -alias vibe
```

然后写 `android/key.properties`（**不要提交进 git**）：

```properties
storePassword=<密码>
keyPassword=<密码>
keyAlias=vibe
storeFile=/home/<你>/esp-claw-vibe.jks
```

并在 `android/app/build.gradle.kts` 的 `android { }` 里引用它，
按 [官方签名文档](https://docs.flutter.dev/deployment/android#signing-the-app) 配置。

## 使用

1. **设备页** → 扫描 → 连接。按 service UUID 扫描，所以设备广播名拿不到也能扫到。
   连上后自动取一次 `device_info`，作为生成阶段的上下文。
2. **设置**（右上角齿轮）→ 选服务商、填 API Key。Key 只存在手机本地。
3. **需求页** → 输入自然语言 → 三个按钮：
   - 「查看模块选择」：只跑第一阶段路由，显示会导入哪些 README，不连设备也能用；
   - 「生成 Lua」：生成后落到 Lua 页，可手改再上传；
   - 「生成并自动迭代」：按设备报错自动改脚本，轮数和等待秒数在页面下方可调。
4. **Lua 页** → 编辑、复制/粘贴、上传并执行。
5. **日志页** → 看设备返回的真实结果，可查询 / 停止 job、拉完整日志。

## 权限

Android 12（API 31）起用 `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`，
`neverForLocation` 表示不用扫描结果推位置。Android 11 及以下用旧的
`BLUETOOTH` / `BLUETOOTH_ADMIN`，且**必须**有定位权限才能拿到扫描结果——
这是系统限制，不是应用能绕过的。首次扫描时会弹窗申请。

扫不到设备时依次确认：手机蓝牙已打开、设备已开机且 BLE bridge 已启用、
设备没有被其它手机占着连接（bridge 同时只接受一个连接）。

## 与电脑端的行为差异

| | 电脑端 | 安卓端 |
|---|---|---|
| BLE 栈 | bleak | flutter_blue_plus |
| 配置存储 | `~/.esp_claw/host_vibe_coding/` | SharedPreferences |
| 模块 README | 直接读仓库文件 | 打包进 assets（需 `sync_assets.py` 同步） |
| 请求写入 | write-without-response | write-with-response |

最后一条是有意为之：设备端 `ble_host_bridge` 的请求队列只有 6 格且分片必须严格
按序到达（见 `ble_host_bridge.c` 的 `BLE_HOST_BRIDGE_REQUEST_QUEUE_LEN`）。
Android 的写入调度比 bleak 激进，不带响应会把队列冲满导致整帧被丢，
所以这里串行发送并等每一帧的确认。

其余行为——两阶段上下文、playbook 匹配、常驻脚本判定、六条迭代停止条件——
与电脑端逐条对齐。

## 迭代停止条件

和电脑端一致。为了不白烧 token，遇到下面几种情况会**立即停止**而不是用满轮数：

| 情况 | 为什么不重试 |
|------|--------------|
| `status=done` | 已经成功 |
| 常驻任务 `running`/`timeout` | 脚本是长跑的（游戏主循环、交互界面），跑着就是预期行为 |
| `status=stopped` | 是有人主动停的 |
| 新脚本与上一版实质相同（忽略注释/空白） | LLM 没有真正响应报错，再试结果一样 |
| 连续两轮同一个错误签名 | LLM 没有收敛到解 |
| 设备没返回 summary 也没返回日志 | 没有可用于修脚本的信息，改动等于瞎猜 |

只有真正的 Lua 报错（`status=failed`）且拿到了诊断信息，才会消耗下一轮。
常驻类任务（游戏、界面）的成败方向是相反的：`done` 且运行不到 5 秒说明画面
闪一下就退了，算失败。

## 测试

```bash
flutter test
```

用假 bridge 和固定 LLM 返回覆盖迭代循环的停止条件、协议解析和脚本校验，
不需要真设备。
